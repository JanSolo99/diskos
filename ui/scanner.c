/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* First-run / rescan music scanner for diskOS.
 *
 * The stock V2.09 scanner (tag 0622) is a no-op - it never populates song.db from the SD.
 * So diskOS scans itself: walk the SD for audio files, read their tags, and rebuild the
 * SONG table of /usr/data/fiio/db/song.db (the same DB the library UI reads). Playlists,
 * favourites and resume state (CUSTOM_PLAYLIST/PLAYLIST_INFO/MY_LOVE/MEMORY_PLAY) are keyed
 * by PATH and left intact, so a rescan doesn't lose them.
 *
 * Tag support: MP3 ID3v2.2/2.3/2.4 (+ID3v1 fallback), FLAC and Ogg/Opus VORBIS_COMMENT,
 * MP4/M4A `ilst` atoms, APEv2 (ape/wv/mpc/tta) and DSF (ID3v2 at the DSD header pointer).
 * WAV/AIFF/DFF/WMA have no parser yet and fall back to the filename - but they are still
 * INDEXED, so every playable file on the card appears in the library. See AUDIO_EXT.
 *
 * Runs on a detached worker thread; progress is published under a mutex for the UI to poll.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include "sqlite3.h"
#include "scanner.h"

#ifndef DB_PATH
#define DB_PATH   "/usr/data/fiio/db/song.db"
#endif
#ifndef SCAN_ROOT
#define SCAN_ROOT "/tmp/sdcard"
#endif
#define MAXPATH   1024
#define TAGLEN    256

/* One file's parsed tags. Passed by POINTER through every container parser so that
 * adding a field (album artist, disc, track) costs one struct member instead of a
 * signature change in a dozen places. */
typedef struct {
    char title[TAGLEN], artist[TAGLEN], album[TAGLEN], genre[TAGLEN];
    char album_artist[TAGLEN];   /* TPE2 / ALBUMARTIST / aART - empty when the file has none */
    int  disc, track;            /* 0 = unknown; sorts LAST, matching the player's ORDER BY */
} tags_t;

/* "7", "07", "7/12", " 7 " -> 7.  Unparseable or absent -> 0 (= unknown).
 * Track/disc tags are text in ID3 and Vorbis and routinely carry the "/total" half. */
static int tag_num(const char *s){
    while(*s == ' ' || *s == '	') s++;
    int v = 0, any = 0;
    while(*s >= '0' && *s <= '9'){ if(v < 100000) v = v*10 + (*s - '0'); any = 1; s++; }
    return any ? v : 0;
}

/* ---- progress (published to the LVGL thread) ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_active;          /* a scan is running */
static int g_done;            /* files inserted so far */
static int g_total;           /* files found (0 until the walk completes) */
static int g_finished_seq;    /* bumped when a scan finishes (UI edge-detect) */
static int g_no_sd;           /* last scan aborted because the SD wasn't mounted */
static int g_scan_err;        /* worker-thread only: any I/O or DB-insert failure during the walk
                               * -> the rebuild is incomplete and must NOT commit over the library */
static int g_skipped;         /* files skipped this scan (unstat-able junk / overlong path) - logged, not fatal */
static int g_prune_blocked;   /* an OVERLONG (unknowable) path was skipped -> its row can't be preserved in
                               * `seen`, so the vanished-row prune must not run this scan. ENOENT/unreadable
                               * audio files ARE preserved (added to seen), so they don't block the prune. */
/* Live progress for the scan screen. The merge pass alone can only report "N files so
 * far" with no idea how many are coming, which is why the old UI could say nothing
 * better than "scan requested". So the worker makes a cheap COUNTING pass first -
 * readdir plus an extension test, no file is opened - and publishes the total. The
 * merge pass then has a real denominator, and the UI can show a real percentage. */
static int  g_phase;          /* 0 = counting, 1 = reading tags */
static int  g_expect;         /* audio files the counting pass found (0 while counting) */
static char g_curname[80];    /* basename of the file being read right now */

int scanner_active(void){ pthread_mutex_lock(&g_mu); int a=g_active; pthread_mutex_unlock(&g_mu); return a; }
void scanner_progress(int *done, int *total){
    pthread_mutex_lock(&g_mu);
    if(done) *done=g_done;
    if(total) *total=g_total;
    pthread_mutex_unlock(&g_mu);
}
int scanner_take_finished(void){
    pthread_mutex_lock(&g_mu);
    static int last=0; int f = (g_finished_seq!=last); last=g_finished_seq;
    pthread_mutex_unlock(&g_mu);
    return f;
}
/* Live progress for the scan screen: phase, files done, files expected, current file. */
void scanner_progress_ex(int *phase, int *done, int *expect, char *name, int cap){
    pthread_mutex_lock(&g_mu);
    if(phase)  *phase  = g_phase;
    if(done)   *done   = g_done;
    if(expect) *expect = g_expect;
    if(name && cap > 0) snprintf(name, cap, "%s", g_curname);
    pthread_mutex_unlock(&g_mu);
}

/* 1 if the most recent scan did nothing because no SD was mounted (library was kept). */
int scanner_no_sd(void){ pthread_mutex_lock(&g_mu); int v=g_no_sd; pthread_mutex_unlock(&g_mu); return v; }

/* ---- helpers ---- */
static void str_trim(char *s){
    char *p=s; while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
    int n=(int)strlen(s);
    while(n>0 && (unsigned char)s[n-1]<=' ') s[--n]=0;
}
/* NAME_CODE/TITLE_CODE/... : first 4 chars packed big-endian, 31-bit (matches the stock DB) */
static int code4(const char *s){
    char b[5]="\0\0\0\0"; int j=0;
    for(const char *p=s?s:""; *p && j<4; p++) b[j++]=(char)tolower((unsigned char)*p);
    unsigned v=0; for(int i=0;i<4;i++) v=(v<<8)|((unsigned char)b[i]);
    return (int)(v & 0x7fffffff);
}
static int has_ext(const char *name, const char *ext){
    size_t nl=strlen(name), el=strlen(ext);
    return nl>el && !strcasecmp(name+nl-el, ext);
}
/* Every container diskOS indexes, in ONE place. This list drives three things that
 * must never disagree: which files the walk picks up, which files get a tag parser,
 * and which rows the vanished-file prune is allowed to delete. When only mp3/flac/wav
 * were listed here, everything else on the card was simply invisible in the library -
 * the "some tracks don't show up after scanning, but they do on stock" report.
 *
 * Tag support per container:
 *   .mp3                       ID3v2 (+ ID3v1 fallback)
 *   .flac                      VORBIS_COMMENT
 *   .ogg .oga .opus            Ogg-framed VORBIS_COMMENT / OpusTags
 *   .m4a .m4b .mp4 .aac        MP4 `moov/udta/meta/ilst` atoms
 *   .ape .wv .mpc .tta         APEv2
 *   .dsf                       ID3v2 at the pointer in the DSD chunk header
 *   .wav .aif .aiff .dff .wma  no tag parser yet -> filename is used as the title
 *
 * A container with no parser is still INDEXED (that is the point): a filename-titled
 * row you can find and play beats a track that does not exist as far as the UI is
 * concerned. */
static const char *const AUDIO_EXT[] = {
    ".mp3", ".flac", ".wav", ".ogg", ".oga", ".opus",
    ".m4a", ".m4b", ".mp4", ".aac", ".ape", ".wv",
    ".mpc", ".tta", ".dsf", ".dff", ".aif", ".aiff", ".wma",
};
#define N_AUDIO_EXT ((int)(sizeof(AUDIO_EXT)/sizeof(AUDIO_EXT[0])))

static int is_audio(const char *name){
    for(int i=0;i<N_AUDIO_EXT;i++) if(has_ext(name, AUDIO_EXT[i])) return 1;
    return 0;
}

/* ---- text encoding -> UTF-8 (bounded) ---- */
static void put_u8(char **o, char *end, unsigned cp){
    char *p=*o;
    if(cp<0x80){ if(p<end) *p++=(char)cp; }
    else if(cp<0x800){ if(p+1<end){ *p++=(char)(0xC0|(cp>>6)); *p++=(char)(0x80|(cp&0x3F)); } }
    else { if(p+2<end){ *p++=(char)(0xE0|(cp>>12)); *p++=(char)(0x80|((cp>>6)&0x3F)); *p++=(char)(0x80|(cp&0x3F)); } }
    *o=p;
}
/* Decode an ID3 text-frame body (enc byte already consumed by caller). enc: 0=Latin1,
 * 1=UTF-16 w/ BOM, 2=UTF-16BE, 3=UTF-8. Writes a NUL-terminated UTF-8 string into out. */
static void id3_decode(int enc, const unsigned char *in, int n, char *out, int cap){
    char *o=out, *end=out+cap-1;
    if(enc==0){                                  /* Latin-1 */
        for(int i=0;i<n && in[i];i++) put_u8(&o,end,in[i]);
    } else if(enc==3){                           /* UTF-8 (copy, stop at NUL) */
        for(int i=0;i<n && in[i];i++){ if(o<end) *o++=(char)in[i]; }
    } else {                                     /* UTF-16 (1=BOM, 2=BE) */
        int be = (enc==2), i=0;
        if(enc==1 && n>=2){ if(in[0]==0xFF && in[1]==0xFE) be=0; else if(in[0]==0xFE && in[1]==0xFF) be=1; i=2; }
        for(; i+1<n; i+=2){
            unsigned u = be ? (in[i]<<8|in[i+1]) : (in[i+1]<<8|in[i]);
            if(u==0) break;
            if(u>=0xD800 && u<=0xDBFF && i+3<n){  /* surrogate pair */
                unsigned lo = be ? (in[i+2]<<8|in[i+3]) : (in[i+3]<<8|in[i+2]);
                if(lo>=0xDC00 && lo<=0xDFFF){ u=0x10000+((u-0xD800)<<10)+(lo-0xDC00); i+=2;
                    char *p=o; if(p+3<end){ *p++=(char)(0xF0|(u>>18)); *p++=(char)(0x80|((u>>12)&0x3F)); *p++=(char)(0x80|((u>>6)&0x3F)); *p++=(char)(0x80|(u&0x3F)); } o=p; continue; }
            }
            if(u>=0xD800 && u<=0xDFFF) continue;  /* lone surrogate */
            put_u8(&o,end,u);
        }
    }
    *o=0; str_trim(out);
}
/* "(13)" or "13" style ID3 numeric genre -> leave as-is if it's plain text; we don't map the
 * 148 legacy IDs (rarely used in modern tags), just strip a leading "(N)" wrapper. */
static void genre_clean(char *g){
    if(g[0]=='(' ){ char *e=strchr(g,')'); if(e && e[1]) memmove(g, e+1, strlen(e+1)+1); }
    str_trim(g);
}

/* Cast before the shift: p[0] promotes to int, so a byte >= 0x80 shifted 24 places
 * overflows a signed 32-bit int - undefined behaviour, and reachable from any file
 * with a large or corrupt size field (UBSan-confirmed while fuzzing MP4 atoms). */
static uint32_t be32(const unsigned char *p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static uint32_t synch32(const unsigned char *p){ return (p[0]<<21)|(p[1]<<14)|(p[2]<<7)|p[3]; }

/* Read ID3v2 text frames (TIT2/TPE1/TALB/TCON, or v2.2 TT2/TP1/TAL/TCO). Returns 1 if the
 * header was present. Fields it doesn't find are left untouched. */
static int id3v2_read(FILE *f, tags_t *t){
    unsigned char h[10];
    if(fread(h,1,10,f)!=10) return 0;
    if(memcmp(h,"ID3",3)!=0) return 0;
    int ver=h[3];
    int unsync=(h[5]&0x80)!=0, exthdr=(h[5]&0x40)!=0;
    long tagsize = synch32(h+6);
    if(tagsize<=0 || tagsize>20*1024*1024) return 1;
    unsigned char *buf=malloc((size_t)tagsize);
    if(!buf) return 1;
    if(fread(buf,1,(size_t)tagsize,f)!=(size_t)tagsize){ free(buf); return 1; }
    long p=0;
    if(exthdr && ver>=3 && tagsize>=6){           /* skip the extended header if present */
        if(ver>=4){ long es=synch32(buf);      if(es>0 && es<=tagsize) p+=es; }        /* v2.4: size incl itself */
        else      { long es=be32(buf);         if(es>0 && es<=tagsize-4) p+=4+es; }    /* v2.3: excl the 4 size bytes */
    }
    (void)unsync;
    int idlen = (ver==2)?3:4, fhdr=(ver==2)?6:10;
    while(p + fhdr <= tagsize){
        char id[5]={0}; memcpy(id, buf+p, idlen);
        if(id[0]==0) break;                       /* padding */
        long fsize;
        if(ver==2) fsize = (buf[p+3]<<16)|(buf[p+4]<<8)|buf[p+5];
        else if(ver==4) fsize = synch32(buf+p+4);
        else fsize = be32(buf+p+4);
        if(fsize<=0 || fsize > tagsize - p - fhdr) break;   /* overflow-safe (tagsize-p-fhdr >= 0) */
        const unsigned char *body = buf+p+fhdr;
        if(id[0]=='T' && fsize>=1){               /* text frame: enc byte + text */
            int enc=body[0];
            char val[TAGLEN]; id3_decode(enc, body+1, (int)fsize-1, val, sizeof val);
            if(val[0]){
                const char *k = id;
                if(!strcmp(k,"TIT2")||!strcmp(k,"TT2")) snprintf(t->title,TAGLEN,"%s",val);
                else if(!strcmp(k,"TPE1")||!strcmp(k,"TP1")) snprintf(t->artist,TAGLEN,"%s",val);
                else if(!strcmp(k,"TALB")||!strcmp(k,"TAL")) snprintf(t->album,TAGLEN,"%s",val);
                else if(!strcmp(k,"TCON")||!strcmp(k,"TCO")){ snprintf(t->genre,TAGLEN,"%s",val); genre_clean(t->genre); }
                /* TPE2 is the ALBUM artist ("50 Cent"), TPE1 the TRACK artist
                 * ("50 Cent feat. Lloyd Banks") - grouping on TPE2 is what stops one
                 * artist fragmenting into a row per collaborator. TRCK/TPOS are text
                 * frames and are usually written "5/12", hence tag_num(). */
                else if(!strcmp(k,"TPE2")||!strcmp(k,"TP2")) snprintf(t->album_artist,TAGLEN,"%s",val);
                else if(!strcmp(k,"TRCK")||!strcmp(k,"TRK")) t->track = tag_num(val);
                else if(!strcmp(k,"TPOS")||!strcmp(k,"TPA")) t->disc  = tag_num(val);
            }
        }
        p += fhdr + fsize;
    }
    free(buf);
    return 1;
}
/* ID3v1: last 128 bytes "TAG" + 30+30+30 title/artist/album (Latin-1). Fallback only. */
static void id3v1_read(FILE *f, tags_t *t){
    if(fseek(f,-128,SEEK_END)!=0) return;
    unsigned char v1[128];                       /* not `t` - that is the tags_t param now */
    if(fread(v1,1,128,f)!=128) return;
    if(memcmp(v1,"TAG",3)!=0) return;
    char tmp[64];
    if(!t->title[0]){  id3_decode(0, v1+3,  30, tmp,sizeof tmp); if(tmp[0]) snprintf(t->title, TAGLEN,"%s",tmp); }
    if(!t->artist[0]){ id3_decode(0, v1+33, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(t->artist,TAGLEN,"%s",tmp); }
    if(!t->album[0]){  id3_decode(0, v1+63, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(t->album, TAGLEN,"%s",tmp); }
    /* ID3v1.1 puts the track number in byte 126 when byte 125 is NUL (plain v1 has
     * a 30-byte comment there, so a non-NUL 125 means there is no track number). */
    if(!t->track && v1[125] == 0 && v1[126]) t->track = v1[126];
}

static uint32_t le32(const unsigned char *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* Parse a VORBIS_COMMENT payload: vendor string, then count x "KEY=VALUE" entries,
 * all little-endian length-prefixed. Shared by FLAC (metadata block type 4) and Ogg
 * (the Vorbis/Opus comment header), which use byte-identical bodies. Every length is
 * checked by SUBTRACTION against the remaining size so a hostile field can't wrap. */
static void vc_parse(const unsigned char *b, uint32_t len,
                     tags_t *t)
{
    if(len < 8) return;
    uint32_t vlen = le32(b);
    if(vlen > len-8) return;
    uint32_t off = 4+vlen;                       /* off <= len-4, so le32(b+off) is in-bounds */
    uint32_t cnt = le32(b+off); off += 4;
    for(uint32_t i=0; i<cnt && off+4<=len; i++){
        uint32_t clen = le32(b+off); off += 4;
        if(clen > len-off) break;                /* overflow-safe (len-off >= 0) */
        char kv[1088];
        if(clen < sizeof kv){
            memcpy(kv, b+off, clen); kv[clen] = 0;
            char *eq = strchr(kv, '=');
            if(eq){ *eq = 0; const char *k = kv, *v = eq+1;
                if(!strcasecmp(k,"TITLE")       && !t->title[0])  snprintf(t->title, TAGLEN,"%s",v);
                else if(!strcasecmp(k,"ARTIST") && !t->artist[0]) snprintf(t->artist,TAGLEN,"%s",v);
                else if(!strcasecmp(k,"ALBUM")  && !t->album[0])  snprintf(t->album, TAGLEN,"%s",v);
                else if(!strcasecmp(k,"GENRE")  && !t->genre[0])  snprintf(t->genre, TAGLEN,"%s",v);
                /* all three spellings are in the wild: Picard writes ALBUMARTIST,
                 * foobar2000 "ALBUM ARTIST", some rippers ALBUM_ARTIST. */
                else if((!strcasecmp(k,"ALBUMARTIST") || !strcasecmp(k,"ALBUM ARTIST") ||
                         !strcasecmp(k,"ALBUM_ARTIST")) && !t->album_artist[0])
                    snprintf(t->album_artist, TAGLEN,"%s",v);
                else if(!strcasecmp(k,"TRACKNUMBER") && !t->track) t->track = tag_num(v);
                else if(!strcasecmp(k,"DISCNUMBER")  && !t->disc)  t->disc  = tag_num(v);
            }
        }
        off += clen;
    }
}

/* FLAC: "fLaC" magic + metadata blocks; parse the VORBIS_COMMENT block (type 4) for
 * TITLE/ARTIST/ALBUM/GENRE (UTF-8, little-endian lengths). Bounded like the ID3 path. */
static void flac_read(FILE *f, tags_t *t){
    unsigned char magic[4];
    if(fread(magic,1,4,f)!=4 || memcmp(magic,"fLaC",4)!=0) return;
    for(int guard=0; guard<256; guard++){
        unsigned char h[4];
        if(fread(h,1,4,f)!=4) return;
        int last = h[0]&0x80, type = h[0]&0x7f;
        uint32_t len = ((uint32_t)h[1]<<16)|((uint32_t)h[2]<<8)|h[3];
        if(type==4){                                    /* VORBIS_COMMENT */
            if(len<8 || len>1024*1024) return;
            unsigned char *b=malloc(len); if(!b) return;
            if(fread(b,1,len,f)==len) vc_parse(b, len, t);
            free(b);
            return;                                      /* got the comment block */
        }
        if(last) return;
        if(fseek(f,(long)len,SEEK_CUR)!=0) return;
    }
}


/* ---- Ogg (Vorbis / Opus) -------------------------------------------------
 * The comment header is the SECOND packet of the logical stream, and a packet can
 * be split across Ogg pages (it usually is when the tags carry cover art). So we
 * de-page first: concatenate the payloads of the first logical stream into one
 * contiguous buffer, then locate the comment header inside it. Searching the raw
 * file for the marker instead would silently parse page headers as tag data.
 *
 * Bounded to OGG_SCAN bytes / OGG_PAGES pages: the comment header lives at the very
 * start of the stream, so this is always enough, and a corrupt file can't make us
 * read the whole card. */
#define OGG_SCAN  (96*1024)
#define OGG_PAGES 24
static void ogg_read(FILE *f, tags_t *t)
{
    unsigned char *raw = malloc(OGG_SCAN);
    if(!raw) return;
    size_t n = fread(raw, 1, OGG_SCAN, f);
    unsigned char *log = malloc(OGG_SCAN);      /* de-paged logical stream */
    if(!log){ free(raw); return; }
    size_t lg = 0, p = 0;
    uint32_t serial = 0; int have_serial = 0;
    for(int page = 0; page < OGG_PAGES && p + 27 <= n; page++){
        if(memcmp(raw+p, "OggS", 4) != 0) break;             /* not (or no longer) a page boundary */
        uint32_t ser = le32(raw+p+14);
        int nseg = raw[p+26];
        if(p + 27 + (size_t)nseg > n) break;
        size_t body = 0;
        for(int i = 0; i < nseg; i++) body += raw[p+27+i];
        size_t bodyp = p + 27 + (size_t)nseg;
        if(bodyp + body > n) break;                          /* truncated page: stop, keep what we have */
        if(!have_serial){ serial = ser; have_serial = 1; }
        if(ser == serial){                                   /* ignore other multiplexed streams */
            size_t room = OGG_SCAN - lg;
            size_t take = body < room ? body : room;
            memcpy(log + lg, raw + bodyp, take);
            lg += take;
            if(take < body) break;
        }
        p = bodyp + body;
    }
    free(raw);

    /* Find the comment header inside the de-paged stream and hand its body to the
     * shared VORBIS_COMMENT parser. Vorbis prefixes "\x03vorbis"; Opus "OpusTags".
     * Both are followed by a byte-identical comment structure. */
    for(size_t i = 0; i + 8 <= lg; i++){
        size_t off = 0;
        if(!memcmp(log+i, "\x03vorbis", 7))      off = i + 7;
        else if(!memcmp(log+i, "OpusTags", 8))   off = i + 8;
        else continue;
        vc_parse(log + off, (uint32_t)(lg - off), t);
        break;
    }
    free(log);
}

/* ---- MP4 / M4A -----------------------------------------------------------
 * Tags live in moov/udta/meta/ilst as one atom per field (©nam, ©ART, ©alb, ©gen),
 * each wrapping a "data" atom whose payload is the string. `meta` is the odd one:
 * it carries 4 bytes of version/flags before its children.
 *
 * Atom = size(4, big-endian, INCLUDING the header) + type(4). size 0 means "to end
 * of file"; size 1 means a 64-bit size follows, which no tag atom ever uses - both
 * are treated as "stop", since we only care about the small metadata atoms. */
#define MP4_DEPTH 6
static void mp4_ilst_field(FILE *f, const char *type, long size,
                           tags_t *t)
{
    /* trkn/disk are BINARY data atoms, not text: after the 16-byte data header the
     * payload is reserved(2) + number(2, big-endian) + total(2) + reserved(2). They
     * must be handled BEFORE the printable-text check below, which would reject them
     * (the same check that deliberately rejects the numeric `gnre` index). */
    if(!memcmp(type, "trkn", 4) || !memcmp(type, "disk", 4)){
        int is_disc = !memcmp(type, "disk", 4);
        if((is_disc ? t->disc : t->track) || size < 20) return;   /* already set, or too short */
        unsigned char nh[16], nb[4];
        if(fread(nh, 1, 16, f) != 16) return;
        if(memcmp(nh+4, "data", 4) != 0) return;
        if(fread(nb, 1, 4, f) != 4) return;
        int v = (nb[2] << 8) | nb[3];                 /* reserved(2), then the 16-bit number */
        if(v > 0){ if(is_disc) t->disc = v; else t->track = v; }
        return;
    }

    char *dst = NULL;
    if     (!memcmp(type, "\xA9""nam", 4)) dst = t->title;
    else if(!memcmp(type, "\xA9""ART", 4)) dst = t->artist;
    else if(!memcmp(type, "\xA9""alb", 4)) dst = t->album;
    else if(!memcmp(type, "aART", 4))       dst = t->album_artist;   /* iTunes album artist */
    else if(!memcmp(type, "\xA9""gen", 4) || !memcmp(type, "gnre", 4)) dst = t->genre;
    if(!dst || dst[0] || size < 16 || size > 64*1024) return;   /* first writer wins, like the other parsers */

    unsigned char hdr[16];
    if(fread(hdr, 1, 16, f) != 16) return;
    if(memcmp(hdr+4, "data", 4) != 0) return;
    long dsize = (long)be32(hdr);                 /* the data atom's own size */
    if(dsize < 16 || dsize > size) return;
    long vlen = dsize - 16;                       /* minus data hdr(8) + version/flags(4) + locale(4) */
    if(vlen <= 0) return;
    if(vlen > TAGLEN-1) vlen = TAGLEN-1;
    unsigned char buf[TAGLEN];
    if(fread(buf, 1, (size_t)vlen, f) != (size_t)vlen) return;
    /* iTunes writes these as UTF-8 (type flag 1); a numeric `gnre` is a 2-byte index we
     * don't map, so only accept something that looks like text. */
    int printable = 0;
    for(long i = 0; i < vlen; i++) if(buf[i] >= 0x20 || buf[i] == '\n'){ printable = 1; break; }
    if(!printable) return;
    id3_decode(3, buf, (int)vlen, dst, TAGLEN);   /* enc 3 = UTF-8 passthrough + trim */
    if(dst == t->genre) genre_clean(t->genre);
}
static void mp4_walk(FILE *f, long end, int depth,
                     tags_t *t)
{
    if(depth > MP4_DEPTH) return;
    for(int guard = 0; guard < 256; guard++){
        long pos = ftell(f);
        if(pos < 0 || pos > end - 8) return;        /* subtraction: `long` is 32-bit here, so
                                                    * `pos + 8` could overflow on a huge offset */
        unsigned char h[8];
        if(fread(h, 1, 8, f) != 8) return;
        uint32_t usize = be32(h);
        if(usize < 8 || usize > (uint32_t)(end - pos)) return;   /* 0/1/garbage sizes: stop, don't guess */
        long size = (long)usize;
        const char *ty = (const char*)h+4;
        long body = pos + 8, next = pos + size;
        if(!memcmp(ty,"moov",4) || !memcmp(ty,"udta",4) || !memcmp(ty,"ilst",4)){
            mp4_walk(f, next, depth+1, t);
        } else if(!memcmp(ty,"meta",4)){
            if(fseek(f, body + 4, SEEK_SET) != 0) return;   /* meta: skip version/flags */
            mp4_walk(f, next, depth+1, t);
        } else if(depth > 0 && ty[0] == (char)0xA9){
            mp4_ilst_field(f, ty, size, t);
        } else if(!memcmp(ty,"gnre",4) || !memcmp(ty,"aART",4) ||
                  !memcmp(ty,"trkn",4) || !memcmp(ty,"disk",4)){
            /* the ilst atoms that do NOT start with the 0xA9 marker: the numeric
             * genre index, the album artist, and the binary track/disc pairs. */
            mp4_ilst_field(f, ty, size, t);
        }
        if(fseek(f, next, SEEK_SET) != 0) return;
    }
}
static void mp4_read(FILE *f, tags_t *t)
{
    if(fseek(f, 0, SEEK_END) != 0) return;
    long end = ftell(f);
    if(end <= 8) return;
    if(fseek(f, 0, SEEK_SET) != 0) return;
    mp4_walk(f, end, 0, t);
}

/* ---- APEv2 (.ape / .wv / .mpc / .tta) ------------------------------------
 * A 32-byte footer sits at EOF - or 128 bytes earlier when an ID3v1 block follows
 * it. Footer: "APETAGEX" + version(4) + tagsize(4, includes the footer) +
 * itemcount(4) + flags(4) + reserved(8), all little-endian. Items are
 * valuesize(4) + flags(4) + key(NUL-terminated ASCII) + value(UTF-8). */
static void apev2_read(FILE *f, tags_t *t)
{
    /* Zeroed: if BOTH probes fail (a file shorter than the footer, or a read error)
     * nothing is ever written here, and the match below would read uninitialised
     * stack - undefined behaviour, and reachable from any truncated file on the card. */
    unsigned char foot[32] = {0};
    long tail = 0;
    for(int attempt = 0; attempt < 2; attempt++){
        tail = attempt ? -160 : -32;             /* plain footer, then footer-before-ID3v1 */
        if(fseek(f, tail, SEEK_END) != 0) continue;
        if(fread(foot, 1, 32, f) != 32) continue;
        if(!memcmp(foot, "APETAGEX", 8)) break;
        foot[0] = 0;
    }
    if(memcmp(foot, "APETAGEX", 8) != 0) return;

    uint32_t tagsize = le32(foot+12), items = le32(foot+16);
    if(tagsize <= 32 || tagsize > 1024*1024 || items == 0 || items > 4096) return;
    uint32_t body = tagsize - 32;                /* the items, excluding this footer */
    if(fseek(f, tail - (long)body, SEEK_END) != 0) return;
    unsigned char *b = malloc(body);
    if(!b) return;
    if(fread(b, 1, body, f) != body){ free(b); return; }

    uint32_t off = 0;
    for(uint32_t i = 0; i < items && off + 8 < body; i++){
        uint32_t vsize = le32(b+off);
        off += 8;                                 /* value size + item flags */
        uint32_t klen = 0;
        while(off + klen < body && b[off+klen]) klen++;
        if(off + klen >= body) break;             /* unterminated key: give up */
        char key[64];
        uint32_t kcopy = klen < sizeof key - 1 ? klen : (uint32_t)sizeof key - 1;
        memcpy(key, b+off, kcopy); key[kcopy] = 0;
        off += klen + 1;
        if(vsize > body - off) break;             /* overflow-safe */
        char *dst = NULL;
        if     (!strcasecmp(key,"Title"))  dst = t->title;
        else if(!strcasecmp(key,"Artist")) dst = t->artist;
        else if(!strcasecmp(key,"Album"))  dst = t->album;
        else if(!strcasecmp(key,"Genre"))  dst = t->genre;
        else if(!strcasecmp(key,"Album Artist") || !strcasecmp(key,"ALBUMARTIST"))
                                           dst = t->album_artist;
        if(dst && !dst[0] && vsize > 0)
            id3_decode(3, b+off, (int)(vsize < TAGLEN-1 ? vsize : TAGLEN-1), dst, TAGLEN);
        else if(vsize > 0 && (!strcasecmp(key,"Track") || !strcasecmp(key,"Disc"))){
            char num[32];   /* APEv2 numerics are text, same "n/total" shape as ID3 */
            id3_decode(3, b+off, (int)(vsize < sizeof num - 1 ? vsize : sizeof num - 1), num, sizeof num);
            int v = tag_num(num);
            if(v > 0){
                if(!strcasecmp(key,"Disc")){ if(!t->disc)  t->disc  = v; }
                else                       { if(!t->track) t->track = v; }
            }
        }
        off += vsize;
    }
    free(b);
    if(t->genre[0]) genre_clean(t->genre);
}

/* ---- DSF -----------------------------------------------------------------
 * "DSD " chunk header: magic(4) + chunk size(8) + total file size(8) + a 64-bit
 * pointer to an ID3v2 tag (0 = none). Seek there and reuse the ID3 reader. */
static void dsf_read(FILE *f, tags_t *t)
{
    unsigned char h[28];
    if(fseek(f, 0, SEEK_SET) != 0) return;
    if(fread(h, 1, 28, f) != 28) return;
    if(memcmp(h, "DSD ", 4) != 0) return;
    /* little-endian 64-bit; anything above 2GB is not a tag pointer we can use */
    uint64_t ptr = (uint64_t)le32(h+20) | ((uint64_t)le32(h+24) << 32);
    if(ptr < 28 || ptr > 0x7FFFFFFFull) return;
    if(fseek(f, (long)ptr, SEEK_SET) != 0) return;
    id3v2_read(f, t);
}

/* 1 = tags/fallback filled OK; 0 = a tag-bearing file (mp3/flac) that could NOT be opened (transient
 * I/O error) - the caller must NOT overwrite an existing good row with filename/"Unknown" fallback. */
/* Containers we have a real tag parser for. Failing to OPEN one of these is treated
 * as a transient error rather than "no tags", so a good existing row is never
 * clobbered with a filename fallback because the card hiccupped. */
static int has_tag_parser(const char *fname){
    return has_ext(fname,".mp3")  || has_ext(fname,".flac") || has_ext(fname,".ogg")
        || has_ext(fname,".oga")  || has_ext(fname,".opus") || has_ext(fname,".m4a")
        || has_ext(fname,".m4b")  || has_ext(fname,".mp4")  || has_ext(fname,".ape")
        || has_ext(fname,".wv")   || has_ext(fname,".mpc")  || has_ext(fname,".tta")
        || has_ext(fname,".dsf");
}
static int tags_from_file(const char *path, const char *fname, tags_t *t){
    memset(t, 0, sizeof *t);
    FILE *f=fopen(path,"rb");
    if(!f && has_tag_parser(fname))
        return 0;   /* can't read a file we should have tags for -> don't clobber; caller skips it */
    if(f){
        if(has_ext(fname,".flac")) flac_read(f,t);
        else if(has_ext(fname,".mp3")){ id3v2_read(f,t); id3v1_read(f,t); }
        else if(has_ext(fname,".ogg") || has_ext(fname,".oga") || has_ext(fname,".opus"))
            ogg_read(f,t);
        else if(has_ext(fname,".m4a") || has_ext(fname,".m4b") || has_ext(fname,".mp4"))
            mp4_read(f,t);
        else if(has_ext(fname,".ape") || has_ext(fname,".wv") || has_ext(fname,".mpc") || has_ext(fname,".tta"))
            apev2_read(f,t);
        else if(has_ext(fname,".dsf")) dsf_read(f,t);
        /* .wav/.aif/.aiff/.dff/.wma/.aac -> filename fallback below; no tag probing, so a
         * WAV whose last 128 bytes happen to start with "TAG" isn't misread as ID3v1. */
        fclose(f);
    }
    if(!t->title[0]){                            /* filename (minus extension) */
        snprintf(t->title,TAGLEN,"%s",fname);
        char *dot=strrchr(t->title,'.'); if(dot) *dot=0;
        str_trim(t->title);
    }
    if(!t->artist[0]) snprintf(t->artist,TAGLEN,"%s","Unknown artist");
    if(!t->album[0])  snprintf(t->album, TAGLEN,"%s","Unknown album");
    if(!t->genre[0])  snprintf(t->genre, TAGLEN,"%s","Unknown genre");
    /* album_artist is left EMPTY when the file has none - musicdb falls back to
     * ARTIST for grouping, and a fabricated value here would be indistinguishable
     * from a real one. disc/track stay 0 = unknown, which sorts last. */
    return 1;
}

/* ---- SQLite: schema + insert ---- */
static const char *SCHEMA_SONG =
    "CREATE TABLE IF NOT EXISTS SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT,"
    "TITLE TEXT, ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT,"
    "IS_DSD INT, OFFSET BIGINT, DURATION BIGINT, NAME_CODE INT, TITLE_CODE INT, ALBUM_CODE INT,"
    "ARTIST_CODE INT, GENRE_CODE INT, ADD_TIME INT8, SAMPLE_RATE INT, BIT_PER_SAMPLE INT, CHANNELS INT,"
    "BIT_RATE INT, SONG_MIMETYPE TEXT, SONG_PRODUCTION_YEAR TEXT, IS_SELECT INT, ALBUM_ARTIST TEXT,"
    /* IS_M3U/M3U_PATH match the V2.28 stock SONG superset (its playback SQL SELECTs them); ACCENT is
     * our private per-song art-accent cache. TEXT for ALBUM_ARTIST matches stock (was wrongly INT). */
    "ALBUM_ARTIST_CODE INT, IS_M3U INT DEFAULT 0, M3U_PATH TEXT DEFAULT '', ACCENT INTEGER DEFAULT 0);";
static const char *INS_SONG =
    "INSERT INTO SONG (PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
    "DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,"
    "BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,"
    /* ALBUM_ARTIST is TEXT (stock type) and binds NULL when the file carries no album
     * artist - see bind_extra. IS_M3U/M3U_PATH/ACCENT are omitted from the column list
     * so they take their schema DEFAULTs. */
    /* EXPLICIT ?N parameters: SQLite numbers a bare '?' by order of appearance, so the
     * DISC/TRACK placeholders (columns 7-8) would otherwise renumber every bind after
     * them. Numbering them 13.. keeps bind_tags' contiguous 2..11 block untouched. */
    "ALBUM_ARTIST_CODE) VALUES (?1,?2,?3,?4,?5,?6,?13,?14,0,0,0,0,0,?7,?8,?9,?10,?11,?12,0,0,0,0,'','',0,?15,?16);";
/* MERGE (not delete+reinsert): UPDATE an existing PATH's metadata in place so its ID and ACCENT are
 * preserved - MEMORY_PLAY.MUSIC_ID (resume) and MY_LOVE.ID (favourites) are ID-keyed, so a delete+
 * reinsert with fresh autoincrement IDs used to break resume + favourites and wipe the art-accent cache
 * on every rescan. ADD_TIME is intentionally left untouched here (keep the original add time). */
static const char *UPD_SONG =
    "UPDATE SONG SET NAME=?1,TITLE=?2,ALBUM=?3,ARTIST=?4,GENRE=?5,"
    "NAME_CODE=?6,TITLE_CODE=?7,ALBUM_CODE=?8,ARTIST_CODE=?9,GENRE_CODE=?10,"
    /* rescanning an existing row must refresh these too, otherwise a library that
     * was indexed before diskOS read them would keep DISC/TRACK 0 for ever. */
    "DISC=?12,TRACK=?13,ALBUM_ARTIST=?14,ALBUM_ARTIST_CODE=?15 WHERE PATH=?11;";
/* per-connection temp table of PATHs seen this scan; drives the post-walk delete of vanished songs. */
static const char *SEEN_DDL = "CREATE TEMP TABLE IF NOT EXISTS seen(PATH TEXT PRIMARY KEY);";
static const char *SEEN_INS = "INSERT OR IGNORE INTO seen(PATH) VALUES(?);";

static sqlite3 *g_db;
static sqlite3_stmt *g_ins, *g_upd, *g_seen;

/* True iff SONG has column `col`. Used to PROVE the V2.28-required columns really exist after the
 * migration ALTERs before we rebuild the library - a silently-failed ALTER (SQLITE_BUSY/FULL/IOERR)
 * must not produce a library the stock V2.28 player can't query. PRAGMA table_info is universally
 * supported (no dependency on the pragma-function feature). */
static int song_has_col(sqlite3 *db, const char *col){
    sqlite3_stmt *st = NULL; int found = 0;
    if(sqlite3_prepare_v2(db, "PRAGMA table_info(SONG);", -1, &st, NULL) == SQLITE_OK){
        while(sqlite3_step(st) == SQLITE_ROW){
            const unsigned char *n = sqlite3_column_text(st, 1);   /* col 1 = name */
            if(n && strcmp((const char*)n, col) == 0){ found = 1; break; }
        }
        sqlite3_finalize(st);
    }
    return found;
}

/* bind the 5 text tags + their 5 codes (fname/title/album/artist/genre) to a prepared stmt starting at
 * parameter index `p0` (used for both the UPDATE's SET list and the INSERT's value list). */
static void bind_tags(sqlite3_stmt *s, int p0, const char *fname, const char *title,
                      const char *album, const char *artist, const char *genre){
    sqlite3_bind_text(s, p0+0, fname, -1, SQLITE_TRANSIENT);   /* NAME = filename */
    sqlite3_bind_text(s, p0+1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+2, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+3, artist,-1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+4, genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, p0+5, code4(fname));
    sqlite3_bind_int (s, p0+6, code4(title));
    sqlite3_bind_int (s, p0+7, code4(album));
    sqlite3_bind_int (s, p0+8, code4(artist));
    sqlite3_bind_int (s, p0+9, code4(genre));
}
/* Bind DISC/TRACK/ALBUM_ARTIST/ALBUM_ARTIST_CODE at `p0`..`p0+3`.
 * ALBUM_ARTIST binds NULL (not "") when the file has none, so "absent" stays
 * distinguishable from "tagged empty" - musicdb's IFNULL handles both. */
static void bind_extra(sqlite3_stmt *s, int p0, const tags_t *t){
    sqlite3_bind_int(s, p0+0, t->disc);
    sqlite3_bind_int(s, p0+1, t->track);
    if(t->album_artist[0]){
        sqlite3_bind_text(s, p0+2, t->album_artist, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (s, p0+3, code4(t->album_artist));
    } else {
        sqlite3_bind_null(s, p0+2);
        sqlite3_bind_int (s, p0+3, 0);
    }
}
/* Record a path in `seen` WITHOUT touching its row - used to PRESERVE an existing row for a file we
 * couldn't index this pass (ENOENT / unreadable), so the vanished-row prune doesn't delete it. If we
 * can't even record it, block the prune (conservative: never delete a file we failed to preserve). */
static void mark_seen(const char *path){
    if(!g_seen){ g_prune_blocked = 1; return; }
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen) != SQLITE_DONE) g_prune_blocked = 1;
}
/* Merge one file into SONG: record it as seen, then UPDATE the existing PATH in place (preserving ID +
 * ACCENT), or INSERT a new row if the PATH is new. Any DB error sets g_scan_err (blocks the commit). */
static void upsert_song(const char *path, const char *fname){
    tags_t t;
    if(!tags_from_file(path, fname, &t)){
        mark_seen(path);   /* unreadable this pass -> keep any existing row; don't clobber good tags */
        g_skipped++;
        return;
    }
    /* record as seen (drives the post-walk delete of paths that vanished from the SD) */
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen)!=SQLITE_DONE){ g_scan_err=1; return; }
    /* UPDATE in place (keeps ID/ACCENT/ADD_TIME). params 1..10 = tags, 11 = PATH (WHERE). */
    sqlite3_reset(g_upd); sqlite3_clear_bindings(g_upd);
    bind_tags(g_upd, 1, fname, t.title, t.album, t.artist, t.genre);
    sqlite3_bind_text(g_upd, 11, path, -1, SQLITE_TRANSIENT);
    bind_extra(g_upd, 12, &t);                         /* 12=DISC 13=TRACK 14=ALBUM_ARTIST 15=code */
    if(sqlite3_step(g_upd)!=SQLITE_DONE){ g_scan_err=1; return; }
    if(sqlite3_changes(g_db) > 0){                     /* existing PATH updated in place */
        pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);
        return;
    }
    /* new PATH -> INSERT (new autoincrement ID, ACCENT = schema default 0). params: 1=PATH, 2..11 tags, 12=ADD_TIME */
    sqlite3_reset(g_ins); sqlite3_clear_bindings(g_ins);
    sqlite3_bind_text(g_ins, 1, path, -1, SQLITE_TRANSIENT);
    bind_tags(g_ins, 2, fname, t.title, t.album, t.artist, t.genre);
    sqlite3_bind_int64(g_ins, 12, (sqlite3_int64)1782180000);   /* ADD_TIME */
    bind_extra(g_ins, 13, &t);                         /* 13=DISC 14=TRACK 15=ALBUM_ARTIST 16=code */
    if(sqlite3_step(g_ins)==SQLITE_DONE){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); }
    else g_scan_err=1;   /* insert failure (disk full, DB corruption) must block the commit */
}

/* Counting pass: the same traversal shape as walk(), but it opens NOTHING - readdir
 * plus an extension test only - so it costs a fraction of the merge pass while giving
 * the progress bar a real total. Errors here are deliberately NOT fatal: a bad count
 * only makes the percentage approximate, and must never fail a scan that would
 * otherwise succeed. */
static int count_walk(const char *dir, int depth){
    if(depth > 24) return 0;
    DIR *d = opendir(dir);
    if(!d) return 0;
    struct dirent *e;
    char path[MAXPATH];
    int n = 0;
    while((e = readdir(d))){
        if(e->d_name[0]=='.') continue;
        int w = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
        if(w<0 || w>=(int)sizeof path) continue;
        struct stat st;
        if(lstat(path,&st)!=0) continue;
        if(S_ISDIR(st.st_mode))                              n += count_walk(path, depth+1);
        else if(S_ISREG(st.st_mode) && is_audio(e->d_name))  n++;
    }
    closedir(d);
    return n;
}

/* recursive walk; inserts every audio file found under dir. lstat (not stat) so symlinks
 * are never followed, + a depth cap, so a hostile/looping tree can't run away. */
static void walk(const char *dir, int depth){
    if(depth > 24){ g_scan_err=1; return; }   /* absurdly deep -> treat as incomplete, don't commit */
    DIR *d=opendir(dir);
    if(!d){ g_scan_err=1; return; }           /* the SD is exFAT/vfat (no perms): a failed opendir means
                                               * I/O error or the card was pulled -> incomplete scan */
    struct dirent *e;
    char path[MAXPATH];
    for(errno=0; (e=readdir(d)); errno=0){    /* errno reset before each readdir so we can detect a read error */
        if(e->d_name[0]=='.') continue;
        int w = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
        if(w<0 || w>=(int)sizeof path){ g_skipped++; g_prune_blocked=1; continue; }   /* overlong path unknowable -> can't preserve its row -> block the prune */
        struct stat st;
        /* Failing the whole scan on ANY per-file lstat error was the killer bug: every real exFAT SD
         * has entries that readdir lists but lstat can't resolve (ENOENT) - orphaned entries and, on
         * this device, real files whose special-character names don't round-trip through the mount's
         * encoding. Those always set g_scan_err=1, so the commit gate rolled back a fully-successful
         * index -> empty library (why the DB had to be hand-built). Skip ONLY the benign ENOENT case
         * (the file can't be opened to index anyway); any OTHER errno (EIO/EACCES = the card going bad)
         * stays fatal so we never commit a silently-incomplete library over a good one. */
        if(lstat(path,&st)!=0){
            if(errno == ENOENT){
                if(is_audio(e->d_name)) mark_seen(path);   /* audio file: preserve its existing row (special-char names) */
                else g_prune_blocked = 1;                  /* could be an unresolvable DIRECTORY whose songs we never
                                                            * walked -> block the prune so its subtree rows aren't deleted */
                g_skipped++; continue;                     /* unresolvable entry -> skip, best-effort */
            }
            g_scan_err = 1; continue;                        /* real I/O/access error -> don't commit a partial index */
        }
        if(S_ISDIR(st.st_mode)) walk(path, depth+1);
        else if(S_ISREG(st.st_mode) && is_audio(e->d_name)){
            /* publish before the (slow) tag read, so the name on screen is the file
             * actually being worked on rather than the previous one */
            pthread_mutex_lock(&g_mu);
            /* Deliberate truncation: this is a progress label on a 360px screen, not a
             * path. Copied explicitly rather than via snprintf so the intent is in the
             * code and the compiler does not have to guess (-Wformat-truncation). */
            size_t nlen = strlen(e->d_name);
            if(nlen >= sizeof g_curname) nlen = sizeof g_curname - 1;
            memcpy(g_curname, e->d_name, nlen);
            g_curname[nlen] = 0;
            pthread_mutex_unlock(&g_mu);
            upsert_song(path, e->d_name);
        }
    }
    if(errno) g_scan_err=1;                   /* readdir error -> this directory listing was incomplete */
    closedir(d);
}

/* True only when SCAN_ROOT is a REAL, readable mountpoint (its st_dev differs from its
 * parent's). A missing/late SD, or a bare placeholder /tmp/sdcard dir, returns 0 - so we
 * never wipe the live library rebuilding from an unmounted card. */
static int scan_root_ready(void){
    struct stat sroot, sparent;
    if(stat(SCAN_ROOT, &sroot)!=0 || !S_ISDIR(sroot.st_mode)) return 0;
    char parent[MAXPATH];
    if(snprintf(parent,sizeof parent,"%s/..",SCAN_ROOT) >= (int)sizeof parent) return 0;
    if(stat(parent, &sparent)!=0) return 0;
    if(sroot.st_dev == sparent.st_dev) return 0;   /* not a separate mount -> SD not mounted */
    DIR *d=opendir(SCAN_ROOT); if(!d) return 0; closedir(d);
    return 1;
}

static void *scan_thread(void *arg){
    (void)arg;
    int ok=0, found=0;
    /* Guard: if the SD isn't actually mounted at SCAN_ROOT, do NOT open a transaction or
     * DELETE anything - keep the existing library intact. This is the #1 data-loss guard:
     * an absent/late mount must never commit an empty SONG table. */
    if(!scan_root_ready()){
        pthread_mutex_lock(&g_mu);
        g_no_sd=1; g_total=0;      /* 0 => library kept; g_no_sd lets the UI say "insert SD" */
        g_active=0; g_finished_seq++;
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    if(sqlite3_open_v2(DB_PATH,&g_db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK){
        sqlite3_busy_timeout(g_db,8000);
        sqlite3_exec(g_db, SCHEMA_SONG, 0,0,0);
        /* Bring an EXISTING (older diskOS or stock) SONG table up to the V2.28 superset BEFORE the
         * scan transaction, so the stock player's playback SQL (which SELECTs IS_M3U/M3U_PATH) always
         * finds the columns instead of depending on the player winning a startup migration race.
         * ALTERs are idempotent here - a duplicate column just errors harmlessly. */
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN IS_M3U INT DEFAULT 0;", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN M3U_PATH TEXT DEFAULT '';", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN ACCENT INTEGER DEFAULT 0;", 0,0,0);
        /* PROVE the required columns exist (a duplicate-column ALTER error is fine, but a real
         * failure - BUSY/FULL/IOERR/corruption - is not). If any is missing, flag a scan error so
         * the commit gate below rolls back and KEEPS the existing library rather than rebuilding one
         * the stock V2.28 player can't query (its playback SQL SELECTs IS_M3U/M3U_PATH). */
        if(!song_has_col(g_db,"IS_M3U") || !song_has_col(g_db,"M3U_PATH") || !song_has_col(g_db,"ACCENT")){
            g_scan_err = 1;
            fprintf(stderr, "scanner: SONG schema migration incomplete -> keeping existing library\n");
        }
        g_ins=NULL; g_upd=NULL; g_seen=NULL;
        if(sqlite3_prepare_v2(g_db, INS_SONG, -1, &g_ins,  NULL)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, UPD_SONG, -1, &g_upd,  NULL)==SQLITE_OK
           && sqlite3_exec(g_db, SEEN_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, SEEN_INS, -1, &g_seen, NULL)==SQLITE_OK){
            /* ONE atomic transaction: the whole MERGE (UPDATE existing / INSERT new / DELETE vanished)
             * commits together, or ROLLBACK on any failure - a failed/partial scan can never corrupt or
             * wipe the library. MERGE (not delete+reinsert) so existing rows KEEP their ID + ACCENT,
             * preserving resume (MEMORY_PLAY.MUSIC_ID) + favourites (MY_LOVE.ID) + art-accent across a
             * rescan. Only mp3/flac/wav rows are ever removed; m4a/ape/dsf/... are never touched. */
            /* Count BEFORE opening the write transaction: the count touches no rows,
             * and doing it inside would hold the DB lock for the whole counting pass. */
            int expect = count_walk(SCAN_ROOT, 0);
            pthread_mutex_lock(&g_mu);
            g_expect = expect; g_phase = 1;
            pthread_mutex_unlock(&g_mu);
            if(sqlite3_exec(g_db,"BEGIN IMMEDIATE;",0,0,0)==SQLITE_OK){
                g_skipped=0; g_prune_blocked=0;
                sqlite3_exec(g_db,"DELETE FROM seen;",0,0,0);   /* start from an empty seen-set */
                walk(SCAN_ROOT, 0);
                pthread_mutex_lock(&g_mu); found = g_done; pthread_mutex_unlock(&g_mu);
                /* observability: one concise line per scan */
                fprintf(stderr,"scanner: merged %d songs, skipped %d unreadable file(s), scan_err=%d\n",
                        found, g_skipped, g_scan_err);
                /* Commit only if the walk found audio AND the SD is STILL mounted afterwards (guards a
                 * card pulled / gone-I/O mid-scan; zero-found rolls back too). Then delete scanned-format
                 * rows whose PATH vanished from the SD - inside the txn, so it rolls back on any failure. */
                if(found > 0 && !g_scan_err && scan_root_ready()){
                    /* Prune vanished songs - but ONLY when pruning wasn't blocked (!g_prune_blocked: no
                     * overlong/unknown-type skip, no mark_seen failure) and scoped to THIS mount (PATH
                     * under SCAN_ROOT). ENOENT/unreadable audio files ARE preserved in `seen`, so they
                     * don't block; only unknowable skips do. Rows from another mount/source that this
                     * scanner never inspected must not be touched. If blocked, we still commit the merge
                     * (UPDATEs/INSERTs) but do NOT delete. */
                    /* Built from AUDIO_EXT so the prune can never drift from what the walk
                     * actually indexes: deleting rows for a format we no longer scan would
                     * silently empty part of the library, and NOT deleting a format we DO
                     * scan leaves ghost rows for files removed from the card. */
                    char del[1024];
                    int dn = snprintf(del, sizeof del, "DELETE FROM SONG WHERE (");
                    for(int i=0;i<N_AUDIO_EXT && dn>0 && dn<(int)sizeof del;i++)
                        dn += snprintf(del+dn, sizeof del - dn, "%slower(PATH) LIKE '%%%s'",
                                       i?" OR ":"", AUDIO_EXT[i]);
                    if(dn > 0 && dn < (int)sizeof del)
                        dn += snprintf(del+dn, sizeof del - dn,
                                       ") AND PATH LIKE '" SCAN_ROOT "/%%' "
                                       "AND PATH NOT IN (SELECT PATH FROM seen);");
                    /* If the statement didn't fit, skip the prune rather than run a truncated
                     * DELETE - committing the merge without pruning is the safe half. */
                    int del_ok = (g_prune_blocked || dn <= 0 || dn >= (int)sizeof del)
                                     ? 1
                                     : (sqlite3_exec(g_db,del,0,0,0)==SQLITE_OK);
                    if(del_ok) ok = (sqlite3_exec(g_db,"COMMIT;",0,0,0)==SQLITE_OK);
                }
                if(!ok) sqlite3_exec(g_db,"ROLLBACK;",0,0,0);
            }
        }
        sqlite3_finalize(g_ins);  g_ins=NULL;
        sqlite3_finalize(g_upd);  g_upd=NULL;
        sqlite3_finalize(g_seen); g_seen=NULL;
    }
    sqlite3_close(g_db); g_db=NULL;   /* close even on a failed open: sqlite3_open_v2 may still return a handle */
    pthread_mutex_lock(&g_mu);
    g_total = ok ? g_done : 0;    /* committed count; 0 signals a failed rebuild (library kept) */
    g_active=0; g_finished_seq++;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int scanner_start(void){
    pthread_mutex_lock(&g_mu);
    if(g_active){ pthread_mutex_unlock(&g_mu); return -1; }   /* already scanning */
    g_active=1; g_done=0; g_total=0; g_no_sd=0; g_scan_err=0;
    g_phase=0; g_expect=0; g_curname[0]=0;   /* fresh progress for this scan */
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    if(pthread_create(&th,NULL,scan_thread,NULL)!=0){
        pthread_mutex_lock(&g_mu); g_active=0; pthread_mutex_unlock(&g_mu);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

/* ===================== [isolated test harness] =========================== */
#ifdef SCANNER_TEST
#include <unistd.h>
int main(void){
    fprintf(stderr,"scan test -> %s (root %s)\n", DB_PATH, SCAN_ROOT);
    if(scanner_start()!=0){ fprintf(stderr,"start failed\n"); return 1; }
    int done,total;
    while(scanner_active()){ scanner_progress(&done,NULL); fprintf(stderr,"  %d...\r",done); usleep(200000); }
    scanner_progress(&done,&total);
    fprintf(stderr,"\nDONE: %d songs\n", total);
    return 0;
}
#endif

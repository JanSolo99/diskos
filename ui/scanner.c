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
 * Tag support: MP3 ID3v2.2/2.3/2.4 (title/artist/album/genre) + ID3v1 fallback; FLAC via its
 * VORBIS_COMMENT block; WAV by filename. Anything without usable tags falls back to the
 * filename (minus extension) as the title. m4a/aac/ogg/ape/dsf are not yet indexed.
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

int scanner_active(void){ pthread_mutex_lock(&g_mu); int a=g_active; pthread_mutex_unlock(&g_mu); return a; }
void scanner_progress(int *done, int *total){
    pthread_mutex_lock(&g_mu);
    if(done)*done=g_done; if(total)*total=g_total;
    pthread_mutex_unlock(&g_mu);
}
int scanner_take_finished(void){
    pthread_mutex_lock(&g_mu);
    static int last=0; int f = (g_finished_seq!=last); last=g_finished_seq;
    pthread_mutex_unlock(&g_mu);
    return f;
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
/* Audio files diskOS indexes. MP3 (ID3) + FLAC (VORBIS_COMMENT) get real tags; WAV falls back
 * to the filename. m4a/aac/ogg/ape/dsf need their own parsers - a documented beta limitation. */
static int is_audio(const char *name){
    return has_ext(name,".mp3") || has_ext(name,".flac") || has_ext(name,".wav");
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

static uint32_t be32(const unsigned char *p){ return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static uint32_t synch32(const unsigned char *p){ return (p[0]<<21)|(p[1]<<14)|(p[2]<<7)|p[3]; }

/* Read ID3v2 text frames (TIT2/TPE1/TALB/TCON, or v2.2 TT2/TP1/TAL/TCO). Returns 1 if the
 * header was present. Fields it doesn't find are left untouched. */
static int id3v2_read(FILE *f, char *title,char *artist,char *album,char *genre){
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
                if(!strcmp(k,"TIT2")||!strcmp(k,"TT2")) snprintf(title,TAGLEN,"%s",val);
                else if(!strcmp(k,"TPE1")||!strcmp(k,"TP1")) snprintf(artist,TAGLEN,"%s",val);
                else if(!strcmp(k,"TALB")||!strcmp(k,"TAL")) snprintf(album,TAGLEN,"%s",val);
                else if(!strcmp(k,"TCON")||!strcmp(k,"TCO")){ snprintf(genre,TAGLEN,"%s",val); genre_clean(genre); }
            }
        }
        p += fhdr + fsize;
    }
    free(buf);
    return 1;
}
/* ID3v1: last 128 bytes "TAG" + 30+30+30 title/artist/album (Latin-1). Fallback only. */
static void id3v1_read(FILE *f, char *title,char *artist,char *album){
    if(fseek(f,-128,SEEK_END)!=0) return;
    unsigned char t[128];
    if(fread(t,1,128,f)!=128) return;
    if(memcmp(t,"TAG",3)!=0) return;
    char tmp[64];
    if(!title[0]){  id3_decode(0, t+3,  30, tmp,sizeof tmp); if(tmp[0]) snprintf(title, TAGLEN,"%s",tmp); }
    if(!artist[0]){ id3_decode(0, t+33, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(artist,TAGLEN,"%s",tmp); }
    if(!album[0]){  id3_decode(0, t+63, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(album, TAGLEN,"%s",tmp); }
}

static uint32_t le32(const unsigned char *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* FLAC: "fLaC" magic + metadata blocks; parse the VORBIS_COMMENT block (type 4) for
 * TITLE/ARTIST/ALBUM/GENRE (UTF-8, little-endian lengths). Bounded like the ID3 path. */
static void flac_read(FILE *f, char *title,char *artist,char *album,char *genre){
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
            if(fread(b,1,len,f)!=len){ free(b); return; }
            /* overflow-safe bounds: len>=8 guaranteed above. Need 4(vendor-len field, = b[0..3])
             * + vlen(vendor) + 4(comment count) <= len, i.e. vlen <= len-8. Use subtraction so a
             * hostile vlen like 0xFFFFFFFA can't wrap an addition past the guard. */
            uint32_t vlen=le32(b);
            if(vlen > len-8){ free(b); return; }
            uint32_t off=4+vlen;                          /* off <= len-4, so le32(b+off) is in-bounds */
            uint32_t cnt=le32(b+off); off+=4;
            for(uint32_t i=0;i<cnt && off+4<=len;i++){
                uint32_t clen=le32(b+off); off+=4;
                if(clen>len-off) break;                  /* overflow-safe (len-off >= 0) */
                char kv[1088];
                if(clen < sizeof kv){
                    memcpy(kv,b+off,clen); kv[clen]=0;
                    char *eq=strchr(kv,'=');
                    if(eq){ *eq=0; const char *k=kv, *v=eq+1;
                        if(!strcasecmp(k,"TITLE")  && !title[0])  snprintf(title, TAGLEN,"%s",v);
                        else if(!strcasecmp(k,"ARTIST") && !artist[0]) snprintf(artist,TAGLEN,"%s",v);
                        else if(!strcasecmp(k,"ALBUM")  && !album[0])  snprintf(album, TAGLEN,"%s",v);
                        else if(!strcasecmp(k,"GENRE")  && !genre[0])  snprintf(genre, TAGLEN,"%s",v);
                    }
                }
                off+=clen;
            }
            free(b);
            return;                                      /* got the comment block */
        }
        if(last) return;
        if(fseek(f,(long)len,SEEK_CUR)!=0) return;
    }
}

/* 1 = tags/fallback filled OK; 0 = a tag-bearing file (mp3/flac) that could NOT be opened (transient
 * I/O error) - the caller must NOT overwrite an existing good row with filename/"Unknown" fallback. */
static int tags_from_file(const char *path, const char *fname,
                          char *title,char *artist,char *album,char *genre){
    title[0]=artist[0]=album[0]=genre[0]=0;
    FILE *f=fopen(path,"rb");
    if(!f && (has_ext(fname,".flac") || has_ext(fname,".mp3")))
        return 0;   /* can't read an mp3/flac we should have tags for -> don't clobber; caller skips it */
    if(f){
        if(has_ext(fname,".flac")) flac_read(f,title,artist,album,genre);
        else if(has_ext(fname,".mp3")){ id3v2_read(f,title,artist,album,genre); id3v1_read(f,title,artist,album); }
        /* .wav (and any other accepted container) -> filename fallback below; no tag probing,
         * so a WAV whose last 128 bytes happen to start with "TAG" isn't misread as ID3v1. */
        fclose(f);
    }
    if(!title[0]){                               /* filename (minus extension) */
        snprintf(title,TAGLEN,"%s",fname);
        char *dot=strrchr(title,'.'); if(dot) *dot=0;
        str_trim(title);
    }
    if(!artist[0]) snprintf(artist,TAGLEN,"%s","Unknown artist");
    if(!album[0])  snprintf(album, TAGLEN,"%s","Unknown album");
    if(!genre[0])  snprintf(genre, TAGLEN,"%s","Unknown genre");
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
    /* ALBUM_ARTIST -> NULL (not 0): the column is TEXT (stock type); populating it is a follow-up.
     * IS_M3U/M3U_PATH/ACCENT are omitted from the column list so they take their schema DEFAULTs. */
    "ALBUM_ARTIST_CODE) VALUES (?,?,?,?,?,?,0,0,0,0,0,0,0,?,?,?,?,?,?,0,0,0,0,'','',0,NULL,0);";
/* MERGE (not delete+reinsert): UPDATE an existing PATH's metadata in place so its ID and ACCENT are
 * preserved - MEMORY_PLAY.MUSIC_ID (resume) and MY_LOVE.ID (favourites) are ID-keyed, so a delete+
 * reinsert with fresh autoincrement IDs used to break resume + favourites and wipe the art-accent cache
 * on every rescan. ADD_TIME is intentionally left untouched here (keep the original add time). */
static const char *UPD_SONG =
    "UPDATE SONG SET NAME=?,TITLE=?,ALBUM=?,ARTIST=?,GENRE=?,"
    "NAME_CODE=?,TITLE_CODE=?,ALBUM_CODE=?,ARTIST_CODE=?,GENRE_CODE=? WHERE PATH=?;";
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
    char title[TAGLEN],artist[TAGLEN],album[TAGLEN],genre[TAGLEN];
    if(!tags_from_file(path, fname, title, artist, album, genre)){
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
    bind_tags(g_upd, 1, fname, title, album, artist, genre);
    sqlite3_bind_text(g_upd, 11, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_upd)!=SQLITE_DONE){ g_scan_err=1; return; }
    if(sqlite3_changes(g_db) > 0){                     /* existing PATH updated in place */
        pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);
        return;
    }
    /* new PATH -> INSERT (new autoincrement ID, ACCENT = schema default 0). params: 1=PATH, 2..11 tags, 12=ADD_TIME */
    sqlite3_reset(g_ins); sqlite3_clear_bindings(g_ins);
    sqlite3_bind_text(g_ins, 1, path, -1, SQLITE_TRANSIENT);
    bind_tags(g_ins, 2, fname, title, album, artist, genre);
    sqlite3_bind_int64(g_ins, 12, (sqlite3_int64)1782180000);   /* ADD_TIME */
    if(sqlite3_step(g_ins)==SQLITE_DONE){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); }
    else g_scan_err=1;   /* insert failure (disk full, DB corruption) must block the commit */
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
        else if(S_ISREG(st.st_mode) && is_audio(e->d_name)) upsert_song(path, e->d_name);
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
                    static const char *DEL_ABSENT =
                        "DELETE FROM SONG WHERE (lower(PATH) LIKE '%.mp3' OR lower(PATH) LIKE '%.flac'"
                        " OR lower(PATH) LIKE '%.wav') AND PATH LIKE '" SCAN_ROOT "/%' "
                        "AND PATH NOT IN (SELECT PATH FROM seen);";
                    int del_ok = (g_prune_blocked) ? 1
                                                   : (sqlite3_exec(g_db,DEL_ABSENT,0,0,0)==SQLITE_OK);
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

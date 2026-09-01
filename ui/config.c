/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define CFG_PATH     "/usr/data/diskos.conf"
#define CFG_DIR      "/usr/data"
#define LEGACY_CONF  "/usr/data/ipodos.conf"    /* pre-rename config; migrated on first load */
#define LEGACY_SWIPE "/usr/data/swipe_thresh"   /* pre-config single-value file */
#define CFG_MAX      64
#define KLEN         28
#define VLEN         48

typedef struct { char k[KLEN]; char v[VLEN]; } cfg_entry_t;
static cfg_entry_t g_cfg[CFG_MAX];
static int g_n = 0;
static int g_loaded = 0;

static int g_save_err = 0;   /* sticky until cfg_take_save_error() reads it */
static int g_load_failed = 0;   /* cfg_load hit a transient open/read error (NOT genuine first-run):
                                 * the in-memory set is empty/partial, so rewrite() must REFUSE to run -
                                 * else a single boot-time read glitch would overwrite a good diskos.conf
                                 * (all EQ/wifi/lastfm/UI settings) with an empty one. */

static cfg_entry_t *find(const char *key){
    for(int i=0;i<g_n;i++) if(!strcmp(g_cfg[i].k,key)) return &g_cfg[i];
    return NULL;
}

/* Atomic, durable rewrite: write a temp file, fsync it, rename over the real file
 * (atomic on POSIX), then fsync the directory. A power cut can no longer leave a
 * truncated/half-written diskos.conf - you keep either the old file or the new one.
 * Returns 0 on success, -1 on any failure (and sets the sticky save-error flag). */
static int rewrite(void){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* never persist an unloaded/partial config over a good file */
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", CFG_PATH);
    FILE *f = fopen(tmp, "w");
    if(!f){ g_save_err = 1; return -1; }
    int ok = 1;
    for(int i=0;i<g_n;i++)
        if(fprintf(f, "%s=%s\n", g_cfg[i].k, g_cfg[i].v) < 0){ ok = 0; break; }
    if(ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = 0;
    if(fclose(f) != 0) ok = 0;
    if(!ok || rename(tmp, CFG_PATH) != 0){ unlink(tmp); g_save_err = 1; return -1; }
    /* persist the rename itself; a dir open/fsync/close failure means the new file may not survive a
     * power cut, so surface it (rewrite()'s contract is durable success), though the content is written. */
    int dfd = open(CFG_DIR, O_RDONLY | O_DIRECTORY);
    if(dfd < 0){ g_save_err = 1; return -1; }
    int dr = fsync(dfd);
    if(close(dfd) != 0 || dr != 0){ g_save_err = 1; return -1; }
    return 0;
}

int cfg_take_save_error(void){ int e = g_save_err; g_save_err = 0; return e; }

static void put(const char *key, const char *val){
    cfg_entry_t *e = find(key);
    if(!e){
        if(g_n >= CFG_MAX) return;
        e = &g_cfg[g_n++];
        snprintf(e->k, KLEN, "%s", key);
    }
    snprintf(e->v, VLEN, "%s", val);
}

void cfg_load(void){
    if(g_loaded) return;
    g_loaded = 1;
    FILE *f = fopen(CFG_PATH, "r");
    if(f){
        char line[KLEN+VLEN+4];
        while(fgets(line, sizeof(line), f)){
            char *nl = strchr(line, '\n'); if(nl) *nl = 0;
            char *eq = strchr(line, '=');
            if(!eq) continue;
            *eq = 0;
            if(line[0]) put(line, eq+1);
        }
        if(ferror(f)) g_load_failed = 1;   /* a mid-read I/O error -> partial load; block rewrites so we
                                            * don't persist a truncated config over the good on-disk one */
        fclose(f);
        return;
    }
    if(errno != ENOENT){   /* open failed for a TRANSIENT reason (EIO/EACCES/EMFILE), not "file absent":
                            * treat as unavailable, not first-run - leave the on-disk config untouched. */
        g_load_failed = 1;
        return;
    }
    /* ENOENT for diskos.conf: migrate settings from the pre-rename ipodos.conf if it
     * exists, so nobody loses their config across the rename. We read it, then rewrite()
     * persists to the new diskos.conf (the old file is left in place, harmless). */
    FILE *lc = fopen(LEGACY_CONF, "r");
    if(lc){
        char line[KLEN+VLEN+4];
        while(fgets(line, sizeof(line), lc)){
            char *nl = strchr(line, '\n'); if(nl) *nl = 0;
            char *eq = strchr(line, '=');
            if(!eq) continue;
            *eq = 0;
            if(line[0]) put(line, eq+1);
        }
        int bad = ferror(lc);
        fclose(lc);
        if(!bad) rewrite();   /* write the migrated set to diskos.conf */
        else g_load_failed = 1;
        return;
    }
    /* genuine first run (ENOENT): import legacy single-value swipe_thresh file if present */
    FILE *lf = fopen(LEGACY_SWIPE, "r");
    if(lf){
        int v=0;
        if(fscanf(lf,"%d",&v)==1 && v>=20 && v<=200){
            char b[16]; snprintf(b,sizeof(b),"%d",v); put("swipe_thresh", b);
        }
        fclose(lf);
        rewrite();
    }
}

int cfg_get_int(const char *key, int def){
    cfg_entry_t *e = find(key);
    return e ? atoi(e->v) : def;
}

int cfg_set_int(const char *key, int v){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* load failed -> fully read-only, no memory mutation */
    cfg_entry_t *e = find(key);
    if(e && atoi(e->v) == v) return 0;   /* unchanged: skip the flash rewrite */
    char b[16]; snprintf(b,sizeof(b),"%d",v);
    put(key, b);
    return rewrite();
}
/* Batched set: update the in-memory value only, NO flash write. Follow a run of these
 * with a single cfg_flush() so bulk updates (e.g. EQ "Flat" = 11 bands) do ONE atomic
 * rewrite/fsync instead of one per key. */
int cfg_set_int_deferred(const char *key, int v){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* load failed -> read-only */
    cfg_entry_t *e = find(key);
    if(e && atoi(e->v) == v) return 0;
    char b[16]; snprintf(b,sizeof(b),"%d",v);
    put(key, b);
    return 0;
}
int cfg_flush(void){ return rewrite(); }

const char *cfg_get_str(const char *key, const char *def){
    cfg_entry_t *e = find(key);
    return e ? e->v : def;
}

int cfg_set_str(const char *key, const char *v){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* load failed -> read-only */
    put(key, v);
    return rewrite();
}

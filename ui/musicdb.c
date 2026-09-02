/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "musicdb.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>

#define DB_PATH "/usr/data/fiio/db/song.db"

/* The whole library is held in a dynamically-sized array (no fixed cap): sized to
 * the actual SONG count at load. Loaded ONCE at startup, so the realloc never moves
 * under live pointers. */
static mdb_song_t *g_songs = NULL;
static int g_n = 0;
static int g_cap = 0;
static int g_load_err = 0;   /* mdb_load hit a DB error (BUSY/IOERR/OOM) vs a genuinely empty library */

/* ---- cached group lists (Artists/Albums/Genres) --------------------------
 * These are derived from g_songs by parse+sort+dedup - expensive to recompute
 * on every list open.  Build once, cache, and return a fast memcpy thereafter
 * (so opening Artists/Albums is as instant as Songs).  mdb_load() invalidates. */
static char (*g_cart)[MDB_STR];                      static int  g_cart_n = -1;   /* artists */
static char (*g_calb)[MDB_STR], (*g_calb_ar)[MDB_STR]; static int *g_calb_ct; static int g_calb_n = -1; /* albums */
static char (*g_cgen)[MDB_STR];                      static int *g_cgen_ct;  static int g_cgen_n = -1;   /* genres */
static void groups_free(void){
    free(g_cart);    g_cart=NULL;    g_cart_n=-1;
    free(g_calb);    free(g_calb_ar); free(g_calb_ct);
    g_calb=NULL; g_calb_ar=NULL; g_calb_ct=NULL; g_calb_n=-1;
    free(g_cgen);    free(g_cgen_ct); g_cgen=NULL; g_cgen_ct=NULL; g_cgen_n=-1;
}

static int mdb_ensure_cap(int need){
    if(need <= g_cap) return 1;
    int nc = g_cap ? g_cap : 256;
    while(nc < need) nc *= 2;
    mdb_song_t *p = realloc(g_songs, (size_t)nc * sizeof *g_songs);
    if(!p) return 0;
    g_songs = p; g_cap = nc; return 1;
}

static void trim(char *s){
    char *p = s; while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s, p, strlen(p)+1);
    int n = (int)strlen(s);
    while(n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n]=0;
}

/* One cached read/write connection, opened lazily. Holds no lock while idle, so
 * it coexists with mq_player (rollback-journal DB) via the busy timeout. All
 * mdb_* calls run on the UI thread. Prepared statements + bound params replace
 * the old popen("sqlite3 ...") + hand-built SQL (no escaping/injection/parse
 * bugs, no fork/exec per query). */
static sqlite3 *g_db;
/* g_db is shared across the UI thread, the art worker, and the prewarm thread.
 * The lazy open MUST be serialized (a plain `if(!g_db) open` is a data race: two
 * threads can both open, publish competing handles, and corrupt state -> SIGSEGV).
 * The mutex also gives a memory barrier so a published g_db is fully initialised
 * before another thread sees it. FULLMUTEX makes concurrent USE of the open handle
 * safe (sqlite serializes every API call on it). */
static pthread_mutex_t g_db_mu = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db(void){
    pthread_mutex_lock(&g_db_mu);
    if(!g_db){
        sqlite3 *tmp = NULL;
        if(sqlite3_open_v2(DB_PATH, &tmp, SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK){
            sqlite3_busy_timeout(tmp, 4000);
            /* persistent per-song accent cache (computed once from album art, survives reboot).
             * ALTER is idempotent here: harmless error if the column already exists. */
            sqlite3_exec(tmp, "ALTER TABLE SONG ADD COLUMN ACCENT INTEGER DEFAULT 0;", 0, 0, 0);
            /* diskOS-owned play history (separate table -> no SONG schema change, survives rescans,
             * ignored by the stock player). Drives Most-Played / Recently-Played. */
            sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS PLAY_STATS(PATH TEXT PRIMARY KEY, "
                              "PLAYS INTEGER DEFAULT 0, LAST_PLAYED INTEGER DEFAULT 0);", 0, 0, 0);
            g_db = tmp;   /* publish only after full init */
        } else if(tmp){ sqlite3_close(tmp); }
    }
    sqlite3 *ret = g_db;
    pthread_mutex_unlock(&g_db_mu);
    return ret;
}
/* Dedicated connection for the worker-thread accent WRITE (mdb_set_song_accent) ONLY.
 * Keeping that UPDATE off g_db means a worker's row-change can never be misread by a
 * UI-thread sqlite3_changes() (which follows the UI thread's own step()), and a worker
 * write can never join/rollback a g_db transaction. INVARIANT: nothing else may use
 * g_db_w, and no code here may call sqlite3_changes() on it (would reintroduce a race
 * between the two worker threads). Shorter busy_timeout than g_db: a stuck UI txn must
 * not wedge art decode for seconds - the accent write is best-effort, retried next decode. */
static sqlite3 *g_db_w;
static pthread_mutex_t g_db_w_mu = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db_w(void){
    db();   /* ensure the main connection (and its ACCENT-column ALTER) is initialised first,
             * so g_db_w never races an ALTER and the SONG.ACCENT column always exists. */
    pthread_mutex_lock(&g_db_w_mu);
    if(!g_db_w){
        sqlite3 *tmp = NULL;
        if(sqlite3_open_v2(DB_PATH, &tmp, SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK){
            sqlite3_busy_timeout(tmp, 500);
            g_db_w = tmp;
        } else if(tmp){ sqlite3_close(tmp); }
    }
    sqlite3 *ret = g_db_w;
    pthread_mutex_unlock(&g_db_w_mu);
    return ret;
}
/* per-song accent (0xRRGGBB packed, 0 = not computed yet). Persisted in SONG.ACCENT. */
int mdb_song_accent(const char *path){
    sqlite3 *d = db(); if(!d || !path) return 0;
    sqlite3_stmt *st; int rgb = 0;
    if(sqlite3_prepare_v2(d, "SELECT ACCENT FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) rgb = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return rgb;
}
void mdb_set_song_accent(const char *path, int rgb){
    sqlite3 *d = db_w(); if(!d || !path) return;   /* worker-only write on its private connection */
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE SONG SET ACCENT=? WHERE PATH=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int(st, 1, rgb); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
        sqlite3_step(st); sqlite3_finalize(st);
    }
}
/* prewarm iterator: next SONG with ID > after_id, ordered by ID (covers ALL rows,
 * not just the in-memory cap). Fills *id and path. Returns 1 if a row was found.
 * Own prepared statement each call -> safe to call from the prewarm thread under
 * SQLITE_THREADSAFE=1 (serialized) alongside the UI thread's DB use. */
int mdb_prewarm_next(int after_id, int *id, char *path, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; int found = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID, PATH FROM SONG WHERE ID > ? ORDER BY ID LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int(st, 1, after_id);
        if(sqlite3_step(st) == SQLITE_ROW){
            if(id) *id = sqlite3_column_int(st, 0);
            const char *p = (const char*)sqlite3_column_text(st, 1);
            if(path && cap>0) snprintf(path, cap, "%s", p ? p : "");
            found = 1;
        }
        sqlite3_finalize(st);
    }
    return found;
}
/* text column, never NULL (so snprintf "%s" is safe) */
static const char *colt(sqlite3_stmt *st, int i){
    const char *t = (const char*)sqlite3_column_text(st, i);
    return t ? t : "";
}

int mdb_load(void){
    g_n = 0; g_load_err = 0;
    groups_free();                 /* library reloaded -> drop cached Artists/Albums/Genres */
    sqlite3 *d = db(); if(!d){ g_load_err = 1; return 0; }
    /* size the array to the real count first (no 1500 cap) */
    sqlite3_stmt *cst;
    int count = 0, count_ok = 0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM SONG;", -1, &cst, NULL) == SQLITE_OK){
        if(sqlite3_step(cst) == SQLITE_ROW){ count = sqlite3_column_int(cst, 0); count_ok = 1; }
        sqlite3_finalize(cst);
    }
    /* A FAILED count query (BUSY/IOERR/corruption) must NOT look like an empty library - the
     * startup auto-scan keys off mdb_song_count()==0, and a spurious rescan on a transient error
     * is wrong. Only count_ok + count==0 is a genuine empty. */
    if(!count_ok){ g_load_err = 1; return 0; }
    if(count > 0 && !mdb_ensure_cap(count)) g_load_err = 1;   /* OOM can't grow to `count`: the load below
                                                               * fills the old g_cap and stops at rc==ROW,
                                                               * which otherwise reads as a benign race -
                                                               * flag it so a partial library isn't silent */
    if(g_cap == 0){ if(count > 0) g_load_err = 1; return 0; }   /* count>0 but no cap => OOM (error); count==0 => empty */
    sqlite3_stmt *st;
    const char *sql =
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(ALBUM,''),"
        "IFNULL(DURATION,0),ID,IFNULL(GENRE,'') FROM SONG ORDER BY 1 COLLATE NOCASE;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){ g_load_err = 1; return 0; }
    int rc = SQLITE_DONE;
    while(g_n < g_cap && (rc = sqlite3_step(st)) == SQLITE_ROW){
        mdb_song_t *s = &g_songs[g_n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        snprintf(s->genre,  MDB_STR, "%s", colt(st,5));
        trim(s->title); trim(s->artist); trim(s->album); trim(s->genre);
    }
    /* rc==ROW means we stopped only because the buffer filled (more rows than COUNT -> a benign
     * add-between-queries race, not an error). Anything other than ROW/DONE is a mid-query
     * BUSY/IOERR -> a partial load that must NOT read as "empty" to the startup auto-scan. */
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) g_load_err = 1;
    sqlite3_finalize(st);
    return g_n;
}

int mdb_song_count(void){ return g_n; }
int mdb_load_failed(void){ return g_load_err; }   /* 1 if the last mdb_load hit a DB error (not just empty) */
const mdb_song_t *mdb_song(int i){ return (i>=0 && i<g_n) ? &g_songs[i] : NULL; }

/* On-demand PATH lookup by song ID (not cached in mdb_song_t to save RAM;
 * taps are rare so a per-tap query is fine). Returns 1 on success. */
int mdb_song_path(int id, char *out, int cap){
    if(!out || cap<=0) return 0;
    out[0]=0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE ID=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    if(sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", colt(st,0));
    sqlite3_finalize(st);
    return out[0] ? 1 : 0;
}

/* On-demand ALBUM for a song ID. Returns 1 if a non-empty album was found. */
int mdb_song_album(int id, char *out, int cap){
    if(!out || cap<=0) return 0;
    out[0]=0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(ALBUM,'') FROM SONG WHERE ID=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    if(sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", colt(st,0));
    sqlite3_finalize(st);
    return out[0] ? 1 : 0;
}

/* Canonical ALBUM + ARTIST for a song by its PATH. The player's a2 metadata
 * strings can differ from the DB's stored values (whitespace/encoding/suffix),
 * so a Go-to-Album/Artist drill must match by the DB record, not the metadata
 * string, or it finds nothing. Returns 1 if the row was found. */
int mdb_song_meta_by_path(const char *path, char *album, int acap, char *artist, int arcap){
    if(album && acap>0) album[0]=0;
    if(artist && arcap>0) artist[0]=0;
    if(!path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(ALBUM,''),IFNULL(ARTIST,'') FROM SONG WHERE PATH=? LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int found = 0;
    if(sqlite3_step(st) == SQLITE_ROW){
        if(album  && acap>0)  snprintf(album,  acap,  "%s", colt(st,0));
        if(artist && arcap>0) snprintf(artist, arcap, "%s", colt(st,1));
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* The current/last "memory play" track (MEMORY_PLAY in song.db) + resume info,
 * so the UI can show what's playing on startup before any a2 frame arrives.
 * MEMORY_PLAY.MUSIC_ID maps to SONG.ID.  Returns 1 if a track was found. */
int mdb_current_play(mdb_song_t *out, int *pos_ms, int *is_playing){
    if(out) memset(out, 0, sizeof *out);
    if(pos_ms) *pos_ms = 0;
    if(is_playing) *is_playing = 0;
    sqlite3 *d = db(); if(!d) return 0;
    int mid = 0, pos = 0, play = 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT MUSIC_ID,IFNULL(POSITION,0),IFNULL(IS_PLAYING,0) "
                             "FROM MEMORY_PLAY ORDER BY ID DESC LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW){
            mid  = sqlite3_column_int(st,0);
            pos  = sqlite3_column_int(st,1);
            play = sqlite3_column_int(st,2);
        }
        sqlite3_finalize(st);
    }
    if(mid <= 0) return 0;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),"
                             "IFNULL(ALBUM,''),IFNULL(DURATION,0),ID FROM SONG WHERE ID=? LIMIT 1;",
                             -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, mid);
    int got = 0;
    if(sqlite3_step(st) == SQLITE_ROW){
        if(out){
            snprintf(out->title,  MDB_STR, "%s", colt(st,0));
            snprintf(out->artist, MDB_STR, "%s", colt(st,1));
            snprintf(out->album,  MDB_STR, "%s", colt(st,2));
            out->dur_ms = sqlite3_column_int(st,3);
            out->id     = sqlite3_column_int(st,4);
        }
        if(pos_ms) *pos_ms = pos;
        if(is_playing) *is_playing = play;
        got = 1;
    }
    sqlite3_finalize(st);
    return got;
}

/* The player rebuilds LIST_SONG_0 with these exact ORDER BY clauses before it
 * starts (reverse-engineered from mq_player's INSERT...SELECT SQL).  To make a
 * song tap land on the EXACT track, we compute the song's 1-based rank within
 * the same filtered+ordered set.  list_type: 1=all,2=artist,3=album,10=genre
 * (anything else = unfiltered all-songs order).
 * Returns the 1-based position (>=1), or 1 if it can't be resolved. */
int mdb_play_pos(int id, int list_type, const char *name){
    /* The title-code ordering the player uses for non-album lists. */
    static const char *ORDER_TAIL =
        "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END,"
        "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END";
    static const char *ORDER_ALBUM =
        "CASE WHEN DISC=0 THEN 1 ELSE 0 END,DISC,"
        "CASE WHEN TRACK=0 THEN 1 ELSE 0 END,TRACK,"
        "CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END";

    sqlite3 *d = db(); if(!d) return 0;
    /* order + filter column are CONSTANTS chosen by list_type (never user text);
     * the playlist NAME is bound, not interpolated. */
    const char *order = (list_type==3) ? ORDER_ALBUM : ORDER_TAIL;
    const char *col   = (list_type==3) ? "ALBUM" : (list_type==2) ? "ARTIST" : (list_type==10) ? "GENRE" : NULL;
    char sql[1400];
    if(col)
        snprintf(sql, sizeof sql,
            "SELECT pos FROM (SELECT ID,ROW_NUMBER() OVER (ORDER BY %s) pos FROM SONG WHERE %s=?) WHERE ID=?;",
            order, col);
    else
        snprintf(sql, sizeof sql,
            "SELECT pos FROM (SELECT ID,ROW_NUMBER() OVER (ORDER BY %s) pos FROM SONG) WHERE ID=?;", order);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if(col){ sqlite3_bind_text(st,1,name?name:"",-1,SQLITE_STATIC); sqlite3_bind_int(st,2,id); }
    else   { sqlite3_bind_int(st,1,id); }
    int pos = 0;   /* 0 = song not found in this list (caller must check) */
    if(sqlite3_step(st) == SQLITE_ROW){ int v=sqlite3_column_int(st,0); if(v>=1) pos=v; }
    sqlite3_finalize(st);
    return pos;
}

int mdb_split_artists(const char *raw, char toks[][MDB_STR], int cap){
    int n = 0;
    char buf[MDB_STR]; snprintf(buf, MDB_STR, "%s", raw);
    /* split on ',' and ';' */
    char *p = buf, *start = buf;
    while(n < cap){
        if(*p==',' || *p==';' || *p==0){
            char c = *p; *p = 0;
            char tok[MDB_STR]; snprintf(tok, MDB_STR, "%s", start); trim(tok);
            if(tok[0]) snprintf(toks[n++], MDB_STR, "%s", tok);
            if(c==0) break;
            start = p+1;
        }
        p++;
    }
    return n;
}


typedef struct { const char *al; const char *ar; } mdb_ai_t;   /* (album, its artist) for sorting */
static int mdb_ai_cmp(const void *a, const void *b){
    return strcasecmp(((const mdb_ai_t*)a)->al, ((const mdb_ai_t*)b)->al);
}
int mdb_albums(char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap){
    if(g_calb_n < 0){                                        /* build once, then cache */
        mdb_ai_t *tmp = malloc((size_t)(g_n>0?g_n:1) * sizeof *tmp);
        if(!tmp) return 0;                                   /* transient OOM: don't cache, retry later */
        int m = 0;
        for(int i=0;i<g_n;i++) if(g_songs[i].album[0]){ tmp[m].al=g_songs[i].album; tmp[m].ar=g_songs[i].artist; m++; }
        qsort(tmp, (size_t)m, sizeof *tmp, mdb_ai_cmp);      /* sort by album, then group adjacent */
        int n = 0;
        for(int i=0;i<m;i++){
            if(n>0 && !strcasecmp(tmp[i].al, names[n-1])) counts[n-1]++;
            else { if(n>=cap) break;
                   snprintf(names[n],MDB_STR,"%s",tmp[i].al); snprintf(artists[n],MDB_STR,"%s",tmp[i].ar); counts[n]=1; n++; }
        }
        free(tmp);
        g_calb=malloc((size_t)(n>0?n:1)*MDB_STR); g_calb_ar=malloc((size_t)(n>0?n:1)*MDB_STR); g_calb_ct=malloc((size_t)(n>0?n:1)*sizeof(int));
        if(g_calb && g_calb_ar && g_calb_ct){
            if(n){ memcpy(g_calb,names,(size_t)n*MDB_STR); memcpy(g_calb_ar,artists,(size_t)n*MDB_STR); memcpy(g_calb_ct,counts,(size_t)n*sizeof(int)); }
            g_calb_n=n;
        } else { free(g_calb); free(g_calb_ar); free(g_calb_ct); g_calb=NULL; g_calb_ar=NULL; g_calb_ct=NULL; }
        return n;
    }
    int n = g_calb_n < cap ? g_calb_n : cap;                 /* cache hit -> instant copy */
    if(n>0){ memcpy(names,g_calb,(size_t)n*MDB_STR); memcpy(artists,g_calb_ar,(size_t)n*MDB_STR); memcpy(counts,g_calb_ct,(size_t)n*sizeof(int)); }
    return n;
}

/* qsort comparator over the flat names[][MDB_STR] array (case-insensitive) */
static int mdb_name_ci_cmp(const void *a, const void *b){ return strcasecmp((const char*)a, (const char*)b); }

int mdb_artists(char names[][MDB_STR], int cap){
    if(g_cart_n < 0){                                        /* build once, then cache */
        /* Build into a temp sized to the EXACT token count (not the caller's cap), so a
         * collab-heavy library can't truncate before dedup. mdb_split_artists splits on
         * ',' / ';' capped at 8/song, so count separators (+1), capped at 8. */
        long maxtok = 0;
        for(int i=0;i<g_n;i++){
            int c=1; for(const char *p=g_songs[i].artist; *p; p++) if(*p==','||*p==';') c++;
            maxtok += c>8?8:c;
        }
        char (*buf)[MDB_STR] = malloc((size_t)(maxtok>0?maxtok:1) * MDB_STR);
        if(!buf) return 0;                                   /* transient OOM: leave -1, retry later */
        int n = 0;
        for(int i=0;i<g_n;i++){
            char toks[8][MDB_STR];
            int t = mdb_split_artists(g_songs[i].artist, toks, 8);
            for(int k=0;k<t && n<maxtok;k++)
                if(toks[k][0]) snprintf(buf[n++], MDB_STR, "%s", toks[k]);
        }
        qsort(buf, (size_t)n, MDB_STR, mdb_name_ci_cmp);
        int w = 0;
        for(int i=0;i<n;i++)
            if(w==0 || strcasecmp(buf[w-1], buf[i]) != 0){
                if(w != i) memcpy(buf[w], buf[i], MDB_STR);
                w++;
            }
        g_cart = malloc((size_t)(w>0?w:1) * MDB_STR);
        if(g_cart){ if(w) memcpy(g_cart, buf, (size_t)w * MDB_STR); g_cart_n = w; }
        free(buf);
        if(g_cart_n < 0) return 0;                           /* cache alloc failed: retry later */
    }
    int n = g_cart_n < cap ? g_cart_n : cap;                 /* cache hit -> instant copy */
    if(n>0) memcpy(names, g_cart, (size_t)n * MDB_STR);
    return n;
}

static int mdb_pstr_cmp(const void *a, const void *b){ return strcasecmp(*(const char*const*)a, *(const char*const*)b); }
int mdb_genres(char names[][MDB_STR], int *counts, int cap){
    if(g_cgen_n < 0){                                        /* build once, then cache */
        const char **tmp = malloc((size_t)(g_n>0?g_n:1) * sizeof(char*));
        if(!tmp) return 0;
        int m=0;
        for(int i=0;i<g_n;i++) if(g_songs[i].genre[0]) tmp[m++]=g_songs[i].genre;
        qsort(tmp, (size_t)m, sizeof(char*), mdb_pstr_cmp);
        int n=0;
        for(int i=0;i<m;i++){
            if(n>0 && !strcasecmp(tmp[i], names[n-1])) counts[n-1]++;
            else { if(n>=cap) break; snprintf(names[n],MDB_STR,"%s",tmp[i]); counts[n]=1; n++; }
        }
        free(tmp);
        g_cgen=malloc((size_t)(n>0?n:1)*MDB_STR); g_cgen_ct=malloc((size_t)(n>0?n:1)*sizeof(int));
        if(g_cgen && g_cgen_ct){ if(n){ memcpy(g_cgen,names,(size_t)n*MDB_STR); memcpy(g_cgen_ct,counts,(size_t)n*sizeof(int)); } g_cgen_n=n; }
        else { free(g_cgen); free(g_cgen_ct); g_cgen=NULL; g_cgen_ct=NULL; }
        return n;
    }
    int n = g_cgen_n < cap ? g_cgen_n : cap;                 /* cache hit -> instant copy */
    if(n>0){ memcpy(names,g_cgen,(size_t)n*MDB_STR); memcpy(counts,g_cgen_ct,(size_t)n*sizeof(int)); }
    return n;
}

int mdb_genre_songs(const char *genre, const mdb_song_t **out, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(!strcasecmp(g_songs[i].genre, genre)) out[n++] = &g_songs[i];
    return n;
}

int mdb_album_songs(const char *album, const mdb_song_t **out, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(!strcasecmp(g_songs[i].album, album)) out[n++] = &g_songs[i];
    return n;
}

int mdb_artist_songs(const char *artist, const mdb_song_t **out, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++){
        char toks[8][MDB_STR];
        int t = mdb_split_artists(g_songs[i].artist, toks, 8);
        for(int k=0;k<t;k++) if(!strcasecmp(toks[k], artist)){ out[n++] = &g_songs[i]; break; }
    }
    return n;
}

/* Albums an artist appears on, with that artist's track count on each - the level
 * the Artists view was missing (tapping an artist used to dump every track they
 * appear on, in one flat list, with no album structure at all).
 *
 * Artist matching is per-TOKEN, the same rule mdb_artist_songs uses, so a collab
 * credited "A, B" puts the album under both A and B. Tracks with no ALBUM tag are
 * not represented here; the caller offers an "All Songs" row so they stay reachable.
 * Result is sorted by album name, case-insensitively. */
int mdb_artist_albums(const char *artist, char names[][MDB_STR], int *counts, int cap){
    if(!artist || !artist[0] || cap <= 0) return 0;
    /* collect this artist's albums (one entry per matching track), then sort+group */
    const char **tmp = malloc((size_t)(g_n>0?g_n:1) * sizeof(char*));
    if(!tmp) return 0;
    int m = 0;
    for(int i=0;i<g_n;i++){
        if(!g_songs[i].album[0]) continue;
        char toks[8][MDB_STR];
        int t = mdb_split_artists(g_songs[i].artist, toks, 8);
        for(int k=0;k<t;k++)
            if(!strcasecmp(toks[k], artist)){ tmp[m++] = g_songs[i].album; break; }
    }
    qsort(tmp, (size_t)m, sizeof(char*), mdb_pstr_cmp);
    int n = 0;
    for(int i=0;i<m;i++){
        if(n>0 && !strcasecmp(tmp[i], names[n-1])) continue;   /* same album again */
        if(n>=cap) break;
        snprintf(names[n], MDB_STR, "%s", tmp[i]); counts[n] = 0; n++;
    }
    free(tmp);

    /* Count the FULL album, not just this artist's share of it: opening one of these
     * rows shows the whole album (that is what an album is, and the only scope the
     * player can queue), so the number beside it has to describe the same thing. One
     * pass over the library with a binary search into the sorted album list. */
    for(int i=0;i<g_n;i++){
        if(!g_songs[i].album[0]) continue;
        int lo = 0, hi = n - 1;
        while(lo <= hi){
            int mid = lo + (hi - lo) / 2;
            int c = strcasecmp(g_songs[i].album, names[mid]);
            if(c == 0){ counts[mid]++; break; }
            if(c < 0) hi = mid - 1; else lo = mid + 1;
        }
    }
    return n;
}

int mdb_favorites(mdb_song_t *out, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(ALBUM,''),"
        "IFNULL(DURATION,0),ID FROM MY_LOVE ORDER BY 1 COLLATE NOCASE;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        mdb_song_t *s = &out[n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        s->genre[0] = 0;
        trim(s->title); trim(s->artist); trim(s->album);
    }
    sqlite3_finalize(st); return n;
}

/* ---- Play history (diskOS PLAY_STATS): Most-Played / Recently-Played ------- */
/* Record one play: bump PLAYS + stamp LAST_PLAYED for this path. Best-effort. */
void mdb_record_play(const char *path){
    if(!path || !path[0]) return;
    sqlite3 *d = db(); if(!d) return;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d,
        "INSERT INTO PLAY_STATS(PATH,PLAYS,LAST_PLAYED) VALUES(?1,1,?2) "
        "ON CONFLICT(PATH) DO UPDATE SET PLAYS=PLAYS+1, LAST_PLAYED=?2;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text (st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
    sqlite3_step(st); sqlite3_finalize(st);
}
/* Fill `out` with songs from PLAY_STATS joined to SONG, ordered by plays (by_recent=0) or by
 * last-played time (by_recent=1). Playback is by SONG.ID, same as favourites. Returns count. */
static int mdb_stats_list(mdb_song_t *out, int cap, int by_recent){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql = by_recent
      ? "SELECT IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')),IFNULL(S.ARTIST,''),IFNULL(S.ALBUM,''),"
        "IFNULL(S.DURATION,0),S.ID FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH "
        "ORDER BY P.LAST_PLAYED DESC LIMIT ?1;"
      : "SELECT IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')),IFNULL(S.ARTIST,''),IFNULL(S.ALBUM,''),"
        "IFNULL(S.DURATION,0),S.ID FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH "
        "WHERE P.PLAYS>0 ORDER BY P.PLAYS DESC, P.LAST_PLAYED DESC LIMIT ?1;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, cap);
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        mdb_song_t *s = &out[n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        s->genre[0] = 0;
        trim(s->title); trim(s->artist); trim(s->album);
    }
    sqlite3_finalize(st); return n;
}
int mdb_mostplayed(mdb_song_t *out, int cap){ return mdb_stats_list(out, cap, 0); }
int mdb_recent(mdb_song_t *out, int cap){ return mdb_stats_list(out, cap, 1); }

/* Remove a song from MY_LOVE (favourites) by its ID. Direct DB delete - the
 * player re-reads MY_LOVE on demand, so refreshing the list reflects it. */
int mdb_unfavorite(int id){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "DELETE FROM MY_LOVE WHERE ID=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(d);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed > 0;   /* true only if a MY_LOVE row was removed */
}

/* ---- custom playlists (PLAYLIST_INFO + CUSTOM_PLAYLIST) ------------------ */
/* The player builds a playlist by copying SONG rows into CUSTOM_PLAYLIST keyed
 * by a PLAYLIST_ID; PLAYLIST_INFO holds the names. We manage both directly. */
#define PL_COLS "PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE"
#define PL_SRC  "PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE"

/* Create a new playlist; returns its id (>0) or 0 on failure. */
long mdb_playlist_create(const char *name){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS PLAYLIST_INFO "
                    "(ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT, ADD_TIME INT8);", 0, 0, 0);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "INSERT INTO PLAYLIST_INFO (NAME,ADD_TIME) VALUES (?, strftime('%s','now'));",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name?name:"", -1, SQLITE_STATIC);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE) return 0;
    long id = (long)sqlite3_last_insert_rowid(d);
    /* clear any orphaned membership rows that would collide with this new id */
    if(sqlite3_prepare_v2(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, id); sqlite3_step(st); sqlite3_finalize(st);
    }
    return id;
}
/* Copy a song (by path, from the SONG table) into a playlist. 1 on success. */
int mdb_playlist_add_song(long pid, const char *path){
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql = "INSERT OR IGNORE INTO CUSTOM_PLAYLIST (" PL_COLS ") "
                      "SELECT ?," PL_SRC " FROM SONG WHERE PATH=?;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    /* OR IGNORE + SELECT-from-SONG can succeed (DONE) yet insert 0 rows (path not in
     * SONG, or already present) - report a real add only when a row changed. */
    return (rc == SQLITE_DONE && sqlite3_changes(d) > 0) ? 1 : 0;
}
/* 1 if the playlist already contains this song path. */
int mdb_playlist_has_song(long pid, const char *path){
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT 1 FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? AND PATH=? LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int yes = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return yes;
}
/* Rename a playlist. */
/* Write a custom EQ curve into the player's PEQ table (STYLE_PRESET slot). The player
 * reloads bands -> biquad coeffs when 0689 selects this preset. Format captured from the
 * stock UI (RE_CATALOGUE §3): PARAMS_JSON = 10 band objects, gain/qValue as STRINGS. */
int mdb_set_peq(int style_preset, double master_gain, const char *params_json){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE PEQ SET MASTER_GAIN=?, PARAMS_JSON=? WHERE STYLE_PRESET=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_double(st, 1, master_gain);
    sqlite3_bind_text(st, 2, params_json?params_json:"", -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, style_preset);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE) return 0;
    if(sqlite3_changes(d) == 0){
        /* slot doesn't exist yet -> create it */
        if(sqlite3_prepare_v2(d, "INSERT INTO PEQ (STYLE_NAME, MASTER_GAIN, PARAMS_JSON, STYLE_PRESET) VALUES (?,?,?,?);", -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, "Custom", -1, SQLITE_STATIC);
        sqlite3_bind_double(st, 2, master_gain);
        sqlite3_bind_text(st, 3, params_json?params_json:"", -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 4, style_preset);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if(rc != SQLITE_DONE) return 0;
    }
    return 1;
}

int mdb_playlist_rename(long pid, const char *name){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE PLAYLIST_INFO SET NAME=? WHERE ID=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name?name:"", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, pid);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return (rc == SQLITE_DONE && sqlite3_changes(d) > 0) ? 1 : 0;   /* real rename only */
}
/* Delete a playlist: removes only the playlist + its membership rows; the SONG
 * table (the actual songs/files) is never touched. */
int mdb_playlist_delete(long pid){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    /* atomic: drop membership + the playlist row together, or roll back (no orphan rows).
     * Every step's rc is checked: a failed BEGIN aborts; any DELETE failure or a failed
     * COMMIT (e.g. SQLITE_BUSY) rolls back and reports failure - so the transaction can
     * never be left open on the shared g_db connection. */
    if(sqlite3_exec(d, "BEGIN;", 0, 0, 0) != SQLITE_OK) return 0;
    sqlite3_stmt *st;
    int memb_ok = 0, info_ok = 0, changed = 0;
    if(sqlite3_prepare_v2(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid); memb_ok = (sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
    }
    if(memb_ok && sqlite3_prepare_v2(d, "DELETE FROM PLAYLIST_INFO WHERE ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid);
        if(sqlite3_step(st) == SQLITE_DONE){ info_ok = 1; changed = (sqlite3_changes(d) > 0); }
        sqlite3_finalize(st);
    }
    /* success only if both DELETEs stepped clean AND COMMIT actually succeeded */
    if(memb_ok && info_ok && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return changed;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* List a playlist's songs in the SAME order the player plays them (its custom
 * list-build orders by the title-code tail), so a tap maps to a play position. */
int mdb_playlist_songs(long pid, mdb_song_t *out, int cap){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(DURATION,0) "
        "FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? ORDER BY "
        "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END,"
        "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        snprintf(out[n].title,  MDB_STR, "%s", colt(st,0));
        snprintf(out[n].artist, MDB_STR, "%s", colt(st,1));
        out[n].album[0]=0; out[n].genre[0]=0;
        out[n].dur_ms = sqlite3_column_int(st,2); out[n].id = 0;
        trim(out[n].title); trim(out[n].artist);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* Track count in a playlist. */
int mdb_playlist_count(long pid){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    int n=0; if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st,0);
    sqlite3_finalize(st);
    return n;
}

/* playlist id by exact name, or 0 if none (so re-import is idempotent). */
static long playlist_id_by_name(const char *name){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; long id = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM PLAYLIST_INFO WHERE NAME=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) id = (long)sqlite3_column_int64(st,0);
        sqlite3_finalize(st);
    }
    return id;
}
/* Resolve an m3u entry to a real SONG.PATH: exact match first, else by filename
 * (so m3u files with relative paths or a different root still resolve). 1=found. */
static int song_resolve(const char *entry, char *out, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; int got = 0;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, entry, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW){ snprintf(out, cap, "%s", colt(st,0)); got = 1; }
        sqlite3_finalize(st);
    }
    if(got) return 1;
    const char *base = strrchr(entry, '/'); base = base ? base+1 : entry;
    if(!base[0]) return 0;
    /* escape LIKE metachars in the basename so a name like "10%_mix.mp3" can't wildcard-match the
     * wrong song; the leading "%/" stays a real wildcard (match any directory). */
    char base_esc[600]; { int j=0; for(const char *s=base; *s && j<(int)sizeof base_esc-2; s++){
        if(*s=='%'||*s=='_'||*s=='\\') base_esc[j++]='\\'; base_esc[j++]=*s; } base_esc[j]=0; }
    char like[600]; snprintf(like, sizeof like, "%%/%s", base_esc);
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE PATH LIKE ? ESCAPE '\\' LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, like, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW){ snprintf(out, cap, "%s", colt(st,0)); got = 1; }
        sqlite3_finalize(st);
    }
    /* fuzzy basename fallback (exact PATH missed) - surface it: if duplicate basenames
     * exist in different dirs this picks an arbitrary one, so make the guess visible. */
    if(got) fprintf(stderr,"m3u resolve: fuzzy basename match '%s' -> '%s'\n", base, out);
    return got;
}
/* Import one .m3u/.m3u8 file as a playlist named after the file. Skips if a
 * playlist with that name already exists (idempotent re-scan). Returns tracks
 * added, or 0 if skipped/empty. */
static int import_m3u_file(const char *m3u_path){
    FILE *fp = fopen(m3u_path, "r"); if(!fp) return 0;
    const char *b = strrchr(m3u_path, '/'); b = b ? b+1 : m3u_path;
    char name[160]; snprintf(name, sizeof name, "%s", b);
    char *dot = strrchr(name, '.'); if(dot) *dot = 0;
    if(playlist_id_by_name(name) > 0){ fclose(fp); return 0; }   /* already imported */
    char dir[400]; snprintf(dir, sizeof dir, "%s", m3u_path);
    char *sl = strrchr(dir, '/'); if(sl) *sl = 0; else dir[0] = 0;
    long pid = 0; int added = 0; char line[700];
    while(fgets(line, sizeof line, fp)){
        char *nl = strpbrk(line, "\r\n"); if(nl) *nl = 0;
        char *p = line; while(*p==' '||*p=='\t') p++;
        if((unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF) p += 3; /* UTF-8 BOM */
        while(*p==' '||*p=='\t') p++;
        for(char *q=p; *q; q++) if(*q=='\\') *q='/';            /* Windows backslash -> '/' so paths resolve */
        if(!*p || *p=='#') continue;                            /* comment / #EXTINF / blank */
        char entry[700];
        if(*p=='/') snprintf(entry, sizeof entry, "%s", p);
        else        snprintf(entry, sizeof entry, "%s/%s", dir, p);
        char songpath[700];
        if(!song_resolve(entry, songpath, sizeof songpath) &&
           !song_resolve(p,     songpath, sizeof songpath)) continue;
        if(!pid){ pid = mdb_playlist_create(name); if(pid<=0) break; }
        if(mdb_playlist_add_song(pid, songpath)) added++;
    }
    int read_err = ferror(fp);
    /* delete a partial/failed import so a later re-scan retries it (else playlist_id_by_name blocks it
     * forever): nothing resolved, OR the file read errored mid-way (the playlist would be incomplete).
     * Report 0 in that case so the caller doesn't count a playlist that no longer exists. */
    if(pid && (added == 0 || read_err)){ mdb_playlist_delete(pid); added = 0; }
    fclose(fp);
    return added;
}
/* Scan a directory (one level) for *.m3u/*.m3u8 and import each new one.
 * Returns the number of NEW playlists imported. */
int mdb_import_m3u_dir(const char *dir){
    DIR *d = opendir(dir); if(!d) return 0;
    int total = 0; struct dirent *e;
    while((e = readdir(d))){
        const char *n = e->d_name; int L = (int)strlen(n);
        int ism3u = (L>4 && !strcasecmp(n+L-4, ".m3u")) || (L>5 && !strcasecmp(n+L-5, ".m3u8"));
        if(!ism3u) continue;
        char path[600]; snprintf(path, sizeof path, "%s/%s", dir, n);
        if(import_m3u_file(path) > 0) total++;
    }
    closedir(d);
    return total;
}

/* Import .m3u/.m3u8 from the SD <root> AND any case-insensitively-named Music / Playlist(s)
 * subdirectory of it (exFAT on Linux is case-sensitive, so "music"/"MUSIC"/"Playlist" all
 * need matching). Returns total playlists imported. */
int mdb_import_m3u_sd(const char *root){
    int total = mdb_import_m3u_dir(root);          /* the root itself */
    DIR *d = opendir(root); if(!d) return total;
    struct dirent *e;
    while((e = readdir(d))){
        const char *nm = e->d_name;
        if(nm[0]=='.') continue;
        if(!strcasecmp(nm,"Music") || !strcasecmp(nm,"Playlist") || !strcasecmp(nm,"Playlists")){
            char path[600]; snprintf(path, sizeof path, "%s/%s", root, nm);
            total += mdb_import_m3u_dir(path);     /* opendir() fails harmlessly if it's a file */
        }
    }
    closedir(d);
    return total;
}

/* number of playlists - lets callers size a dynamic buffer to the real count (no fixed cap). */
int mdb_playlist_num(void){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; int n=0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM PLAYLIST_INFO;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st,0);
        sqlite3_finalize(st);
    }
    return n;
}

int mdb_playlists(char names[][MDB_STR], long *ids, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT NAME,ID FROM PLAYLIST_INFO ORDER BY ADD_TIME,ID;", -1, &st, NULL) != SQLITE_OK) return 0;
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        snprintf(names[n], MDB_STR, "%s", colt(st,0)); trim(names[n]);
        ids[n] = (long)sqlite3_column_int64(st,1); n++;
    }
    sqlite3_finalize(st);
    return n;
}

int mdb_search(const char *q, const mdb_song_t **out, int cap){
    if(!q || !q[0]) return 0;
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++){
        const mdb_song_t *s = &g_songs[i];
        if(strcasestr(s->title, q) || strcasestr(s->artist, q) || strcasestr(s->album, q))
            out[n++] = s;
    }
    return n;
}

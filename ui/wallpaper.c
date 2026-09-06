/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "wallpaper.h"
#include "config.h"
#include "fileutil.h"
#include "txtfold.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

/* Wallpaper: a still image behind Home when there is no album art worth showing.
 *
 * This deliberately copies the album-art pipeline rather than inventing one. ffmpeg is
 * already on the device and art.c already shells out to it; LVGL is already handed a
 * 360x360 BGR24 BMP from tmpfs for the blurred cover. So the whole feature is "produce
 * the same kind of file from a different source image", which means:
 *
 *   - no image decoder resident in our address space, and none on the LVGL thread
 *   - one conversion per picture EVER (cached on the SD, keyed by file fingerprint)
 *   - a per-frame cost identical to the backdrop that already ships, i.e. none
 *
 * Measured budget this is held to: mq_ui is ~8.4 MB RSS with music playing and the
 * device has ~60 MB available, so one 389 KB working file in tmpfs is ~4% growth, once,
 * and it is removed when the selection changes. */

#define WP_DIR_A   "/tmp/sdcard/Wallpapers"
#define WP_DIR_B   "/tmp/sdcard/wallpapers"     /* same lenience fontpick gives Fonts/fonts */
#define WP_CACHE   "/tmp/sdcard/.diskos/wallpaper"
#define MIN_FREE_BYTES (1024ULL*1024*1024)      /* leave the card room, as artcache does */
/* Bump when the CONVERSION changes (scale/crop/format) so cached files invalidate. */
#define WP_RECIPE_VER 1
#define WP_FFMPEG_TIMEOUT_S 25

#define CFG_MODE "wallpaper_mode"
#define CFG_FILE "wallpaper_file"

/* g_src is written by the worker and read by the UI loop. A mutex rather than an atomic
 * because it is a string: the reader must never see a half-written path. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char        g_src[160];              /* "A:/tmp/wp_<fp>.bmp", or empty */
static char        g_work[160];             /* the /tmp file g_src points at, for cleanup */
static atomic_int  g_busy;                  /* 1 while a conversion is in flight */
static int         g_mode;
static char        g_file[WP_NAME_MAX];     /* selected basename, "" = none */

/* ---------------------------------------------------------------- helpers */

static int is_image(const char *n){
    const char *d = strrchr(n, '.');
    if(!d) return 0;
    return !strcasecmp(d,".jpg") || !strcasecmp(d,".jpeg") ||
           !strcasecmp(d,".png") || !strcasecmp(d,".bmp");
}

/* Whichever spelling of the directory the user actually made. Empty if neither. */
static void wp_dir(char *out, int cap){
    struct stat s;
    if(stat(WP_DIR_A,&s)==0 && S_ISDIR(s.st_mode)){ snprintf(out,cap,"%s",WP_DIR_A); return; }
    if(stat(WP_DIR_B,&s)==0 && S_ISDIR(s.st_mode)){ snprintf(out,cap,"%s",WP_DIR_B); return; }
    out[0]=0;
}

/* FNV-1a over path + size + mtime, salted with the recipe version - the same scheme
 * artcache.c uses, and for the same reason: replace the file and the key changes, so a
 * stale entry simply misses instead of showing yesterday's picture. */
static int fingerprint(const char *path, char *out, int cap){
    struct stat st;
    if(!path || !*path || stat(path,&st)!=0) return -1;
    uint64_t h = 1469598103934665603ULL;
    h ^= (unsigned)WP_RECIPE_VER; h *= 1099511628211ULL;
    for(const char *p=path; *p; p++){ h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    uint64_t sz=(uint64_t)st.st_size, mt=(uint64_t)st.st_mtime;
    for(int i=0;i<8;i++){ h ^= (sz>>(i*8))&0xff; h *= 1099511628211ULL; }
    for(int i=0;i<8;i++){ h ^= (mt>>(i*8))&0xff; h *= 1099511628211ULL; }
    snprintf(out,cap,"%016llx",(unsigned long long)h);
    return 0;
}

static int file_ok(const char *p){ struct stat s; return stat(p,&s)==0 && s.st_size>100; }

static int enough_free(void){
    struct statvfs v;
    if(statvfs("/tmp/sdcard",&v)!=0) return 0;
    return ((uint64_t)v.f_bavail * v.f_frsize) > MIN_FREE_BYTES;
}

/* Single-quote for /bin/sh -c, same as art.c's shesc: a filename off the card is
 * attacker-ish input as far as this is concerned, and it is going into a shell. */
static void shesc(const char *in, char *out, int cap){
    int o=0;
    if(cap<3){ if(cap>0) out[0]=0; return; }
    for(int i=0; in[i] && o<cap-5; i++){
        if(in[i]=='\''){ if(o>cap-6) break;
            out[o++]='\''; out[o++]='\\'; out[o++]='\''; out[o++]='\''; }
        else out[o++]=in[i];
    }
    out[o]=0;
}

/* Run ffmpeg as a child, BOUNDED. Bounded here rather than reusing art.c's helper
 * because that one relies on art_cancel() being called when a track changes, and there
 * is no equivalent event for a wallpaper - a corrupt image would otherwise wedge this
 * worker for the life of the process. The device has no busybox `timeout` applet (S97
 * logs that every boot), so the bound is done here in C. */
static int run_bounded(const char *cmd, int timeout_s){
    pid_t pid = fork();
    if(pid < 0) return -1;
    if(pid == 0){
        setpgid(0,0);                       /* own group: one kill takes the whole pipeline */
        execl("/bin/sh","sh","-c",cmd,(char*)NULL);
        _exit(127);
    }
    setpgid(pid,pid);                       /* parent side of the race; ignore EACCES/ESRCH */
    int status=0;
    for(int waited=0; waited <= timeout_s*10; waited++){
        pid_t w = waitpid(pid,&status,WNOHANG);
        if(w == pid) return (WIFEXITED(status) && WEXITSTATUS(status)==0) ? 0 : -1;
        if(w < 0 && errno != EINTR) return -1;
        struct timespec ts = {0, 100*1000*1000};   /* 100ms */
        nanosleep(&ts,NULL);
    }
    kill(-pid, SIGKILL);                    /* timed out: take the group */
    do { } while(waitpid(pid,&status,0) < 0 && errno == EINTR);
    return -1;
}

/* Scale to FILL a 360x360 square and centre-crop. force_original_aspect_ratio=increase
 * then crop, so nothing is stretched and nothing is letterboxed - which matters more
 * than usual here, because the panel is a circle and letterbox bars would show as
 * chords. bilinear, not bicubic: this runs once per picture, but on a BogoMIPS-2387
 * core there is no reason to pay for the difference at 360px. */
static int convert(const char *img, const char *out_bmp){
    char esc[600], eout[600], cmd[1600];
    shesc(img, esc, sizeof esc);
    shesc(out_bmp, eout, sizeof eout);
    snprintf(cmd, sizeof cmd,
        "ffmpeg -y -loglevel quiet -i '%s' -an -vframes 1 "
        "-vf 'scale=360:360:force_original_aspect_ratio=increase:flags=bilinear,crop=360:360' "
        "-pix_fmt bgr24 -f image2 '%s'", esc, eout);
    if(run_bounded(cmd, WP_FFMPEG_TIMEOUT_S) != 0) return -1;
    return file_ok(out_bmp) ? 0 : -1;
}

/* ---------------------------------------------------------------- worker */

/* Publish a prepared wallpaper, and drop the tmpfs copy the previous one was using.
 * Removing the old file is what keeps this at ONE 389 KB working file rather than one
 * per wallpaper the user has ever tried. */
static void publish(const char *work, const char *src){
    char old[160];
    pthread_mutex_lock(&g_mu);
    snprintf(old,sizeof old,"%s",g_work);
    snprintf(g_work,sizeof g_work,"%s",work ? work : "");
    snprintf(g_src ,sizeof g_src ,"%s",src  ? src  : "");
    pthread_mutex_unlock(&g_mu);
    if(old[0] && strcmp(old, work ? work : "") != 0) unlink(old);
}

static void prepare_async(void);   /* the worker can re-arm itself - see below */

/* Prepare ONE selection. Split out so the worker can repeat it when the choice moves
 * mid-conversion. */
static void prepare_one(const char *name){
    char dir[160];
    if(!name[0]){ publish(NULL,NULL); return; }
    wp_dir(dir,sizeof dir);
    if(!dir[0]){ publish(NULL,NULL); return; }

    /* dir + '/' + a 255-byte dirent name + NUL. Sized from its inputs rather than
     * guessed at: truncating THIS path does not fail loudly, it silently addresses a
     * different file - or none. (The cross compiler caught this; the host -Wall pass
     * did not, so the Docker build stays the authority on warnings.) */
    char full[sizeof(dir) + WP_NAME_MAX + 2];
    snprintf(full,sizeof full,"%s/%s",dir,name);

    char fp[24];
    if(fingerprint(full,fp,sizeof fp)!=0){ publish(NULL,NULL); return; }

    char cached[320], work[192], src[sizeof work + 8];
    snprintf(cached,sizeof cached,"%s/%s.bmp",WP_CACHE,fp);
    snprintf(work  ,sizeof work  ,"/tmp/wp_%s.bmp",fp);
    snprintf(src   ,sizeof src   ,"A:%s",work);

    /* Already staged in tmpfs from earlier this boot - nothing to do. */
    if(file_ok(work)){ publish(work,src); return; }

    if(file_ok(cached)){
        /* Cache hit: copy SD -> tmpfs. LVGL reads from tmpfs on purpose (the card is
         * slow), which is the same reason ui.c stages cover/thumb/backdrop there.
         * file_copy_atomic rather than a shelled `cp`: no fork, no /bin/sh, no quoting
         * of a filename that came off the card - and dst never exists half-written, so
         * a reader cannot catch a partial wallpaper. */
        if(file_copy_atomic(cached, work, 0)==0 && file_ok(work)){ publish(work,src); return; }
    }

    /* Miss: convert once, then keep it on the card so this never happens again for
     * this picture - across reboots included. */
    if(convert(full, work) != 0){ unlink(work); publish(NULL,NULL); return; }
    if(enough_free()){
        mkdir("/tmp/sdcard/.diskos",0755);   /* ignore EEXIST */
        mkdir(WP_CACHE,0755);
        file_copy_atomic(work, cached, 0);   /* best effort: a failed cache is not a failure */
    }
    publish(work,src);
}

static void *prepare_worker(void *arg){
    (void)arg;
    char last[WP_NAME_MAX] = "";
    /* Loop until the selection stops moving. Picking a second wallpaper while the first
     * was still converting used to be dropped on the floor: prepare_async's
     * compare-exchange saw g_busy, returned, and nothing ever retried - so the newest
     * choice silently never appeared. */
    for(;;){
        char name[WP_NAME_MAX];
        pthread_mutex_lock(&g_mu);
        snprintf(name,sizeof name,"%s",g_file);
        pthread_mutex_unlock(&g_mu);

        prepare_one(name);
        snprintf(last,sizeof last,"%s",name);

        pthread_mutex_lock(&g_mu);
        int moved = strcmp(name, g_file) != 0;
        pthread_mutex_unlock(&g_mu);
        if(!moved) break;
    }
    atomic_store(&g_busy,0);
    /* Re-check AFTER clearing g_busy: a select() landing between the comparison above
     * and this store would have been refused by prepare_async, so pick it up here
     * rather than leave the newest choice unprepared. Terminates because each pass
     * handles the then-current name. */
    pthread_mutex_lock(&g_mu);
    int stale = strcmp(last, g_file) != 0;
    pthread_mutex_unlock(&g_mu);
    if(stale) prepare_async();
    return NULL;
}

static void prepare_async(void){
    int expected = 0;
    if(!atomic_compare_exchange_strong(&g_busy,&expected,1)) return;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64*1024);      /* it only runs fork/exec + snprintf */
    if(pthread_create(&th,&at,prepare_worker,NULL)!=0) atomic_store(&g_busy,0);
    pthread_attr_destroy(&at);
}

/* ---------------------------------------------------------------- public */

void wallpaper_init(void){
    g_mode = cfg_get_int(CFG_MODE, WP_OFF);
    snprintf(g_file,sizeof g_file,"%s",cfg_get_str(CFG_FILE,""));
    if(g_mode != WP_OFF && g_file[0]) prepare_async();
}

int wallpaper_list(char names[][WP_NAME_MAX], char disp[][WP_NAME_MAX], int cap){
    char dir[160]; wp_dir(dir,sizeof dir);
    if(!dir[0]) return 0;
    DIR *d = opendir(dir);
    if(!d) return 0;
    struct dirent *e;
    int n = 0;
    while(n < cap && (e = readdir(d))){
        if(e->d_name[0]=='.') continue;
        if(!is_image(e->d_name)) continue;
        snprintf(names[n],WP_NAME_MAX,"%s",e->d_name);
        /* Fold only the DISPLAY copy. The name in names[] has to keep its exact bytes,
         * because that is what opens the file - the same split wifi.c makes for SSIDs. */
        snprintf(disp[n],WP_NAME_MAX,"%s",e->d_name);
        txt_fold_ascii(disp[n]);
        n++;
    }
    closedir(d);
    return n;
}

void wallpaper_select(const char *name){
    pthread_mutex_lock(&g_mu);
    snprintf(g_file,sizeof g_file,"%s",name ? name : "");
    pthread_mutex_unlock(&g_mu);
    cfg_set_str(CFG_FILE, name ? name : "");
    if(!name || !*name){ publish(NULL,NULL); return; }
    prepare_async();
}

const char *wallpaper_selected(void){ return g_file; }

int  wallpaper_mode(void){ return g_mode; }

void wallpaper_set_mode(int mode){
    if(mode < WP_OFF || mode > WP_ALWAYS) mode = WP_OFF;
    g_mode = mode;
    cfg_set_int(CFG_MODE, mode);
    if(mode != WP_OFF && g_file[0]) prepare_async();
}

const char *wallpaper_src(void){
    if(g_mode == WP_OFF) return NULL;
    /* Snapshot under the lock, because the worker can rewrite g_src at any moment and
     * the caller keeps the pointer for the rest of its frame. `snap` is static and so
     * not reentrant - deliberately: the only caller is the LVGL thread. */
    static char snap[160];
    pthread_mutex_lock(&g_mu);
    snprintf(snap,sizeof snap,"%s",g_src);
    pthread_mutex_unlock(&g_mu);
    return snap[0] ? snap : NULL;
}

const char *wallpaper_home_src(const char *art_src, int have_track){
    if(g_mode == WP_OFF) return art_src;
    const char *wp = wallpaper_src();
    if(g_mode == WP_ALWAYS) return wp ? wp : art_src;
    /* WP_IDLE: the album backdrop belongs to whatever is loaded, so it wins while a
     * track is loaded - including paused, because flipping the whole background on a
     * pause would read as a bug rather than a feature. */
    if(have_track && art_src) return art_src;
    return wp ? wp : art_src;
}

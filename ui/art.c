/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "art.h"
#include "imgconv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>

/* ffmpeg reads the embedded cover art straight from the track file and, in a
 * single split filtergraph, writes the 148px cover, 42px Home thumb, and the
 * 360px blurred backdrop. No manual JPEG extraction, no big RAM buffer, and it
 * finds art anywhere in the file (not just the first ~1.3MB). tjpgd garbles
 * large covers, so we never use it. */

/* Escape a path for single-quoted shell inclusion: ' -> '\'' */
static void shesc(const char *in, char *out, size_t cap){
    size_t o = 0;
    for(size_t i=0; in[i] && o+4 < cap; i++){
        if(in[i]=='\''){ out[o++]='\''; out[o++]='\\'; out[o++]='\''; out[o++]='\''; }
        else out[o++] = in[i];
    }
    out[o] = 0;
}

/* The LIVE (user-driven) decode registers its child here so art_cancel() can kill
 * it when the user skips ahead. Prewarm / fallback decodes pass cancellable=0 and
 * are never registered, so cancelling a skip only targets the on-screen track. */
static pthread_mutex_t g_pid_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile pid_t  g_live_pid = 0;

/* Kill the in-flight LIVE decode (process group), if any. Safe to call anytime. */
void art_cancel(void){
    pthread_mutex_lock(&g_pid_mu);
    pid_t p = g_live_pid;
    if(p > 0) kill(-p, SIGKILL);
    pthread_mutex_unlock(&g_pid_mu);
}

/* Run one ffmpeg command as a killable child (sh -c, own process group).
 * cancellable=1 registers the child for art_cancel(). Returns 0 on success.
 *
 * Hold g_pid_mu across fork + setpgid + register so art_cancel() can't run in
 * the window between fork and registration (it would otherwise miss this child).
 * Both parent and child call setpgid (idempotent race) so the process group is
 * guaranteed to exist before we unlock -> kill(-pid) can't ESRCH on a missing pgid. */
static int art_run_cmd(const char *cmd, int cancellable){
    if(cancellable) pthread_mutex_lock(&g_pid_mu);
    pid_t pid = fork();
    if(pid < 0){ if(cancellable) pthread_mutex_unlock(&g_pid_mu); return -1; }
    if(pid == 0){
        setpgid(0, 0);                       /* own group so a single kill takes the whole pipeline */
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);                          /* exec failed */
    }
    if(cancellable){
        setpgid(pid, pid);                   /* parent side of the race; ignore EACCES/ESRCH */
        g_live_pid = pid;
        pthread_mutex_unlock(&g_pid_mu);
    }
    int status = 0; pid_t w;
    do { w = waitpid(pid, &status, 0); } while(w < 0 && errno == EINTR);   /* retry only on EINTR */
    if(cancellable){
        pthread_mutex_lock(&g_pid_mu); g_live_pid = 0; pthread_mutex_unlock(&g_pid_mu);
    }
    if(w < 0) return -1;                                            /* waitpid failed */
    if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;   /* killed or ffmpeg error */
    return 0;
}

/* Produce the three outputs from `src`. The filtergraph is identical whether the pixels
 * come from the TRACK (normal path) or from a cover we had to extract and convert
 * ourselves (the PNG path below), so it takes an input path rather than a track. */
static int art_run_graph(const char *src, const char *cover_bmp,
                         const char *thumb_bmp, const char *backdrop_bmp, int cancellable){
    char esc[600]; shesc(src, esc, sizeof esc);
    char cmd[1500];
    /* Backdrop = the actual cover art, scaled to fill the screen and gaussian-blurred
     * (iOS/Apple-Music "frosted cover" look). sigma chosen to soften detail while the
     * cover is still recognisable as itself. */
    snprintf(cmd, sizeof cmd,
        "ffmpeg -y -loglevel quiet -i '%s' -an -filter_complex "
        "'[0:v]split=3[a][b][c];[a]scale=148:148:flags=area[cv];[b]scale=42:42:flags=area[th];"
        "[c]scale=360:360:flags=bilinear,gblur=sigma=19[bg]' "
        "-map '[cv]' -pix_fmt bgr24 -f image2 '%s' "
        "-map '[th]' -pix_fmt bgr24 -f image2 '%s' "
        "-map '[bg]' -pix_fmt bgr24 -f image2 '%s'",
        esc, cover_bmp, thumb_bmp, backdrop_bmp);

    if(art_run_cmd(cmd, cancellable) != 0) return -1;

    /* all three outputs must exist + be non-trivial - catches a broken filtergraph
     * (e.g. a missing filter) immediately instead of shipping a half-written backdrop. */
    const char *outs[3] = { cover_bmp, thumb_bmp, backdrop_bmp };
    for(int i=0;i<3;i++){
        FILE *b = fopen(outs[i], "rb"); if(!b) return -1;
        fseek(b, 0, SEEK_END); long sz = ftell(b); fclose(b);
        if(sz <= 100) return -1;
    }
    return 0;
}

/* PNG-cover fallback.
 *
 * The device's ffmpeg (4.2.2, stock, read-only rootfs) has NO png decoder - measured on
 * hardware 2026-09-06; its image decoders are bmp, mjpeg, mjpegb and webp. So every
 * track whose embedded cover is a PNG failed the graph above with
 * "Decoder (codec png) not found" - which is most .m4a files and any MP3 tagged with a
 * PNG. That is what "m4a album art does not work" actually was.
 *
 * ffmpeg can still COPY the picture out (a stream copy needs no decoder) and can read a
 * BMP back in, so the only missing step is the PNG decode itself - and lodepng, vendored
 * with LVGL, does that. Convert once, then run the SAME graph, so the scaling, the blur,
 * the sizes and the cache contract downstream are all untouched.
 *
 * 4 megapixels: lodepng expands to RGBA, so that is a 16 MB transient on a device with
 * ~48 MB available and an audio engine to keep fed. Bigger than that is refused rather
 * than risking the player. */
#define ART_PNG_MAX_PIXELS (4u*1000u*1000u)

static int art_try_png_cover(const char *track, const char *cover_bmp,
                             const char *thumb_bmp, const char *backdrop_bmp,
                             int cancellable){
    char raw[128], bmp[128];
    /* getpid keeps two concurrent decodes (a prewarm and a track change) off each
     * other's temporaries. */
    snprintf(raw, sizeof raw, "/tmp/artpng_%d.png", (int)getpid());
    snprintf(bmp, sizeof bmp, "/tmp/artpng_%d.bmp", (int)getpid());
    remove(raw); remove(bmp);

    char esc[600], eraw[300], cmd[1100];
    shesc(track, esc, sizeof esc);
    shesc(raw, eraw, sizeof eraw);
    snprintf(cmd, sizeof cmd,
        "ffmpeg -y -loglevel quiet -i '%s' -an -map 0:v:0 -c:v copy -f image2 '%s'",
        esc, eraw);
    if(art_run_cmd(cmd, cancellable) != 0 || !img_is_png(raw)){ remove(raw); return -1; }

    int rc = img_png_to_bmp(raw, bmp, ART_PNG_MAX_PIXELS);
    remove(raw);
    if(rc != 0){ remove(bmp); return -1; }

    rc = art_run_graph(bmp, cover_bmp, thumb_bmp, backdrop_bmp, cancellable);
    remove(bmp);
    return rc;
}

/* Try the direct graph, then the PNG bridge. A track with NO cover at all fails both and
 * costs one extra ffmpeg spawn - accepted deliberately, because knowing in advance would
 * mean parsing every container ourselves to find out. */
int art_make_all_ex(const char *track, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable){
    if(art_run_graph(track, cover_bmp, thumb_bmp, backdrop_bmp, cancellable) == 0) return 0;
    return art_try_png_cover(track, cover_bmp, thumb_bmp, backdrop_bmp, cancellable);
}

/* Non-cancellable convenience wrapper (prewarm + synchronous fallback). */
int art_make_all(const char *track, const char *cover_bmp,
                 const char *thumb_bmp, const char *backdrop_bmp){
    return art_make_all_ex(track, cover_bmp, thumb_bmp, backdrop_bmp, 0);   /* NOLINT */
}

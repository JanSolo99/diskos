/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>

/* One atomic-copy implementation for the whole UI. It grew up in artcache.c (album-art
 * cache entries) and was then duplicated for the user font cache; keeping two meant a
 * durability fix to one would not reach the other, which is exactly what happened -
 * neither fsynced, so both could publish a truncated file under a valid name after a
 * power cut. */
int file_copy_atomic(const char *src, const char *dst, size_t max_bytes)
{
    FILE *in = fopen(src, "rb");
    if(!in) return -1;

    if(max_bytes){
        if(fseek(in, 0, SEEK_END) != 0){ fclose(in); return -1; }
        long sz = ftell(in);
        if(sz < 0 || (size_t)sz > max_bytes){ fclose(in); return -1; }
        rewind(in);
    }

    /* Unique temp per call: two copies to the SAME dst can be in flight at once (a
     * live album-art decode and the prewarm sweep), and a shared "<dst>.tmp" would
     * let them interleave into one file. */
    static _Atomic unsigned g_tmp_ctr = 0;
    unsigned seq = atomic_fetch_add(&g_tmp_ctr, 1u);
    char tmp[600];
    if(snprintf(tmp, sizeof tmp, "%s.tmp%u", dst, seq) >= (int)sizeof tmp){ fclose(in); return -1; }

    FILE *out = fopen(tmp, "wb");
    if(!out){ fclose(in); return -1; }

    char buf[16384];
    size_t n;
    int ok = 1;
    while((n = fread(buf, 1, sizeof buf, in)) > 0)
        if(fwrite(buf, 1, n, out) != n){ ok = 0; break; }
    if(ferror(in)) ok = 0;                     /* read error mid-copy: don't publish it */
    fclose(in);

    /* fflush only pushes stdio buffers into the kernel - the bytes are still page
     * cache when rename() commits the name. fsync before the rename is what makes
     * "the file exists" and "the file has contents" the same event. */
    if(fflush(out) != 0) ok = 0;
    if(ok && fsync(fileno(out)) != 0) ok = 0;
    fclose(out);
    if(!ok){ unlink(tmp); return -1; }

    if(rename(tmp, dst) != 0){ unlink(tmp); return -1; }

    /* fsync the DIRECTORY too, or the rename itself can be lost while the (synced)
     * data survives under the temporary name. Best-effort: a filesystem that refuses
     * to open the directory is not a reason to fail a copy that already succeeded. */
    char dir[600];
    snprintf(dir, sizeof dir, "%s", dst);
    char *slash = strrchr(dir, '/');
    if(slash){
        *slash = 0;
        int dfd = open(dir[0] ? dir : "/", O_RDONLY);
        if(dfd >= 0){ fsync(dfd); close(dfd); }
    }
    return 0;
}

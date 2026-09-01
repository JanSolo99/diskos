/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef ARTCACHE_H
#define ARTCACHE_H
/* Persistent decoded-art cache on the SD card, keyed by a file fingerprint
 * (path + size + mtime). Lets covers survive reboots and re-decode (ffmpeg) only
 * once per file, ever - so song-switching is instant once a song has been seen.
 * Keyed by file identity (not album/title), so it works regardless of metadata. */

/* Satisfy a request from cache: copies the cached cover/thumb/backdrop BMPs to the
 * given output paths. Returns 0 on a full hit (all three written), -1 on miss. */
int  artcache_get(const char *track, const char *cover_out, const char *thumb_out, const char *bg_out);

/* Store freshly-decoded BMPs into the cache for this track (atomic, best-effort).
 * No-op if the SD is nearly full. */
void artcache_put(const char *track, const char *cover, const char *thumb, const char *bg);

/* 1 if this track already has a complete cache entry (cover+thumb+backdrop). */
int  artcache_has(const char *track);

#endif

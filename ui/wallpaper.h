/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef WALLPAPER_H
#define WALLPAPER_H

/* A still image behind Home when there is no album art worth showing.
 *
 * Deliberately the same shape as the album backdrop that already ships: ffmpeg scales
 * the user's picture ONCE to a 360x360 BGR24 BMP, the result is cached on the SD card
 * keyed by a file fingerprint, and LVGL is handed a path in tmpfs. That path is already
 * proven on device for the blurred cover, so the wallpaper adds no new rendering route,
 * no per-frame decode, and one 389 KB working file rather than a resident decoder. */

enum { WP_OFF = 0, WP_IDLE = 1, WP_ALWAYS = 2 };

/* A dirent name can be 255 bytes. Truncating one would produce a name that no longer
 * OPENS, silently, so every buffer that holds a wallpaper filename is sized for the
 * worst case the filesystem can actually hand us. */
#define WP_NAME_MAX 256

/* Boot: adopt the stored mode/file and, if a wallpaper is wanted, prepare it on a
 * detached worker. Never blocks. */
void wallpaper_init(void);

/* Picker: names (basenames) of the images found on the card. Returns how many were
 * written. `disp` receives an ASCII-folded copy for drawing - the name in `names` keeps
 * its exact bytes, because that is what has to open. */
int  wallpaper_list(char names[][WP_NAME_MAX], char disp[][WP_NAME_MAX], int cap);

/* Choose one (a basename from wallpaper_list, or NULL/"" for none) and persist it.
 * Kicks the conversion on a worker; wallpaper_src() starts returning it when ready. */
void wallpaper_select(const char *name);
const char *wallpaper_selected(void);       /* stored basename, "" if none */

int  wallpaper_mode(void);                  /* WP_OFF / WP_IDLE / WP_ALWAYS */
void wallpaper_set_mode(int mode);

/* The LVGL image source for the prepared wallpaper, or NULL when off, unset, or not
 * converted yet. Cheap - safe to call from the UI loop. */
const char *wallpaper_src(void);

/* What Home should show behind itself, given the current album backdrop (or NULL) and
 * whether a track is loaded. Keeps the policy in one place instead of at each caller. */
const char *wallpaper_home_src(const char *art_src, int have_track);

#endif

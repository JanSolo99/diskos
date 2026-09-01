/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef SCANNER_H
#define SCANNER_H
/* diskOS music scanner - walks the SD, reads ID3 tags, rebuilds song.db's SONG table.
 * All calls are safe from the LVGL/main thread; the scan itself runs on a worker thread. */
int  scanner_start(void);                     /* begin a rescan; 0 = started, -1 = busy/failed */
int  scanner_active(void);                    /* 1 while a scan is running */
void scanner_progress(int *done, int *total); /* files done so far; total is 0 until finished */
/* Live progress for the scan screen: phase (0=counting files, 1=reading tags), files
 * done, files expected (0 while still counting), and the file being read right now.
 * Safe to call from the LVGL thread at any time; all fields are optional. */
void scanner_progress_ex(int *phase, int *done, int *expect, char *name, int cap);
int  scanner_take_finished(void);             /* returns 1 exactly once after a scan finishes */
int  scanner_no_sd(void);                     /* 1 if the last scan aborted: no SD mounted (library kept) */
#endif

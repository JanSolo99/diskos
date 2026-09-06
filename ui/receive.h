/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef RECEIVE_H
#define RECEIVE_H

/* Receive Music: a transient HTTP server that accepts file uploads onto the SD card.
 *
 * It exists ONLY while the Receive screen is open. There is no always-on listener, no
 * daemon, and nothing bound when you are not deliberately using it - the same lifecycle
 * lastfm.c gives its credential-setup server, for the same reason.
 *
 * Uploads STREAM to the card. A 60 MB memory budget cannot buffer an album, so the body
 * is written as it arrives, to a dot-prefixed part file that is renamed into place only
 * on a complete transfer - so a half-finished upload is never a file the scanner indexes
 * or the player tries to open. */

/* Start listening. Returns 0 on success, -1 if Wi-Fi is down or the socket fails.
 * Idempotent: a second call while running is a no-op. */
int  receive_start(void);
void receive_stop(void);

/* "http://<ip>:<port>/<token>/" while running, "" otherwise. Shown as text and as a QR. */
const char *receive_url(void);

/* Counters for the screen. `bytes` is of the transfer in flight, 0 when idle. */
int  receive_files_done(void);
int  receive_active(void);          /* 1 while a transfer is in progress */
long receive_active_bytes(void);
const char *receive_last_name(void);  /* basename of the most recent completed file */
const char *receive_last_error(void); /* "" when nothing has gone wrong */

/* 1 if anything landed this session - the screen offers a scan on the way out. */
int  receive_got_files(void);

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef SYSCONFIG_H
#define SYSCONFIG_H

/* Read/write a single INT column of the stock SYSCONFIG row (ID=1) in
 * /usr/data/fiio/db/sysconfig.db - the settings table mq_player and the MCU glue
 * read. Column names are whitelisted in sysconfig.c; anything else is refused.
 *
 * Both calls fail SOFT and are bounded: a locked, missing, or older-firmware DB
 * returns 0 and changes nothing, so callers must treat 0 as "unknown / not
 * applied" rather than as a value. */
int sysconfig_get_int(const char *col, int *out);   /* 1 = *out filled, 0 = unavailable */
int sysconfig_set_int(const char *col, int v);      /* 1 = one row updated, 0 = failed */

#endif

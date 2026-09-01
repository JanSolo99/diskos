/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sysconfig.h"
#include "sqlite3.h"
#include <string.h>
#include <stdio.h>

/* Typed access to the STOCK settings table.
 *
 * /usr/data/fiio/db/sysconfig.db holds one row (ID=1) with one INT column per
 * device setting - it is where mq_player and the MCU glue read things diskOS has
 * no command for. Charge protection is the motivating case: the stock UI's
 * "charging optimization" is the CHARGE_PROTECT column, and with diskOS installed
 * there was no way to see or change it without booting back into the stock UI,
 * which is exactly what the beta review reported.
 *
 * Two rules keep this safe:
 *  - Column names are NEVER interpolated from anything but a compile-time literal
 *    supplied by the caller, and are validated against a whitelist first. SQLite
 *    cannot bind an identifier, so the whitelist IS the injection defence.
 *  - Every call is bounded (busy timeout) and fails soft. This DB belongs to the
 *    stock player; if it is locked, missing, or lacks the column on a given
 *    firmware, we report "unknown" and change nothing rather than guessing.
 */

#ifndef SYSCONFIG_DB
#define SYSCONFIG_DB "/usr/data/fiio/db/sysconfig.db"
#endif
#define BUSY_MS 2500

/* Only these columns may be read or written. Adding one is a deliberate act: each
 * is a real setting the stock firmware acts on, and a typo would otherwise become a
 * silent no-op (SQLite happily reports "no such column" and we would swallow it). */
static const char *const ALLOWED[] = {
    "CHARGE_PROTECT",   /* charging optimisation: stop around 80-85% to spare the cell */
    "USB_MODE",         /* what a USB connection does (values not fully mapped yet) */
    "WORK_MODE",        /* audio source; diskOS forces LOCALPLAYER(4) at boot */
    "THEME_MODE",
    "POWER_SAVE",
};
static int allowed(const char *col){
    if(!col) return 0;
    for(int i=0;i<(int)(sizeof(ALLOWED)/sizeof(ALLOWED[0]));i++)
        if(!strcmp(ALLOWED[i], col)) return 1;
    return 0;
}

/* Open read-only or read-write. Never CREATE: if the stock DB is absent, this
 * firmware has no such settings and inventing an empty one would be worse than
 * doing nothing. */
static sqlite3 *db_open(int write)
{
    sqlite3 *db = NULL;
    int flags = write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
    if(sqlite3_open_v2(SYSCONFIG_DB, &db, flags, NULL) != SQLITE_OK){
        if(db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, BUSY_MS);
    return db;
}

int sysconfig_get_int(const char *col, int *out)
{
    if(!allowed(col) || !out) return 0;
    sqlite3 *db = db_open(0);
    if(!db) return 0;
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT %s FROM SYSCONFIG WHERE ID=1;", col);   /* col is whitelisted */
    sqlite3_stmt *st = NULL;
    int got = 0;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL){
            *out = sqlite3_column_int(st, 0);
            got = 1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return got;
}

int sysconfig_set_int(const char *col, int v)
{
    if(!allowed(col)) return 0;
    sqlite3 *db = db_open(1);
    if(!db) return 0;
    char sql[160];
    snprintf(sql, sizeof sql, "UPDATE SYSCONFIG SET %s=%d WHERE ID=1;", col, v);   /* col whitelisted, v is an int */
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, 0, 0, &err);
    if(err){ fprintf(stderr, "sysconfig: %s failed: %s\n", col, err); sqlite3_free(err); }
    /* changes()==0 means the column exists but no row matched (an empty/foreign DB) -
     * report that as failure so the UI doesn't claim a setting took effect. */
    int ok = (rc == SQLITE_OK) && (sqlite3_changes(db) > 0);
    sqlite3_close(db);
    return ok;
}

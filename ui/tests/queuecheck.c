/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host test for the LIST_SONG_0 queue-write layer in musicdb.c.
 *
 * This is the code path where a mistake does not crash - it plays the wrong song.
 * The player addresses rows in this table by position, so the invariant that
 * matters is: after ANY mutation, IDs are still contiguous from 1 and still in
 * play order. Every case below asserts exactly that.
 *
 * Build (from ui/):
 *   gcc -DQUEUECHECK -std=gnu11 -D_GNU_SOURCE -I. -fsanitize=address,undefined \
 *       -o /tmp/queuecheck tests/queuecheck.c musicdb.c txtfold.c sqlite3.c \
 *       -lpthread -ldl -lm
 * musicdb.c hardcodes DB_PATH, so the fixture is written there.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../musicdb.h"
#include "../sqlite3.h"

#define DBP "/usr/data/fiio/db/song.db"

static int fails;
static void ck(int cond, const char *what){
    printf("  %-56s %s\n", what, cond ? "ok" : "FAIL");
    if(!cond) fails++;
}

static void ex(sqlite3 *d, const char *sql){
    char *err = NULL;
    if(sqlite3_exec(d, sql, 0, 0, &err) != SQLITE_OK){
        printf("  setup SQL failed: %s\n", err ? err : "?");
        exit(2);
    }
}

/* the queue as "id:title,id:title,..." so a wrong order is visible, not just counted */
static void snapshot(sqlite3 *d, char *out, int cap){
    out[0] = 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT ID,TITLE FROM LIST_SONG_0 ORDER BY ID;", -1, &st, NULL) != SQLITE_OK) return;
    int n = 0;
    while(sqlite3_step(st) == SQLITE_ROW){
        n += snprintf(out + n, (size_t)(cap - n), "%s%d:%s", n ? "," : "",
                      sqlite3_column_int(st, 0), (const char*)sqlite3_column_text(st, 1));
        if(n >= cap - 1) break;
    }
    sqlite3_finalize(st);
}

/* IDs must be 1..N with no gaps - the player indexes by position */
static int contiguous(sqlite3 *d){
    sqlite3_stmt *st; int ok = 1, expect = 1;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM LIST_SONG_0 ORDER BY ID;", -1, &st, NULL) != SQLITE_OK) return 0;
    while(sqlite3_step(st) == SQLITE_ROW)
        if(sqlite3_column_int(st, 0) != expect++) ok = 0;
    sqlite3_finalize(st);
    return ok;
}

static void reset(sqlite3 *d){
    ex(d, "DELETE FROM LIST_SONG_0;");
    ex(d, "INSERT INTO LIST_SONG_0 (ID,LIST_ID,POS_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,"
          "IS_CUE,IS_ISO,OFFSET,DURATION,ADD_TIME,IS_SELECT,ALBUM_ARTIST) "
          "SELECT ROW_NUMBER() OVER (ORDER BY TRACK),NULL,NULL,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,"
          "DISC,TRACK,IS_CUE,IS_ISO,OFFSET,DURATION,ADD_TIME,IS_SELECT,ALBUM_ARTIST "
          "FROM SONG WHERE ALBUM='Rec' ORDER BY TRACK;");
    ex(d, "DELETE FROM MEMORY_PLAY; INSERT INTO MEMORY_PLAY (ID,MUSIC_ID,IS_PLAYING,POSITION,"
          "IS_CUE,IS_ISO,TRACK,IS_NAS,IS_M3U) VALUES (1,2,1,0,0,0,2,0,0);");
}

int main(void){
    sqlite3 *d;
    if(sqlite3_open(DBP, &d) != SQLITE_OK){ printf("cannot open %s\n", DBP); return 2; }

    /* fixture: 4-track album in the queue, plus one outsider to add */
    ex(d, "DROP TABLE IF EXISTS SONG; DROP TABLE IF EXISTS LIST_SONG_0; DROP TABLE IF EXISTS MEMORY_PLAY;");
    ex(d, "CREATE TABLE SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT, TITLE TEXT,"
          "ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT,"
          "OFFSET BIGINT, DURATION BIGINT, ADD_TIME INT8, IS_SELECT INT, ALBUM_ARTIST TEXT);");
    ex(d, "CREATE TABLE LIST_SONG_0 (ID INTEGER PRIMARY KEY autoincrement, LIST_ID INT, POS_ID INT,"
          "PATH TEXT, NAME TEXT, TITLE TEXT, ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT,"
          "IS_CUE INT, IS_ISO INT, OFFSET BIGINT, DURATION BIGINT, ADD_TIME INT8, IS_SELECT INT,"
          "SONG_TYPE INT, ALBUM_ARTIST TEXT, IS_M3U INT, M3U_PATH TEXT);");
    ex(d, "CREATE TABLE MEMORY_PLAY (ID INTEGER PRIMARY KEY autoincrement, MUSIC_ID INT, IS_PLAYING INT,"
          "POSITION INT, IS_CUE INT, IS_ISO INT, TRACK INT, IS_NAS INT, IS_M3U INT);");
    ex(d, "INSERT INTO SONG (PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,OFFSET,"
          "DURATION,ADD_TIME,IS_SELECT,ALBUM_ARTIST) VALUES"
          "('/sd/1.mp3','1.mp3','One','Rec','B','R',1,1,0,0,0,1000,0,0,'B'),"
          "('/sd/2.mp3','2.mp3','Two','Rec','B','R',1,2,0,0,0,1000,0,0,'B'),"
          "('/sd/3.mp3','3.mp3','Three','Rec','B','R',1,3,0,0,0,1000,0,0,'B'),"
          "('/sd/4.mp3','4.mp3','Four','Rec','B','R',1,4,0,0,0,1000,0,0,'B'),"
          "('/sd/x.mp3','x.mp3','Outsider','Other','S','J',1,7,0,0,0,1000,0,0,'S');");

    char snap[512];

    printf("-- append -----------------------------------------------------------\n");
    reset(d);
    ck(mdb_queue_count() == 4, "fixture queue has 4 rows");
    ck(mdb_queue_append("/sd/x.mp3") == 1, "append succeeds");
    snapshot(d, snap, sizeof snap);
    printf("     %s\n", snap);
    ck(mdb_queue_count() == 5,      "append grew the queue to 5");
    ck(contiguous(d),               "IDs still contiguous after append");
    ck(strstr(snap, "5:Outsider") != NULL, "appended row is LAST");
    ck(mdb_queue_append("/sd/nope.mp3") == 0, "appending an unknown path fails cleanly");
    ck(mdb_queue_count() == 5,      "...and changed nothing");

    printf("\n-- play next (insert after the playing row) --------------------------\n");
    reset(d);
    ck(mdb_queue_playing_id() == 2, "playing row resolves from MEMORY_PLAY.MUSIC_ID");
    ck(mdb_queue_insert_after(2, "/sd/x.mp3") == 1, "insert after row 2 succeeds");
    snapshot(d, snap, sizeof snap);
    printf("     %s\n", snap);
    ck(contiguous(d), "IDs still contiguous after a mid-queue insert");
    ck(strcmp(snap, "1:One,2:Two,3:Outsider,4:Three,5:Four") == 0,
       "order is One,Two,Outsider,Three,Four");
    ck(mdb_queue_playing_id() == 2, "the PLAYING row did not move (resume stays valid)");

    printf("\n-- remove -----------------------------------------------------------\n");
    reset(d);
    ck(mdb_queue_remove(3) == 1, "remove row 3 succeeds");
    snapshot(d, snap, sizeof snap);
    printf("     %s\n", snap);
    ck(contiguous(d), "IDs still contiguous after remove");
    ck(strcmp(snap, "1:One,2:Two,3:Four") == 0, "the gap closed, order preserved");
    ck(mdb_queue_remove(99) == 0, "removing a row that is not there fails cleanly");

    printf("\n-- clear after ------------------------------------------------------\n");
    reset(d);
    ck(mdb_queue_clear_after(2) == 1, "clear after row 2 succeeds");
    snapshot(d, snap, sizeof snap);
    printf("     %s\n", snap);
    ck(mdb_queue_count() == 2, "only the playing row and what precedes it remain");
    ck(contiguous(d),          "IDs still contiguous after clear");

    printf("\n-- the player's row shape is mirrored --------------------------------\n");
    reset(d);
    mdb_queue_append("/sd/x.mp3");
    sqlite3_stmt *st;
    int nulls = 0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM LIST_SONG_0 WHERE LIST_ID IS NULL AND POS_ID IS NULL;",
                          -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) nulls = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    ck(nulls == 5, "LIST_ID and POS_ID left NULL, exactly as the player writes them");

    sqlite3_close(d);
    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return fails ? 1 : 0;
}

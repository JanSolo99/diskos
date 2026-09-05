/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef MUSICDB_H
#define MUSICDB_H

#define MDB_MAX_SONGS   1500
#define MDB_MAX_GROUPS  600
#define MDB_STR         160

typedef struct {
    int  id;
    char title[MDB_STR];
    char artist[MDB_STR];   /* raw ARTIST field (may be "A, B", or "A feat. B") */
    char album_artist[MDB_STR]; /* ALBUM_ARTIST; EMPTY when the file had none */
    char album[MDB_STR];
    char genre[MDB_STR];    /* GENRE field - used here as user mood/tag */
    int  dur_ms;
    int  disc, track;       /* 0 = unknown; sorts LAST, matching the player's ORDER BY */
} mdb_song_t;

/* Load the whole library once (one sqlite3 call). Safe to call repeatedly;
 * reloads. Returns song count. */
int mdb_load(void);
int mdb_load_failed(void);   /* 1 if the last mdb_load hit a DB error (BUSY/IOERR/OOM), not just empty */

int               mdb_song_count(void);
const mdb_song_t *mdb_song(int i);
/* On-demand absolute file path for a song ID (queries the DB). 1 on success. */
int               mdb_song_path(int id, char *out, int cap);
/* On-demand ALBUM for a song ID. 1 if a non-empty album was found. */
int               mdb_song_album(int id, char *out, int cap);
int               mdb_song_meta_by_path(const char *path, char *album, int acap, char *artist, int arcap);
/* Year and average bitrate for a path (Song Info only; queried on screen entry).
 * Either out-param is set to 0 when unknown. 1 if the path was found. */
int               mdb_song_extra_by_path(const char *path, int *year, int *bitrate_kbps);
/* 1-based position of a song within the player's rebuilt list for a given
 * list_type (0=all,2=artist,3=album,10=genre) + name, matching mq_player's
 * exact ORDER BY so a tap lands on the exact track. Returns >=1. */
int               mdb_play_pos(int id, int list_type, const char *name);
/* Current/last playing track from MEMORY_PLAY (for startup state-sync).
 * Fills *out (title/artist/album/dur_ms/id), *pos_ms, *is_playing. 1 if found. */
int               mdb_current_play(mdb_song_t *out, int *pos_ms, int *is_playing);

/* Which name an artist list groups by. The Library offers both, because they answer
 * different questions and neither is a superset of the other:
 *   MDB_AR_TRACK - the raw ARTIST tag, comma/semicolon split. A guest on one track is
 *                  findable under their own name ("Eminem" via a 50 Cent feature).
 *   MDB_AR_ALBUM - ALBUM_ARTIST when the file has one, else ARTIST with a "feat." tail
 *                  stripped. One row per album artist, so a rip does not list "50 Cent",
 *                  "50 Cent feat. Eminem" and "50 Cent feat. Lloyd Banks" separately. */
enum { MDB_AR_TRACK = 0, MDB_AR_ALBUM = 1, MDB_AR_AXES = 2 };

/* Distinct albums (with a representative artist + track count). */
int  mdb_albums(char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap);
/* Distinct artists on the given axis (MDB_AR_TRACK / MDB_AR_ALBUM), comma-separated
 * names split so a collab shows under each individual name. Cached per axis. */
int  mdb_artists(int axis, char names[][MDB_STR], int cap);
/* Distinct genres/tags (with track count). */
int  mdb_genres(char names[][MDB_STR], int *counts, int cap);

/* Albums an artist appears on, sorted by album name, each with the album's FULL track
 * count (opening a row shows the whole album). Drives Artist -> Albums -> Tracks. */
int  mdb_artist_albums(int axis, const char *artist, char names[][MDB_STR], int *counts, int cap);

/* Fill out[] with songs in an album / by an artist token / in a genre. Returns count.
 * mdb_album_songs returns DISC/TRACK order (the player's album order), not alphabetical. */
int  mdb_album_songs(const char *album, const mdb_song_t **out, int cap);
int  mdb_artist_songs(int axis, const char *artist, const mdb_song_t **out, int cap);
int  mdb_genre_songs(const char *genre, const mdb_song_t **out, int cap);

/* Case-insensitive substring match on title/artist/album. Returns count. */
int  mdb_search(const char *q, const mdb_song_t **out, int cap);

/* Favourites (MY_LOVE) and playlists (PLAY_LIST). Both may be empty. */
int  mdb_favorites(mdb_song_t *out, int cap);
void mdb_record_play(const char *path);            /* bump PLAY_STATS on a new track */
int  mdb_mostplayed(mdb_song_t *out, int cap);     /* songs by play count desc */
int  mdb_recent(mdb_song_t *out, int cap);         /* songs by last-played desc */
int  mdb_unfavorite(int id);
int  mdb_playlist_num(void);   /* count of playlists (to size a dynamic buffer, no fixed cap) */
int  mdb_playlists(char names[][MDB_STR], long *ids, int cap);
/* custom playlists: create returns id (>0) or 0; add copies a SONG row by path */
long mdb_playlist_create(const char *name);
int  mdb_playlist_add_song(long pid, const char *path);
int  mdb_playlist_has_song(long pid, const char *path);
int  mdb_playlist_rename(long pid, const char *name);
int  mdb_playlist_delete(long pid);
int  mdb_playlist_songs(long pid, mdb_song_t *out, int cap);
int  mdb_playlist_count(long pid);
/* scan a dir for .m3u and .m3u8 files and import each new one as a playlist; returns # imported */
int  mdb_import_m3u_dir(const char *dir);
/* import from the SD root + any case-insensitive Music/Playlist(s) subdir; returns # imported */
int  mdb_import_m3u_sd(const char *root);

/* Split a raw ARTIST string into individual names (comma / ; / "feat."). */
int  mdb_split_artists(const char *raw, char toks[][MDB_STR], int cap);

/* ---- the player's live play queue (LIST_SONG_0) --------------------------
 * mq_player keeps its active queue in LIST_SONG_0 and RE-READS it as it advances -
 * device-verified 2026-09-05, see docs/QUEUE_DESIGN.md. So a row written here is
 * played, with no rebuild and no IPC command. That is what makes add-to-queue
 * possible at all; everything below is a plain SQL edit of that table.
 *
 * Row shape MIRRORS what the player writes: IDs contiguous from 1 (play order),
 * LIST_ID and POS_ID left NULL. Never renumber at or before the playing row -
 * MEMORY_PLAY.MUSIC_ID points at it and resume depends on that staying valid.
 *
 * All return 1 on success, 0 on failure. */
int  mdb_queue_count(void);                 /* rows currently in the queue */
/* Read the queue itself, in play order. `id` is the LIST_SONG_0.ID, which is also
 * the 1-based position the player addresses. This reads the REAL table rather than
 * reconstructing a scope, so it cannot disagree with what will actually play. */
int  mdb_queue_rows(mdb_song_t *out, int cap);
int  mdb_queue_playing_id(void);            /* LIST_SONG_0.ID of the playing row, 0 if unknown */
int  mdb_queue_append(const char *path);    /* add to the end */
/* Append many songs by SONG.ID in one transaction - a whole album or artist.
 * Returns the number of rows actually added. One statement per row would be a
 * commit per row, which on a 20-track album is 20 fsyncs on a flash device. */
int  mdb_queue_append_ids(const int *ids, int n);
int  mdb_queue_insert_after(int after_id, const char *path);  /* "play next" when given the playing row */
int  mdb_queue_remove(int id);              /* drop one row, closing the gap */
/* Swap two rows' positions. Used for move-up / move-down; both ids must exist. */
int  mdb_queue_swap(int id_a, int id_b);
int  mdb_queue_clear_after(int id);         /* drop everything past `id` (keeps what is playing) */

/* persistent per-song accent cache (0xRRGGBB; 0 = not computed). Survives reboot. */
int  mdb_song_accent(const char *path);
void mdb_set_song_accent(const char *path, int rgb);
/* prewarm iterator: next SONG row with ID > after_id. Returns 1 + fills id/path. */
int  mdb_prewarm_next(int after_id, int *id, char *path, int cap);

/* write a custom EQ curve to the player's PEQ table (STYLE_PRESET slot), then select it
 * with 0689 to apply. params_json = stock format (10 bands, gain/qValue as strings). */
int  mdb_set_peq(int style_preset, double master_gain, const char *params_json);

#endif

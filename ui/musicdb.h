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
/* 1-based position of a song within the player's rebuilt list for a given
 * list_type (0=all,2=artist,3=album,10=genre) + name, matching mq_player's
 * exact ORDER BY so a tap lands on the exact track. Returns >=1. */
int               mdb_play_pos(int id, int list_type, const char *name);
/* Current/last playing track from MEMORY_PLAY (for startup state-sync).
 * Fills *out (title/artist/album/dur_ms/id), *pos_ms, *is_playing. 1 if found. */
int               mdb_current_play(mdb_song_t *out, int *pos_ms, int *is_playing);

/* Distinct albums (with a representative artist + track count). */
int  mdb_albums(char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap);
/* Distinct artists, with comma-separated artists split so a collab shows under
 * each individual name. Grouped by ALBUM_ARTIST where the file has one - see
 * song_group_src() in musicdb.c for why. */
int  mdb_artists(char names[][MDB_STR], int cap);
/* Distinct genres/tags (with track count). */
int  mdb_genres(char names[][MDB_STR], int *counts, int cap);

/* Albums an artist appears on, sorted by album name, each with the album's FULL track
 * count (opening a row shows the whole album). Drives Artist -> Albums -> Tracks. */
int  mdb_artist_albums(const char *artist, char names[][MDB_STR], int *counts, int cap);

/* Fill out[] with songs in an album / by an artist token / in a genre. Returns count.
 * mdb_album_songs returns DISC/TRACK order (the player's album order), not alphabetical. */
int  mdb_album_songs(const char *album, const mdb_song_t **out, int cap);
int  mdb_artist_songs(const char *artist, const mdb_song_t **out, int cap);
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
/* scan a dir for *.m3u/*.m3u8 and import each new one as a playlist; returns # imported */
int  mdb_import_m3u_dir(const char *dir);
/* import from the SD root + any case-insensitive Music/Playlist(s) subdir; returns # imported */
int  mdb_import_m3u_sd(const char *root);

/* Split a raw ARTIST string into individual names (comma / ; / "feat."). */
int  mdb_split_artists(const char *raw, char toks[][MDB_STR], int cap);

/* persistent per-song accent cache (0xRRGGBB; 0 = not computed). Survives reboot. */
int  mdb_song_accent(const char *path);
void mdb_set_song_accent(const char *path, int rgb);
/* prewarm iterator: next SONG row with ID > after_id. Returns 1 + fills id/path. */
int  mdb_prewarm_next(int after_id, int *id, char *path, int cap);

/* write a custom EQ curve to the player's PEQ table (STYLE_PRESET slot), then select it
 * with 0689 to apply. params_json = stock format (10 bands, gain/qValue as strings). */
int  mdb_set_peq(int style_preset, double master_gain, const char *params_json);

#endif

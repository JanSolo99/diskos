/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "musicdb.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Queue screen (SCR_QUEUE): shows the current play queue and lets the user
 * jump to any track. Opened from the NP hub (right-swipe).
 *
 * The player builds LIST_SONG_0 internally from a list_type + name scope.
 * We reconstruct that scope's track list from the local DB so the queue
 * matches what the player will play. Tapping a row sends the 0100 jump
 * to that position.
 *
 * Two backing stores:
 *  - g_q_ptrs: array of const mdb_song_t* used for scopes backed by g_songs
 *    (all-songs, artist, album, genre) - pointers are stable until mdb_load().
 *  - g_q_owned: owned mdb_song_t buffer for scopes that return copies
 *    (playlists, favourites) - must persist until the next q_rebuild().
 *  Only one is active at a time; q_free releases both. */

static lv_obj_t *g_title_lbl;
static lv_obj_t *g_queue_list;
static lv_obj_t *g_status_lbl;

/* current scope */
static int  g_q_list_type = 1;   /* default: all songs */
static char g_q_name[256] = "";

/* active row pointers (what the list widgets show) */
static const mdb_song_t **g_q_ptrs = NULL;
static int g_q_count = 0;

/* backing stores */
static const mdb_song_t **g_q_indirect = NULL;  /* for artist/album/genre (pointers into g_songs) */
static mdb_song_t        *g_q_owned = NULL;      /* for playlist/favourites (copies we must keep) */
static int g_q_cap = 0;

static void q_free(void){
    free(g_q_indirect); g_q_indirect = NULL;
    free(g_q_owned);     g_q_owned = NULL;
    g_q_ptrs = NULL; g_q_count = 0; g_q_cap = 0;
}

static void q_ensure(int need){
    if(need <= g_q_cap) return;
    int n = g_q_cap ? g_q_cap : 64;
    while(n < need && n <= INT_MAX/2) n *= 2;
    if(n < need) n = need;
    const mdb_song_t **p = realloc(g_q_indirect, (size_t)n * sizeof(*p));
    if(!p) return;
    g_q_indirect = p; g_q_cap = n;
}

static void q_rebuild(void){
    q_free();
    if(!g_queue_list) return;
    lv_obj_clean(g_queue_list);

    int cap = mdb_song_count();
    if(cap < 1) cap = 1;
    q_ensure(cap);
    if(!g_q_indirect) return;

    if(g_q_list_type == 1){
        /* all songs - iterate g_songs in order */
        for(int i=0;i<cap;i++){
            const mdb_song_t *s = mdb_song(i);
            if(s) g_q_indirect[g_q_count++] = s;
        }
        g_q_ptrs = g_q_indirect;
    } else if(g_q_list_type == 2 && g_q_name[0]){
        g_q_count = mdb_artist_songs(MDB_AR_TRACK, g_q_name, g_q_indirect, g_q_cap);
        g_q_ptrs = g_q_indirect;
    } else if(g_q_list_type == 3 && g_q_name[0]){
        g_q_count = mdb_album_songs(g_q_name, g_q_indirect, g_q_cap);
        g_q_ptrs = g_q_indirect;
    } else if(g_q_list_type == 5 && g_q_name[0]){
        long pid = strtol(g_q_name, NULL, 10);
        if(pid > 0){
            g_q_owned = calloc((size_t)cap, sizeof(*g_q_owned));
            if(g_q_owned){
                int n = mdb_playlist_songs(pid, g_q_owned, cap);
                for(int i=0;i<n;i++) g_q_indirect[i] = &g_q_owned[i];
                g_q_count = n;
                g_q_ptrs = g_q_indirect;
            }
        }
    } else if(g_q_list_type == 6){
        g_q_owned = calloc((size_t)cap, sizeof(*g_q_owned));
        if(g_q_owned){
            int n = mdb_favorites(g_q_owned, cap);
            for(int i=0;i<n;i++) g_q_indirect[i] = &g_q_owned[i];
            g_q_count = n;
            g_q_ptrs = g_q_indirect;
        }
    } else if(g_q_list_type == 10 && g_q_name[0]){
        g_q_count = mdb_genre_songs(g_q_name, g_q_indirect, g_q_cap);
        g_q_ptrs = g_q_indirect;
    }
}

static void q_row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if(idx<0 || idx>=g_q_count) return;
    ui_play_list(g_q_list_type, g_q_name[0]?g_q_name:NULL, idx+1);
    screen_show(SCR_NOWPLAYING);
}

void queue_refresh(void){
    /* mirror whatever list the player currently has built (all-songs if none) */
    ui_play_scope_get(&g_q_list_type, g_q_name, sizeof g_q_name);
    q_rebuild();
    if(!g_queue_list) return;
    if(g_title_lbl) lv_label_set_text(g_title_lbl, "Queue");
    lv_obj_clean(g_queue_list);

    const char *scope = "All Songs";
    if(g_q_list_type == 2 && g_q_name[0]) scope = g_q_name;
    else if(g_q_list_type == 3 && g_q_name[0]) scope = g_q_name;
    else if(g_q_list_type == 5 && g_q_name[0]){
        long pid = strtol(g_q_name, NULL, 10);
        if(pid > 0){
            char names[64][MDB_STR]; long ids[64];
            int nc = mdb_playlists(names, ids, 64);
            for(int i=0;i<nc;i++) if(ids[i]==pid){ scope = names[i]; break; }
        }
    } else if(g_q_list_type == 6) scope = "Favourites";
    else if(g_q_list_type == 10 && g_q_name[0]) scope = g_q_name;

    char st[280]; snprintf(st, sizeof st, "%s  -  %d tracks", scope, g_q_count);
    lv_label_set_text(g_status_lbl, st);

    for(int i=0;i<g_q_count;i++){
        const mdb_song_t *s = g_q_ptrs[i];
        if(!s) continue;
        lv_obj_t *r = lv_button_create(g_queue_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 280, 52);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_color(r, th_card(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(r, th_card_press(), LV_STATE_PRESSED);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, q_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *pn = lv_label_create(r);
        char num[12]; snprintf(num, sizeof num, "%d", i+1);
        lv_label_set_text(pn, num);
        lv_obj_set_pos(pn, 8, 16);
        lv_obj_set_size(pn, 28, 20);
        lv_obj_set_style_text_align(pn, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(pn, th_font(14), 0);
        lv_obj_set_style_text_color(pn, th_text3(), 0);

        lv_obj_t *t = lv_label_create(r);
        lv_label_set_text(t, s->title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 44, 4);
        lv_obj_set_size(t, 230, 20);
        lv_obj_set_style_text_font(t, th_font(14), 0);
        lv_obj_set_style_text_color(t, th_text(), 0);

        lv_obj_t *a = lv_label_create(r);
        lv_label_set_text(a, s->artist);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(a, 44, 24);
        lv_obj_set_size(a, 230, 16);
        lv_obj_set_style_text_font(a, th_font(12), 0);
        lv_obj_set_style_text_color(a, th_text3(), 0);
    }
}

static void back_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back();
}

void queue_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title_lbl = ui_header_cb(root, "Queue", back_cb);

    g_status_lbl = lv_label_create(root);
    lv_label_set_text(g_status_lbl, "All Songs");
    lv_obj_set_pos(g_status_lbl, 40, 58);
    lv_obj_set_size(g_status_lbl, 280, 18);
    lv_obj_set_style_text_font(g_status_lbl, th_font(14), 0);
    lv_obj_set_style_text_color(g_status_lbl, th_text3(), 0);

    g_queue_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_queue_list);
    lv_obj_set_pos(g_queue_list, 40, 86);
    lv_obj_set_size(g_queue_list, 290, 250);
    lv_obj_set_style_bg_opa(g_queue_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_queue_list, 4, 0);
    lv_obj_set_flex_flow(g_queue_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_queue_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_queue_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_queue_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    g_q_ptrs = NULL; g_q_count = 0; g_q_cap = 0;
    queue_refresh();
}

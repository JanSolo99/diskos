/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "musicdb.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Queue screen (SCR_QUEUE): the player's live play queue. Opened from the NP hub
 * (swipe right from Now Playing).
 *
 * It reads LIST_SONG_0 DIRECTLY. That table is the queue - mq_player builds it,
 * re-reads it as it advances (device-verified, docs/QUEUE_DESIGN.md), and plays
 * its rows in ID order. Reading it beats reconstructing the scope from musicdb,
 * because a reconstruction can disagree with what will actually play, and the
 * failure mode of disagreeing is starting the wrong song.
 *
 * Tapping row N sends a bare position jump - no rebuild, so the queue survives.
 * Rows can be removed, and everything after the playing track can be cleared. */

static lv_obj_t *g_title_lbl;
static lv_obj_t *g_queue_list;
static lv_obj_t *g_status_lbl;

static mdb_song_t *g_rows = NULL;   /* the queue as read from LIST_SONG_0 */
static int         g_nrows = 0;
static int         g_cap = 0;
static int         g_playing_id = 0;   /* LIST_SONG_0.ID the player is on, 0 = unknown */

static void q_free(void){ free(g_rows); g_rows = NULL; g_nrows = 0; g_cap = 0; }

/* ---- adding to the queue from anywhere in the UI --------------------------
 * Both entry points do the same two things: write the row, then take ownership of
 * LIST_SONG_0 so nothing sends a rebuild over the user's list. Ownership is claimed
 * on the FIRST add, so the rows already there (whatever the player last built)
 * become the START of the queue rather than being discarded - which is what "add to
 * queue" has to mean while something is already playing.
 *
 * No IPC is sent. The player picks the row up when it reaches it, so the track
 * currently playing is never touched, restarted or re-queued. */
static int queue_add(const char *path, int next)
{
    if(!path || !path[0]) return 0;
    int ok;
    if(next){
        int cur = mdb_queue_playing_id();
        /* No resume row means we cannot know where "next" is. Appending is the
         * honest fallback - better than inserting at position 1 and yanking
         * playback backwards. */
        ok = cur > 0 ? mdb_queue_insert_after(cur, path) : mdb_queue_append(path);
    } else {
        ok = mdb_queue_append(path);
    }
    if(ok) ui_queue_take_ownership();
    return ok;
}
int ui_queue_add_next(const char *path){ return queue_add(path, 1); }
int ui_queue_add_end (const char *path){ return queue_add(path, 0); }

/* Append a whole album / artist / genre. Same ownership rule as a single add. */
int ui_queue_add_ids(const int *ids, int n){
    int added = mdb_queue_append_ids(ids, n);
    if(added) ui_queue_take_ownership();
    return added;
}

/* ---- rows ---------------------------------------------------------------- */

static void row_play_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_nrows) return;
    /* g_rows[i].id IS the 1-based position: LIST_SONG_0.ID is contiguous from 1 and
     * is the order the player plays. Jump, never rebuild. */
    if(ui_queue_owns_list()) ui_queue_play_pos(g_rows[i].id);
    else {
        int lt; char name[256];
        ui_play_scope_get(&lt, name, sizeof name);
        ui_play_list(lt, name[0] ? name : NULL, g_rows[i].id);
    }
    screen_show(SCR_NOWPLAYING);
}

/* Long-press removes. Deliberately not a swipe: the screen already uses
 * horizontal swipes for back-navigation, and a half-recognised swipe that
 * deletes a track is exactly the kind of surprise this UI should not have. */
static void row_remove_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_nrows) return;
    if(g_rows[i].id == g_playing_id){ ui_toast("That one is playing"); return; }
    if(mdb_queue_remove(g_rows[i].id)){
        ui_queue_take_ownership();
        ui_toast("Removed");
        queue_refresh();
    } else {
        ui_toast("Couldn't remove");
    }
}

static void clear_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int cur = mdb_queue_playing_id();
    if(cur <= 0){ ui_toast("Nothing playing"); return; }
    if(mdb_queue_clear_after(cur)){
        ui_queue_take_ownership();
        ui_toast("Cleared what's next");
        queue_refresh();
    }
}

void queue_refresh(void)
{
    if(!g_queue_list) return;
    lv_obj_clean(g_queue_list);

    int n = mdb_queue_count();
    if(n > g_cap){
        q_free();
        g_rows = malloc((size_t)n * sizeof *g_rows);
        if(!g_rows){ lv_label_set_text(g_status_lbl, "Out of memory"); return; }
        g_cap = n;
    }
    g_nrows = g_rows ? mdb_queue_rows(g_rows, g_cap) : 0;
    g_playing_id = mdb_queue_playing_id();

    if(g_title_lbl) lv_label_set_text(g_title_lbl, "Queue");

    if(g_nrows <= 0){
        lv_label_set_text(g_status_lbl, "Nothing queued");
        return;
    }
    char st[96];
    snprintf(st, sizeof st, "%d track%s%s", g_nrows, g_nrows == 1 ? "" : "s",
             ui_queue_owns_list() ? "  -  your queue" : "");
    lv_label_set_text(g_status_lbl, st);

    for(int i = 0; i < g_nrows; i++){
        const mdb_song_t *s = &g_rows[i];
        int is_now = (s->id == g_playing_id);

        lv_obj_t *r = lv_button_create(g_queue_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 280, 52);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_color(r, th_card(), 0);
        lv_obj_set_style_bg_opa(r, is_now ? LV_OPA_COVER : LV_OPA_70, 0);
        lv_obj_set_style_bg_color(r, th_card_press(), LV_STATE_PRESSED);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, row_play_cb,   LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(r, row_remove_cb, LV_EVENT_LONG_PRESSED,  (void*)(intptr_t)i);

        /* the playing row is marked with the play glyph instead of its number, so
         * "where am I" is answerable at a glance rather than by counting */
        lv_obj_t *pn = lv_label_create(r);
        char num[12];
        if(is_now) snprintf(num, sizeof num, "%s", LV_SYMBOL_PLAY);
        else       snprintf(num, sizeof num, "%d", s->id);
        lv_label_set_text(pn, num);
        lv_obj_set_pos(pn, 8, 16);
        lv_obj_set_size(pn, 28, 20);
        lv_obj_set_style_text_align(pn, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(pn, th_font(14), 0);
        lv_obj_set_style_text_color(pn, is_now ? ui_current_accent() : th_text3(), 0);

        lv_obj_t *t = lv_label_create(r);
        lv_label_set_text(t, s->title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 44, 4);
        lv_obj_set_size(t, 230, 20);
        lv_obj_set_style_text_font(t, ui_font_cjk(14), 0);
        lv_obj_set_style_text_color(t, is_now ? ui_current_accent() : th_text(), 0);

        lv_obj_t *a = lv_label_create(r);
        lv_label_set_text(a, s->artist);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(a, 44, 24);
        lv_obj_set_size(a, 230, 16);
        lv_obj_set_style_text_font(a, ui_font_cjk(14), 0);
        lv_obj_set_style_text_color(a, th_text3(), 0);
    }
}

static void back_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) screen_back();
}

void queue_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title_lbl = ui_header_cb(root, "Queue", back_cb);

    g_status_lbl = lv_label_create(root);
    lv_label_set_text(g_status_lbl, "");
    lv_obj_set_pos(g_status_lbl, 40, 58);
    lv_obj_set_size(g_status_lbl, 220, 18);
    lv_obj_set_style_text_font(g_status_lbl, th_font(14), 0);
    lv_obj_set_style_text_color(g_status_lbl, th_text3(), 0);

    /* "Clear" sits beside the count rather than in a menu: it is the one queue-wide
     * action, and burying a destructive action is worse than showing it. */
    lv_obj_t *clr = lv_button_create(root);
    lv_obj_remove_style_all(clr);
    lv_obj_set_pos(clr, 258, 52);
    lv_obj_set_size(clr, 60, 28);
    lv_obj_set_style_radius(clr, 14, 0);
    lv_obj_set_style_bg_color(clr, th_card(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(clr, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(clr, 6);
    lv_obj_add_event_cb(clr, clear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clr);
    lv_label_set_text(cl, "Clear");
    lv_obj_center(cl);
    lv_obj_set_style_text_font(cl, th_font(14), 0);
    lv_obj_set_style_text_color(cl, th_text2(), 0);

    /* 40..330 wide, 86..336 tall. The round screen pinches hard at the bottom, so
     * rows are 280 wide and the list stops at 336 rather than running to the edge. */
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

    queue_refresh();
}

lv_obj_t *queue_scroller(void){ return g_queue_list; }

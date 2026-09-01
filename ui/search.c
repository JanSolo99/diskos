/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "musicdb.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_RESULTS 80

static lv_obj_t *g_bar_lbl;   /* the search-bar text (query or placeholder) */
static lv_obj_t *g_clear;     /* clear-query (X) button, shown only with a query */
static lv_obj_t *g_results;
static library_song_click_cb_t g_song_cb;
static char g_query[96];

void search_set_song_click_cb(library_song_click_cb_t cb){ g_song_cb = cb; }

static void result_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int id = (int)(uintptr_t)lv_event_get_user_data(e);
    if(g_song_cb) g_song_cb(id);
    screen_show(SCR_NOWPLAYING);
}
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

static void rebuild(const char *q){
    lv_obj_clean(g_results);
    static const mdb_song_t *buf[MAX_RESULTS + 1];   /* +1 to detect "more than 80" vs "exactly 80" */
    int n = (q && q[0]) ? mdb_search(q, buf, MAX_RESULTS + 1) : 0;
    int shown = n > MAX_RESULTS ? MAX_RESULTS : n;
    if(n<=0){
        lv_obj_t *l = lv_label_create(g_results);
        lv_label_set_text(l, (q && q[0]) ? "No results" : "Tap the bar to search");
        lv_obj_set_style_text_color(l, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        return;
    }
    for(int i=0;i<shown;i++){
        const mdb_song_t *s = buf[i];
        lv_obj_t *row = lv_obj_create(g_results);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 296, 46);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1C1C1E), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, result_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)s->id);
        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, s->title[0]?s->title:"Untitled");
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 12, 5); lv_obj_set_size(t, 268, 19);
        lv_obj_set_style_text_font(t, ui_font_cjk(16), 0);   /* CJK titles via Source Han Sans fallback */
        lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
        lv_obj_t *a = lv_label_create(row);
        lv_label_set_text(a, s->artist);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(a, 12, 25); lv_obj_set_size(a, 268, 16);
        lv_obj_set_style_text_font(a, ui_font_cjk(14), 0);
        lv_obj_set_style_text_color(a, lv_color_hex(0xC7C7CC), 0);
    }
    if(n > MAX_RESULTS){    /* genuinely truncated - tell the user to narrow down */
        lv_obj_t *f = lv_label_create(g_results);
        lv_label_set_text(f, "Showing first 80 - refine to narrow");
        lv_obj_set_style_text_color(f, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(f, &lv_font_montserrat_14, 0);
    }
}

static void on_query(const char *text){
    snprintf(g_query, sizeof g_query, "%s", text ? text : "");
    if(g_bar_lbl) lv_label_set_text(g_bar_lbl, g_query[0] ? g_query : "Search songs, artists, albums");
    if(g_bar_lbl) lv_obj_set_style_text_color(g_bar_lbl,
        lv_color_hex(g_query[0] ? 0xFFFFFF : 0x8E8E93), 0);
    if(g_clear){
        if(g_query[0]) lv_obj_clear_flag(g_clear, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(g_clear, LV_OBJ_FLAG_HIDDEN);
    }
    rebuild(g_query);
}
static void clear_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) on_query(""); }
static void bar_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) kbinput_open("Search", g_query, on_query);
}

void search_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* back */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, 80, 28);          /* round-safe header position (x18 was in the clipped corner) */
    lv_obj_set_size(back, 44, 40);
    lv_obj_set_ext_click_area(back, 8);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1C1C1E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bi = lv_label_create(back);
    lv_label_set_text(bi, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bi, lv_color_hex(0xC7C7CC), 0);
    lv_obj_align(bi, LV_ALIGN_CENTER, 0, -4);

    /* search bar - tap to open the keyboard modal */
    lv_obj_t *bar = lv_button_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 132, 26);          /* shifted right of the relocated back button */
    lv_obj_set_size(bar, 176, 38);
    lv_obj_set_style_radius(bar, 10, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_CLICKED, NULL);
    g_bar_lbl = lv_label_create(bar);
    lv_label_set_text(g_bar_lbl, "Search songs, artists, albums");
    lv_label_set_long_mode(g_bar_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_bar_lbl, 126);
    lv_obj_set_style_text_font(g_bar_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_bar_lbl, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(g_bar_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    /* clear (X) - sits at the bar's right edge, only visible with a query */
    g_clear = lv_button_create(root);
    lv_obj_remove_style_all(g_clear);
    lv_obj_set_pos(g_clear, 274, 26);      /* right edge of the (now narrower) bar */
    lv_obj_set_size(g_clear, 34, 38);
    lv_obj_set_ext_click_area(g_clear, 8);
    lv_obj_set_style_radius(g_clear, 10, 0);
    lv_obj_set_style_bg_color(g_clear, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(g_clear, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(g_clear, clear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ci = lv_label_create(g_clear);
    lv_label_set_text(ci, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(ci, lv_color_hex(0xC7C7CC), 0);
    lv_obj_center(ci);
    lv_obj_add_flag(g_clear, LV_OBJ_FLAG_HIDDEN);

    /* results - now own the whole lower screen */
    g_results = lv_obj_create(root);
    lv_obj_remove_style_all(g_results);
    lv_obj_set_pos(g_results, 32, 74);
    lv_obj_set_size(g_results, 300, 268);
    lv_obj_set_style_bg_opa(g_results, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_results, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_results, 6, 0);
    lv_obj_set_style_pad_bottom(g_results, 44, 0);   /* last rows scroll clear of the round bottom bezel */
    lv_obj_set_scroll_dir(g_results, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_results, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_results, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    rebuild("");
}

lv_obj_t *search_scroller(void){ return g_results; }

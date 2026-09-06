/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "wallpaper.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Wallpaper picker (SCR_WALLPICK). Opened from Settings -> Display -> Wallpaper.
 *
 * Deliberately a plain list of names, not a grid of thumbnails: a thumbnail grid would
 * mean decoding every picture on the card to show the screen, which is the one thing
 * this feature is built to avoid. Conversion happens once, on a worker, for the ONE
 * picture actually chosen - so opening this screen costs a readdir and nothing else. */

#define MAX_WP 64

static lv_obj_t *g_root, *g_list;
static char g_names[MAX_WP][128];   /* exact bytes - this is what opens the file */
static char g_disp [MAX_WP][128];   /* ASCII-folded copy - this is what we draw */
static int  g_n;

static void pick_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if(idx < 0){                       /* the "None" row */
        wallpaper_select("");
        ui_toast("Wallpaper off");
    } else if(idx < g_n){
        wallpaper_select(g_names[idx]);
        /* If the user picks a picture while the feature is off, turning it on is
         * obviously what they meant - a chosen wallpaper that does not appear reads as
         * broken. Only ever nudges OFF -> IDLE; it never overrides ALWAYS. */
        if(wallpaper_mode() == WP_OFF) wallpaper_set_mode(WP_IDLE);
        ui_toast("Preparing wallpaper...");
    }
    screen_back();
}

static void row(const char *label, const char *sub, int idx, int selected)
{
    lv_obj_t *b = lv_button_create(g_list);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, 250);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, th_card(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, th_card_press(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(b, 12, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_width(l, 226);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, th_font(16), 0);
    lv_obj_set_style_text_color(l, selected ? ui_current_accent() : th_text(), 0);

    if(sub){
        lv_obj_t *s = lv_label_create(b);
        lv_label_set_text(s, sub);
        lv_obj_set_style_text_font(s, th_font(12), 0);
        lv_obj_set_style_text_color(s, th_text3(), 0);
    }
}

static void note(const char *text)
{
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, 244);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, th_font(14), 0);
    lv_obj_set_style_text_color(l, th_text3(), 0);
}

void wallpick_open(void)
{
    if(!g_root) return;
    if(g_list) lv_obj_del(g_list);

    g_list = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 55, 72);
    lv_obj_set_size(g_list, 250, 240);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_bottom(g_list, 30, 0);
    lv_obj_set_style_pad_row(g_list, 10, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    g_n = wallpaper_list(g_names, g_disp, MAX_WP);
    const char *cur = wallpaper_selected();

    row("None", "Album art only", -1, !cur || !cur[0]);
    for(int i = 0; i < g_n; i++)
        row(g_disp[i], NULL, i, cur && !strcmp(cur, g_names[i]));

    if(g_n == 0)
        note("No images found. Put .jpg or .png files in a Wallpapers folder at the top "
             "of the SD card, next to Music, then open this again.");

    screen_show(SCR_WALLPICK);
}

void wallpick_create(lv_obj_t *root)
{
    g_root = root;
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Wallpaper");
}

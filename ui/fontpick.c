/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>   /* readlink/execv/sync: the UI re-exec that applies theme + font */

/* Font picker: choose the UI typeface.
 *
 * "Built-in" is Montserrat, compiled in as glyph arrays. Anything else is a .ttf or
 * .otf the user dropped on the SD card - in a Fonts folder, or loose in the root -
 * rendered at runtime by LVGL's tiny_ttf. Every label in the UI goes through
 * th_font(), so one choice re-faces the whole interface.
 *
 * Two honest limitations, stated on screen rather than discovered later:
 *  - The icon glyphs (play, wifi, battery...) are a separate embedded icon font and
 *    are NOT replaced; a custom face changes text only.
 *  - A face with no CJK coverage will show boxes for CJK titles, because the
 *    built-in Source Han Sans fallback only chains behind the built-in face.
 *
 * Selecting a font restarts the UI, for the same reason the theme does: faces are
 * resolved once when each screen is built. */

#define MAX_FONTS 24
static lv_obj_t *g_root, *g_list;
static char g_names[MAX_FONTS][64];
static int  g_n;

static void restart_now(lv_timer_t *t)
{
    if(t) lv_timer_del(t);
    ui_restart_self(1);
    ui_toast("Couldn't restart the UI");
}

/* ---- installing a font runs OFF the LVGL thread -----------------------------
 * Adopting a font copies it from the SD card to internal storage, and a CJK face can
 * be several megabytes off a slow card. bt.c documents what happens if you do that
 * kind of work inline here: "running that inline froze the LVGL loop for seconds (H5)
 * and risked the fiio_init hardware watchdog". So the copy runs on a detached worker
 * that touches no LVGL state, and a timer on the main thread picks up the result.
 *
 * The choice is persisted only once the face is actually on internal storage, so a
 * failed copy can never leave a saved font that silently renders as the built-in one. */
static _Atomic int g_inst_busy;      /* 1 while a worker is in flight */
static _Atomic int g_inst_done;      /* worker finished */
static _Atomic int g_inst_ok;        /* ...and whether it worked */
static char        g_inst_name[64];  /* written by main before the worker starts; worker reads only */
static lv_timer_t *g_inst_poll;

static void *install_worker(void *arg)
{
    (void)arg;
    int ok = theme_font_install(g_inst_name);
    atomic_store(&g_inst_ok, ok);
    atomic_store(&g_inst_done, 1);
    return NULL;
}

static void install_poll_cb(lv_timer_t *t)
{
    if(!atomic_load(&g_inst_done)) return;
    lv_timer_del(t);
    g_inst_poll = NULL;
    atomic_store(&g_inst_busy, 0);
    if(!atomic_load(&g_inst_ok)){
        ui_toast("Couldn't load that font");
        return;                                   /* nothing persisted, nothing changed */
    }
    cfg_set_str("font_file", g_inst_name);
    ui_toast("Applying font\xE2\x80\xA6");
    lv_timer_t *r = lv_timer_create(restart_now, 900, NULL);
    lv_timer_set_repeat_count(r, 1);
}

static void install_start(const char *name)
{
    int expected = 0;
    if(!atomic_compare_exchange_strong(&g_inst_busy, &expected, 1)) return;   /* one at a time */
    snprintf(g_inst_name, sizeof g_inst_name, "%s", name);
    atomic_store(&g_inst_done, 0);
    atomic_store(&g_inst_ok, 0);

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64*1024);
    int rc = pthread_create(&th, &at, install_worker, NULL);
    pthread_attr_destroy(&at);
    if(rc != 0){ atomic_store(&g_inst_busy, 0); ui_toast("Couldn't load that font"); return; }

    ui_toast("Installing font\xE2\x80\xA6");
    if(!g_inst_poll) g_inst_poll = lv_timer_create(install_poll_cb, 120, NULL);
}

static void pick_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const char *want = (i < 0) ? "Built-in" : g_names[i];
    /* "Already active" has to mean the face is REALLY installed, not merely named in
     * the config: a configured font with no cache behind it (upgraded from a build
     * without one, or a cache lost to a factory reset) renders as built-in at every
     * cold boot, and a name-only check would make its own row a no-op - leaving no
     * way to repair it from the picker at all. */
    if(!strcmp(want, theme_font_name()) && theme_font_is_installed(want)){ screen_back(); return; }
    install_start(want);
}

static lv_obj_t *row(const char *label, const char *sub, int idx, int selected)
{
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 276, sub ? 54 : 46);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_bg_color(r, th_card(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(r, th_card_press(), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, 14, sub ? 8 : 13);
    lv_obj_set_size(l, 216, 21);
    lv_obj_set_style_text_font(l, th_font(16), 0);
    lv_obj_set_style_text_color(l, th_text(), 0);

    if(sub){
        lv_obj_t *sl = lv_label_create(r);
        lv_label_set_text(sl, sub);
        lv_obj_set_pos(sl, 14, 30);
        lv_obj_set_size(sl, 216, 17);
        lv_obj_set_style_text_font(sl, th_font(12), 0);
        lv_obj_set_style_text_color(sl, th_text3(), 0);
    }
    if(selected){
        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, LV_SYMBOL_OK);
        lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_color(ck, ui_current_accent(), 0);
        lv_obj_set_style_text_font(ck, th_font(16), 0);
    }
    return r;
}

static void note(const char *text)
{
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 264);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, th_font(12), 0);
    lv_obj_set_style_text_color(l, th_text3(), 0);
}

void fontpick_open(void)
{
    if(!g_root) return;
    lv_obj_clean(g_root);
    lv_obj_set_style_bg_color(g_root, th_bg(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    ui_header(g_root, "Font");

    g_list = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 42, 70);
    lv_obj_set_size(g_list, 276, 250);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_bottom(g_list, 24, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    const char *cur = theme_font_name();
    row("Built-in", "Montserrat", -1, !strcmp(cur, "Built-in"));

    g_n = theme_font_list(g_names, MAX_FONTS);
    for(int i = 0; i < g_n; i++)
        row(g_names[i], NULL, i, !strcmp(cur, g_names[i]));

    if(g_n == 0)
        note("No fonts found.\n\nPut .ttf or .otf files in a folder called Fonts on the SD card, then come back here.");
    else
        note("Custom fonts change text only - icons keep their built-in glyphs, and a face without CJK coverage will show boxes for CJK titles.");

    screen_show(SCR_FONTPICK);
}

void fontpick_create(lv_obj_t *root){ g_root = root; }

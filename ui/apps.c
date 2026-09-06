/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "anim.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "txtfold.h"

/* The Home MENU (swipe left from Home) - the "proper accessible home screen menu
 * rather than putting everything in the settings page" the beta review asked for.
 *
 * It is a scrolling 2-wide tile grid holding the places you actually go: the
 * library and search, the things that used to be buried several rows down a
 * thirty-row settings scroll (Working Mode, Equalizer, Rescan), the built-in apps,
 * any homebrew app, and Settings itself.
 *
 * Homebrew apps come from /usr/data/apps/<name>/; each may carry an app.conf
 * ("name=...", "exec=..."), else it defaults to the directory name + .../app.
 * Tapping one asks main.c to fork/exec it (the app owns fb0 + touch while it
 * runs). Built-in destinations are plain screen_show()s. */

#define APPS_DIR "/usr/data/apps"
#define MAX_APPS 24

LV_FONT_DECLARE(font_icons_28)          /* FontAwesome 28px: play-mode + app-tile glyphs */
LV_FONT_DECLARE(font_weather16)         /* FontAwesome weather glyphs */
#define LFM_ICON "\xEF\x88\x82"         /* f202 lastfm    -> Last.fm */
#define WEATHER_SUN "\xEF\x86\x85"      /* f185 sun */
#define WEATHER_CLOUD "\xEF\x83\x82"    /* f0c2 cloud */
/* Settings/File reuse LV_SYMBOL_SETTINGS (f013) / LV_SYMBOL_FILE (f15b), now in this font */

typedef struct { char name[64]; char exec[256]; } app_t;
static app_t g_apps[MAX_APPS];
static int g_napps;
static lv_obj_t *g_list;

static void scan_apps(void){
    g_napps = 0;
    DIR *d = opendir(APPS_DIR);
    if(!d) return;
    struct dirent *de;
    while((de = readdir(d)) && g_napps < MAX_APPS){
        if(de->d_name[0] == '.') continue;
        app_t *a = &g_apps[g_napps];
        snprintf(a->name, sizeof a->name, "%.63s", de->d_name);
        snprintf(a->exec, sizeof a->exec, APPS_DIR "/%.220s/app", de->d_name);
        char conf[320]; snprintf(conf, sizeof conf, APPS_DIR "/%.280s/app.conf", de->d_name);
        FILE *f = fopen(conf, "r");
        if(f){
            char line[320];
            while(fgets(line, sizeof line, f)){
                char *nl = strpbrk(line, "\r\n"); if(nl) *nl = 0;   /* strip CRLF too (Windows-edited app.conf) */
                if(!strncmp(line, "name=", 5)) snprintf(a->name, sizeof a->name, "%.63s", line+5);
                else if(!strncmp(line, "exec=", 5)) snprintf(a->exec, sizeof a->exec, "%.255s", line+5);
            }
            fclose(f);
        }
        txt_fold_ascii(a->name);   /* app.conf is user-authored; exec stays byte-exact */
        g_napps++;
    }
    closedir(d);
}

static void app_row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    if(i>=0 && i<g_napps) app_launch(g_apps[i].exec);
}
static void lastfm_row_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) lastfm_open(); }
static void settings_row_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_show(SCR_SETTINGS); }
static void nav_row_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_show((int)(uintptr_t)lv_event_get_user_data(e));
}
static void weather_row_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) weather_app_open(); }
static void mode_row_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) modes_open(); }
static void scan_row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    ui_invalidate_play_scope();
    scanview_open();
}

/* one app tile in the 2-wide grid (g_list is ROW_WRAP): a big glyph over a small caption */
static void make_tile(const char *icon, const lv_font_t *ifont, const char *name, lv_event_cb_t cb, void *ud){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 128, 108);
    lv_obj_set_style_radius(r, 18, 0);
    lv_obj_set_style_bg_color(r, th_card(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, th_card_press(), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(r, 4);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    if(!strcmp(name, "Weather")){
        /* The weather font has the two shapes needed for a cloud-and-sun mark, but
         * not at the regular 28 px app-icon size. Layer the existing glyphs. */
        lv_obj_t *sun = lv_label_create(r);
        lv_label_set_text(sun, WEATHER_SUN);
        lv_obj_set_style_text_font(sun, &font_weather16, 0);
        lv_obj_set_style_text_color(sun, th_text(), 0);
        lv_obj_set_pos(sun, 43, 22);
        lv_obj_t *cloud = lv_label_create(r);
        lv_label_set_text(cloud, WEATHER_CLOUD);
        lv_obj_set_style_text_font(cloud, &font_weather16, 0);
        lv_obj_set_style_text_color(cloud, th_text(), 0);
        lv_obj_set_pos(cloud, 56, 31);
    } else {
        lv_obj_t *ic = lv_label_create(r);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_font(ic, ifont, 0);
        lv_obj_set_style_text_color(ic, th_text(), 0);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 22);
    }
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_font(l, th_font(14), 0);
    lv_obj_set_style_text_color(l, th_text2(), 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void apps_reload(void){
    if(!g_list) return;
    lv_obj_clean(g_list);
    scan_apps();

    /* Order is by how often you reach for it, not by category: browsing first, then
     * the controls that were buried in Settings, then apps, then Settings itself.
     * Symbol glyphs render from the built-in font (icons live in Montserrat's
     * private-use range, which a custom UI font does not carry - theme.c chains the
     * built-in face behind a custom one so these keep working either way). */
    make_tile(LV_SYMBOL_DIRECTORY, th_font(28), "Library",  nav_row_cb, (void*)(uintptr_t)SCR_LIBRARY);
    make_tile(LV_SYMBOL_LIST,      th_font(28), "Search",   nav_row_cb, (void*)(uintptr_t)SCR_SEARCH);
    make_tile(LV_SYMBOL_AUDIO,     th_font(28), "Playing",  nav_row_cb, (void*)(uintptr_t)SCR_NOWPLAYING);
    /* Working Mode + Equalizer + Rescan were all several screens deep in Settings. */
    make_tile(LV_SYMBOL_USB,       th_font(28), "Mode",     mode_row_cb, NULL);
    make_tile(LV_SYMBOL_SETTINGS,  th_font(28), "EQ",       nav_row_cb, (void*)(uintptr_t)SCR_EQ);
    make_tile(LV_SYMBOL_REFRESH,   th_font(28), "Scan",     scan_row_cb, NULL);
    make_tile(LV_SYMBOL_DOWNLOAD,  th_font(28), "Receive",  nav_row_cb, (void*)(uintptr_t)SCR_RECEIVE);
    make_tile(WEATHER_CLOUD,       &font_weather16, "Weather",  weather_row_cb, NULL);
    /* built-in: Last.fm scrobbling (the FA lastfm brand glyph) */
    make_tile(LFM_ICON, &font_icons_28, "Last.fm", lastfm_row_cb, NULL);
    /* homebrew apps from /usr/data/apps */
    for(int i=0;i<g_napps;i++) make_tile(LV_SYMBOL_FILE, &font_icons_28, g_apps[i].name, app_row_cb, (void*)(uintptr_t)i);
    /* built-in: Settings */
    make_tile(LV_SYMBOL_SETTINGS, &font_icons_28, "Settings", settings_row_cb, NULL);
}

void apps_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Menu");   /* shared standard header */

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    /* Viewport height is EXACTLY two tile rows (108 + 10 + 108 = 226), and it stops at
     * y=296. Both numbers are load-bearing on a round screen:
     *
     *  - 252px tall was 2.13 rows, so a scroll almost always came to rest with the top
     *    row sliced in half and the bottom row sliced again - the "buttons are cut off"
     *    report. Snapping to a whole row (below) only works if the viewport IS whole rows.
     *  - the old bottom edge (74+252 = y=326) has just 210px of chord on the 360px
     *    circle, but a row of tiles is 266px wide, so the bottom row was ALSO being
     *    clipped by the bezel, not merely by the container. y=296 is the lowest line
     *    where the chord (275px) still clears a full row. */
    lv_obj_set_pos(g_list, 44, 70); lv_obj_set_size(g_list, 272, 226);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 10, 0);
    lv_obj_set_style_pad_column(g_list, 10, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_ROW_WRAP);   /* 2-wide tile grid (128px tiles + 10 gap) */
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(g_list, LV_SCROLL_SNAP_START);  /* always rest on a row boundary */
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);

    apps_reload();
}

lv_obj_t *apps_scroller(void){ return g_list; }

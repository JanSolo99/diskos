/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "config.h"
#include "musicdb.h"
#include <stdint.h>
#include <stdio.h>

/* Custom graphic EQ: 10 bands (32 Hz..16 kHz) + master, -12..+12 dB, matching the stock
 * player's PEQ. Values persist to cfg (eq_b0..9, eq_master) AND apply live to the audio
 * engine: on each change we write the curve into the player's PEQ slot (STYLE_PRESET 11 =
 * "User 1") in stock format and select it with 0689 (ui_apply_eq). Format captured live
 * from the stock UI - see RE_CATALOGUE §3. The 10 bands scroll horizontally (round screen
 * can't show them all at once), like the stock editor. */

void ui_apply_eq(int preset);            /* main.c: 0689 select */
#define ACC 0xFF375F
#define NBAND 10
#define EQ_CUSTOM_PRESET 11              /* User 1 slot (RE: User 1-10 = STYLE_PRESET 11-20) */

static const int         FREQHZ[NBAND] = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
static const char *const FREQLBL[NBAND]= { "32","64","125","250","500","1k","2k","4k","8k","16k" };
static const char *const KEY[NBAND]    = { "eq_b0","eq_b1","eq_b2","eq_b3","eq_b4",
                                           "eq_b5","eq_b6","eq_b7","eq_b8","eq_b9" };
#define MKEY "eq_master"

static lv_obj_t *g_band[NBAND];          /* band sliders */
static lv_obj_t *g_bval[NBAND];          /* band value labels */
static lv_obj_t *g_master;               /* master gain slider */
static lv_obj_t *g_mval;                 /* master value label */


/* build the stock PARAMS_JSON from the current band values + write+select the PEQ slot.
 * Returns 1 if the curve actually reached the player (DB write + select), 0 otherwise. */
static int apply_now(void){
    char json[1100]; int n = 0;
    n += snprintf(json+n, sizeof json-n, "[");
    for(int i=0;i<NBAND;i++){
        if(n < 0 || n > (int)sizeof json - 90) return 0;   /* truncation guard (never hit at 10 bands) */
        int g = g_band[i] ? lv_slider_get_value(g_band[i]) : 0;
        n += snprintf(json+n, sizeof json-n,
            "%s{\"filterType\":0,\"frequency\":%d,\"position\":%d,\"gain\":\"%d.0\",\"qValue\":\"0.7\"}",
            i?",":"", FREQHZ[i], i, g);
    }
    if(n < 0 || n > (int)sizeof json - 2) return 0;
    n += snprintf(json+n, sizeof json-n, "]");
    int master = g_master ? lv_slider_get_value(g_master) : 0;
    if(mdb_set_peq(EQ_CUSTOM_PRESET, (double)master, json)){
        ui_apply_eq(EQ_CUSTOM_PRESET);   /* select -> player reloads bands -> coeffs -> cascade */
        cfg_set_int("eq_preset", EQ_CUSTOM_PRESET);  /* so Settings/Tune show "Custom", not a stale preset */
        return 1;
    }
    return 0;
}

/* live preview of the value label (no persist/apply on every drag tick) */
static void band_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_obj_t *lbl = (i==NBAND) ? g_mval : (i>=0 && i<NBAND ? g_bval[i] : NULL);
    if(lbl){ char b[8]; snprintf(b,sizeof b,"%+d", v); lv_label_set_text(lbl, v?b:"0"); }
}
/* persist this control + apply the whole curve when the finger lifts (or slides off) */
static void band_release_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    /* apply first; persist the value only once the curve actually reached the player, so a PEQ
     * write/select failure can't leave cfg/UI showing a band the audio engine never received */
    if(apply_now()) cfg_set_int((i==NBAND) ? MKEY : KEY[i], v);
    else ui_toast("Couldn't apply EQ");
}

static void flat_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    /* LV_ANIM_OFF so the slider VALUES are actually 0 when apply_now() reads them below - with
     * LV_ANIM_ON the sliders are still mid-animation and apply_now() would send the old/intermediate
     * curve to the player while cfg + labels already said 0. */
    for(int i=0;i<NBAND;i++){
        lv_slider_set_value(g_band[i], 0, LV_ANIM_OFF);
        if(g_bval[i]) lv_label_set_text(g_bval[i], "0");
    }
    lv_slider_set_value(g_master, 0, LV_ANIM_OFF);
    if(g_mval) lv_label_set_text(g_mval, "0");
    /* apply FIRST; persist the flat curve only if it actually reached the player (mirrors band_release_cb) */
    if(apply_now()){
        for(int i=0;i<NBAND;i++) cfg_set_int_deferred(KEY[i], 0);   /* batch: one flush, not 11 fsyncs */
        cfg_set_int_deferred(MKEY, 0);
        cfg_flush();
    } else ui_toast("Couldn't apply EQ");
}

/* one EQ column: value label on top, vertical slider, freq label below. idx==NBAND = master.
 * Built inside the horizontal scroller. */
static void make_col(lv_obj_t *parent, int idx, const char *flabel, const char *cfgkey,
                     lv_obj_t **slot_slider, lv_obj_t **slot_val){
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 42, 206);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(col);
    lv_obj_set_pos(val, 0, 0); lv_obj_set_width(val, 42);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(ACC), 0);

    lv_obj_t *sl = lv_slider_create(col);
    lv_obj_set_size(sl, 16, 150);
    lv_obj_set_ext_click_area(sl, 8);
    lv_obj_set_pos(sl, 13, 22);
    lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);   /* fill from 0 dB centre */
    lv_slider_set_range(sl, -12, 12);
    int cv = cfg_get_int(cfgkey, 0);
    lv_slider_set_value(sl, cv, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(ACC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, band_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(sl, band_release_cb, LV_EVENT_RELEASED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(sl, band_release_cb, LV_EVENT_PRESS_LOST, (void*)(intptr_t)idx);

    { char b[8]; snprintf(b,sizeof b,"%+d", cv); lv_label_set_text(val, cv?b:"0"); }

    lv_obj_t *fl = lv_label_create(col);
    lv_obj_set_pos(fl, 0, 182); lv_obj_set_width(fl, 42);
    lv_label_set_text(fl, flabel);
    lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fl, lv_color_hex(idx==NBAND ? 0xC7C7CC : 0x8E8E93), 0);

    *slot_slider = sl;
    *slot_val = val;
}

void eqcustom_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Custom EQ");   /* shared standard header */

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "Swipe sideways for more bands");
    lv_obj_set_pos(hint, 20, 58); lv_obj_set_width(hint, 320);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8E8E93), 0);

    /* horizontally-scrollable row of EQ columns (master + 10 bands) */
    lv_obj_t *scr = lv_obj_create(root);
    lv_obj_remove_style_all(scr);
    lv_obj_set_pos(scr, 0, 80);
    lv_obj_set_size(scr, 360, 212);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(scr, LV_DIR_HOR);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_left(scr, 20, 0);
    lv_obj_set_style_pad_right(scr, 20, 0);
    lv_obj_set_style_pad_column(scr, 4, 0);

    make_col(scr, NBAND, "MSTR", MKEY, &g_master, &g_mval);    /* master first */
    for(int i=0;i<NBAND;i++)
        make_col(scr, i, FREQLBL[i], KEY[i], &g_band[i], &g_bval[i]);

    lv_obj_t *flat = lv_button_create(root);
    lv_obj_remove_style_all(flat);
    lv_obj_set_size(flat, 120, 34); lv_obj_align(flat, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_set_style_radius(flat, 17, 0);
    lv_obj_set_style_bg_color(flat, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(flat, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(flat, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_add_event_cb(flat, flat_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fll=lv_label_create(flat); lv_label_set_text(fll, "Flat");
    lv_obj_set_style_text_font(fll, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fll, lv_color_hex(0xFFFFFF), 0); lv_obj_center(fll);
}

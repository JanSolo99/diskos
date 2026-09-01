/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "ipc.h"
#include "config.h"
#include <stdint.h>

/* Quick Settings: an iOS-Control-Center-style pull-down panel (top-edge swipe-down; gesture handled
 * in main.c). Grouped cards on the round 360 screen: a knobless brightness capsule, a transport card,
 * and three labeled shortcut tiles (Wi-Fi / Bluetooth / Sleep) whose state circle turns blue when on.
 * One accent (blue) for state; white glyphs elsewhere. Dismissed by the back-swipe (screen_back). */

LV_FONT_DECLARE(font_icons_28)          /* FontAwesome 28px */
#define WI_SUN "\xEF\x86\x85"           /* f185 sun */

/* "on" stays iOS system blue in both palettes - it is a STATE colour, not a
 * surface, and blue reads as active on white as well as on black. Everything
 * else comes from the theme so the panel follows light/dark with the rest. */
#define QS_ON     0x0A84FF              /* iOS system blue = "on" */
#define qs_card()  th_card()
#define qs_press() th_card_press()
#define qs_track() th_card_press()
#define qs_off()   th_fill3()
#define qs_grab()  th_text3()
#define qs_txt2()  th_text3()

static lv_obj_t *g_bright;
static lv_obj_t *g_pp_glyph;
static lv_obj_t *g_wifi_dot, *g_bt_dot;   /* shortcut state circles, recolored on refresh */

static void bright_change_cb(lv_event_t *e){ ui_backlight(lv_slider_get_value(lv_event_get_target(e))); }
static void bright_release_cb(lv_event_t *e){ ui_set_brightness(lv_slider_get_value(lv_event_get_target(e))); }
static void cmd_cb(lv_event_t *e){ const char *c=(const char*)lv_event_get_user_data(e); if(c) ipc_send_cmd(c); }
static void sleep_cb(lv_event_t *e){ (void)e; ui_request_sleep(); screen_show(SCR_SAVER); }

/* Wi-Fi / Bluetooth tiles behave like iOS Control Center: a short press toggles the radio,
 * a long press opens that radio's full settings screen. The state circle recolours at once
 * to the new intent (blue=on / grey=off); the bring-up itself is async. */
static void wifi_short_cb(lv_event_t *e){ (void)e;
    int on = wifi_toggle();
    if(g_wifi_dot) lv_obj_set_style_bg_color(g_wifi_dot, (on ? lv_color_hex(QS_ON) : qs_off()), 0);
    ui_toast(on ? "Turning on Wi-Fi\xE2\x80\xA6" : "Wi-Fi off");
}
static void wifi_long_cb(lv_event_t *e){ (void)e; wifi_open(); }
static void bt_short_cb(lv_event_t *e){ (void)e;
    int on = bt_toggle();
    if(g_bt_dot) lv_obj_set_style_bg_color(g_bt_dot, (on ? lv_color_hex(QS_ON) : qs_off()), 0);
    ui_toast(on ? "Turning on Bluetooth\xE2\x80\xA6" : "Bluetooth off");
}
static void bt_long_cb(lv_event_t *e){ (void)e; bt_open(); }

/* A transport glyph button seated INSIDE the card (parent = card). Returns the glyph label. */
static lv_obj_t *tp_btn(lv_obj_t *card, const char *sym, const lv_font_t *font, int x, int sz, void *cmd){
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, sz, sz);
    lv_obj_align(b, LV_ALIGN_CENTER, x, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, qs_off(), LV_STATE_PRESSED);   /* circular press flash */
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, cmd_cb, LV_EVENT_CLICKED, cmd);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, th_text(), 0);
    lv_obj_center(l);
    return l;
}

/* A labeled shortcut tile: clickable tile + a state circle (glyph inside) + a caption. The decorative
 * children drop CLICKABLE so taps reach the tile. Returns the state circle (recolored on refresh). */
static lv_obj_t *shortcut_tile(lv_obj_t *root, int x, const char *glyph, const char *label,
                               lv_event_cb_t short_cb, lv_event_cb_t long_cb, lv_color_t dot_color){
    lv_obj_t *tile = lv_button_create(root);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 84, 76);
    lv_obj_align(tile, LV_ALIGN_TOP_MID, x, 232);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_bg_color(tile, qs_card(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, qs_press(), LV_STATE_PRESSED);
    /* SHORT_CLICKED (not CLICKED) so a long press fires ONLY long_cb, never the toggle too */
    lv_obj_add_event_cb(tile, short_cb, LV_EVENT_SHORT_CLICKED, NULL);
    if(long_cb) lv_obj_add_event_cb(tile, long_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_remove_style_all(dot);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 40, 40);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, dot_color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_t *g = lv_label_create(dot);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_font(g, th_font(20), 0);
    lv_obj_set_style_text_color(g, th_text(), 0);
    lv_obj_center(g);

    lv_obj_t *lb = lv_label_create(tile);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, th_font(14), 0);
    lv_obj_set_style_text_color(lb, qs_txt2(), 0);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -8);
    return dot;
}

void quicksettings_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* grabber */
    lv_obj_t *grab = lv_obj_create(root);
    lv_obj_remove_style_all(grab);
    lv_obj_clear_flag(grab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grab, 36, 5);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_color(grab, qs_grab(), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);

    /* brightness capsule - knobless: the white fill IS the control */
    g_bright = lv_slider_create(root);
    lv_obj_set_size(g_bright, 248, 48);
    lv_obj_set_ext_click_area(g_bright, 6);
    lv_obj_align(g_bright, LV_ALIGN_TOP_MID, 0, 64);
    lv_slider_set_range(g_bright, 4, 40);   /* match Settings' brightness range */
    lv_slider_set_value(g_bright, ui_get_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bright, qs_track(), LV_PART_MAIN);
    lv_obj_set_style_radius(g_bright, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bright, th_text(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bright, 24, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_bright, LV_OPA_TRANSP, LV_PART_KNOB);   /* invisible knob, still draggable */
    lv_obj_set_style_pad_all(g_bright, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(g_bright, bright_change_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_bright, bright_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_bright, bright_release_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_t *sun = lv_label_create(g_bright);   /* dark sun glyph sitting on the white fill (iOS) */
    lv_obj_clear_flag(sun, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(sun, WI_SUN);
    lv_obj_set_style_text_font(sun, &font_icons_28, 0);
    lv_obj_set_style_text_color(sun, qs_off(), 0);
    lv_obj_align(sun, LV_ALIGN_LEFT_MID, 14, 0);

    /* transport card */
    lv_obj_t *tcard = lv_obj_create(root);
    lv_obj_remove_style_all(tcard);
    lv_obj_clear_flag(tcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tcard, 280, 88);
    lv_obj_align(tcard, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_radius(tcard, 28, 0);
    lv_obj_set_style_bg_color(tcard, qs_card(), 0);
    lv_obj_set_style_bg_opa(tcard, LV_OPA_COVER, 0);
    tp_btn(tcard, LV_SYMBOL_PREV, th_font(24), -86, 56, (void *)"0201000C0002");
    g_pp_glyph = tp_btn(tcard, LV_SYMBOL_PAUSE, th_font(28), 0, 64, (void *)"0201000C0000");
    tp_btn(tcard, LV_SYMBOL_NEXT, th_font(24), 86, 56, (void *)"0201000C0001");

    /* labeled shortcut tiles (state circle blue when on). EYE_CLOSE = honest sleep-to-saver glyph. */
    g_wifi_dot = shortcut_tile(root, -94, LV_SYMBOL_WIFI,      "Wi-Fi",     wifi_short_cb, wifi_long_cb,
                               cfg_get_int("wifi_on", 1) ? lv_color_hex(QS_ON) : qs_off());
    g_bt_dot   = shortcut_tile(root,    0, LV_SYMBOL_BLUETOOTH, "Bluetooth", bt_short_cb,   bt_long_cb,
                               cfg_get_int("bt_on", 0) ? lv_color_hex(QS_ON) : qs_off());
    shortcut_tile(root,  94, LV_SYMBOL_EYE_CLOSE, "Sleep", sleep_cb, NULL, qs_off());
}

/* called from main.c on state change + when the panel opens */
void quicksettings_refresh(int playing)
{
    if(g_pp_glyph) lv_label_set_text(g_pp_glyph, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    /* don't fight an active brightness drag (refresh fires on every state update) */
    if(g_bright && !lv_obj_has_state(g_bright, LV_STATE_PRESSED))
        lv_slider_set_value(g_bright, ui_get_brightness(), LV_ANIM_OFF);
    if(g_wifi_dot) lv_obj_set_style_bg_color(g_wifi_dot, (cfg_get_int("wifi_on", 1) ? lv_color_hex(QS_ON) : qs_off()), 0);
    if(g_bt_dot)   lv_obj_set_style_bg_color(g_bt_dot,   (cfg_get_int("bt_on", 0) ? lv_color_hex(QS_ON) : qs_off()), 0);
}

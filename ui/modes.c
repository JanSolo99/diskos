/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include <stdint.h>
#include <stdio.h>

/* Working Mode (audio source) picker - mirrors stock's "Working mode" list. Tapping a mode replays
 * the captured V2.28 switch sequence via ui_set_source_mode() and marks it selected.
 * 0=Local 1=USB-DAC 2=BT-Receiving 3=USB-Storage. */

typedef struct { const char *name, *sub; } modeinfo_t;
static const modeinfo_t MODES[] = {
    { "Local Playback",      "Play from the microSD card" },
    { "USB DAC",             "Be a USB sound card for a PC" },
    { "Bluetooth Receiving", "Play audio sent from a phone" },
    { "USB Storage",         "Open the card on a computer" },
};
#define N_MODES ((int)(sizeof(MODES)/sizeof(MODES[0])))

static lv_obj_t *g_check[N_MODES];   /* per-row checkmark label */
static lv_obj_t *g_row[N_MODES];     /* per-row button (for the selected highlight) */

static void mark_selected(void){
    /* Show the EFFECTIVE mode, not our own last request: the player can enter USB
     * storage by itself when a computer is plugged in, and a tick beside "Local
     * Playback" while nothing will play is exactly the confusion this screen caused
     * ("won't play even though it's on Local Playback"). */
    int cur = ui_source_mode_effective();
    for(int i=0;i<N_MODES;i++){
        if(g_check[i]){ lv_label_set_text(g_check[i], i==cur ? LV_SYMBOL_OK : "");
                        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0); }  /* track accent changes */
        if(g_row[i]){   /* selected row gets an accent ring + slightly lifted fill */
            lv_obj_set_style_border_width(g_row[i], i==cur ? 2 : 0, 0);
            lv_obj_set_style_border_color(g_row[i], ui_current_accent(), 0);
            lv_obj_set_style_bg_color(g_row[i], lv_color_hex(i==cur ? 0x242426 : 0x1C1C1E), 0);
        }
    }
}


static uint32_t g_last_switch = 0;   /* debounce: a switch takes a few seconds to apply in the player */

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int m = (int)(uintptr_t)lv_event_get_user_data(e);
    /* Serialise: ignore taps while the previous switch is still applying (the player's gadget
     * state-machine is asynchronous). NB we do NOT early-return on "same mode" - re-issuing must
     * always be allowed so Local works as a recover even if our cached mode is stale. */
    if(g_last_switch && lv_tick_elaps(g_last_switch) < 3000){ ui_toast("Switching\xE2\x80\xA6"); return; }
    g_last_switch = lv_tick_get();
    if(ui_set_source_mode(m) == 0){
        mark_selected();
        /* honest wording: the frames are queued; the async switch completes a moment later. */
        static const char *msg[N_MODES] = {
            "Switching to local playback", "Switching to USB DAC",
            "Bluetooth receiving on \xE2\x80\x93 connect your phone",
            "USB storage on \xE2\x80\x93 connect a computer" };
        ui_toast(msg[m]);
    } else {
        ui_toast("Couldn't switch mode");
    }
}

void modes_open(void){ mark_selected(); screen_show(SCR_WORKMODE); }

void modes_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* back button (kept out of the clipped top-left corner) */
    ui_header(root, "Working Mode");   /* shared standard header */

    /* vertical list of mode rows */
    lv_obj_t *col = lv_obj_create(root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 300, 250); lv_obj_set_pos(col, 30, 76);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    for(int i=0;i<N_MODES;i++){
        lv_obj_t *row = lv_button_create(col);
        g_row[i] = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 276, 54);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, th_card(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th_card_press(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, MODES[i].name);
        lv_obj_set_pos(nm, 16, 9);
        lv_obj_set_style_text_font(nm, th_font(16), 0);
        lv_obj_set_style_text_color(nm, th_text(), 0);

        lv_obj_t *sb = lv_label_create(row);
        lv_label_set_text(sb, MODES[i].sub);
        lv_obj_set_pos(sb, 16, 30);
        lv_obj_set_width(sb, 210);                       /* keep clear of the right-side checkmark */
        lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sb, th_font(12), 0);
        lv_obj_set_style_text_color(sb, th_text3(), 0);

        g_check[i] = lv_label_create(row);
        lv_label_set_text(g_check[i], "");
        lv_obj_align(g_check[i], LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0);
        lv_obj_set_style_text_font(g_check[i], th_font(18), 0);
    }
    mark_selected();
}


/* ---- "plugged into a computer" prompt -------------------------------------
 * Shown when the player opens the card as USB storage on its own and the user has
 * left "On USB Connect" on Ask. Three outcomes, all one tap, and no default action
 * is taken behind their back: whatever they pick becomes the mode.
 *
 * Lives on lv_layer_top rather than being a screen, so it survives whatever the
 * user was looking at and does not disturb the nav stack. */
static lv_obj_t *g_usb_dlg;

static void usb_dlg_close(void){
    if(g_usb_dlg){ lv_obj_delete_async(g_usb_dlg); g_usb_dlg = NULL; }
}
static void usb_pick_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    usb_dlg_close();
    if(mode < 0) return;                     /* "Not now": leave the player as it is */
    if(ui_set_source_mode(mode) != 0){ ui_toast("Couldn't switch mode"); return; }
    static const char *msg[4] = { "Charging \xE2\x80\x93 still playing", "USB DAC", NULL,
                                  "Card open on the computer" };
    if(mode >= 0 && mode < 4 && msg[mode]) ui_toast(msg[mode]);
}
static void usb_dlg_btn(lv_obj_t *card, int y, const char *txt, int mode, int primary){
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 232, 40);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, primary ? ui_current_accent() : th_card_press(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(b, usb_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)mode);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, th_font(15), 0);
    lv_obj_set_style_text_color(l, th_text(), 0);
    lv_obj_center(l);
}
void usbprompt_show(void){
    usb_dlg_close();
    g_usb_dlg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_usb_dlg);
    lv_obj_set_size(g_usb_dlg, 360, 360); lv_obj_center(g_usb_dlg);
    lv_obj_set_style_bg_color(g_usb_dlg, th_bg(), 0);
    lv_obj_set_style_bg_opa(g_usb_dlg, LV_OPA_80, 0);
    lv_obj_clear_flag(g_usb_dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_usb_dlg, LV_OBJ_FLAG_CLICKABLE);   /* absorb taps meant for the screen behind */

    lv_obj_t *card = lv_obj_create(g_usb_dlg);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 268, 246); lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, th_card(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Connected to a computer");
    lv_obj_set_width(t, 236);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(t, th_font(16), 0);
    lv_obj_set_style_text_color(t, th_text(), 0);

    usb_dlg_btn(card,  50, "Keep playing",      0, 1);
    usb_dlg_btn(card,  96, "Open the card",     3, 0);
    usb_dlg_btn(card, 142, "Use as USB DAC",    1, 0);
    usb_dlg_btn(card, 188, "Not now",          -1, 0);
}

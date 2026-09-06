/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "receive.h"
#include <stdio.h>
#include <string.h>

/* Receive Music screen (SCR_RECEIVE). Menu -> Receive.
 *
 * The server runs ONLY while this screen is on top. screenmgr starts it on entry and
 * stops it on the way out, so there is never a listener bound when you are not looking
 * at this - which is the whole security model, together with the random path token.
 *
 * The 1 Hz timer is created on entry and DELETED on exit rather than left running and
 * gated: an idle timer still wakes the CPU, and the point of a screen-scoped feature is
 * that it costs nothing when you are not in it. */

static lv_obj_t *g_root, *g_qr, *g_url, *g_stat, *g_hint;
static lv_timer_t *g_tick;
static int g_last_done = -1;

static void fmt_size(long b, char *out, int cap){
    /* tenths, done exactly: remainder*10/MB. The obvious /(1024*105) is not a tenth of
     * a megabyte and drifts by ~2% up the scale. */
    if(b >= 1024*1024) snprintf(out,cap,"%ld.%ld MB", b/(1024*1024), ((b%(1024*1024))*10)/(1024*1024));
    else if(b >= 1024) snprintf(out,cap,"%ld KB", b/1024);
    else               snprintf(out,cap,"%ld B", b);
}

static void refresh(void)
{
    if(!g_stat) return;

    const char *err = receive_last_error();
    int done = receive_files_done();
    char s[160];

    if(receive_active()){
        char sz[32]; fmt_size(receive_active_bytes(), sz, sizeof sz);
        snprintf(s,sizeof s,"Receiving... %s", sz);
    } else if(err[0]){
        snprintf(s,sizeof s,"%s", err);
    } else if(done > 0){
        const char *last = receive_last_name();
        if(last[0]) snprintf(s,sizeof s,"%d file%s - last: %s", done, done==1?"":"s", last);
        else        snprintf(s,sizeof s,"%d file%s received", done, done==1?"":"s");
    } else {
        snprintf(s,sizeof s,"Waiting for a connection");
    }
    lv_label_set_text(g_stat, s);
    lv_obj_set_style_text_color(g_stat, err[0] ? lv_color_hex(0xE05555) : th_text3(), 0);

    if(done != g_last_done){
        g_last_done = done;
        if(done > 0) lv_label_set_text(g_hint, "Leave this screen to scan them in");
    }
}

static void tick_cb(lv_timer_t *t){ (void)t; refresh(); }

/* Called by screenmgr on ENTRY. */
void receive_ui_enter(void)
{
    if(!g_root) return;
    g_last_done = -1;

    if(receive_start() != 0){
        lv_qrcode_update(g_qr, "", 0);
        lv_obj_add_flag(g_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_url, "Wi-Fi is off");
        lv_label_set_text(g_stat, "Connect to Wi-Fi first, then come back.");
        lv_obj_set_style_text_color(g_stat, th_text3(), 0);
        lv_label_set_text(g_hint, "");
        return;
    }

    const char *url = receive_url();
    lv_obj_clear_flag(g_qr, LV_OBJ_FLAG_HIDDEN);
    lv_qrcode_update(g_qr, url, (uint32_t)strlen(url));
    lv_label_set_text(g_url, url);
    lv_label_set_text(g_hint, "Scan this, or type it into a browser");
    refresh();

    if(!g_tick) g_tick = lv_timer_create(tick_cb, 1000, NULL);
}

/* Called by screenmgr on EXIT. Stops the server and drops the timer entirely. */
void receive_ui_leave(void)
{
    if(g_tick){ lv_timer_delete(g_tick); g_tick = NULL; }
    int got = receive_got_files();
    receive_stop();
    /* Offer the scan rather than doing it: someone sending twenty files should not
     * trigger twenty rescans, and a scan the user did not ask for on the way out of a
     * screen is exactly the kind of surprise this UI avoids. */
    if(got) ui_toast("Files received - Menu > Scan to add them");
}

void receive_create(lv_obj_t *root)
{
    g_root = root;
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Receive");

    /* QR at 148px centred: at y=76..224 the chord is comfortably wider than that, so
     * the code is never clipped by the round panel. It needs a white quiet zone to
     * scan, hence the padded white plate rather than drawing straight on the theme. */
    lv_obj_t *plate = lv_obj_create(root);
    lv_obj_remove_style_all(plate);
    lv_obj_set_size(plate, 164, 164);
    lv_obj_set_pos(plate, (360-164)/2, 72);
    lv_obj_set_style_bg_color(plate, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(plate, 10, 0);
    lv_obj_clear_flag(plate, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_qr = lv_qrcode_create(plate);
    lv_qrcode_set_size(g_qr, 148);
    lv_qrcode_set_dark_color(g_qr, lv_color_black());
    lv_qrcode_set_light_color(g_qr, lv_color_white());
    lv_obj_center(g_qr);

    g_url = lv_label_create(root);
    lv_obj_set_width(g_url, 264);
    lv_obj_set_pos(g_url, (360-264)/2, 246);
    lv_obj_set_style_text_align(g_url, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_url, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_url, th_font(14), 0);
    lv_obj_set_style_text_color(g_url, th_text(), 0);
    lv_label_set_text(g_url, "");

    g_stat = lv_label_create(root);
    lv_obj_set_width(g_stat, 250);
    lv_obj_set_pos(g_stat, (360-250)/2, 284);
    lv_obj_set_style_text_align(g_stat, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_stat, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_stat, th_font(14), 0);
    lv_obj_set_style_text_color(g_stat, th_text3(), 0);
    lv_label_set_text(g_stat, "");

    /* y=306 is close to the bottom of the circle - the chord there is ~250px, so this
     * label is 230 wide and centred rather than the 264 used higher up. */
    g_hint = lv_label_create(root);
    lv_obj_set_width(g_hint, 230);
    lv_obj_set_pos(g_hint, (360-230)/2, 306);
    lv_obj_set_style_text_align(g_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_hint, th_font(12), 0);
    lv_obj_set_style_text_color(g_hint, th_text3(), 0);
    lv_label_set_text(g_hint, "");
}

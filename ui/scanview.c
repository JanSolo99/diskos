/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "scanner.h"
#include <stdio.h>
#include <string.h>

/* Library-scan progress screen.
 *
 * "Rescan Library" used to toast "Rescan requested" and then say nothing ever again:
 * the scan ran on its worker thread with no way to tell whether it was working, how
 * far along it was, or when it had finished. On a card with a few thousand tracks
 * that is minutes of silence.
 *
 * The scanner now runs a cheap counting pass before the merge pass (see scanner.c),
 * so there is a real denominator to show. This screen renders it: an arc that fills,
 * a percentage, the running count, and the file being read right now - the last one
 * matters most, because it is the only thing that proves the scan is still moving
 * when a single large file takes a while.
 *
 * Leaving the screen does NOT stop the scan; it runs to completion either way and
 * main.c's scanner_poll reports the outcome. */

static lv_obj_t *g_root, *g_arc, *g_pct, *g_phase, *g_count, *g_file, *g_hint;
static lv_timer_t *g_tick;
static int g_auto_back;   /* 1 = we opened this screen, so pop it when the scan ends */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if(!g_root || screen_current() != SCR_SCAN) return;

    int phase = 0, done = 0, expect = 0;
    char name[80];
    scanner_progress_ex(&phase, &done, &expect, name, sizeof name);
    int active = scanner_active();

    if(!active){
        /* Finished (or never started). Fill the arc so the last frame reads as
         * "complete" rather than freezing at 97%, then step back. main.c's
         * scanner_poll owns the result toast - don't duplicate it here. */
        lv_arc_set_value(g_arc, 100);
        lv_label_set_text(g_pct, "100%");
        lv_label_set_text(g_phase, "Finished");
        lv_label_set_text(g_file, "");
        if(g_auto_back){ g_auto_back = 0; screen_back(); }
        return;
    }

    if(phase == 0 || expect <= 0){
        /* Counting: there is no denominator yet, so don't fake one. An indeterminate
         * label is honest and still shows the scan is alive. */
        lv_arc_set_value(g_arc, 0);
        lv_label_set_text(g_pct, "\xE2\x80\xA6");        /* ellipsis */
        lv_label_set_text(g_phase, "Counting files");
        lv_label_set_text(g_count, "");
        lv_label_set_text(g_file, "");
        return;
    }

    int pct = (int)((long)done * 100 / expect);
    if(pct > 100) pct = 100;                              /* more files than counted: clamp */
    lv_arc_set_value(g_arc, pct);
    char b[32];
    snprintf(b, sizeof b, "%d%%", pct);
    lv_label_set_text(g_pct, b);
    lv_label_set_text(g_phase, "Scanning music");
    snprintf(b, sizeof b, "%d of %d", done, expect);
    lv_label_set_text(g_count, b);
    lv_label_set_text(g_file, name);
}

void scanview_create(lv_obj_t *root)
{
    g_root = root;
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Scanning");

    g_arc = lv_arc_create(root);
    lv_obj_set_size(g_arc, 176, 176);
    lv_obj_align(g_arc, LV_ALIGN_CENTER, 0, -14);
    lv_arc_set_range(g_arc, 0, 100);
    lv_arc_set_value(g_arc, 0);
    lv_arc_set_bg_angles(g_arc, 0, 360);
    lv_arc_set_rotation(g_arc, 270);
    lv_obj_remove_style(g_arc, NULL, LV_PART_KNOB);        /* display only, not draggable */
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc, th_card_press(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc, ui_current_accent(), LV_PART_INDICATOR);

    g_pct = lv_label_create(root);
    lv_label_set_text(g_pct, "\xE2\x80\xA6");
    lv_obj_set_width(g_pct, 360);
    lv_obj_align(g_pct, LV_ALIGN_CENTER, 0, -34);
    lv_obj_set_style_text_align(g_pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_pct, th_font(32), 0);
    lv_obj_set_style_text_color(g_pct, th_text(), 0);

    g_phase = lv_label_create(root);
    lv_label_set_text(g_phase, "Counting files");
    lv_obj_set_width(g_phase, 360);
    lv_obj_align(g_phase, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_style_text_align(g_phase, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_phase, th_font(16), 0);
    lv_obj_set_style_text_color(g_phase, th_text2(), 0);

    g_count = lv_label_create(root);
    lv_label_set_text(g_count, "");
    lv_obj_set_width(g_count, 360);
    lv_obj_align(g_count, LV_ALIGN_CENTER, 0, 26);
    lv_obj_set_style_text_align(g_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_count, th_font(14), 0);
    lv_obj_set_style_text_color(g_count, th_text3(), 0);

    /* The filename uses the CJK-capable face: it is the one label here that shows
     * arbitrary user text, and a tofu row would defeat the point of showing it. */
    g_file = lv_label_create(root);
    lv_label_set_text(g_file, "");
    lv_label_set_long_mode(g_file, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_file, 240);
    lv_obj_align(g_file, LV_ALIGN_CENTER, 0, 76);
    lv_obj_set_style_text_align(g_file, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_file, ui_font_cjk(14), 0);
    lv_obj_set_style_text_color(g_file, th_text3(), 0);

    g_hint = lv_label_create(root);
    lv_label_set_text(g_hint, "You can keep using diskOS");
    lv_obj_set_width(g_hint, 300);
    lv_obj_align(g_hint, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_set_style_text_align(g_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_hint, th_font(12), 0);
    lv_obj_set_style_text_color(g_hint, th_text3(), 0);

    /* One shared 250ms timer, paused unless the screen is up: fast enough that the
     * filename visibly changes, slow enough to cost nothing while scanning. */
    g_tick = lv_timer_create(tick_cb, 250, NULL);
    lv_timer_pause(g_tick);
}

/* Start a scan (unless one is already running) and show the progress screen. */
void scanview_open(void)
{
    int already = scanner_active();
    if(!already && scanner_start() != 0){
        ui_toast("Couldn't start the scan");
        return;
    }
    g_auto_back = 1;
    if(g_arc) lv_obj_set_style_arc_color(g_arc, ui_current_accent(), LV_PART_INDICATOR);
    screen_show(SCR_SCAN);
    if(g_tick){ lv_timer_resume(g_tick); lv_timer_ready(g_tick); }   /* paint the first frame now */
}

/* Called by the screen manager when SCR_SCAN is shown/left, so the poll only runs
 * while it is actually visible. */
void scanview_set_visible(int on)
{
    if(!g_tick) return;
    if(on){ lv_timer_resume(g_tick); lv_timer_ready(g_tick); }
    else  { lv_timer_pause(g_tick); g_auto_back = 0; }
}

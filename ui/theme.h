/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef THEME_H
#define THEME_H
#include "lvgl/lvgl.h"
#include <stdint.h>

/* diskOS theme tokens.
 *
 * Every surface colour in the UI comes from one of these accessors instead of a
 * hard-coded hex, so the whole interface can be re-skinned from one place. Two
 * palettes ship: DARK (the original near-black look) and LIGHT (a high-luminance
 * palette for outdoor/bright-sun use, where the dark UI is effectively unreadable
 * on this panel).
 *
 * Naming follows the role, not the colour, so a token means the same thing in
 * both palettes:
 *   bg          screen background
 *   card        a raised surface sitting on bg (rows, tiles, dialogs)
 *   card_press  that surface while pressed
 *   fill3       tertiary fill: slider tracks, "off" state circles, chips
 *   text        primary content (labels, glyphs, hairlines) - INVERTS per palette
 *   text2       secondary content (values, captions)
 *   text3       tertiary content (group headers, disabled)
 *   hairline    1px separators / card outlines
 *   scrim       the veil laid over an album-art backdrop so text stays legible;
 *               black in dark mode, white in light mode, so th_text() reads on it
 *
 * The accent colour is NOT a theme token - it stays user/artwork driven
 * (ui_current_accent()).
 */

enum { THEME_DARK = 0, THEME_LIGHT = 1 };

void theme_init(void);            /* load the persisted choice; call before any screen is built */
int  theme_is_light(void);
int  theme_get(void);
void theme_set(int mode);         /* persist only - the caller restarts the UI to apply */

/* palette accessors (lv_color_t, ready for lv_obj_set_style_*) */
lv_color_t th_bg(void);
lv_color_t th_card(void);
lv_color_t th_card_press(void);
lv_color_t th_fill3(void);
lv_color_t th_text(void);
lv_color_t th_text2(void);
lv_color_t th_text3(void);
lv_color_t th_hairline(void);
lv_color_t th_scrim(void);
lv_opa_t   th_scrim_opa(void);    /* backdrop veil strength (light mode needs more) */
lv_color_t th_knob(void);         /* slider knob: stays light-on-dark readable in both */
lv_color_t th_danger(void);       /* destructive action text (delete / stop) */
lv_color_t th_ok(void);           /* success/connected state */

/* raw 0xRRGGBB, for the few call sites that still want a hex (e.g. printf-ish APIs) */
uint32_t th_bg_hex(void);
uint32_t th_card_hex(void);
uint32_t th_card_press_hex(void);
uint32_t th_text_hex(void);

/* ---- font indirection ----------------------------------------------------
 * All UI text goes through th_font(size) instead of &lv_font_montserrat_NN, so a
 * user-supplied TTF from the SD card (Settings -> Display -> Font) can replace the
 * built-in face everywhere at once, and a Font Size preference can shift the whole
 * UI up or down a step. Falls back to the built-in Montserrat whenever no custom
 * face is loaded or the requested size has no built-in match. */
void             theme_font_init(void);   /* load the user TTF (if any) + size scale */
const lv_font_t *th_font(int size);       /* size = the nominal design size (8..48) */
int              theme_font_scale(void);  /* -2..+2 steps, 0 = design size */
const char      *theme_font_name(void);   /* "Built-in" or the TTF's filename */
/* Enumerate .ttf/.otf files on the SD card (root + a /Fonts dir). Returns count. */
int              theme_font_list(char names[][64], int cap);

#endif

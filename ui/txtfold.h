/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef TXTFOLD_H
#define TXTFOLD_H

/* Fold the codepoints the built-in face cannot draw down to ASCII, in place.
 *
 * WHY: the LVGL Montserrat fonts we ship carry U+0020..U+007E plus the LV_SYMBOL
 * private-use icons - nothing else. The Source Han CJK fallback (ui_font_cjk)
 * starts at U+3001, so everything BETWEEN the two - Latin-1 accents and the
 * General Punctuation block - has no glyph in any font we load and LVGL draws
 * its placeholder box instead (LV_USE_FONT_PLACEHOLDER). That is the black box
 * seen in a title like "Don<box>t Push Me": taggers write the TYPOGRAPHIC
 * apostrophe U+2019, not ASCII U+0027.
 *
 * So: "Don’t" -> "Don't", "Björk"/"Björk" -> "Bjork",
 * "a – b" -> "a - b", "etc…" -> "etc...".
 *
 * CJK is deliberately left ALONE (>= U+2E80): it has a real fallback font, and
 * romanising it would be worse than the correct glyph we already draw.
 * Anything else unmapped is also left alone - this only ever replaces a
 * codepoint it has a considered ASCII spelling for.
 *
 * In-place is safe: every mapping emits at most as many bytes as the UTF-8
 * source it replaces (a 2-byte source maps to <=2 ASCII chars, a 3-byte source
 * to <=3), so the write cursor can never overtake the read cursor.
 *
 * This is a DISPLAY fold, applied where text enters the UI (ipc.c metadata,
 * musicdb.c rows) - never to a file path, which has to keep its exact bytes to
 * open. */
void txt_fold_ascii(char *s);

#endif

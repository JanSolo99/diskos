/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/*
 * Runtime test for the user-font path in theme.c - the parts that cannot be checked by
 * reading the code, because they depend on how LVGL actually behaves:
 *
 *   - lv_tiny_ttf_create_file() opens through LVGL's virtual filesystem, not fopen(),
 *     so it needs a drive-letter path. Handed a bare POSIX path it returns NULL and the
 *     UI silently falls back to Montserrat with nothing on screen to explain why. That
 *     bug shipped once; this catches it.
 *   - The built-in face must be chained behind a custom one, or every LV_SYMBOL icon
 *     (private-use codepoints only Montserrat carries) turns into a tofu box.
 *   - The chosen font must survive a COLD BOOT. Fonts live on the SD card, which is
 *     mounted by an async worker started milliseconds before theme_font_init() runs, so
 *     the card is absent at startup. The test simulates that by moving the card away.
 *
 * Runs against a fake SD card, so it needs no device. It exercises the real theme.c and
 * config.c, and writes to the real config + font cache paths - run it on a build host,
 * not on a Disc you care about.
 *
 *   make fontcheck && ./fontcheck
 */
#include <stdio.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "theme.h"
#include "config.h"

static int fail;
#define CHECK(cond, msg) do{ printf("%-58s %s\n", msg, (cond)?"ok":"FAIL"); if(!(cond)) fail=1; }while(0)

int main(int argc, char **argv)
{
    lv_init();
    /* a display is needed before fonts can be measured */
    static uint8_t buf[360*10*4];
    lv_display_t *d = lv_display_create(360,360);
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);

    cfg_load();
    theme_init();

    /* Deterministic start: this harness persists to the real config, so a previous
       run's font_scale would otherwise make the baseline face the wrong size. */
    cfg_set_str("font_file", "Built-in");
    cfg_set_int("font_scale", 0);
    remove("/usr/data/diskos_font.ttf");

    /* 1) built-in path */
    theme_font_init();
    const lv_font_t *b = th_font(16);
    CHECK(b != NULL, "built-in: th_font(16) returns a font");
    CHECK(!strcmp(theme_font_name(), "Built-in"), "built-in: reported name is Built-in");
    lv_font_glyph_dsc_t g;
    CHECK(lv_font_get_glyph_dsc(b, &g, 'A', 0), "built-in: has glyph 'A'");

    /* 2) discovery finds the font we planted on the fake card */
    char names[24][64];
    int n = theme_font_list(names, 24);
    printf("   discovered %d font(s):", n);
    for(int i=0;i<n;i++) printf(" %s", names[i]);
    printf("\n");
    CHECK(n >= 1, "discovery: found at least one font on the card");

    /* 3) select it and prove a REAL ttf face comes back - not a silent fallback */
    if(n >= 1){
        CHECK(theme_font_install(names[0]), "install: font copied to internal storage");
        cfg_set_str("font_file", names[0]);
        theme_font_init();
        CHECK(!strcmp(theme_font_name(), names[0]), "custom: reported name is the chosen file");
        const lv_font_t *c = th_font(16);
        CHECK(c != NULL, "custom: th_font(16) returns a font");
        CHECK(c != b,    "custom: it is NOT the built-in face (the VFS path worked)");
        CHECK(lv_font_get_glyph_dsc(c, &g, 'A', 0), "custom: renders Latin 'A'");
        /* the built-in must be chained behind it, or every LV_SYMBOL icon turns to tofu */
        CHECK(c->fallback != NULL, "custom: built-in face is chained as fallback");
        /* LV_SYMBOL_OK is a private-use codepoint no user font carries; it must
           still resolve, via that fallback. */
        CHECK(lv_font_get_glyph_dsc(c, &g, 0xF00C, 0), "custom: LV_SYMBOL icon still resolves (via fallback)");
        /* size ladder: a different size must give a different instance, cached */
        const lv_font_t *c24 = th_font(24);
        CHECK(c24 != NULL && c24 != c, "custom: a second size yields a second instance");
        CHECK(th_font(16) == c,        "custom: instances are cached, not re-parsed");
    }

    /* 4) THE COLD-BOOT CASE: the card is mounted asynchronously and is NOT there when
       theme_font_init() runs at startup. The chosen font must still load, from the
       internal copy. Simulate it by making the card unreachable. */
    if(n >= 1){
        char away[512];
        snprintf(away, sizeof away, "%s.away", FONT_DIR_A);
        rename(FONT_DIR_A, away);                      /* card not mounted yet */
        theme_font_init();
        CHECK(!strcmp(theme_font_name(), names[0]), "cold boot: chosen font survives an absent card");
        const lv_font_t *cb = th_font(16);
        CHECK(cb != NULL, "cold boot: returns a font");
        lv_font_glyph_dsc_t gg;
        CHECK(lv_font_get_glyph_dsc(cb, &gg, 'A', 0), "cold boot: it still renders text");
        char names2[24][64];
        CHECK(theme_font_list(names2, 24) == 0, "cold boot: discovery correctly finds nothing");
        rename(away, FONT_DIR_A);                      /* card turns up */
    }

    /* 5) installing a font that is not there must FAIL rather than half-apply */
    CHECK(!theme_font_install("definitely-not-here.ttf"), "install: a missing file is refused");

    /* 5a) The cache must know WHICH face it holds. A config naming a different font
       than the one cached (hand-edited, or a cfg write that failed after the copy)
       must not render the cached face while reporting the configured name. */
    if(n >= 1){
        CHECK(theme_font_install(names[0]), "identity: reinstall for the mismatch check");
        cfg_set_str("font_file", "some-other-font.ttf");   /* config disagrees with the cache */
        theme_font_init();
        CHECK(strcmp(theme_font_name(), "some-other-font.ttf") != 0,
              "identity: a config/cache mismatch does not claim the wrong face");
        CHECK(th_font(16) != NULL, "identity: still returns a usable font");
        CHECK(!theme_font_is_installed("some-other-font.ttf"),
              "identity: an uncached name reports as not installed");
        CHECK(theme_font_is_installed(names[0]), "identity: the cached name reports as installed");
    }

    /* 5b) A name configured with NO cache behind it must still be installable - a
       name-only "already selected" check would make its own row a no-op and leave the
       user no way to repair it from the picker. */
    if(n >= 1){
        remove("/usr/data/diskos_font.ttf");
        remove("/usr/data/diskos_font.ttf.name");
        cfg_set_str("font_file", names[0]);
        theme_font_init();                                  /* card is present, so it loads */
        CHECK(!theme_font_is_installed(names[0]),
              "repair: a configured-but-uncached font is reported as not installed");
    }

    /* 6) a stale config pointing at a vanished font falls back cleanly */
    remove("/usr/data/diskos_font.ttf");
    cfg_set_str("font_file", "definitely-not-here.ttf");
    theme_font_init();
    CHECK(!strcmp(theme_font_name(), "Built-in"), "missing file: falls back to Built-in");
    CHECK(th_font(16) != NULL, "missing file: still returns a usable font");

    /* 7) selecting Built-in clears the cache */
    CHECK(theme_font_install("Built-in"), "install: Built-in always succeeds");
    { FILE *cf = fopen("/usr/data/diskos_font.ttf","rb");
      CHECK(cf == NULL, "install: Built-in removes the cached face"); if(cf) fclose(cf); }

    /* 8) text size shifts the whole ladder */
    cfg_set_str("font_file", "Built-in");
    cfg_set_int("font_scale", 2);
    theme_font_init();
    CHECK(theme_font_scale() == 2, "text size: scale is read back");
    CHECK(th_font(16) == th_font(16), "text size: stable across calls");
    CHECK(th_font(16) != b, "text size: +2 steps gives a larger built-in than the default");

    printf("\n%s\n", fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return fail;
}

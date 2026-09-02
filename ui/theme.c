/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "theme.h"
#include "config.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>

/* ---- palettes -----------------------------------------------------------
 * DARK is the original diskOS palette (Apple's dark system greys on true black -
 * true black matters here: the panel is a round LCD behind a black bezel, so a
 * black background makes the bezel disappear).
 *
 * LIGHT is tuned for the thing the dark UI fails at: direct sunlight. It is a
 * high-luminance, high-contrast palette (near-white ground, near-black text)
 * rather than a "soft" light theme, because at 40/40 backlight in daylight the
 * limiting factor is the luminance of the *background*, not of the text. */
typedef struct {
    uint32_t bg, card, card_press, fill3;
    uint32_t text, text2, text3, hairline;
    uint32_t scrim; uint8_t scrim_opa;
    uint32_t knob, danger, ok;
} palette_t;

static const palette_t P_DARK = {
    .bg = 0x000000, .card = 0x1C1C1E, .card_press = 0x2C2C2E, .fill3 = 0x3A3A3C,
    .text = 0xFFFFFF, .text2 = 0xC7C7CC, .text3 = 0x8E8E93, .hairline = 0xFFFFFF,
    .scrim = 0x000000, .scrim_opa = LV_OPA_50,
    .knob = 0xFFFFFF, .danger = 0xFF453A, .ok = 0x34C759,
};
static const palette_t P_LIGHT = {
    .bg = 0xF7F7F9, .card = 0xFFFFFF, .card_press = 0xE1E1E6, .fill3 = 0xC9C9CE,
    .text = 0x0A0A0C, .text2 = 0x3C3C43, .text3 = 0x6C6C70, .hairline = 0x000000,
    /* a WHITE veil over the album backdrop: without it the dark artwork behind
     * near-black text destroys contrast exactly where light mode is needed most.
     * Heavier than the dark scrim because covers are on average darker than
     * 0xF7F7F9, so more veil is needed to reach the same text contrast. */
    .scrim = 0xFFFFFF, .scrim_opa = 190,
    /* the knob stays white: it sits on the accent-filled part of a slider in both
     * palettes, and a dark knob there reads as a hole. */
    .knob = 0xFFFFFF, .danger = 0xD70015, .ok = 0x248A3D,
};

static const palette_t *g_p = &P_DARK;
static int g_mode = THEME_DARK;

void theme_init(void)
{
    g_mode = cfg_get_int("theme", THEME_DARK) == THEME_LIGHT ? THEME_LIGHT : THEME_DARK;
    g_p = (g_mode == THEME_LIGHT) ? &P_LIGHT : &P_DARK;
}
int  theme_is_light(void){ return g_mode == THEME_LIGHT; }
int  theme_get(void){ return g_mode; }
/* PERSIST ONLY - deliberately does not touch the live palette. Screens resolve their
 * colours when they are built, so flipping g_p here would repaint whatever screen is
 * in front in the new palette while every other screen stayed in the old one, for the
 * moment before the UI restarts. The restart is what applies it. */
void theme_set(int mode)
{
    cfg_set_int("theme", (mode == THEME_LIGHT) ? THEME_LIGHT : THEME_DARK);
}

lv_color_t th_bg(void)         { return lv_color_hex(g_p->bg); }
lv_color_t th_card(void)       { return lv_color_hex(g_p->card); }
lv_color_t th_card_press(void) { return lv_color_hex(g_p->card_press); }
lv_color_t th_fill3(void)      { return lv_color_hex(g_p->fill3); }
lv_color_t th_text(void)       { return lv_color_hex(g_p->text); }
lv_color_t th_text2(void)      { return lv_color_hex(g_p->text2); }
lv_color_t th_text3(void)      { return lv_color_hex(g_p->text3); }
lv_color_t th_hairline(void)   { return lv_color_hex(g_p->hairline); }
lv_color_t th_scrim(void)      { return lv_color_hex(g_p->scrim); }
lv_opa_t   th_scrim_opa(void)  { return g_p->scrim_opa; }
lv_color_t th_knob(void)       { return lv_color_hex(g_p->knob); }
lv_color_t th_danger(void)     { return lv_color_hex(g_p->danger); }
lv_color_t th_ok(void)         { return lv_color_hex(g_p->ok); }

uint32_t th_bg_hex(void)         { return g_p->bg; }
uint32_t th_card_hex(void)       { return g_p->card; }
uint32_t th_card_press_hex(void) { return g_p->card_press; }
uint32_t th_text_hex(void)       { return g_p->text; }

/* ---- fonts --------------------------------------------------------------- */

/* The built-in ladder. th_font() snaps any requested size to the nearest entry,
 * so a call site asking for a size we don't compile in still gets a sane face
 * instead of NULL (which LVGL renders as nothing at all). */
static const lv_font_t *const BUILTIN[] = {
    &lv_font_montserrat_8,  &lv_font_montserrat_10, &lv_font_montserrat_12,
    &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18,
    &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
    &lv_font_montserrat_26, &lv_font_montserrat_28, &lv_font_montserrat_30,
    &lv_font_montserrat_32, &lv_font_montserrat_34, &lv_font_montserrat_36,
    &lv_font_montserrat_38, &lv_font_montserrat_40, &lv_font_montserrat_42,
    &lv_font_montserrat_44, &lv_font_montserrat_46, &lv_font_montserrat_48,
};
static const int BUILTIN_SZ[] = { 8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48 };
#define N_BUILTIN ((int)(sizeof(BUILTIN)/sizeof(BUILTIN[0])))

/* Where fonts are looked for, in order: a Fonts folder on the card (either case),
 * then the card root. Overridable at build time so the loader can be exercised
 * against a fake card off-device. */
#ifndef FONT_DIR_A
#define FONT_DIR_A "/tmp/sdcard/Fonts"
#endif
#ifndef FONT_DIR_B
#define FONT_DIR_B "/tmp/sdcard/fonts"
#endif
#ifndef FONT_ROOT
#define FONT_ROOT  "/tmp/sdcard"
#endif
/* Where the CHOSEN font actually lives once selected.
 *
 * The card is mounted by an async worker that main() kicks off milliseconds before
 * theme_font_init() runs, so at cold boot /tmp/sdcard is not there yet - resolving
 * the face straight off the card would fail on every single power-on and silently
 * revert to the built-in font. Fonts are also the kind of thing you set once and
 * forget, and the card can be pulled.
 *
 * So selecting a font COPIES it to NAND, which is always mounted, and that copy is
 * what gets loaded. Discovery still scans the card. */
#ifndef FONT_CACHE
#define FONT_CACHE "/usr/data/diskos_font.ttf"
#endif
#define FONT_CACHE_MAX (12*1024*1024)   /* refuse anything absurd; NAND is small */

static int   g_scale;                     /* -2..+2 design-size steps */
static char  g_font_file[128];            /* POSIX path of the loaded TTF, "" = built-in */
static char  g_font_lvpath[132];          /* the same file as an LVGL VFS path (see below) */
static char  g_font_name[64] = "Built-in";

/* Custom faces are instantiated lazily, one lv_tiny_ttf instance per size actually
 * used. There are ~10 distinct sizes in the UI, so the table is small and every
 * entry is created at most once for the life of the process. */
#if LV_USE_TINY_TTF
typedef struct { int size; lv_font_t *f; } ttf_slot_t;
static ttf_slot_t g_ttf[24];
static int        g_ttf_n;
#endif

static int has_ext_ci(const char *n, const char *ext)
{
    size_t nl = strlen(n), el = strlen(ext);
    return nl > el && !strcasecmp(n + nl - el, ext);
}
static int is_font_file(const char *n){ return has_ext_ci(n, ".ttf") || has_ext_ci(n, ".otf"); }

/* Look for `name` in the font directories, then the SD root. 1 + fills `out` if found. */
static int font_resolve(const char *name, char *out, int cap)
{
    static const char *const DIRS[] = { FONT_DIR_A, FONT_DIR_B, FONT_ROOT };
    for(int i = 0; i < 3; i++){
        char p[192];
        snprintf(p, sizeof p, "%s/%s", DIRS[i], name);
        FILE *f = fopen(p, "rb");
        if(f){ fclose(f); snprintf(out, cap, "%s", p); return 1; }
    }
    return 0;
}

int theme_font_list(char names[][64], int cap)
{
    static const char *const DIRS[] = { FONT_DIR_A, FONT_DIR_B, FONT_ROOT };
    int n = 0;
    for(int i = 0; i < 3 && n < cap; i++){
        DIR *d = opendir(DIRS[i]);
        if(!d) continue;
        struct dirent *e;
        while(n < cap && (e = readdir(d))){
            if(e->d_name[0] == '.' || !is_font_file(e->d_name)) continue;
            /* Skip a name that would not fit rather than storing a truncated one: the
             * truncated form is what gets persisted and later resolved, and it would
             * never match a real file - so the font would silently revert to built-in
             * with nothing to explain why. */
            if(strlen(e->d_name) >= 64) continue;
            int dup = 0;   /* the same filename in two dirs is one entry */
            for(int k = 0; k < n; k++) if(!strcasecmp(names[k], e->d_name)){ dup = 1; break; }
            if(!dup){ memcpy(names[n], e->d_name, strlen(e->d_name) + 1); n++; }
        }
        closedir(d);
    }
    return n;
}

/* Copy `src` to the NAND cache, atomically (temp file + rename), so a power cut
 * mid-copy can never leave a half-written face that renders as garbage. */
static int font_cache_store(const char *src)
{
    FILE *in = fopen(src, "rb");
    if(!in) return 0;
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    if(sz <= 0 || sz > FONT_CACHE_MAX){ fclose(in); return 0; }
    rewind(in);

    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", FONT_CACHE);
    FILE *out = fopen(tmp, "wb");
    if(!out){ fclose(in); return 0; }

    char buf[8192];
    size_t n, total = 0;
    int ok = 1;
    while((n = fread(buf, 1, sizeof buf, in)) > 0){
        if(fwrite(buf, 1, n, out) != n){ ok = 0; break; }
        total += n;
    }
    if(ferror(in)) ok = 0;
    if(fflush(out) != 0) ok = 0;
    fclose(in);
    fclose(out);
    if(ok && total != (size_t)sz) ok = 0;            /* short read: not the whole face */
    if(!ok || rename(tmp, FONT_CACHE) != 0){ remove(tmp); return 0; }
    return 1;
}

int theme_font_install(const char *name)
{
    if(!name || !name[0] || !strcasecmp(name, "Built-in")){
        remove(FONT_CACHE);                          /* back to the built-in face */
        return 1;
    }
    char src[192];
    if(!font_resolve(name, src, sizeof src)) return 0;   /* not on the card (any more) */
    return font_cache_store(src);
}

void theme_font_init(void)
{
    g_scale = cfg_get_int("font_scale", 0);
    if(g_scale < -2) g_scale = -2;
    if(g_scale >  2) g_scale =  2;

    g_font_file[0] = 0;
    g_font_lvpath[0] = 0;
    snprintf(g_font_name, sizeof g_font_name, "Built-in");
#if LV_USE_TINY_TTF
    const char *want = cfg_get_str("font_file", "");
    if(want && want[0] && strcasecmp(want, "Built-in") != 0){
        /* The NAND copy first - it is the one that exists this early in boot. Falling
         * back to the card covers a config edited by hand, or a cache lost to a
         * factory reset, at whatever point in the session the card turns up. */
        FILE *f = fopen(FONT_CACHE, "rb");
        if(f){ fclose(f); snprintf(g_font_file, sizeof g_font_file, "%s", FONT_CACHE); }
        else if(!font_resolve(want, g_font_file, sizeof g_font_file)) g_font_file[0] = 0;

        if(g_font_file[0]){
            snprintf(g_font_name, sizeof g_font_name, "%s", want);
            /* lv_tiny_ttf_create_file() opens through LVGL's virtual filesystem, NOT
             * through fopen(), so it needs a DRIVE-LETTER path. Handing it the bare
             * POSIX path makes lv_fs_open() return NOT_EX and every custom font falls
             * back to the built-in face - silently, with nothing to explain why.
             * LV_FS_STDIO_LETTER is 'A' (lv_conf.h), matching the "A:/tmp/..." form
             * the album-art code already uses. */
            snprintf(g_font_lvpath, sizeof g_font_lvpath, "A:%s", g_font_file);
        }
        /* else: the card was swapped and there is no cache - fall back silently to the
         * built-in face rather than rendering an empty UI. */
    }
#endif
}

int theme_font_scale(void){ return g_scale; }
const char *theme_font_name(void){ return g_font_name; }

/* Nearest built-in >= the request, clamped to the ladder. */
static const lv_font_t *builtin_for(int size)
{
    for(int i = 0; i < N_BUILTIN; i++) if(BUILTIN_SZ[i] >= size) return BUILTIN[i];
    return BUILTIN[N_BUILTIN - 1];
}

const lv_font_t *th_font(int size)
{
    /* Font Size shifts every call site by the same number of ladder steps, so the
     * type hierarchy (28 title / 16 body / 14 caption) is preserved rather than
     * flattened. 2px per step keeps the shift inside the built-in ladder. */
    int s = size + g_scale * 2;
    if(s < 8)  s = 8;
    if(s > 48) s = 48;

#if LV_USE_TINY_TTF
    if(g_font_file[0]){
        for(int i = 0; i < g_ttf_n; i++) if(g_ttf[i].size == s) return g_ttf[i].f ? g_ttf[i].f : builtin_for(s);
        if(g_ttf_n < (int)(sizeof(g_ttf)/sizeof(g_ttf[0]))){
            lv_font_t *f = lv_tiny_ttf_create_file(g_font_lvpath, s);
            /* Chain the built-in face behind the custom one. The UI's icons are
             * LV_SYMBOL_* private-use codepoints that live in Montserrat, and almost
             * no user font carries them - without this fallback, choosing a custom
             * font would replace every play/wifi/battery glyph in the interface with
             * a tofu box. The fallback also covers any Latin glyph the face lacks. */
            if(f) f->fallback = builtin_for(s);
            g_ttf[g_ttf_n].size = s;
            g_ttf[g_ttf_n].f    = f;     /* NULL is cached too: a bad TTF must not be
                                          * re-parsed on every label we draw */
            g_ttf_n++;
            if(f) return f;
        }
    }
#endif
    return builtin_for(s);
}

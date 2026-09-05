/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "txtfold.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>

LV_FONT_DECLARE(font_weather16)
static lv_font_t s_appwfont;   /* montserrat_22 + weather-icon fallback */

/* Weather via wttr.in (auto-geolocates by IP). Fetched on a detached thread so
 * the blocking wget never stalls the UI; the result is picked up on the main
 * thread by weather_poll() (LVGL is not thread-safe). */

/* FontAwesome 4 weather glyphs (UTF-8), rendered via font_weather16 fallback. */
#define WI_SUN   "\xEF\x86\x85"   /* f185 sun-o      */
#define WI_MOON  "\xEF\x86\x86"   /* f186 moon-o     */
#define WI_CLOUD "\xEF\x83\x82"   /* f0c2 cloud      */
#define WI_RAIN  "\xEF\x83\xA9"   /* f0e9 umbrella   */
#define WI_BOLT  "\xEF\x83\xA7"   /* f0e7 bolt       */
#define WI_SNOW  "\xEF\x8B\x9C"   /* f2dc snowflake-o*/

static char g_wbuf[160];
static char g_loc[160];               /* percent-encoded location (set on main thread).
                                       * config VLEN caps weather_loc at ~47 bytes raw, so
                                       * worst-case %-encoding (47*3=141) fits with margin. */
/* worker->main handoff is guarded by a mutex (volatile gives no cross-thread memory
 * ordering; on the dual-core X2000 the main thread could otherwise observe g_wready
 * before the g_wbuf writes are visible). */
static pthread_mutex_t g_wx_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_wready = 0;     /* a fresh result is waiting          (guarded) */
static int g_inflight = 0;   /* a fetch thread is running          (guarded) */
static int g_have = 0;       /* got weather at least once          (main thread only) */
static uint32_t g_last = 0;  /* tick of last fetch start           (main thread only) */
static int g_wfail = 0;      /* last fetch returned no usable data (guarded) */
static int g_wgen = 0;       /* bumps when the location changes    (main thread only) */
static int g_wjobgen = 0;    /* g_wgen the in-flight fetch serves  (main thread only) */

/* percent-encode into a URL-safe AND shell-safe form (only [A-Za-z0-9-_.~] + %XX),
 * so g_loc can never break out of the single-quoted wget command. */
static void wx_urlenc(const char *s, char *out, int cap)
{
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for(; *s && o < cap - 4; s++){
        unsigned char c = (unsigned char)*s;
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||c==',')
            out[o++] = (char)c;
        else { out[o++]='%'; out[o++]=hex[c>>4]; out[o++]=hex[c&15]; }
    }
    out[o] = 0;
}

static void *weather_thread(void *arg)
{
    (void)arg;
    int fail = 1;    /* result of this fetch, published under the lock at the end */
    char result[160] = {0};   /* built here; copied into g_wbuf UNDER the lock at publish (no torn reads) */
    char line[256] = {0};
    char cmd[320];   /* fits "wget ... 'http://wttr.in/<g_loc≤141>?format=...' ..." */
    if (g_loc[0])
        snprintf(cmd, sizeof cmd, "wget -qO- -T 8 'http://wttr.in/%s?format=%%t~%%C~%%l' 2>/dev/null", g_loc);
    else
        snprintf(cmd, sizeof cmd, "wget -qO- -T 8 'http://wttr.in/?format=%%t~%%C~%%l' 2>/dev/null");
    FILE *f = popen(cmd, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; pclose(f); }
    else fprintf(stderr, "weather popen failed: %s\n", strerror(errno));
    if (line[0] && strchr(line, '~')) {
        char temp[32] = {0}, cond[128] = {0};
        char *t1 = strchr(line, '~');
        int tl = t1 - line; if (tl > 31) tl = 31;
        memcpy(temp, line, tl); temp[tl] = 0;
        char *c = t1 + 1;
        char *t2 = strchr(c, '~');
        int full = t2 ? (int)(t2 - c) : (int)strlen(c);
        while (full > 0 && (c[full-1] == '\n' || c[full-1] == '\r')) full--;
        /* lowercase the full condition phrase for icon matching */
        char low[128]; int li = 0;
        for (int i = 0; i < full && li < 127; i++) {
            char ch = c[i]; if (ch >= 'A' && ch <= 'Z') ch += 32; low[li++] = ch;
        }
        low[li] = 0;
        const char *ic = WI_CLOUD;
        if      (strstr(low,"thunder") || strstr(low,"storm"))                       ic = WI_BOLT;
        else if (strstr(low,"snow") || strstr(low,"sleet") || strstr(low,"blizzard") ||
                 strstr(low,"ice"))                                                  ic = WI_SNOW;
        else if (strstr(low,"rain") || strstr(low,"shower") || strstr(low,"drizzle"))ic = WI_RAIN;
        else if (strstr(low,"fog") || strstr(low,"mist") || strstr(low,"haze") ||
                 strstr(low,"cloud") || strstr(low,"overcast"))                      ic = WI_CLOUD;
        else if (strstr(low,"sun") || strstr(low,"clear"))                           ic = WI_SUN;
        /* display condition = first phrase (cut at comma) */
        int cl = full;
        char *comma = memchr(c, ',', cl); if (comma) cl = (int)(comma - c);
        if (cl > 120) cl = 120;
        memcpy(cond, c, cl); cond[cl] = 0;
        char *tp = temp; if (*tp == '+') tp++;   /* drop leading + on positive temps */
        snprintf(result, sizeof result, "%s  %s  %s", ic, tp, cond);   /* into local; g_wbuf write is under the lock */
        /* wttr.in sends a real degree sign, and "feels like" conditions can carry
         * accented words - none of which Montserrat can draw (see txtfold.h). The
         * weather-icon glyphs are private-use and pass through untouched. */
        txt_fold_ascii(result);
        fail = 0;
    }
    if (fail) fprintf(stderr, "weather: no usable data (resp='%.48s')\n", line);
    /* publish atomically: the lock release/acquire pairs with weather_poll so the
     * g_wbuf writes above are guaranteed visible once it observes g_wready. */
    pthread_mutex_lock(&g_wx_mu);
    if (!fail) memcpy(g_wbuf, result, sizeof g_wbuf);   /* g_wbuf written ONLY under the lock */
    g_wfail = fail; g_wready = 1; g_inflight = 0;
    pthread_mutex_unlock(&g_wx_mu);
    return NULL;
}

void weather_fetch_async(void)
{
    int go = 0;
    pthread_mutex_lock(&g_wx_mu);
    if (!g_inflight) { g_inflight = 1; g_wready = 0; go = 1; }   /* claim slot + drop any stale unconsumed result */
    pthread_mutex_unlock(&g_wx_mu);
    if (!go) return;
    g_wjobgen = g_wgen;   /* this fetch serves the current location generation */
    /* read the configured location on the main thread (cfg is not thread-safe);
     * percent-encode it so it's URL-safe AND can't break out of the single-quoted
     * wget command. Empty => wttr.in auto-geolocates by IP. Set before pthread_create,
     * whose barrier publishes it to the worker. */
    wx_urlenc(cfg_get_str("weather_loc", ""), g_loc, sizeof g_loc);
    g_last = lv_tick_get();
    pthread_t th;
    if (pthread_create(&th, NULL, weather_thread, NULL) == 0) pthread_detach(th);
    else {   /* no worker -> publish a failure so the app shows "unavailable", not a stuck "Fetching..." */
        pthread_mutex_lock(&g_wx_mu);
        g_inflight = 0; g_wfail = 1; g_wready = 1;
        pthread_mutex_unlock(&g_wx_mu);
    }
}

/* Main-thread tick: apply a finished fetch, and retry every 20s until the first
 * success (covers WiFi coming up after boot), then refresh every 10 min. */
static void weather_app_refresh(void);   /* fwd */

/* Enable/disable passive weather (home + screensaver display + the background fetch). Called from
 * Settings. Off = no wget, no display (battery); on = force a refetch on the next poll. */
void weather_set_enabled(int on)
{
    if (on) { g_last = 0; }
    else    { home_set_weather(""); saver_set_weather(""); }
}

void weather_poll(lv_timer_t *t)
{
    (void)t;
    if (!cfg_get_int("weather_on", 1)) {
        /* weather off -> no fetch, no display. But DRAIN any in-flight worker result so a fetch that
         * completes while disabled can't flash a stale reading the instant it's re-enabled. */
        pthread_mutex_lock(&g_wx_mu);
        g_wready = 0;
        pthread_mutex_unlock(&g_wx_mu);
        return;
    }
    int ready = 0, fail = 0, inflight;
    pthread_mutex_lock(&g_wx_mu);
    if (g_wready) { g_wready = 0; ready = 1; fail = g_wfail; }
    inflight = g_inflight;
    pthread_mutex_unlock(&g_wx_mu);
    if (ready) {
        if (g_wjobgen != g_wgen) {
            /* result is for a since-changed location -> discard + refetch the current one now */
            g_last = 0;
        } else if (!fail) {        /* keep last-good on failure; only update on success.
                                    * g_wbuf safe here: worker set g_inflight=0 under the same
                                    * lock and the next fetch only starts below, after this use. */
            g_have = 1;
            home_set_weather(g_wbuf);
            saver_set_weather(g_wbuf);
        }
        weather_app_refresh();     /* shows data, or "Weather unavailable" if none yet */
    }
    uint32_t now = lv_tick_get();
    uint32_t interval = g_have ? 600000u : 20000u;
    if (!inflight && (now - g_last) > interval) weather_fetch_async();
}

/* ---- Weather app: set the location used for home + screensaver weather ---- */
static lv_obj_t *g_app_w;    /* current weather text */
static lv_obj_t *g_app_loc;  /* current location line */

static void weather_app_refresh(void)
{
    if (g_app_w) {
        /* snapshot g_wbuf under the lock - the worker can be mid-write when the app screen
         * refreshes, which would otherwise show torn text */
        char snap[160]; int wfail;
        pthread_mutex_lock(&g_wx_mu);
        snprintf(snap, sizeof snap, "%s", g_wbuf);
        wfail = g_wfail;   /* snapshot the guarded flag under the SAME lock (was an unlocked read) */
        pthread_mutex_unlock(&g_wx_mu);
        lv_label_set_text(g_app_w, g_have ? snap
                                          : (wfail ? "Weather unavailable" : "Fetching..."));
    }
    if (g_app_loc) {
        const char *l = cfg_get_str("weather_loc", "");
        char b[128];
        snprintf(b, sizeof b, "Location:  %s", (l && l[0]) ? l : "Auto (by IP)");
        lv_label_set_text(g_app_loc, b);
    }
}

void weather_app_open(void) { weather_app_refresh(); screen_show(SCR_WEATHER); }

static void loc_done(const char *text)
{
    cfg_set_str("weather_loc", text ? text : "");
    g_wgen++;                   /* invalidate any in-flight fetch for the old location */
    g_have = 0;                 /* force a fresh fetch + show "Fetching" */
    g_last = 0;
    weather_fetch_async();
    weather_app_refresh();
}
static void set_loc_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    kbinput_open("City / location", cfg_get_str("weather_loc", ""), loc_done);
}
static void auto_loc_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cfg_set_str("weather_loc", "");
    g_wgen++;                   /* invalidate any in-flight fetch for the old location */
    g_have = 0; g_last = 0;
    weather_fetch_async();
    weather_app_refresh();
}

static lv_obj_t *wapp_btn(lv_obj_t *root, int y, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, 50, y); lv_obj_set_size(b, 260, 52);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, th_card(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(b, th_card_press(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, th_font(16), 0);
    lv_obj_set_style_text_color(l, th_text(), 0);
    lv_obj_center(l);
    return b;
}

void weather_app_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Weather");   /* shared standard header */

    s_appwfont = *th_font(22);
    s_appwfont.fallback = &font_weather16;
    g_app_w = lv_label_create(root);
    lv_obj_set_pos(g_app_w, 0, 96); lv_obj_set_width(g_app_w, 360);
    lv_obj_set_style_text_align(g_app_w, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_app_w, &s_appwfont, 0);
    lv_obj_set_style_text_color(g_app_w, th_text(), 0);

    g_app_loc = lv_label_create(root);
    lv_obj_set_pos(g_app_loc, 0, 140); lv_obj_set_width(g_app_loc, 360);
    lv_obj_set_style_text_align(g_app_loc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_app_loc, th_font(14), 0);
    lv_obj_set_style_text_color(g_app_loc, th_text3(), 0);

    wapp_btn(root, 186, "Set Location", set_loc_cb);
    wapp_btn(root, 248, "Auto (by IP)", auto_loc_cb);

    weather_app_refresh();
}

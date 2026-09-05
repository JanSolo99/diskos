/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "anim.h"
#include "config.h"
#include <unistd.h>

#define SCR_W 360

static lv_obj_t *s_roots[SCR_COUNT];
static lv_obj_t *s_scrim;        /* depth overlay dimming the screen beneath the moving panel */
static int s_current = SCR_HOME;
static int s_stack[16];
static int s_sp = 0;
#define PUSH_MS 320   /* entrance (expo-out: covers distance fast, settles gently) */
#define POP_MS  240   /* exit faster than entrance - a premium-motion reflex */
static int s_anim = 1;   /* slide transitions; disabled by /usr/data/anim_off */

static lv_obj_t *screen_make_root(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    /* full black square; the physical round bezel masks the shape. Clipping to a
     * circle here just exposed the lighter screen behind at the corners. */
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* Slide is the only GPU-less-smooth transition (a blit, not a resample). Each root carries a
     * 1px leading-edge hairline so the sliding boundary reads as a card edge. */
    anim_panel_shadow(root);
    return root;
}

/* Reset a root to its rest state (x=0, full opacity, 100% scale) - used by the up-front cleanup and
 * the completion callbacks so a screen left mid-zoom by an interrupt is always normalized. */
static void root_rest(lv_obj_t *o)
{
    lv_obj_set_x(o, 0);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale_x(o, 256, 0);
    lv_obj_set_style_transform_scale_y(o, 256, 0);
}


static void show_raw(int which)
{
    if (which < 0 || which >= SCR_COUNT) return;

    for (int i = 0; i < SCR_COUNT; i++) {
        if (s_roots[i]) lv_obj_add_flag(s_roots[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (s_roots[which]) {
        lv_obj_clear_flag(s_roots[which], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_roots[which]);
    }
}

/* hide + reset the depth scrim (transition finished or was interrupted). Delete any live scrim
 * fade FIRST: with the anim-cap snap path a fade can still be alive here and would otherwise
 * rewrite the opacity back to 36 after this reset, breaking the hidden-rest invariant. */
static void scrim_off(void)
{
    if (s_scrim) {
        lv_anim_delete(s_scrim, NULL);
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_scrim, LV_OPA_TRANSP, 0);
    }
}

static void anim_done_hide(lv_anim_t *a)
{
    lv_obj_t *o = (lv_obj_t *)a->var;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    root_rest(o);        /* it faded/scaled out - restore rest state for next time it's shown */
    scrim_off();
}

/* Forward-push completion: the incoming screen finished sliding in on top, so hide the screen it
 * now fully covers (tracked in s_push_hide). lv_anim_delete never fires a completed-cb, so an
 * interrupted push leaves the stale index unused until the next push overwrites it - and the
 * up-front straggler cleanup hides any covered root in that case. */
static int s_push_hide = -1;
static void anim_done_push(lv_anim_t *a)
{
    (void)a;
    int h = s_push_hide; s_push_hide = -1;
    if (h >= 0 && h < SCR_COUNT && s_roots[h]) {
        lv_obj_add_flag(s_roots[h], LV_OBJ_FLAG_HIDDEN);
        root_rest(s_roots[h]);
    }
    scrim_off();
}

/* Slide between screens. dir = +1 push (new from right), -1 pop (new from left). */
static void transition(int from, int to, int dir)
{
    lv_obj_t *nw = (to >= 0 && to < SCR_COUNT) ? s_roots[to] : NULL;
    lv_obj_t *od = (from >= 0 && from < SCR_COUNT) ? s_roots[from] : NULL;

    /* Any screen change dismisses transient lv_layer_top popups (e.g. the duplicate-add
     * confirm dialog) so they can't survive onto another screen and act on stale state. */
    npmenu_close_transients();

    /* Re-sync screens built once, on EVERY entry incl. back-navigation (screen_back also
     * routes through here - screen_show alone missed the back case, leaving stale labels
     * e.g. after editing Custom EQ / a setting detail page). */
    /* The scan screen's poll is gated on visibility. This is a SEPARATE statement from
     * the entry-refresh chain below on purpose: folding it in as another `else if`
     * would swallow the rest of the chain whenever we leave the scan screen - and the
     * most important case is leaving it FOR the Library, right after a scan changed
     * what the Library should show. */
    if (from == SCR_SCAN && to != SCR_SCAN) scanview_set_visible(0);
    else if (to == SCR_SCAN)               scanview_set_visible(1);

    if (to == SCR_SETTINGS) settings_refresh_list();
    else if (to == SCR_SETTINGS_GROUP) settings_group_refresh();   /* rebuilt on every entry: values are never stale */
    else if (to == SCR_TUNE)  tune_refresh();
    else if (to == SCR_SAVER) saver_show_sync();
    else if (to == SCR_PLVIEW) plview_refresh();   /* fresh song list every entry (no stale tap positions) */
    else if (to == SCR_QUEUE)  queue_refresh();    /* re-aim at the player's current list + rebuild rows */
    else if (to == SCR_LIBRARY) library_refresh(); /* pick up playlists created (NP New Playlist) or imported
                                                    * (Settings) elsewhere, without needing a restart */

    /* Cancel any in-flight animation on BOTH screens + reset their offset up front, so a
     * stale anim_done_hide from a prior fast transition can't fire later and hide the new
     * current screen. Covers every path below incl. show_raw and the NP/hub special case. */
    if (nw) { lv_anim_delete(nw, NULL); root_rest(nw); }
    if (od) { lv_anim_delete(od, NULL); root_rest(od); }

    /* Also normalize any OTHER root a prior interrupted transition left mid-flight (e.g. a screen
     * still scaling/fading on top): delete its anim, hide it, restore rest state. Runs BEFORE every
     * path below so a stray root can never linger over - or steal input from - the new screen. */
    for (int i = 0; i < SCR_COUNT; i++) {
        if (i == to || i == from || !s_roots[i]) continue;
        lv_anim_delete(s_roots[i], NULL);
        lv_obj_add_flag(s_roots[i], LV_OBJ_FLAG_HIDDEN);
        root_rest(s_roots[i]);
    }
    if (s_scrim) { lv_anim_delete(s_scrim, NULL); scrim_off(); }   /* scrim unused by zoom; keep clear */

    if (!nw || to == from || !s_anim) { show_raw(to); return; }
    /* Full-screen overlays (screensaver, quick-settings pulldown) are takeovers, not hierarchical
     * navigation - show/hide instantly. */
    if (to == SCR_SAVER || from == SCR_SAVER) { show_raw(to); return; }
    if (to == SCR_QUICK || from == SCR_QUICK) { show_raw(to); return; }

    /* ---- UNIFIED push/pop - ONE motion language for the whole UI ----------------------------
     * The top screen SLIDES horizontally over a STATIC screen beneath (a blit - the only smooth
     * motion on this GPU-less renderer; scale/zoom resamples and stutters). Leading-edge hairline
     * + a subtle depth scrim on the covered screen. Only ONE screen moves, so the heavy scene
     * underneath (NP arc / Home's blurred art) is never re-rendered mid-slide. Expo-out easing;
     * exit faster than entrance. Forward: new screen in from the right. Back: current out to the
     * right, revealing the previous. */
    if (dir > 0) {                                  /* FORWARD: `nw` slides in over static `od` */
        if (od) { lv_obj_set_x(od, 0); lv_obj_clear_flag(od, LV_OBJ_FLAG_HIDDEN); }
        if (s_scrim && od) {                        /* dim the covered screen: recedes into depth */
            lv_obj_set_style_opa(s_scrim, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_scrim);        /* scrim above od, below nw */
            anim_scrim_fade(s_scrim, 1, PUSH_MS);
        }
        lv_obj_set_x(nw, SCR_W);
        lv_obj_clear_flag(nw, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(nw);                 /* incoming on top: gets input */
        s_push_hide = (od && from != to) ? from : -1;   /* hide the covered screen when the slide ends */
        anim_page_slide(nw, SCR_W, 0, PUSH_MS, anim_done_push);
    } else {                                        /* BACK: `od` slides out, revealing static `nw` */
        lv_obj_set_x(nw, 0);
        lv_obj_clear_flag(nw, LV_OBJ_FLAG_HIDDEN);
        if (s_scrim) {                              /* revealed screen lifts out of depth */
            lv_obj_set_style_opa(s_scrim, 36, 0);
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_scrim);        /* scrim above nw, below od */
            anim_scrim_fade(s_scrim, 0, POP_MS);
        }
        if (od) {
            lv_obj_set_x(od, 0);
            lv_obj_clear_flag(od, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(od);             /* outgoing on top slides away to the right */
            anim_page_slide(od, 0, SCR_W, POP_MS, anim_done_hide);
        } else {
            lv_obj_move_foreground(nw);
        }
    }
}

/* Screens that are TAKEOVERS, not places: the pull-down panel and the screensaver
 * cover whatever you were doing and are dismissed back to it. They must never
 * become a rung on the ladder, or "pull down, dismiss, pull down, dismiss" quietly
 * grows the back stack. */
static int is_overlay(int which){ return which == SCR_QUICK || which == SCR_SAVER; }

void screen_show(int which)
{
    if (which < 0 || which >= SCR_COUNT) return;
    int from = s_current;
    if (which == s_current) return;      /* already here: not a navigation at all */

    /* ---- REVISIT COLLAPSE ------------------------------------------------------
     * The stack is a LADDER of distinct places, not a log of every screen you have
     * ever looked at. Bouncing between two screens (Now Playing <-> its hub, a list
     * <-> a detail) used to push a fresh entry every single time, so three round
     * trips meant six back-swipes to reach Home - with no way to shortcut it.
     *
     * So: if `which` is already ON the stack, this is a RETURN to somewhere we came
     * from, not a step deeper. Unwind to that rung (dropping everything above it)
     * and play the BACK transition, which is also what the motion should say. The
     * result is that the depth of the stack tracks how deep you actually are, and
     * Home is always at the bottom. */
    for (int i = s_sp - 1; i >= 0; i--) {
        if (s_stack[i] != which) continue;
        s_sp = i;                        /* drop this entry and everything above it */
        s_current = which;
        transition(from, which, is_overlay(from) ? +1 : -1);
        return;
    }

    if (!is_overlay(s_current)) {         /* overlays are dismissed, never returned to */
        int cap = (int)(sizeof(s_stack)/sizeof(s_stack[0]));
        if (s_sp >= cap) {                /* full: keep the root (s_stack[0]) so Back still
                                           * reaches Home; drop the 2nd-oldest instead */
            for (int i = 2; i < cap; i++) s_stack[i-1] = s_stack[i];
            s_sp = cap - 1;
        }
        s_stack[s_sp++] = s_current;
    }
    s_current = which;
    transition(from, which, +1);
}

/* Pop the nav stack (swipe / back gesture). Home is the root: no-op. */
void screen_back(void)
{
    if (s_sp <= 0) return;
    int from = s_current;
    int prev = s_stack[--s_sp];
    s_current = prev;
    transition(from, prev, -1);
}

/* Jump straight to Home from anywhere, discarding the whole ladder - the escape
 * hatch the stock UI has and diskOS did not. Wired to the Quick Settings Home tile
 * and to a long-press on any screen header's back chevron. */
void screen_home(void)
{
    int from = s_current;
    s_sp = 0;
    if (from == SCR_HOME) { transition(from, SCR_HOME, -1); return; }
    s_current = SCR_HOME;
    transition(from, SCR_HOME, -1);   /* back-motion: you are unwinding, not descending */
}

/* How deep the current screen sits (0 = Home). Lets a screen decide whether to
 * offer a "Home" affordance at all. */
int screen_depth(void){ return s_sp; }

lv_obj_t *screen_get_root(int which)
{
    if (which < 0 || which >= SCR_COUNT) return NULL;
    return s_roots[which];
}

int screen_current(void){ return s_current; }

void screens_init(void)
{
    if (s_roots[SCR_HOME]) {
        screen_home();
        return;
    }

    s_anim = cfg_get_int("anim", 1);
    if (access("/usr/data/anim_off", 0) == 0) s_anim = 0;   /* legacy override */

    lv_obj_t *parent = lv_screen_active();
    lv_obj_set_style_bg_color(parent, th_bg(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    /* The screen container must not scroll: during a slide, the incoming screen sits off-screen
     * to the right (x=+360), which overflows the parent and makes LVGL draw a horizontal
     * scrollbar along the bottom that slides in with it. Fixed container -> no stray bar. */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);

    s_roots[SCR_HOME] = screen_make_root(parent);
    s_roots[SCR_LIBRARY] = screen_make_root(parent);
    s_roots[SCR_NOWPLAYING] = screen_make_root(parent);
    s_roots[SCR_SETTINGS] = screen_make_root(parent);
    s_roots[SCR_SETTINGS_GROUP] = screen_make_root(parent);
    s_roots[SCR_SETTING_DETAIL] = screen_make_root(parent);
    s_roots[SCR_SEARCH] = screen_make_root(parent);
    s_roots[SCR_SAVER] = screen_make_root(parent);
    s_roots[SCR_QUICK] = screen_make_root(parent);
    s_roots[SCR_SONGINFO] = screen_make_root(parent);
    s_roots[SCR_NPMENU] = screen_make_root(parent);
    s_roots[SCR_TUNE] = screen_make_root(parent);
    s_roots[SCR_EQ] = screen_make_root(parent);
    s_roots[SCR_APPS] = screen_make_root(parent);
    s_roots[SCR_NPHUB] = screen_make_root(parent);
    s_roots[SCR_PLPICK] = screen_make_root(parent);
    s_roots[SCR_PLVIEW] = screen_make_root(parent);
    s_roots[SCR_WIFI] = screen_make_root(parent);
    s_roots[SCR_WIFI_INFO] = screen_make_root(parent);
    s_roots[SCR_BT] = screen_make_root(parent);
    s_roots[SCR_BT_INFO] = screen_make_root(parent);
    s_roots[SCR_WEATHER] = screen_make_root(parent);
    s_roots[SCR_LYRICS] = screen_make_root(parent);
    s_roots[SCR_COLORPICK] = screen_make_root(parent);
    s_roots[SCR_LASTFM] = screen_make_root(parent);
    s_roots[SCR_WORKMODE] = screen_make_root(parent);
    s_roots[SCR_DEBUG] = screen_make_root(parent);
    s_roots[SCR_SCAN] = screen_make_root(parent);
    s_roots[SCR_FONTPICK] = screen_make_root(parent);
    s_roots[SCR_QUEUE]    = screen_make_root(parent);

    /* depth scrim: a full-screen translucent-black overlay, created LAST so it sits above the
     * roots in sibling order; re-parented in z during a transition to dim the screen beneath the
     * moving panel. Hidden at rest. */
    s_scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, th_bg(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_COVER, 0);   /* object opacity (animated) gates visibility */
    lv_obj_set_style_opa(s_scrim, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    home_create(s_roots[SCR_HOME]);
    weather_app_create(s_roots[SCR_WEATHER]);
    lyrics_create(s_roots[SCR_LYRICS]);
    library_create(s_roots[SCR_LIBRARY]);
    ui_create(s_roots[SCR_NOWPLAYING]);
    settings_create(s_roots[SCR_SETTINGS]);
    settings_group_create(s_roots[SCR_SETTINGS_GROUP]);
    setting_detail_create(s_roots[SCR_SETTING_DETAIL]);
    search_create(s_roots[SCR_SEARCH]);
    saver_create(s_roots[SCR_SAVER]);
    quicksettings_create(s_roots[SCR_QUICK]);
    songinfo_create(s_roots[SCR_SONGINFO]);
    npmenu_create(s_roots[SCR_NPMENU]);
    tune_create(s_roots[SCR_TUNE]);
    eqcustom_create(s_roots[SCR_EQ]);
    colorpick_create(s_roots[SCR_COLORPICK]);
    modes_create(s_roots[SCR_WORKMODE]);
    debug_create(s_roots[SCR_DEBUG]);
    scanview_create(s_roots[SCR_SCAN]);
    fontpick_create(s_roots[SCR_FONTPICK]);
    queue_create(s_roots[SCR_QUEUE]);
    apps_create(s_roots[SCR_APPS]);
    nphub_create(s_roots[SCR_NPHUB]);
    plpick_create(s_roots[SCR_PLPICK]);
    plview_create(s_roots[SCR_PLVIEW]);
    wifi_create(s_roots[SCR_WIFI]);
    wifi_info_create(s_roots[SCR_WIFI_INFO]);
    bt_create(s_roots[SCR_BT]);
    bt_info_create(s_roots[SCR_BT_INFO]);
    lastfm_create(s_roots[SCR_LASTFM]);

    /* screen_home(), NOT screen_show(SCR_HOME): s_current already IS SCR_HOME here, and
     * screen_show() correctly treats "go to where you already are" as a no-op. Every root
     * is created visible and stacked, so something must explicitly raise Home and hide the
     * rest - that is what the transition inside screen_home() does. */
    screen_home();
}

void screen_set_anim(int on){ s_anim = on ? 1 : 0; }

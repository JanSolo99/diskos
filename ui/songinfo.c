/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Song Info: a metadata detail page (title/artist/album/format/rate/duration/
 * file). Opened by tapping the Now Playing album art; back-swipe / back returns.
 * Populated from the live track_state_t via songinfo_set(). */

enum { F_TITLE, F_ARTIST, F_ALBUM, F_FORMAT, F_RATE, F_DURATION, F_FILE, F_COUNT };
static const char *KEYS[F_COUNT] = { "Title","Artist","Album","Format","Sample Rate","Duration","File" };
static lv_obj_t *g_val[F_COUNT];

void songinfo_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, th_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Song Info");   /* standard shared header */

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 36, 70); lv_obj_set_size(list, 290, 252);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_bottom(list, 30, 0);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    for(int i=0;i<F_COUNT;i++){
        lv_obj_t *cell = lv_obj_create(list);
        lv_obj_remove_style_all(cell);
        lv_obj_set_width(cell, 278);
        lv_obj_set_height(cell, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *k = lv_label_create(cell);
        lv_label_set_text(k, KEYS[i]);
        lv_obj_set_style_text_font(k, th_font(14), 0);
        lv_obj_set_style_text_color(k, th_text3(), 0);

        lv_obj_t *v = lv_label_create(cell);
        lv_obj_set_width(v, 278);
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(v, ui_font_cjk(16), 0);   /* CJK titles/artist/album via Source Han Sans fallback (was montserrat_16 -> boxes) */
        lv_obj_set_style_text_color(v, th_text(), 0);
        lv_label_set_text(v, "-");
        g_val[i] = v;
    }
}

void songinfo_set(const track_state_t *st)
{
    if(!g_val[0]) return;
    if(!st || !st->have_track){
        for(int i=0;i<F_COUNT;i++) if(g_val[i]) lv_label_set_text(g_val[i], "-");
        return;
    }

    lv_label_set_text(g_val[F_TITLE],  st->title[0]?st->title:"Untitled");
    lv_label_set_text(g_val[F_ARTIST], st->artist[0]?st->artist:"-");
    lv_label_set_text(g_val[F_ALBUM],  st->album[0]?st->album:"-");

    /* format from extension only - this is the CONTAINER, not the codec. Don't claim
     * "ALAC" for .m4a: an m4a holds AAC (lossy) or ALAC (lossless) and the extension
     * can't distinguish them, so show the honest container label rather than guess. */
    char fmt[12]="-"; const char *ext=strrchr(st->path,'.');
    const char *sl=strrchr(st->path,'/');
    if(ext && (!sl || ext>sl) && ext[1]){   /* dot must follow the last '/' + have a suffix, else it's a dir dot */
        ext++; size_t i; for(i=0;i<sizeof(fmt)-1&&ext[i];i++) fmt[i]=toupper((unsigned char)ext[i]); fmt[i]=0; }
    lv_label_set_text(g_val[F_FORMAT], st->is_dsd?"DSD":fmt);

    char rate[24]="-";
    if(st->sample_rate>0){
        int khz=st->sample_rate/1000, frac=(st->sample_rate%1000)/100;
        if(frac) snprintf(rate,sizeof rate,"%d.%d kHz", khz, frac);
        else     snprintf(rate,sizeof rate,"%d kHz", khz);
    }
    lv_label_set_text(g_val[F_RATE], rate);

    char dur[16]="-";
    if(st->duration_ms>0){ int t=(int)(st->duration_ms/1000); snprintf(dur,sizeof dur,"%d:%02d", t/60, t%60); }
    lv_label_set_text(g_val[F_DURATION], dur);

    /* file basename */
    const char *base=strrchr(st->path,'/'); base = base?base+1:st->path;
    lv_label_set_text(g_val[F_FILE], base[0]?base:"-");
}

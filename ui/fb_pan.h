/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef FB_PAN_H
#define FB_PAN_H
#include "lvgl/lvgl.h"
/* Custom triple-buffered framebuffer pan display driver for /dev/fb0 (XRGB8888, 360x360, vyres=1080). */
lv_display_t *fbpan_create(const char *dev);
#endif

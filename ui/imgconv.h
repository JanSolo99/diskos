/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef IMGCONV_H
#define IMGCONV_H

/* PNG -> BMP, because the device's ffmpeg cannot read PNG.
 *
 * Measured on hardware 2026-09-06: the stock /usr/bin/ffmpeg (4.2.2) has image decoders
 * for bmp, mjpeg, mjpegb and webp - and NO png. So every track whose embedded cover is
 * a PNG fails with "Decoder (codec png) not found", which is most .m4a files and any
 * MP3 tagged with a PNG. ffmpeg lives in the read-only rootfs, so it cannot be fixed
 * there.
 *
 * It CAN still copy the cover out losslessly (-c:v copy needs no decoder), and it can
 * read a BMP back in. So the gap is exactly one step - decode the PNG - and we can do
 * that ourselves: LVGL vendors lodepng, and enabling LV_USE_LODEPNG links it.
 *
 * Everything downstream is unchanged. Both callers hand ffmpeg a BMP and the existing
 * scale/blur/cache pipeline runs exactly as before. */

/* 1 if the file starts with the PNG signature. Cheap - reads 8 bytes. */
int img_is_png(const char *path);

/* Decode `png` and write it as a 24-bit BMP at `bmp`.
 *
 * max_pixels caps the decode: lodepng expands to RGBA, so a 3000x3000 cover would want
 * 36 MB on a device with ~48 MB available and an audio engine to keep fed. The
 * dimensions are read from the IHDR header BEFORE decoding, so an oversized image is
 * refused without ever allocating for it.
 *
 * Returns 0 on success. */
int img_png_to_bmp(const char *png, const char *bmp, unsigned max_pixels);

#endif

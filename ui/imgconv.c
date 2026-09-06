/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "imgconv.h"
#include "lvgl/src/libs/lodepng/lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* See imgconv.h for why this exists: the device's ffmpeg has no PNG decoder, so we
 * decode PNG ourselves and hand ffmpeg a BMP, which it can read. */

int img_is_png(const char *path){
    if(!path) return 0;
    FILE *f = fopen(path,"rb"); if(!f) return 0;
    unsigned char m[8];
    size_t n = fread(m,1,8,f);
    fclose(f);
    static const unsigned char SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    return n == 8 && memcmp(m,SIG,8) == 0;
}

/* IHDR is fixed: signature(8) + length(4) + "IHDR"(4) + width(4) + height(4), all
 * big-endian. Reading it costs 24 bytes and is what lets an oversized cover be refused
 * BEFORE lodepng allocates w*h*4 for it. */
static int png_size(const char *path, unsigned *w, unsigned *h){
    FILE *f = fopen(path,"rb"); if(!f) return -1;
    unsigned char b[24];
    size_t n = fread(b,1,sizeof b,f);
    fclose(f);
    if(n != sizeof b || memcmp(b+12,"IHDR",4) != 0) return -1;
    *w = ((unsigned)b[16]<<24)|((unsigned)b[17]<<16)|((unsigned)b[18]<<8)|b[19];
    *h = ((unsigned)b[20]<<24)|((unsigned)b[21]<<16)|((unsigned)b[22]<<8)|b[23];
    return (*w && *h) ? 0 : -1;
}

/* 24-bit BGR, bottom-up, no palette - the plainest BMP there is, because the only
 * consumer is ffmpeg's bmp decoder and anything fancier is surface for no gain. */
static int write_bmp24(const char *path, const unsigned char *rgba, unsigned w, unsigned h){
    unsigned rowlen = w*3;
    unsigned pad    = (4 - (rowlen & 3)) & 3;
    unsigned stride = rowlen + pad;
    unsigned long dataSize = (unsigned long)stride * h;
    unsigned long fileSize = 54 + dataSize;

    FILE *f = fopen(path,"wb"); if(!f) return -1;

    unsigned char hdr[54];
    memset(hdr,0,sizeof hdr);
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=(unsigned char)(fileSize); hdr[3]=(unsigned char)(fileSize>>8);
    hdr[4]=(unsigned char)(fileSize>>16); hdr[5]=(unsigned char)(fileSize>>24);
    hdr[10]=54;                       /* pixel data offset */
    hdr[14]=40;                       /* BITMAPINFOHEADER */
    hdr[18]=(unsigned char)(w); hdr[19]=(unsigned char)(w>>8);
    hdr[20]=(unsigned char)(w>>16); hdr[21]=(unsigned char)(w>>24);
    hdr[22]=(unsigned char)(h); hdr[23]=(unsigned char)(h>>8);
    hdr[24]=(unsigned char)(h>>16); hdr[25]=(unsigned char)(h>>24);
    hdr[26]=1;                        /* planes */
    hdr[28]=24;                       /* bpp */
    hdr[34]=(unsigned char)(dataSize); hdr[35]=(unsigned char)(dataSize>>8);
    hdr[36]=(unsigned char)(dataSize>>16); hdr[37]=(unsigned char)(dataSize>>24);
    if(fwrite(hdr,1,54,f) != 54){ fclose(f); return -1; }

    unsigned char *row = malloc(stride);
    if(!row){ fclose(f); return -1; }
    memset(row,0,stride);

    /* BMP rows run bottom-up, and lodepng gives RGBA top-down. Alpha is composited on
     * BLACK rather than dropped: a cover with a transparent corner would otherwise get
     * whatever the uninitialised byte held, and black matches the panel behind it. */
    for(unsigned y = 0; y < h; y++){
        const unsigned char *src = rgba + (size_t)(h - 1 - y) * w * 4;
        for(unsigned x = 0; x < w; x++){
            unsigned a = src[3];
            row[x*3+0] = (unsigned char)((src[2]*a)/255);   /* B */
            row[x*3+1] = (unsigned char)((src[1]*a)/255);   /* G */
            row[x*3+2] = (unsigned char)((src[0]*a)/255);   /* R */
            src += 4;
        }
        if(fwrite(row,1,stride,f) != stride){ free(row); fclose(f); return -1; }
    }
    free(row);
    if(fclose(f) != 0) return -1;
    return 0;
}

int img_png_to_bmp(const char *png, const char *bmp, unsigned max_pixels){
    if(!png || !bmp) return -1;
    if(!img_is_png(png)) return -1;

    unsigned w=0, h=0;
    if(png_size(png,&w,&h) != 0) return -1;
    if(max_pixels && (unsigned long)w*h > max_pixels) return -1;   /* refuse before allocating */

    unsigned char *rgba = NULL;
    unsigned dw=0, dh=0;
    if(lodepng_decode32_file(&rgba,&dw,&dh,png) != 0 || !rgba) return -1;

    int rc = write_bmp24(bmp,rgba,dw,dh);
    free(rgba);                       /* lodepng allocates with malloc */
    if(rc != 0) remove(bmp);
    return rc;
}

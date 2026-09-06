/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "txtfold.h"
#include <stdint.h>
#include <stddef.h>

/* U+00C0..U+00FF - Latin-1 Supplement letters (the accented Latin a tagger writes).
 * Indexed by cp - 0xC0. Two bytes of UTF-8 in, at most two ASCII chars out. */
static const char *const L1[64] = {
    "A","A","A","A","A","A","AE","C",   "E","E","E","E",  "I","I","I","I",
    "D","N","O","O","O","O","O","x",    "O","U","U","U",  "U","Y","TH","ss",
    "a","a","a","a","a","a","ae","c",   "e","e","e","e",  "i","i","i","i",
    "d","n","o","o","o","o","o","/",    "o","u","u","u",  "u","y","th","y",
};

/* U+0100..U+017F - Latin Extended-A (Polish, Czech, Turkish, Baltic...).
 * Indexed by cp - 0x100. */
static const char *const LA[128] = {
    "A","a","A","a","A","a",             /* 0100 Ā ā Ă ă Ą ą */
    "C","c","C","c","C","c","C","c",     /* 0106 Ć ć Ĉ ĉ Ċ ċ Č č */
    "D","d","D","d",                     /* 010E Ď ď Đ đ */
    "E","e","E","e","E","e","E","e","E","e", /* 0112 Ē ē Ĕ ĕ Ė ė Ę ę Ě ě */
    "G","g","G","g","G","g","G","g",     /* 011C Ĝ ĝ Ğ ğ Ġ ġ Ģ ģ */
    "H","h","H","h",                     /* 0124 Ĥ ĥ Ħ ħ */
    "I","i","I","i","I","i","I","i","I","i", /* 0128 Ĩ ĩ Ī ī Ĭ ĭ Į į İ ı */
    "IJ","ij",                           /* 0132 Ĳ ĳ */
    "J","j",                             /* 0134 Ĵ ĵ */
    "K","k","k",                         /* 0136 Ķ ķ ĸ */
    "L","l","L","l","L","l","L","l","L","l", /* 0139 Ĺ ĺ Ļ ļ Ľ ľ Ŀ ŀ Ł ł */
    "N","n","N","n","N","n","n","N","n", /* 0143 Ń ń Ņ ņ Ň ň ŉ Ŋ ŋ */
    "O","o","O","o","O","o",             /* 014C Ō ō Ŏ ŏ Ő ő */
    "OE","oe",                           /* 0152 Œ œ */
    "R","r","R","r","R","r",             /* 0154 Ŕ ŕ Ŗ ŗ Ř ř */
    "S","s","S","s","S","s","S","s",     /* 015A Ś ś Ŝ ŝ Ş ş Š š */
    "T","t","T","t","T","t",             /* 0162 Ţ ţ Ť ť Ŧ ŧ */
    "U","u","U","u","U","u","U","u","U","u","U","u", /* 0168 Ũ ũ Ū ū Ŭ ŭ Ů ů Ű ű Ų ų */
    "W","w",                             /* 0174 Ŵ ŵ */
    "Y","y","Y",                         /* 0176 Ŷ ŷ Ÿ */
    "Z","z","Z","z","Z","z",             /* 0179 Ź ź Ż ż Ž ž */
    "s",                                 /* 017F ſ */
};

/* Everything else worth spelling out. "" means "delete the codepoint" (the
 * zero-width and bidi marks, which otherwise draw a box for nothing). */
static const char *fold_cp(uint32_t cp)
{
    if(cp >= 0xC0 && cp <= 0xFF)   return L1[cp - 0xC0];
    if(cp >= 0x100 && cp <= 0x17F) return LA[cp - 0x100];
    switch(cp){
        case 0xA0:   return " ";     /* no-break space */
        case 0xAA:   return "a";     /* feminine ordinal */
        case 0xB5:   return "u";     /* micro sign (mu-Ziq et al) */
        case 0xBA:   return "o";     /* masculine ordinal */
        case 0xAB:   return "\"";    /* << */
        case 0xBB:   return "\"";    /* >> */
        case 0xB0:   return "";      /* degree sign - wttr.in sends one and no font we
                                      * load has it, so "20C" beats "20<box>C". */
        case 0xB4:   return "'";     /* acute accent used as an apostrophe */
        case 0x2018: case 0x2019:    /* ' ' - THE apostrophe bug */
        case 0x201A: case 0x201B:
        case 0x2032:
        /* Apostrophe LOOKALIKES that are not punctuation at all. Seen on a real track
         * 2026-09-06: "8. (SALTILLO)Cosmic(SALTILLO).m4a" - U+A78C, a Latin LETTER that
         * merely looks like an apostrophe. Taggers and rippers substitute these on
         * purpose because a real ' is awkward in a filename, so they turn up in exactly
         * the titles a music player has to draw. */
        case 0xA78B: case 0xA78C:  /* LATIN CAPITAL/SMALL LETTER SALTILLO */
        case 0x02B9:               /* MODIFIER LETTER PRIME */
        case 0x02BB: case 0x02BC:  /* MODIFIER LETTER TURNED COMMA / APOSTROPHE */
        case 0x02BD: case 0x02BE:  /* MODIFIER LETTER REVERSED COMMA / RIGHT HALF RING */
        case 0x02BF: case 0x02C8:  /* MODIFIER LETTER LEFT HALF RING / VERTICAL LINE */
        case 0x055A:               /* ARMENIAN APOSTROPHE */
        case 0x2035:               /* REVERSED PRIME */
        case 0xFF07:               /* FULLWIDTH APOSTROPHE */
                                   return "'";
        case 0x201C: case 0x201D:    /* " " */
        case 0x201E: case 0x201F:
        case 0x2033:
        case 0x02BA:               /* MODIFIER LETTER DOUBLE PRIME */
        case 0x2036:               /* REVERSED DOUBLE PRIME */
        case 0x3003:               /* DITTO MARK */
        case 0xFF02:               /* FULLWIDTH QUOTATION MARK */
                                   return "\"";
        case 0x2010: case 0x2011:    /* hyphens */
        case 0x2012: case 0x2013:    /* en dash */
        case 0x2014: case 0x2015:    /* em dash */
        case 0x2212:               return "-";   /* minus sign */
        case 0x2022: case 0x2023:  return "*";   /* bullet */
        case 0x2026:               return "...";
        case 0x2039:               return "<";
        case 0x203A:               return ">";
        case 0x2044:               return "/";
        case 0x2122:               return "TM";
        case 0x20AC:               return "EUR";
        default: break;
    }
    /* the assorted Unicode spaces (en/em/thin/figure/hair...) and the zero-width
     * + bidi marks, which are invisible in the source but a box on screen */
    if(cp >= 0x2000 && cp <= 0x200A) return " ";
    if(cp >= 0x200B && cp <= 0x200F) return "";
    if(cp >= 0x202A && cp <= 0x202E) return "";
    if(cp == 0xFEFF)                 return "";   /* BOM / zero-width no-break space */
    return NULL;                                  /* unmapped: leave the bytes alone */
}

void txt_fold_ascii(char *s)
{
    if(!s) return;
    unsigned char *r = (unsigned char *)s;   /* read cursor  */
    unsigned char *w = (unsigned char *)s;   /* write cursor - never passes r */

    while(*r){
        if(*r < 0x80){ *w++ = *r++; continue; }   /* plain ASCII, the common case */

        /* decode one UTF-8 sequence; a malformed lead/continuation byte is copied
         * through verbatim rather than resynchronised - we are a display filter,
         * not a validator, and mangling unknown bytes helps nobody. */
        uint32_t cp; int len;
        if((*r & 0xE0) == 0xC0)      { cp = *r & 0x1Fu; len = 2; }
        else if((*r & 0xF0) == 0xE0) { cp = *r & 0x0Fu; len = 3; }
        else if((*r & 0xF8) == 0xF0) { cp = *r & 0x07u; len = 4; }
        else                         { *w++ = *r++; continue; }

        int i;
        for(i = 1; i < len; i++) if((r[i] & 0xC0) != 0x80) break;
        if(i != len){ *w++ = *r++; continue; }    /* truncated - pass the lead byte through */
        for(i = 1; i < len; i++) cp = (cp << 6) | (uint32_t)(r[i] & 0x3F);

        const char *rep = fold_cp(cp);
        if(rep){
            /* every mapping is <= len bytes (see txtfold.h), so w cannot overtake r */
            while(*rep) *w++ = (unsigned char)*rep++;
            r += len;
        } else {
            for(i = 0; i < len; i++) *w++ = *r++;  /* CJK and friends: untouched */
        }
    }
    *w = 0;
}

# Bundled license texts

This directory carries the full license texts for the third-party components the diskOS installer
bundles. See `../NOTICE.md` for the component→license mapping and the static-linking obligations.

| Component (bundled) | License | Full text |
|---|---|---|
| usbboot | GPL-2.0-or-later | [`GPL-2.0.txt`](GPL-2.0.txt) |
| squashfs-tools 4.6.1 | GPL-2.0 | [`GPL-2.0.txt`](GPL-2.0.txt) |
| liblzo2 (static in mksquashfs/unsquashfs) | GPL-2.0-or-later | [`GPL-2.0.txt`](GPL-2.0.txt) |
| libusb-1.0.27 (static in usbboot) | LGPL-2.1 | [`LGPL-2.1.txt`](LGPL-2.1.txt) |
| glibc (static in usbboot/mksquashfs/unsquashfs) | LGPL-2.1-or-later | [`LGPL-2.1.txt`](LGPL-2.1.txt); corresponding source in `../corresponding-source/glibc_2.39*` (see `../NOTICE.md` for the relink path) |
| PyInstaller bootloader (only for a self-built onefile) | GPL-2.0 + bootloader exception | [`GPL-2.0.txt`](GPL-2.0.txt) + exception note in `../NOTICE.md` |
| zlib / liblzma (static in squashfs-tools) | zlib / public-domain | permissive notices travel with the squashfs-tools sources the build script fetches |
| pyusb | BSD-3-Clause | [`BSD-3-Clause-pyusb.txt`](BSD-3-Clause-pyusb.txt) (verbatim from pyusb 1.3.1) |
| pycryptodome | BSD-2-Clause + public-domain | [`pycryptodome-LICENSE.txt`](pycryptodome-LICENSE.txt) (verbatim from pycryptodome 3.23.0) |
| CPython | Python Software Foundation License | [`PSF-Python.txt`](PSF-Python.txt) (verbatim from the bundled CPython) |
| LVGL (diskOS UI) | MIT | [`MIT-LVGL.txt`](MIT-LVGL.txt) (verbatim from `../ui/lvgl/LICENCE.txt`) |
| JSMN (diskOS UI JSON parser) | MIT | [`MIT-JSMN.txt`](MIT-JSMN.txt) (verbatim from `../ui/jsmn.h`, © 2010 Serge Zaitsev) |
| Font Awesome Free glyphs (via LVGL `LV_SYMBOL_*`) | OFL-1.1 (fonts) + CC-BY-4.0 (icons) | [`OFL-1.1-FontAwesome.txt`](OFL-1.1-FontAwesome.txt) (Fonticons' own LICENSE.txt: full OFL-1.1 + MIT) + [`CC-BY-4.0.txt`](CC-BY-4.0.txt) |
| musl libc (static in diskOS UI `mq_ui`) | MIT | [`MIT-musl.txt`](MIT-musl.txt) |
| SQLite (amalgamation in `mq_ui`) | Public domain | https://sqlite.org/copyright.html (no license text required) |
| QR Code generator, Nayuki (via LVGL, in `mq_ui`) | MIT | [`MIT-qrcodegen.txt`](MIT-qrcodegen.txt) (verbatim from the LVGL source header) |
| TJpgDec, ChaN (via LVGL, in `mq_ui`) | BSD-style | [`LICENSE-TJpgDec.txt`](LICENSE-TJpgDec.txt) (verbatim from the LVGL source header) |
| Montserrat font (via LVGL built-in fonts, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-Montserrat.txt`](OFL-1.1-Montserrat.txt) |
| Source Han Sans SC (CJK fallback font in `mq_ui`) | OFL-1.1 | [`OFL-1.1-SourceHanSans.txt`](OFL-1.1-SourceHanSans.txt) |
| MD5, Peslyak/"Solar Designer" (in `mq_ui`) | Public domain | in `../ui/md5.c`; no license text required |
| dropbearmulti (Dropbear SSH 2022.83, Debug Mode) | MIT-style | [`LICENSE-dropbear.txt`](LICENSE-dropbear.txt) (verbatim upstream) |
| disc_spl_lpddr3.bin (X2000 stage-1 SPL) | GPL-2.0 | [`GPL-2.0.txt`](GPL-2.0.txt); corresponding source in `../spl-src/` (see `../SPL_SOURCE.md`) |

All texts above are included **verbatim from the actual upstream sources** (not hand-written), with
their real copyright lines. **Copyleft texts (GPL-2.0, LGPL-2.1)** carry the redistribution
obligations (corresponding source + relink path; see `../NOTICE.md`). The only text not vendored here
is zlib/liblzma's permissive notice, which travels inside the squashfs-tools source the build script
fetches.

## diskOS's own license
The **Python installer, build scripts, and docs** are licensed **MIT** - `Copyright (c) 2026 diskOS
contributors`. See [`../LICENSE`](../LICENSE). The **diskOS UI source** (`ui/`, built to
`payload/mq_ui`) is licensed **GPL-3.0-or-later** (`GPL-3.0.txt`, `../ui/COPYING`; © diskOS
contributors) - deliberately separate from the MIT tooling so UI forks stay open. Its embedded
third-party notices - LVGL (MIT), SQLite (public domain), JSMN (MIT), md5 (public domain), and
Montserrat / Source Han Sans / Font Awesome fonts (OFL-1.1) - ship in this directory and in
`../ui/licenses/`.

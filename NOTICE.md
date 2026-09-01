# diskOS installer - third-party components & licenses

The diskOS installer ships the third-party programs and libraries below. Each is invoked as a
separate program or loaded as a library; this document provides the required attribution and points
to the corresponding source that ships alongside the binaries. The full verbatim license texts are in
[`licenses/`](licenses/); this file is the component-to-license index.

## Bundled native programs (called as subprocesses - mere aggregation)

| Component | Purpose | License | Source |
|---|---|---|---|
| **usbboot** | Ingenic X2000 mask-ROM USB loader (the flasher front-end) | **GPL-2.0-or-later** - © 2021 Aidan MacDonald, © 2015 Amaury Pouly | included: `src/usbboot/` (built from this) |
| **mksquashfs / unsquashfs** (squashfs-tools **4.6.1**) | build/extract the rootfs image | **GPL-2.0** | https://github.com/plougher/squashfs-tools (v4.6.1) |
| **dropbearmulti** (Dropbear SSH **2022.83**) | embedded in the diskOS image; started by the opt-in Debug Mode for SSH-over-WiFi | **MIT-style** (© 2002-2020 Matt Johnston; components under MIT/BSD/public-domain) - text in `licenses/LICENSE-dropbear.txt` | https://matt.ucc.asn.au/dropbear/dropbear.html (v2022.83; **predates the CVE-2023-48795 "Terrapin" Strict-KEX mitigation - an update is planned**. Debug Mode SSH is opt-in and short-lived, and Dropbear's Terrapin exposure is limited.) |

`usbboot` and squashfs-tools are **GPL-2.0**, so anyone redistributing them must make their
corresponding source available and include the GPL-2.0 license text. (`dropbearmulti` is MIT-style -
no source-availability obligation, only its license text as noted above.) **The complete
corresponding source ships in-tree and in every release tarball**, so it travels from the same place
as the binaries (GPL-2.0 §3(a), plus the "same place" provision in §3's final paragraph):
`usbboot`'s own source is `src/usbboot/usbboot.c`;
**squashfs-tools 4.6.1** source is `corresponding-source/squashfs-tools_4.6.1*`. Both are rebuilt by
`build/build-usbboot-static.sh` / `build/build-squashfs-static.sh`.

**Static linking (Linux):** the prebuilt Linux `usbboot`, `mksquashfs`, and `unsquashfs` are shipped
**fully statically linked** (built with `gcc -static` on a glibc host). They therefore statically
incorporate:
- **libusb-1.0.27 (LGPL-2.1)** - in `usbboot`
- **liblzo2 2.10 (GPL-2.0-or-later)**, **zlib**, **liblzma** (permissive) - in squashfs-tools
- the **GNU C Library (glibc, LGPL-2.1-or-later)** - the host's system C runtime, in all three

Because liblzo2 is GPL, the resulting `mksquashfs`/`unsquashfs` binaries are effectively GPL-2.0. The
corresponding source for the statically-linked **GPL/LGPL** libraries is provided so their §3/§6
obligations are met:
- **liblzo2** (`corresponding-source/lzo2_2.10*`) and **libusb-1.0** (`corresponding-source/libusb-1.0_1.0.27*`)
  ship in-tree.
- **glibc** (statically linked, so it travels inside the binary): the **complete corresponding source**
  is vendored in `corresponding-source/glibc_2.39*` (the exact version the shipped binaries were built
  against). Because it is statically linked it does not qualify for the "system library" exception, so
  its source ships alongside the binaries like the others.

**Relink path (satisfies LGPL-2.1 §6 for libusb *and* glibc):** we ship the **complete source** of
`usbboot` (`src/usbboot/usbboot.c`) and squashfs-tools (`corresponding-source/squashfs-tools_4.6.1*`)
plus the exact build recipes (`build/build-usbboot-static.sh`, `build/build-squashfs-static.sh`). A
user can therefore rebuild and relink these tools against a **modified** libusb, liblzo2, **or glibc**
of their choosing - the freedom §6 exists to protect. See
[`corresponding-source/README.md`](corresponding-source/README.md).

**Python runtime + libraries - NOT redistributed by this project.** The installer runs **from
source** with **your own Python**. The two Python dependencies (`pyusb`, `pycryptodome`) are fetched
from PyPI by *your* `pip` into a local `.venv` (created by `./install.sh`); CPython, `tkinter`, and
their transitive native libraries (OpenSSL, Tk/Tcl, the X11 stack, freetype, fontconfig, zlib, etc.)
all come from **your system's Python** - this project ships none of them, so their redistribution
notices are not our obligation. Their license texts (for reference) are still listed in
[`licenses/README.md`](licenses/README.md).

> If you instead build a self-contained PyInstaller onefile yourself (`build/build.sh`), *that*
> binary embeds CPython + ~90 native libraries and you become the redistributor of them - see
> [`licenses/THIRD_PARTY_BUNDLED.md`](licenses/THIRD_PARTY_BUNDLED.md) for the enumeration and
> obligations. The **published release does not do this**; it distributes source + the native flash
> tools below.

## Bundled device blobs

| Component | Purpose | Notes |
|---|---|---|
| **my_write5_dram.bin** | the bad-block-aware DRAM NAND writer run on-device | diskOS project code - source: `flash/my_write5.c` |
| **disc_spl_lpddr3.bin** | Ingenic X2000 USB stage-1 SPL (DRAM init) | **GPL-2.0** - built from source: Ingenic-community/uboot-xburst `1060b516` + our LPDDR3 patches. Corresponding source in `spl-src/`; see `SPL_SOURCE.md`. (No vendor/USBCloner binary is redistributed.) |

## Python dependencies (fetched by your pip, not redistributed by us)

`./install.sh` installs these from PyPI into a local `.venv`; **CPython** comes from your own system
Python. This project does not ship any of them, so their license texts are listed here only for
reference. (**PyInstaller** is used *only* if you build the optional onefile yourself; it is not
involved in the source release.)

| Component | License |
|---|---|
| **pyusb** (USB device detection) | BSD-3-Clause |
| **pycryptodome** (AES-decrypt of FiiO OTA chunks) | BSD-2-Clause + public-domain (Unlicense) |
| **CPython** (your system's) | Python Software Foundation License |
| **PyInstaller** runtime bootloader (only for a self-built onefile) | GPL-2.0 **with a bootloader exception** permitting distribution of the packaged app under any license |

## Bundled shared libraries (macOS build)

| Component | License |
|---|---|
| **libusb-1.0** (bundled next to usbboot on macOS) | LGPL-2.1 |

## diskOS's own code

- **The Python installer, build scripts, and docs** in this repo are licensed **MIT** -
  `Copyright (c) 2026 diskOS contributors`. See `LICENSE`.
- **The diskOS UI source** (`ui/`, built to `payload/mq_ui`, static-musl MIPS) is licensed
  **GPL-3.0-or-later** (`ui/COPYING`, `licenses/GPL-3.0.txt`; © diskOS contributors) - deliberately
  separate from the MIT tooling above so UI forks stay open. It bundles LVGL (MIT), SQLite (public
  domain), jsmn (MIT), md5 (public domain), and Montserrat / Source Han Sans / Font Awesome fonts
  (all OFL-1.1); see `ui/README.md` and `ui/licenses/`.

  The built `mq_ui` binary statically incorporates these third-party components, whose licenses
  apply to the shipped binary (and whose source ships in `ui/` / `ui/lvgl/`):

  | Embedded in `mq_ui` | License | Text / source |
  |---|---|---|
  | **musl libc** (static C runtime) | MIT | `licenses/MIT-musl.txt`; source: https://musl.libc.org/ |
  | **LVGL** (UI toolkit) | MIT | `licenses/MIT-LVGL.txt` |
  | **JSMN** (JSON parser) | MIT | `licenses/MIT-JSMN.txt` |
  | **SQLite** (amalgamation) | Public domain | https://sqlite.org/copyright.html |
  | **MD5** (Peslyak/Solar Designer) | Public domain | `ui/md5.c` |
  | **QR Code generator** (Nayuki `qrcodegen`) | MIT | `licenses/MIT-qrcodegen.txt` |
  | **TJpgDec** (tiny JPEG decoder, via LVGL) | BSD-style (ChaN) | `licenses/LICENSE-TJpgDec.txt` |
  | **Montserrat** font (via LVGL built-in fonts) | SIL OFL 1.1 | `licenses/OFL-1.1-Montserrat.txt` |
  | **Source Han Sans SC** (CJK fallback font, subset) | SIL OFL 1.1 | `licenses/OFL-1.1-SourceHanSans.txt` |
  | **Font Awesome Free** glyphs (via LVGL `LV_SYMBOL_*`) | OFL 1.1 (fonts) + CC-BY 4.0 (icons) | `licenses/OFL-1.1-FontAwesome.txt`, `licenses/CC-BY-4.0.txt` |

## NOT distributed by this installer

- **FiiO's stock rootfs** - you supply your own official firmware; the installer only reads it locally
  and never redistributes it.
- **ffmpeg** - used at runtime for album-art decoding, but it is part of FiiO's stock firmware already
  on the device (`/usr/bin/ffmpeg`); the installer does **not** ship it.

---

### Compliance checklist (done)
- ✅ Full license texts shipped in [`licenses/`](licenses/) (GPL-2.0, LGPL-2.1, BSD, PSF, LVGL-MIT,
  dropbear, Font Awesome OFL/CC-BY) - see [`licenses/README.md`](licenses/README.md).
- ✅ diskOS installer/tooling license: **MIT** (`LICENSE`). The diskOS **UI source** is published
  under **GPL-3.0-or-later** in `ui/` (`ui/COPYING`, `licenses/GPL-3.0.txt`) with its embedded
  LVGL/SQLite/jsmn/Font Awesome/Montserrat/Source Han Sans notices shipped.
- ✅ Exact upstream versions pinned and their **complete corresponding source shipped in-tree** for the
  GPL/LGPL native binaries: squashfs-tools 4.6.1, liblzo2 2.10, libusb-1.0 1.0.27, the SPL, and
  usbboot - in [`corresponding-source/`](corresponding-source/) and [`spl-src/`](spl-src/). Source
  travels from the same place as the binaries; no separate written offer is needed.

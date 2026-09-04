# diskOS UI (`mq_ui`) - native player interface for the FiiO Snowsky Disc

The on-device UI for [diskOS](https://github.com/b0hemia/diskos): a custom, round-screen music-player
interface for the **FiiO Snowsky Disc** ($80 round-display DAP; Ingenic X2000 / MIPS32, Linux 4.4).
It replaces the stock `mq_ui` while keeping the stock `mq_player` audio engine untouched, talking to
it over the device's POSIX message-queue IPC.

Built with **LVGL 9.2.2** (software renderer) on a static-musl mipsel toolchain. This produces the
`mq_ui` binary that the diskOS installer bakes into the flashed image.

> **License:** this UI is **GPL-3.0-or-later** (see [`COPYING`](COPYING)). That is a deliberate,
> different choice from the diskOS *installer/tooling*, which is MIT - the UI is the part meant to be
> forked and kept open. Bundled third-party components keep their own licenses (see [License](#license)).

## Hardware target
- **Display:** `/dev/fb0`, 360×360, 32bpp XRGB8888, triple-buffered pan-flip; panel mounted 180°
  (reverse-copied in the flush callback, `fb_pan.c`).
- **Touch:** `cst816t` capacitive controller on `/dev/input/event1` (multitouch evdev), calibrated
  for the 180° panel. Physical keys on `/dev/input/event0`.
- **IPC:** POSIX message queues `/ui` (state/metadata player→UI) and `/player` (commands UI→player);
  ASCII framing `TAG(4hex)+LEN(4hex)+VALUE`, values often JSON.
- **Library:** on-device SQLite `song.db` (bundled SQLite amalgamation, compiled in-process).

## Source layout
| Area | Files |
|---|---|
| Core / loop / nav | `main.c`, `screenmgr.c` / `screens.h`, `fb_pan.c`, `ipc.c` / `ipc.h`, `config.c` |
| Look & feel | `theme.c` / `theme.h` (palette + font tokens), `fontpick.c`, `anim.c` |
| Now Playing + art | `ui.c` / `ui.h`, `art.c` / `art.h`, `artcache.c`, `saver.c`, `songinfo.c`, `npmenus.c` |
| Library / browse | `home.c`, `library.c`, `musicdb.c`, `search.c`, `playlistview.c`, `scanner.c`, `scanview.c` |
| Settings / features | `settings.c`, `sysconfig.c` (stock SYSCONFIG access), `eqcustom.c`, `quicksettings.c`, `modes.c`, `kbinput.c`, `colorpick.c`, `toast.c`, `apps.c`, `debug_ui.c`, `fwcaps.c` |
| Shared utility | `fileutil.c` / `fileutil.h` (atomic, durable file copy) |
| Connectivity | `wifi.c`, `bt.c`, `weather.c`, `lyrics.c`, `lastfm.c` / `lastfm_ui.c` |
| Bundled third-party | `sqlite3.c` (public domain), `jsmn.h` (MIT), `md5.c` (public domain), `font_*.c` (generated glyph data) |
| Dev/diagnostic tools | `fbshot.c`, and other small on-device helpers (not part of the shipped UI) |

## Theming and fonts

Every surface colour and every typeface in the UI comes from a token in `theme.c` rather than a
hard-coded value, so the whole interface can be re-skinned from one place.

- **Palettes.** `th_bg()`, `th_card()`, `th_text()` and friends are named by ROLE, not by colour, so
  a token means the same thing in both the dark and light palettes. Adding a third palette is a
  struct literal. Anything that must stay a fixed colour - the Last.fm red, the iOS state blue, the
  user's accent - deliberately stays a literal.
- **Fonts.** Call sites use `th_font(size)`, never `&lv_font_montserrat_NN`. That indirection is what
  lets a user-supplied `.ttf`/`.otf` from the SD card replace the face everywhere, and lets Text Size
  shift the whole type ladder together instead of flattening it. A custom face is chained in front of
  the built-in one, so `LV_SYMBOL_*` icons (private-use codepoints that only Montserrat carries)
  keep rendering.
- Both are resolved once, when each screen is built, so changing either re-execs `mq_ui`
  (`ui_restart_self()`). That restarts the UI only - `mq_player`, the music and the SD card are
  untouched, and the pid is unchanged.

If you add a screen, paint it from the tokens. A hard-coded `lv_color_hex()` will look correct in
whichever palette you developed against and wrong in the other.

## Host tests

Some behaviour can only be checked by running it, and needs no device - see [`tests/`](tests/):

```sh
make fontcheck && ./fontcheck    # the user-font path: LVGL's VFS, the icon fallback chain,
                                 # per-size caching, and cold boot with the SD card absent
```

The scanner has an in-tree harness instead: build `scanner.c` with `-DSCANNER_TEST` and point
`-DSCAN_ROOT` / `-DDB_PATH` at a scratch directory and database.

A quick way to check the whole tree still compiles and links, without the cross toolchain, is to
build it for the host: `make CROSS= CFLAGS="-O0 -std=gnu11 -pthread -D_GNU_SOURCE
-DLV_CONF_INCLUDE_SIMPLE -I. -w" LDLIBS="-lm -lrt -lcrypt"`. The result will not run (it wants the
Disc's framebuffer and `mq_player`), but it catches missing symbols and bad prototypes in seconds.

## Build
Requires:
- A static **mipsel-linux-musl** cross toolchain (e.g. from [musl.cc](https://musl.cc) or crosstool-NG).
- **LVGL 9.2.2** - **vendored in `./lvgl`** (MIT). It is *lightly customized* (a merged Source Han
  Sans + Font Awesome CJK font, plus font/TJPG tweaks), so an upstream v9.2.2 clone will **not**
  build this tree - use the vendored copy that ships here.

```sh
# toolchain on PATH as mipsel-linux-musl-gcc, LVGL at ./lvgl:
make

# or point at your toolchain / LVGL explicitly:
make CROSS=/opt/x-tools/mipsel-linux-musl/bin/mipsel-linux-musl-  LVGL=/path/to/lvgl
```

`make` produces the static `mq_ui` binary. For a persistent local setup, put your overrides in a
`config.mk` (gitignored) instead of passing them each time:

```make
CROSS := /opt/x-tools/mipsel-linux-musl/bin/mipsel-linux-musl-
LVGL  := /path/to/lvgl
```

## Build in Docker (reproducible, recommended)

If your host toolchain fights the build, use the pinned container - it locks the exact
mipsel-linux-musl toolchain (by SHA-256) the binary is built with, so you don't have to source one:

```sh
docker build -t diskos-ui-builder .                                  # from this ui/ directory
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" diskos-ui-builder
```

The static `mq_ui` lands in the current directory. LVGL is vendored at `lvgl/`, so nothing else is
needed. The `-u` flag keeps the build outputs owned by you, not root.

The toolchain host binaries are 32-bit x86, so the image is `linux/amd64`. On Apple Silicon / ARM,
Docker runs it under emulation automatically (slower, but it works).

## Troubleshooting

- **`musl` / SQLite `fcntl64` errors:** this is a **toolchain mismatch**, not a flag problem. The
  bundled `sqlite3.c` compiles cleanly against the pinned musl mipsel GCC 11.2.x (verified with and
  without `_GNU_SOURCE` / `_FILE_OFFSET_BITS=64`). If your cross-compiler errors on `fcntl64`, it's a
  different/mixed toolchain - use the Docker builder above, which pins the working one. (Note the
  Makefile compiles `sqlite3.c` with a **dedicated rule** that omits `_GNU_SOURCE`; keep that if you
  build by hand.)
- **`Makefile:NN: *** missing separator`:** a recipe line (the command under a rule) must start with a
  real **Tab**, not spaces - an editor or a copy-paste turned the leading Tab into spaces. Re-indent
  that line with a single Tab.

## Deploy / flash
`mq_ui` is normally delivered by the diskOS **installer**, which bakes it into a rootfs image and
flashes it over the chip's mask-ROM USB mode - see the [main diskOS repo](https://github.com/b0hemia/diskos).
The installer keeps the stock `mq_player` audio engine; only the UI is replaced.

## License
- **This UI:** GPL-3.0-or-later - [`COPYING`](COPYING). © diskOS contributors.
- **LVGL 9.2.2** (vendored in `lvgl/`): MIT - [`licenses/LICENSE-LVGL-MIT.txt`](licenses/LICENSE-LVGL-MIT.txt).
  Enabled LVGL-bundled helpers: **TJpgDec** (JPEG decode, BSD-style © ChaN) and **qrcodegen**
  (QR codes, MIT © Project Nayuki) - texts in the repo `licenses/` (`LICENSE-TJpgDec.txt`, `MIT-qrcodegen.txt`).
- **SQLite amalgamation** (`sqlite3.c/.h`): public domain.
- **jsmn** (`jsmn.h`): MIT (Serge Zaitsev).
- **md5, RFC 1321** (`md5.c`): public domain (Alexander Peslyak).
- **Embedded fonts** (generated glyph arrays), all under the SIL Open Font License 1.1:
  - **Montserrat** - Latin UI text - [`licenses/OFL-1.1-Montserrat.txt`](licenses/OFL-1.1-Montserrat.txt)
  - **Source Han Sans** - CJK glyph fallback - [`licenses/OFL-1.1-SourceHanSans.txt`](licenses/OFL-1.1-SourceHanSans.txt)
  - **Font Awesome** - UI + weather icon glyphs (`font_icons_28.c`, `font_weather16.c`) - [`licenses/OFL-1.1-FontAwesome.txt`](licenses/OFL-1.1-FontAwesome.txt)

All bundled components are permissive or public-domain and GPL-compatible.

## Contributing
Issues and patches welcome via the [diskOS repo](https://github.com/b0hemia/diskos). By contributing
to this directory you agree your changes are licensed GPL-3.0-or-later.

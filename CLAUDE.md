# diskOS - working notes

Custom firmware for the **FiiO Snowsky Disc** ($80 round-screen DAP; Ingenic X2000 / MIPS32,
Linux 4.4). Read this before changing anything; most of it is a rule someone learned the hard way.

## What we actually own

| Layer | Owner |
|---|---|
| DRAM bring-up SPL, NAND writer | **ours** - patched GPL u-boot over Ingenic mask-ROM USB |
| Kernel (Linux 4.4), root filesystem | stock FiiO. Not rebuilt; `mtd2` is rewritten to hook `fiio_init.sh` |
| `mq_player` (audio engine) | **stock, untouched.** Driven over POSIX mqueue IPC (~221 tags decoded) |
| `mq_ui` (`ui/`) | **100% ours.** This is diskOS at runtime |
| Installer / manager (`diskos_installer/`) | **ours.** Python, host side |

Deep to install, thin to run: at runtime we own exactly one process.

## Rules that will bite you

- **Never kill `mq_player`.** It frees the SD card and the MCU hard-reboots the device. Only ever
  restart `mq_ui`.
- **`argv[0]` must stay bare `mq_ui`.** `fiio_init.sh` watchdogs with `pgrep -x mq_ui`, and busybox
  matches the full `argv[0]`, not `comm`. Get it wrong and the watchdog kills diskOS and respawns the
  **stock** UI. `main()` normalises `argv[0]`; `ui_restart_self()` passes bare `mq_ui` and re-execs in
  place so the PID never changes. Never add an argv element to the restart - use the environment
  (see `DISKOS_UI_RESTART`).
- **Paint from theme tokens.** `th_bg()`, `th_card()`, `th_text()`... never a raw `lv_color_hex()`. A
  literal looks right in whichever palette you developed against and wrong in the other. Fixed brand
  or state colours (Last.fm red, iOS state blue, the user's accent) are the deliberate exception.
- **Use `th_font(size)`,** never `&lv_font_montserrat_NN` - that indirection is what makes the
  user-font and Text Size settings work everywhere at once.
- **Nothing slow on the LVGL thread.** An unbounded `popen` there is what made the screen unwakeable
  (see the `hcitool` fix in `main.c`); `bt.c` documents the same hazard risking the hardware watchdog.
  Bound it, or move it to a detached worker that publishes plain data.
- **The screen is ROUND, 360x360.** Usable width narrows sharply toward the edges - at y=302 the chord
  is only ~264px. Check any new layout against the chord at its lowest edge, not the bounding box.
- **Docs are ASCII.** Upstream did a pass converting `...` and dashes; match it.

## Build

```sh
cd ui
docker build -t diskos-ui-builder .                                   # pinned musl mipsel toolchain
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" diskos-ui-builder
```

Fast correctness check with no cross toolchain - catches missing symbols and bad prototypes in
seconds, though the result cannot run:

```sh
cd ui && make CROSS= CFLAGS="-O0 -std=gnu11 -pthread -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE -I. -w" \
  LDLIBS="-lm -lrt -lcrypt" mq_ui
```

Adding a `ui/*.c` file means adding it to `APP_SRCS` in `ui/Makefile`.

## Test

```sh
cd ui && make fontcheck && ./fontcheck   # user-font path: LVGL VFS, icon fallback, cold boot
python3 tests/managercheck.py            # device reporting, restore-point lifecycle
python3 tests/diagcheck.py               # redaction, run log, diagnostic report
```

Scanner harness (tag parsers against real byte layouts):

```sh
cd ui && gcc -DSCANNER_TEST -DSCAN_ROOT='"/tmp/sd"' -DDB_PATH='"/tmp/song.db"' \
  -std=gnu11 -D_GNU_SOURCE -I. -fsanitize=address,undefined -o /tmp/scantest \
  scanner.c sqlite3.c -lpthread -ldl -lm
```

`SCAN_ROOT` must be a real mountpoint (the scanner refuses to rebuild from an unmounted card) - a
`tmpfs` works.

## Iterate on hardware

Do not reflash to test a UI change. Enable Debug Mode on the device for SSH, then:

```sh
tools/diskos-deploy.sh <ip> <password>   # build, verify by md5, hot-reload, prune stock safely
tools/diskos-shot.sh                     # framebuffer to PNG
tools/diskos-touch.sh                    # inject taps/swipes
```

See `docs/DEV_WORKFLOW.md`. Hand-deployed binaries revert on reboot (S97 verifies `/usr/data/mq_ui`
against a baked manifest), which makes this safe to experiment with.

## Diagnostics

Host side: every installer/manager run appends to `<state>/logs/diskos.log` (always on, rotated).
`diskos-manager report` writes one redacted file for a bug report; `--debug` adds tracebacks to the
console. Engine events reach the log by wrapping any reporter in `diag.TeeReporter`.

Device side: `/usr/data/diskos_boot.log`, `coldplug.log`. Not yet pulled into the host report.

Errors carry stable codes - `E1xx` preflight, `E2xx` build, `E3xx` flash, `F1xx` the device writer's
own verdict.

## Reference

`docs/RE_CATALOGUE.md` (what is decoded, what is not), `docs/COMMAND_MAP.md` (IPC tags),
`docs/HARDWARE.md` (probed hardware + a gap table), `docs/DEV_WORKFLOW.md`, `ui/README.md`
(theming and font contract).

## State of this branch

`claude/custom-os-fixes-wishlist-f5f002` - fixes and features from the r/snowsky beta review: theme
system with light mode, user fonts, nav-stack fix, UI watchdog, wider audio-format support with a
scan progress screen, artist->albums, settings categories, expanded Quick Settings, charge limit, USB
connect handling, plus the installer's manager front end and diagnostics.

**None of it has run on hardware.** Treat every device-facing claim as unverified. Highest-risk areas
to check first: boot, theme switching, and a rescan.

Known gaps worth doing next, in rough order: the scanner never writes `DISC`/`TRACK` (album tracks
list and play alphabetically), `DURATION`, `ALBUM_ARTIST`, year or bitrate, and binds `ADD_TIME` to a
hardcoded constant; there is no on-device update path (every UI change is a 60-90 min reflash); no
folder browser; no play queue. `mq_player` embeds a working AirPlay stack that only needs its trigger
tag pinned - `docs/RE_CATALOGUE.md` names the exact next step.

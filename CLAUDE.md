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
- **Holding Vol-Up at boot means "the other UI," not "diskOS."** `main.c`'s boot check is
  `flag_stock ^ volup` against `/usr/data/boot_default_stock` (present => default is Stock; written
  only by the Settings -> System -> Default UI toggle, `settings.c`). A **fresh flash has no such
  file**, so the default is diskOS already - holding Vol-Up on a new flash hands off to STOCK, the
  opposite of what you'd guess from a device that was previously toggled to default-stock. Plain
  power-on is the way to reach diskOS on a device that has never had Default UI touched.
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
- **`RunLock` (`diskos_installer/runlock.py`) is a real `flock`, scoped per-`open()`, not per-process.**
  A `with RunLock():` nested inside another already-held one in the SAME process still self-deadlocks
  ("another diskOS installer is already running" against nothing at all). `__main__.py`'s dispatcher
  takes it generically for every command not in its read-only list; any `cmd_*` handler that ALSO
  self-locks (like `manager.cmd_backup`, around its mutating branches) must be in that list too -
  currently `doctor`, `status`, `detect`, `report`, `backup`.
- **The `make CROSS=` fast check writes an x86-64 binary to `ui/mq_ui` - the exact path
  `tools/diskos-deploy.sh` pushes by default.** Deploying it cannot work and fails in a way
  that looks like nothing happened: the device never brings the binary up, the script
  correctly refuses to prune, and `fiio_init`'s watchdog respawns the **stock** UI. You are
  then testing stock while believing you are testing your branch - stock has all the
  original complaints (artists fragmented by "feat.", albums in alphabetical order), so the
  symptom is "my fixes did nothing". **Run `file ui/mq_ui` and expect `MIPS` before every
  deploy**, or build the fast check to a different name.
- **`payload/mq_ui` is stale on purpose-by-accident:** a snapshot from the very first installer commit
  (`e0bc478`, 2026-08-26), predating every UI fix on this branch. `install`/`diskos-manager install`
  without an explicit `--ui path/to/fresh/mq_ui` flashes that old binary silently. Always pass `--ui`
  when testing branch work.

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

## Windows host (WSL2)

Bare Windows can't do ANY of the above: no Docker, the mipsel cross-toolchain is Linux ELF regardless,
and `diskos_installer/runlock.py` refuses Windows outright (the installer needs a real Linux host).
WSL2 + Docker Engine (skip Docker Desktop, just `apt install docker.io` inside the distro) works,
driven straight from Windows via `wsl.exe -d <distro> -u <user> -- <cmd>` - no need to open a separate
WSL terminal. Verified end to end on Ubuntu-on-WSL2 this way: build, native `usbboot`/`mksquashfs`
tools, and a real mask-ROM flash.

- **Clone into WSL's own filesystem** (e.g. `~/diskos-build`), not `/mnt/c/...` or `/mnt/f/...`.
  Windows git's `autocrlf` leaves every `.sh` with CRLF, which breaks `set -euo pipefail`/`set -eu`
  parsing (`invalid option name`, stray `$'\r'`) as soon as a script runs under WSL's bash. A
  local-path `git clone /mnt/f/diskos ~/diskos-build` re-checks-out from the LF blobs using Linux
  git's own defaults, fixing it without touching the Windows working tree. It's also faster (native
  ext4 vs. the 9p bridge) for compiling the vendored LVGL/sqlite3 tree.
- `install.sh` refuses to run as root, so that clone needs to be owned by your normal WSL user, not
  root - and `/root` itself blocks non-root traversal, so if you set up as root first (simplest for
  `apt`/`docker` install), `mv` the clone out from under `/root` and `chown -R` it before running
  `install.sh` or `diskos-installer`/`diskos-manager`.
- **USB passthrough** for the mask-ROM flash needs `usbipd-win` on the Windows side (`winget install
  usbipd`, elevated, one-time). With the device in mask-ROM mode (power off, hold Vol-Down, plug in):
  `usbipd list` for its BUSID, `usbipd bind --busid <id>` (elevated, one-time per port), then
  `usbipd attach --wsl --busid <id>` (every session) to hand it to the running WSL instance.
- **The repo's own `udev/70-diskos-maskrom.rules` will not grant access inside WSL2**, even installed
  correctly: its `TAG+="uaccess"` needs systemd-logind to see an active *seat*, and WSL2 sessions have
  none (`loginctl` shows every session with `SEAT -`). Enumeration still works (the node is
  root:root, mode 0664 = world-readable), so `doctor`/`status` see the device fine, but the actual
  flash's `usbboot` fails immediately with "Could not open USB device" - a bare-second failure, no
  actual write attempted, device left untouched. Fix is WSL-local, not a repo change (the shipped
  rule is correct for a normal desktop with a real seat): add a second rule scoped to the same
  vendor:product with `MODE="0666"`, then `udevadm control --reload-rules && udevadm trigger`.
- `wsl.exe -u root --` needs no sudo password, so setup (`apt`, `docker`, the udev fallback above) is
  all root-doable without any Windows-side elevated prompt beyond the two one-time installs
  (`wsl --install`, `winget install usbipd`).

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

**First real-hardware flash happened, and diskOS BOOTS** (device was stock V228 going in; a verified
restore point is saved). Built via the Docker toolchain inside WSL2 (see above), flashed with
`--variant public --ui <fresh build>` - a real `usbboot` write, F001 SUCCESS, ~91 minutes, 2 factory
bad blocks correctly skipped. A plain power-on with no buttons held reaches diskOS, exactly as the
Vol-Up rule above predicts. Home, Menu, library browsing and playback all work on hardware.

Fixed from the first hardware session (all built + unit-tested, **none confirmed on device yet**):

- `ui/txtfold.c` (new): the Montserrat faces cover U+0020..U+007E and the Source Han CJK fallback
  starts at U+3001, so **everything between - every curly apostrophe and every accented Latin
  letter - had no glyph in any font we load** and drew LVGL's placeholder box ("Don<box>t Push Me",
  and Bjork/Beyonce/Sigur Ros silently mangled the same way). Folded to ASCII on the way in
  (`ipc.c` metadata, `musicdb.c` rows), never on a file path. Table verified against Unicode NFD.
- `scanner.c`: now actually parses `ALBUM_ARTIST` (TPE2 / ALBUMARTIST / `aART`) and `DISC`/`TRACK`
  (TRCK/TPOS, TRACKNUMBER/DISCNUMBER, and MP4's BINARY `trkn`/`disk` atoms). Parsers were refactored
  to pass one `tags_t*` instead of four `char*` out-params. **Needs a RESCAN to take effect.**
- `musicdb.c`: Artists group by `ALBUM_ARTIST` when present, else `ARTIST` with a "feat." tail
  stripped; `mdb_album_songs()` returns DISC/TRACK order replicating the player's own `ORDER_ALBUM`
  exactly (they MUST agree - a tap is sent as a position in the player's list).
- `apps.c`: the Menu grid was 2.13 rows tall so a scroll always rested mid-row, AND its bottom edge
  sat at y=326 where the round screen gives only 210px of chord against a 266px row - the bottom row
  was clipped by the bezel. Now exactly two rows ending at y=296, with `LV_SCROLL_SNAP_START`.
- `library.c`: per-view scroll memory, restored on BACK only (forward drills still start at top).
- `main.c`: volume keys are read straight off the GPB pin (bit 13/14), so the volume bar paints on
  the key edge instead of waiting for the player's a714 - the player grabs `event0` exclusively and
  sits on a single press watching for a double-press track skip. **Confirmed faster on device.**
- `ipc.c`: ignore a `song_duration_time` of 0 for the track already playing (the 0102 mode-change
  reply re-announces the song with a partial body, snapping the NP ring to the start for a frame).
  Hypothesis-driven - not yet confirmed against a captured a2 frame.

Still unverified on hardware: theme switching, a library rescan, and everything in the list above
except the volume fix.

Known gaps worth doing next, in rough order: the scanner still never writes `DURATION`, year or
bitrate, and binds `ADD_TIME` to a hardcoded constant; there is no on-device update path (every
PERMANENT UI change is a 60-90 min reflash - `tools/diskos-deploy.sh` covers testing, but S97 reverts
a hand-deployed binary on the next boot); no folder browser; no play queue; the Library wants a
separate "Album Artists" axis alongside the raw-ARTIST "Artists" one. `mq_player` embeds a working
AirPlay stack that only needs its trigger tag pinned - `docs/RE_CATALOGUE.md` names the exact next
step.

One inherent limit: "play all by artist" sends the player `list_type 2` + a name, and the player
filters `WHERE ARTIST=?`. An album-artist-grouped row therefore queues fewer tracks than the UI
lists. Tapping an individual track is fine - `on_song_play` already falls back to the all-songs
scope when the player's exact list does not contain the song. We cannot change the player's filter
column, so this is structural.

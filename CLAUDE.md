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

## Codebase map

Everything under `ui/` is ours and compiles into the single `mq_ui` binary, EXCEPT two
vendored blobs that must not be edited: LVGL at `ui/lvgl/` and the SQLite amalgamation
`ui/sqlite3.c`. Line counts below are a rough "how much is in here".

**Core runtime**

| File | What it is |
|---|---|
| `main.c` (2000) | Process entry. Boot-time UI hand-off (reads the Vol-Up GPIO), the LVGL main loop, touch gestures, hardware volume keys, backlight + screensaver timing, app launching, the watchdog. Also holds the **decode seams** - `ui_set_volume`, `ui_set_workmode`, `ui_apply_eq` etc. - which are the only places that turn a UI action into an IPC command. |
| `screenmgr.c` (366) | The screen stack. One LVGL root per `SCR_*`, push/pop with a slide animation, and the per-screen "refresh on entry" hooks. `screen_show()` / `screen_back()` live here. |
| `ui.c` (1730) | The Now Playing screen, plus shared UI helpers every other screen uses: `ui_header()`, the volume overlay, album art + accent colour, `ui_font_cjk()`. |
| `ipc.c` (430) | mqueue IPC with `mq_player`. A dedicated rx thread parses `a1` (position), `a2` (metadata JSON), `a714` (volume) and `a607` (work mode) into a `track_state_t`, and recovers when the player recreates the queues. |
| `theme.c` (340) | Palettes (dark/light) behind the `th_*()` colour tokens, the `th_font(size)` ladder, and user-TTF loading/caching. |
| `config.c` (160) | `cfg_get_int` / `cfg_set_int` / `cfg_get_str` / `cfg_set_str` - the persisted key-value store behind every diskOS setting. Batch writes with `cfg_set_int_deferred` + `cfg_flush` (one atomic rewrite). |
| `fb_pan.c` (100) | Framebuffer flush. The panel is mounted 180 degrees rotated and this is what un-rotates it. |
| `anim.c` (126) | Animation helpers: spring paths, slides, fades, the press bounce. |
| `toast.c` (44) | `ui_toast("...")` - the transient message pill. |
| `fileutil.c` (70) | One atomic-copy implementation, shared by the art cache and the font cache. |
| `txtfold.c` (120) | Folds codepoints no font we ship can draw down to ASCII. See "Text that reaches the screen" below. |

**Screens** - each has a `*_create(lv_obj_t *root)` called once from `screens_init()`

`home.c` (400) Home - clock, weather, Library pill, now-playing pill.
`library.c` (830) the Library browser: category menu, Songs / Albums / Artists / Album
Artists / Genres / Playlists / Favourites / History, drill-in, the A-Z scrubber, per-view
scroll memory. `apps.c` (160) the Menu tile grid (swipe left from Home).
`settings.c` (780) the whole Settings tree, driven by one table (see the recipe below).
`search.c` `songinfo.c` `npmenus.c` (Now Playing side panels) `eqcustom.c` (10-band EQ)
`colorpick.c` (accent picker) `modes.c` (audio source) `playlistview.c` `quicksettings.c`
(pull-down panel) `saver.c` (screensaver styles) `scanview.c` (scan progress)
`debug_ui.c` (Debug Mode - SSH password) `fontpick.c` `lastfm_ui.c` `kbinput.c`
(on-screen keyboard) `lyrics.c` `weather.c` `wifi.c` `bt.c`.

**Subsystems (no UI of their own)**

| File | What it is |
|---|---|
| `musicdb.c` (1000) | All reads of `song.db`. Loads the library into memory once, then serves grouping (artists/albums/genres), ordering, playlists, favourites and play stats. **Every list on screen comes from here.** |
| `scanner.c` (950) | The SD-card scanner and every tag parser (ID3v1/v2, Vorbis, MP4, APEv2, DSF). The only thing that WRITES `song.db`. |
| `art.c` / `artcache.c` | Album art: `art.c` shells out to ffmpeg to pull the embedded cover out of a track and render cover/thumb/backdrop; `artcache.c` caches the results on the SD card. |
| `lastfm.c` | Scrobbling engine (`lastfm_ui.c` is only its view). `md5.c` signs its requests. |
| `sysconfig.c` | Typed access to the STOCK settings table, `/usr/data/fiio/db/sysconfig.db`. |
| `fwcaps.c` | Firmware-version capability table - which IPC tag a given stock version accepts. |

**Standalone device tools** - built separately, NOT part of `mq_ui`:
`fbshot.c` (framebuffer to PNG), `mqcap.c` (capture `/ui` frames), `mqfeed.c` (inject fake
metadata), `mqsend.c` / `psend.c` (send one raw frame), `touchcap.c` / `touchdisc.c`.

Adding a new `ui/*.c` means adding it to `APP_SRCS` in `ui/Makefile` or it will not link.

## How the data flows

Know these four paths before changing anything; most bugs are a misunderstanding of one.

**1. Now Playing text** - `mq_player` sends an `a2` JSON frame -> `ipc.c parse_a2()` ->
`g_state` (a `track_state_t`) -> `main.c` reads it each loop -> `ui_update()` paints it.
The player is the source of truth here, NOT our database.

**2. Library lists** - `scanner.c` walks the SD card and writes `song.db` -> `musicdb.c
mdb_load()` reads the whole table into `g_songs` ONCE at startup -> `library.c` calls
`mdb_artists()` / `mdb_album_songs()` etc. to build each list.

**Consequence you must remember: a scanner change does nothing until the user rescans.**
A `musicdb.c` change applies immediately, because it happens on read.

**3. Playing a track** - the UI does NOT hand the player a file path. It asks the player to
rebuild its own list (`list_type` + a name) and then jumps to a POSITION in that list.
`mdb_play_pos()` recomputes that position by replicating the player's exact `ORDER BY`.
**So any list you re-order on screen must be re-ordered to match `mdb_play_pos`,** or a tap
starts the wrong song. `on_song_play()` in `main.c` falls back to the all-songs scope when
the player's list does not contain the track.

**4. Text that reaches the screen** - the fonts we ship cover ASCII `U+0020..U+007E`, the
`LV_SYMBOL` icons, and (only on labels that opt in via `ui_font_cjk`) CJK from `U+3001`.
Nothing else has a glyph, so it draws as a black box. Two rules follow:

- **Runtime text from outside** (tags, weather, anything off the network) must go through
  `txt_fold_ascii()` - already done for `ipc.c` metadata, `musicdb.c` rows and `weather.c`.
  Never fold a FILE PATH; it has to keep its exact bytes to open.
- **Literals in our own source must be plain ASCII.** No typographic ellipsis, en dash, em
  dash, middle dot, curly quote or accented letter. Write `...` and `-`.
  `python3 tests/glyphcheck.py` enforces this and will fail the build if you forget.

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
- **Never invent, guess or "try" an IPC tag.** Every frame diskOS sends was reverse-engineered
  and verified against the stock UI; `docs/COMMAND_MAP.md` is the ground truth and
  `docs/RE_CATALOGUE.md` says what is still unknown. Guessing here does not fail politely:
  routing to Bluetooth with `0666000C0002` but skipping its `0666000C0006` pre-stop makes
  `mq_player` SIGSEGV, which frees the SD card and hard-reboots the device from the MCU.
  Sending `0666` at all to an idle, freshly-booted player wedges it (`g_fiio_local` null).
  An early play-mode toggle used `0657` - the audio SOURCE switch - and could have done the
  same. Tags are also FIRMWARE-VERSION dependent (V2.09 sets gain with `0645`, V2.28 with
  `0649`; see `fwcaps.c`). Send only through an existing decode seam in `main.c`.
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
- **Physical power-key input is owned by `mq_player`:** it exclusively grabs `/dev/input/event0`,
  which also carries volume/play/power buttons. The player forwards power-key activity as the
  documented `aa1c` frame on `/ui`; `ipc.c` turns that into a wake event and `main.c` restores the
  backlight when the saver is dimmed/off. Do not read event0 or invent a power command from the UI.

## Build

```sh
cd ui
docker build -t diskos-ui-builder .                                   # pinned musl mipsel toolchain
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" diskos-ui-builder
```

On this Windows checkout, Docker may not be able to bind-mount either `/mnt/f` or the WSL-native
clone, depending on which daemon/context is active. If the container says `/src/Makefile` is
missing, use the WSL-native clone and `docker cp` the source into a temporary container instead of
changing the source tree or deploying the empty/missing output. Always verify the final binary with
`file ui/mq_ui`; it must report MIPS.

Fast correctness check with no cross toolchain - catches missing symbols and bad prototypes in
seconds, though the result cannot run:

```sh
cd ui && make CROSS= CFLAGS="-O0 -std=gnu11 -pthread -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE -I. -w" \
  LDLIBS="-lm -lrt -lcrypt" mq_ui
```

**The tree holds objects for ONE architecture at a time, and `make` cannot tell.** Both
builds write `.o` files next to the sources, so switching between them in EITHER direction
fails at the link step on the leftovers:

- cross build after a native one: `relocations in generic ELF (EM: 62)` - EM 62 is x86-64
- native build after a cross one: `relocations in generic ELF (EM: 8)` - EM 8 is MIPS

Both say `file in wrong format` and neither mentions architecture, so it reads as a broken
toolchain rather than stale output. Clean before EVERY switch, in either direction:

```sh
cd ui && find . -name '*.o' -delete && rm -f mq_ui
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
python3 tests/glyphcheck.py              # no UI string literal the device cannot draw
```

`fontcheck` needs a real `.ttf` in `ui/tests/sdcard/Fonts/` and a WRITABLE `/usr/data`
(it exercises the font cache); both missing on a fresh WSL box, and both look like code
failures when they are not. `diagcheck` currently fails one redaction case on any account
whose home is literally `/home/<username>` - `diag.redact()` collapses the home path to
`~` before the `<user>` rule can fire, so that test is only green for root. Pre-existing.

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

## Recipes

Copy these. They are the shapes the codebase already uses; inventing a different one is
almost always wrong.

**Add a UI source file**
1. Create `ui/yourfile.c`, starting with the two SPDX/copyright lines every file has.
2. Add `$(APP)/yourfile.c` to `APP_SRCS` in `ui/Makefile`. Missing this = link error.
3. Declare anything public in `ui/screens.h` (screens) or its own `.h`.

**Add a screen**
1. Add `SCR_YOURS` to the `enum` in `ui/screens.h`, **appended at the END, immediately
   before `SCR_COUNT`** - inserting it mid-list renumbers every screen after it.
2. Declare `void yours_create(lv_obj_t *root);` in `screens.h`.
3. In `screenmgr.c screens_init()`: add `s_roots[SCR_YOURS] = screen_make_root(parent);`
   next to the others, then `yours_create(s_roots[SCR_YOURS]);` in the create block below.
4. In your `yours_create()`, paint the background from `th_bg()` and add `ui_header(root,
   "Title")` for the standard back chevron.
5. Open it with `screen_show(SCR_YOURS)`; `screen_back()` returns.
6. If the screen shows values that can change while it is hidden, add a refresh hook in
   `screenmgr.c` next to the `to == SCR_SETTINGS` / `to == SCR_TUNE` cases, or it will show
   stale data on re-entry.

**Add a setting**
Append one row to the table in `settings.c` (fields, in order):
`{ group, label, type, cfg_key, min, max, step, opts, nopts, ro_val, apply, def, desc, opt_descs }`
- `type` is `ST_TOGGLE` / `ST_SLIDER` / `ST_CYCLER` / `ST_READONLY` / `ST_ACTION`.
- `cfg_key` is the `config.c` key it persists to. Pick a new, unique one.
- `apply` is an optional hook run when the value changes - put anything that must reach the
  player in a decode seam in `main.c`, not in `settings.c`.
- `group` puts it under an existing Settings category; a new string makes a new category.

**Add a Library view**
1. Add `VIEW_YOURS` to the `enum` at the top of `library.c`, before `VIEW_COUNT`.
2. Add its title to `VIEW_TITLE[]` **at the matching index** - a `_Static_assert` enforces
   the count, but not the order, so put it in the right slot.
3. Add it to `CATS[]` / `CATV[]` in the `VIEW_MENU` branch to get a menu row.
4. Add an `else if(g_view==VIEW_YOURS)` branch in `library_reload()` that fills `g_gnames`
   (or `g_buf`), sets `g_count`, and calls `fill_start(n)`.
5. **Add a `case VIEW_YOURS:` to the switch in `add_row()`** - the one that actually draws
   each row. That switch has NO `default:`, and `add_row()` creates the row object BEFORE
   it, so a missing case does not crash or warn: the list renders the right NUMBER of
   completely BLANK rows. It looks like "the view is empty and a rescan does not help",
   which sends you hunting in the database instead of the twelve lines that draw it. This
   is exactly how the Album Artists view shipped broken.
6. Handle it in `library_back()` so Back leaves it correctly.
7. If anything deep-links INTO your view (see `library_open_artist`), set every piece of
   context it depends on there too - a deep-link inherits the last-used state otherwise.

**Parse a new tag in the scanner**
1. Add the field to `tags_t` in `scanner.c`.
2. Fill it in each container parser you care about: `id3v2_read` (ID3 text frames),
   `vc_parse` (FLAC/Ogg/Opus), `mp4_ilst_field` (M4A - note `trkn`/`disk` are BINARY, and
   any atom that does not start with the `0xA9` marker must also be added to the dispatch
   list in `mp4_walk`), `apev2_read`.
3. Bind it into BOTH `INS_SONG` and `UPD_SONG` (the two SQL strings in `scanner.c`), and
   into the `bind_tags()` / `bind_extra()` helper that fills them. Both statements use
   **explicit `?N` parameters** because SQLite numbers a bare `?` by order of appearance -
   adding a placeholder mid-list would silently renumber every later bind.
4. Read it in `musicdb.c mdb_load()` and add it to `mdb_song_t`.
5. **It only appears after a rescan.** Test with the scanner harness, not on hardware.

**Change something on the Now Playing screen** - it is `ui.c`, function `ui_update()`.
That runs every loop iteration, so do no work there beyond painting.

## Before you say it is done

Run all of it. Do not skip a step because a change "looks safe".

```sh
# Start every line FROM THE REPO ROOT (the directory holding ui/ and tests/).
# The lines do not chain - go back to the root before each one.

# 0. The tree may hold objects for the OTHER architecture from last time, and the
#    link would fail on them ("file in wrong format"). Start clean.
cd ui && find . -name '*.o' -delete && rm -f mq_ui

# 1. compiles + links. No cross toolchain, so the result CANNOT RUN - this only
#    catches missing symbols and bad prototypes, in seconds.
cd ui && make CROSS= CFLAGS="-O0 -std=gnu11 -pthread -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE -I. -w" \
  LDLIBS="-lm -lrt -lcrypt" mq_ui

# 2. the test suites (glyphcheck/managercheck/diagcheck run from the ROOT, fontcheck from ui/)
cd ui && make fontcheck && ./fontcheck
python3 tests/managercheck.py
python3 tests/diagcheck.py
python3 tests/glyphcheck.py

# 3. only if you touched scanner.c. SCAN_ROOT must be a REAL MOUNTPOINT (the scanner
#    refuses to rebuild from an unmounted card): sudo mount -t tmpfs tmpfs /tmp/sd
cd ui && gcc -DSCANNER_TEST -DSCAN_ROOT='"/tmp/sd"' -DDB_PATH='"/tmp/song.db"' \
  -std=gnu11 -D_GNU_SOURCE -I. -fsanitize=address,undefined -o /tmp/scantest \
  scanner.c sqlite3.c -lpthread -ldl -lm && /tmp/scantest

# 4. STEP 1 LEFT AN x86 BINARY AND x86 OBJECTS BEHIND - the same clean again, for the
#    same reason in the other direction. Skipping it is the single most common way to
#    waste an hour here.
cd ui && find . -name '*.o' -delete && rm -f mq_ui
cd ui && docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" diskos-ui-builder
cd ui && file mq_ui      # MUST say MIPS. If it says x86-64, DO NOT DEPLOY IT.
```

State plainly which of these you ran and what they said. If something fails, say so with
the output rather than describing the change as complete.

## Diagnostics

Host side: every installer/manager run appends to `<state>/logs/diskos.log` (always on, rotated).
`diskos-manager report` writes one redacted file for a bug report; `--debug` adds tracebacks to the
console. Engine events reach the log by wrapping any reporter in `diag.TeeReporter`.

Device side: `/usr/data/diskos_boot.log`, `coldplug.log`. Not yet pulled into the host report.

Errors carry stable codes - `E1xx` preflight, `E2xx` build, `E3xx` flash, `F1xx` the device writer's
own verdict.

## Symptom -> cause

| What you see | What it actually is |
|---|---|
| Deployed a build and NOTHING changed; stock behaviour is back | You deployed an x86 binary. The device could not run it, `diskos-deploy.sh` correctly refused to prune, and `fiio_init`'s watchdog respawned the STOCK UI. Check with `file ui/mq_ui`. |
| Any link fails with `relocations in generic ELF` / `file in wrong format` | Stale objects from the OTHER architecture - `EM: 62` is x86-64 left by the native fast check, `EM: 8` is MIPS left by the Docker build. `cd ui && find . -name '*.o' -delete && rm -f mq_ui`, then rebuild. |
| A black box where a character should be | No font we ship has that glyph. If it is our own string, make it ASCII (`tests/glyphcheck.py` finds them). If it is tag/network text, route it through `txt_fold_ascii()`. |
| A scanner change had no effect | `song.db` is only rewritten by a rescan. Menu -> Scan on the device, or re-run the scanner harness. |
| Tapping a track plays a DIFFERENT track | The on-screen order and `mdb_play_pos()`'s `ORDER BY` disagree. A tap is a position in the player's list, not a path. |
| One album advances to an unexpected track in Sequential mode | Rescan first, then inspect that album's exact `ALBUM`, `DISC`, `TRACK`, `IS_CUE`, and `IS_ISO` rows. Missing/stale track tags or CUE/ISO rows can make the UI/player order differ; do not change `0102` based on one album. |
| Power button does not wake a screen-off panel | `mq_player` owns event0; diskOS must receive `aa1c` and wake through the IPC event path. Do not kill or trace `mq_player`; capture `mq_ui`/`/ui` behavior only. |
| The whole UI froze, or the screen will not wake | Something slow on the LVGL thread - an unbounded `popen`, a blocking network call. Bound it or move it to a worker. |
| Settings shows a stale value on re-entry | The screen needs a refresh hook in `screenmgr.c`'s entry switch. |
| `install.sh` or a `tools/*.sh` dies with `invalid option name` or `$'\r'` | You ran a CRLF checkout of the script under WSL bash. Run it from the WSL-native clone (`~/diskos-build`), not `/mnt/f/...`. |
| The device reboots itself (~10s) | Something killed `mq_player`. Never do that - only ever restart `mq_ui`. |

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

Fixed from the first hardware session (all built + unit-tested; what has and has not been seen
on hardware is spelled out after the list):

- `ui/txtfold.c` (new): the Montserrat faces cover U+0020..U+007E and the Source Han CJK fallback
  starts at U+3001, so **everything between - every curly apostrophe and every accented Latin
  letter - had no glyph in any font we load** and drew LVGL's placeholder box ("Don<box>t Push Me",
  and Bjork/Beyonce/Sigur Ros silently mangled the same way). Folded to ASCII on the way in
  (`ipc.c` metadata, `musicdb.c` rows), never on a file path. Table verified against Unicode NFD.
- `scanner.c`: now actually parses `ALBUM_ARTIST` (TPE2 / ALBUMARTIST / `aART`) and `DISC`/`TRACK`
  (TRCK/TPOS, TRACKNUMBER/DISCNUMBER, and MP4's BINARY `trkn`/`disk` atoms). Parsers were refactored
  to pass one `tags_t*` instead of four `char*` out-params. **Needs a RESCAN to take effect.**
- `musicdb.c`: the Library now offers TWO artist axes, because neither answers the other's
  question - `mdb_artists()` and friends take an `MDB_AR_TRACK` / `MDB_AR_ALBUM` axis and cache
  per axis. **Artists** is the raw `ARTIST` tag (a guest stays findable under their own name);
  **Album Artists** is `ALBUM_ARTIST`, else `ARTIST` with a "feat." tail stripped - including
  the BRACKETED forms, "50 Cent (feat. Eminem)" and "50 Cent [ft. Nate Dogg]", which is how
  most taggers actually write it and which the first attempt missed entirely.
  `mdb_album_songs()` returns DISC/TRACK order replicating the player's own `ORDER_ALBUM`
  exactly (they MUST agree - a tap is sent as a position in the player's list).
- **Undrawable literals in our OWN strings**, which had nothing to do with anybody's tags:
  "Turning on Wi-Fi<box>", "Charging <box> still playing", "Scan failed <box> library kept",
  the scan-progress label (which was ONLY an ellipsis), and "Album <box> 3/19" permanently on
  Now Playing. 17 escapes replaced with ASCII. Weather text now folds too - `wttr.in` sends a
  real degree sign. `tests/glyphcheck.py` is new and fails on any UI literal outside what we
  can draw; it was verified to fail on a planted literal before being trusted.
- `apps.c`: the Menu grid was 2.13 rows tall so a scroll always rested mid-row, AND its bottom edge
  sat at y=326 where the round screen gives only 210px of chord against a 266px row - the bottom row
  was clipped by the bezel. Now exactly two rows ending at y=296, with `LV_SCROLL_SNAP_START`.
- `library.c`: per-view scroll memory, restored on BACK only (forward drills still start at top).
- `library.c`: two follow-up bugs from the two-axis split, BOTH of which made a list look empty
  instead of failing - worth knowing because the shape recurs:
  - `add_row()` decides what a row CONTAINS with a `switch(g_view)`, and the new
    `VIEW_ALBUM_ARTISTS` was added to every place that builds and routes the view but not to
    the one that draws it. That switch has no `default:` and `add_row()` creates the row object
    BEFORE it, so the miss neither crashed nor warned: the view queried correctly and rendered
    the right NUMBER of completely blank rows. On device that reads as "the list is empty and a
    rescan does not help", which sends you to the database - the one place the bug was not.
    (`mdb_artists()` cannot return zero for a non-empty library: the album axis falls back to
    `ARTIST` and the scanner guarantees at least "Unknown artist". A real data miss shows the
    words "No artists found" instead. That is how you tell the two apart.)
  - `library_open_artist()` (Now Playing -> 3-dot -> Artist) never set `g_ar_axis`, so it
    inherited whichever axis was last browsed. `npmenus` hands it the raw `ARTIST` column, so
    resolving it on the ALBUM axis matched nothing and gave "No songs by this artist". A
    deep-link must set every piece of context it depends on.
- `main.c`: volume keys are read straight off the GPB pin (bit 13/14), so the volume bar paints on
  the key edge instead of waiting for the player's a714 - the player grabs `event0` exclusively and
  sits on a single press watching for a double-press track skip. **Confirmed faster on device.**
- `ipc.c`/`main.c`: physical power-key activity is reported by `mq_player` as `aa1c`; diskOS now
  consumes that event to wake the dimmed/off backlight and leave the saver. The payload semantics
  for press/release/long-press remain stock-player behavior, so this is a wake notification only.
  Built and pushed as commit `b02da75`; hardware confirmation of the direct power-button wake is
  still pending.
- `ipc.c`: ignore a `song_duration_time` of 0 for the track already playing (the 0102 mode-change
  reply re-announces the song with a partial body, snapping the NP ring to the start for a frame).
  Hypothesis-driven - not yet confirmed against a captured a2 frame.

**What has actually been seen on hardware**, as distinct from built-and-tested:

- The volume fix is confirmed faster on device.
- A build carrying the two-axis Artists reached the device and ran - the "Album Artists" row
  appeared in the Library menu - so the deploy path works and this change set has executed on
  hardware. On that build the list itself rendered blank (the `add_row` bug above).
- Reported on that same build and NOT yet re-tested since the later commits: an apostrophe still
  drawing as a box, album tracks still alphabetical, and "feat." variants still splitting the
  artist list. Each has a CANDIDATE cause that was fixed afterwards - respectively the
  undrawable literals in our OWN strings, `DISC`/`TRACK` needing a rescan to populate, and
  `strip_featuring()` not handling the bracketed "Artist (feat. Guest)" form. Those are
  hypotheses, not diagnoses: none was reproduced against that exact build, and none of the
  fixes has been on hardware yet. Re-test before assuming any of the three is closed.
- Never yet exercised on device: theme switching, a library rescan, the menu-grid layout, the
  scroll memory, the Now Playing progress-flash fix, and the new power-key wake path.

The honest summary: the deploy path is proven, the volume fix is proven, and the power-key wake
path is built and pushed but needs hardware confirmation. Library ordering fixes still require one
deploy-plus-rescan pass; a C418 Minecraft Volume Alpha report is currently undiagnosed and may be
album metadata/database ordering rather than Sequential mode.

Known gaps worth doing next, in rough order: the scanner still never writes `DURATION`, year or
bitrate, and binds `ADD_TIME` to a hardcoded constant; there is no on-device update path (every
PERMANENT UI change is a 60-90 min reflash - `tools/diskos-deploy.sh` covers testing, but S97 reverts
a hand-deployed binary on the next boot); no folder browser; no play queue. `mq_player` embeds a
working AirPlay stack that only needs its trigger tag pinned - `docs/RE_CATALOGUE.md` names the
exact next step.

Runtime text that is still NOT folded, and will still show boxes if it contains anything outside
ASCII: Wi-Fi SSIDs (`wifi.c`), Bluetooth device names (`bt.c`), fetched lyrics (`lyrics.c`),
filenames shown during a scan (`scanview.c`), and homebrew app names from `app.conf` (`apps.c`).
Each is a one-line `txt_fold_ascii()` call at the point the name is displayed.

One inherent limit: "play all by artist" sends the player `list_type 2` + a name, and the player
filters `WHERE ARTIST=?`. An album-artist-grouped row therefore queues fewer tracks than the UI
lists. Tapping an individual track is fine - `on_song_play` already falls back to the all-songs
scope when the player's exact list does not contain the song. We cannot change the player's filter
column, so this is structural.

# diskOS roadmap - what "polished and ready" needs

Written 2026-09-05. Ordered by the brief: **the essentials perfect first**, then correctness,
then the features a DAP is expected to have, then power. Items are marked **[device]** when
they cannot be finished without hardware, and **[host]** when they can be built and tested
without it.

Definition of done for v1: you can pick the device up, and volume / play / pause / power / next
behave instantly and identically every time; the library is correct and fast; you can queue
music; nothing on screen is a black box; and a day of listening does not surprise you on
battery. Feature count is not a goal - anything that does not earn its place is left out.

---

## Tier 0 - the basics, which have to be perfect

These are the operations you touch every minute. Everything else is secondary to them.

- [ ] **Physical key semantics** [device]. `0820`/`0821`/`0822` = key single / double / long
  action are string-verified in Table A but have never been used. Today the *meaning* of every
  press is stock-player behaviour we do not control - which is why volume needed a GPIO
  workaround rather than a fix. Pin these on V2.28 and the keys become ours: repeat rate,
  long-press, double-press-to-skip, all tunable. **This is the single highest-value RE left.**
- [ ] **Volume** [device]. The GPIO fast path is confirmed faster. Still to check: behaviour
  from screen-off, during a track change, and at the 0/max ends.
- [ ] **Power on / off** [device]. The power-key wake path (`b02da75`) is built but unconfirmed.
  Shutdown is `SET_POWER_DOWN_TO_MCU` and is not currently ours at all.
- [ ] **Play / pause latency** [device]. `0201` is the toggle; measure press-to-audio.
- [ ] **Screen wake** [device]. Confirm the watchdog and wake paths after the BT fix.

## Tier 1 - correctness we already know about

- [ ] **Verify the outstanding fix set** [device]. One deploy + rescan settles: apostrophes and
  accented names, album track order, artist grouping, the Menu grid, scroll memory, and the
  Now Playing progress flash. All built and host-tested; none confirmed on hardware.
- [ ] **Fold the remaining runtime text** [host]. Wi-Fi SSIDs (`wifi.c`), Bluetooth device names
  (`bt.c`), fetched lyrics (`lyrics.c`), filenames during a scan (`scanview.c`), homebrew app
  names (`apps.c`). Each is a one-line `txt_fold_ascii()` at the display point. Cheap, and the
  same black-box bug is waiting in every one of them.
- [ ] **Scanner: real `DURATION`** [host]. Still unwritten, so track lengths and the progress
  bar depend entirely on the player. Also year, bitrate, and `ADD_TIME` (currently a hardcoded
  constant, so "recently added" is meaningless).
- [ ] **`diagcheck` redaction bug** [host]. Pre-existing: `diag.redact()` collapses the home
  path to `~` before the `<user>` rule can fire, so that test is only green for root.

## Tier 2 - the play queue (in flight)

Route B is **confirmed on hardware**: the player re-reads `LIST_SONG_0` as it advances, so we
can edit its live queue with no rebuild and no IPC tag. See `docs/QUEUE_DESIGN.md`.

- [x] Write layer - append / insert-after / remove / clear, with `tests/queuecheck.c`.
- [ ] **Scope ownership** [host]. A Library tap currently sends a rebuild that wipes the queue.
  Needs the `Q:` sentinel so a user-built queue is never silently destroyed.
- [ ] **UI** [host]. Long-press any track -> *Play next* / *Add to queue*; album and artist rows
  get *Add to queue* for the whole scope; the Queue screen gains remove, reorder and clear.
- [ ] **Fix "play all by artist"** [host]. Today it sends the player `list_type 2` + a name and
  the player filters `WHERE ARTIST=?`, so an album-artist row queues fewer tracks than the UI
  lists. Now that we can write `LIST_SONG_0` ourselves, we can build the exact list instead -
  this stops being a structural limitation and becomes a bug we can close.
- [ ] **Device pass** [device]. Add while playing without glitching the current track; reorder
  ahead of the playhead; reboot and confirm resume still lands correctly.

## Tier 3 - features a DAP should have

- [ ] **Folder browser** [device first]. `0408` = file/folder browse is string-verified. Probe
  it before building anything - the player may already do the work.
- [ ] **Gapless** [device]. `0647` is wired to a setting; never verified that it works.
- [ ] **AirPlay** [device]. `mq_player` embeds a working stack; needs its trigger tag pinned.
  `docs/RE_CATALOGUE.md` names the exact next step. Genuinely useful, and already paid for.
- [ ] **Resume correctness** [device]. `MEMORY_PLAY.POSITION` is never updated during playback
  (probe-confirmed), so resume-to-exact-position may not work at all. Worth checking before
  promising it.

Deliberately NOT planned: cross-device sync, a "radio"/auto-queue, multi-select, themes beyond
light/dark, and anything social. Each adds surface without making the device better at playing
music.

## Tier 4 - iteration speed (this gates everything above)

- [ ] **On-device update path** [device]. Every *permanent* UI change is a 60-90 minute reflash.
  `tools/diskos-deploy.sh` covers testing, but S97 reverts a hand-deployed binary on the next
  boot. Until this exists, every item above pays a reflash tax to ship. **Arguably the highest
  leverage item on this page** even though it is invisible to the user.

## Tier 5 - power

Measured on hardware 2026-09-05, which changed the priorities - see the verdict block at the
top of `docs/POWER_OPTIMIZATION_PLAN.md`.

- [x] BT auto-route `popen` on the LVGL thread every 3s - fixed in `2db5f3c`.
- [ ] **Wi-Fi policy** [host]. On by default, no screen-off or idle-off policy. The radio is a
  real current draw where the UI loop is not.
- [ ] **Stop rendering to a dark panel** [host]. The clock push and analog saver still
  invalidate and flush while `bl_power=4`.
- [ ] ~~Main loop 30ms floor~~ **DEPRIORITISED.** Measured: `mq_ui` idles at ~0.5% of one core.
  The wakeups are real but the cost is not; `core_timerevent` at ~490/s is the audio path, not
  us. Accurately described in the plan, worth almost nothing to fix.

## Unknowns worth closing

- `LIST_SONG_3`, `CUSTOM_PLAYLIST_INDEX`, `RECORD_SONG` - present on V2.28, undocumented. What
  selects list 3 over list 0 should be known before anything writes to either.
- Whether the player addresses queue rows by ID value or ordinal position. The current design
  sidesteps it by mirroring the player exactly, but it constrains future work.
- Every V2.09 tag meaning is unverified on V2.28, and `fwcaps.c` documents a handler silently
  becoming a NULL pointer across exactly that gap. Verify before use, fail closed.

## Suggested order

1. **Tier 4** (on-device updates) - it pays for itself immediately.
2. **Tier 0** key semantics - the biggest felt-quality win, and one RE session.
3. **Tier 1** verification pass - one deploy + rescan closes a lot of open questions at once.
4. **Tier 2** queue UI - the write layer is done and tested.
5. Tier 3 and 5 as they earn it.

Tiers 1, 2 and 5 have host-side work that can proceed in parallel with any device session.

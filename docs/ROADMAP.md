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

- [x] **Verify the outstanding fix set** - user-confirmed working on device 2026-09-05
  ("all seems to work") after deploying the queue build. Covers apostrophes and accented names,
  album track order, artist grouping, the Menu grid, scroll memory, and the queue itself.
  Reported as a whole rather than item-by-item, so treat any single one as likely-good rather
  than individually proven.
- [x] **Fold the remaining runtime text** - `07c8b16`. Wi-Fi SSIDs, Bluetooth device names,
  fetched lyrics, filenames during a scan, and homebrew app names all fold now. SSIDs and BT
  names fold into a COPY: the original is the key we match on and has to stay byte-exact.
- [x] **Scanner: real `DURATION`** - `00192f0`. FLAC reads it exactly from STREAMINFO; MP3 takes
  the ID3 TLEN frame. Track lengths in the Library had been blank since the scanner was written.
- [x] **Scanner: `ADD_TIME`** - `70fd409`. Now the file mtime, from the stat the walk already
  does. Every row used to share one hardcoded constant, so "Recently Added" sorted by nothing.
- [x] **Scanner: year and bitrate** - year from ID3 TDRC/TYER/TDRL, ID3v1's fixed 4-byte field,
  Vorbis DATE/YEAR, MP4 `(C)day` and APEv2 Year, dug out of whatever shape the tag is in
  ("1998", "1998-05-12T00:00:00Z", "12/05/1998"). Bitrate is the AVERAGE, from file size and
  duration - there is no cheap exact answer, and size*8/duration is what a player shows for VBR
  anyway. Both appear in Song Info, looked up from song.db on screen entry: the player reports
  neither, and neither belongs on the per-frame or per-track path for two rows nobody sees until
  they open that screen. **Needs a rescan** like every scanner change.
- [x] **`diagcheck` redaction bug** - `6ac38b6`. The implementation was right and the test was
  wrong; it only ever passed for root. Now asserts the property (no username survives) rather
  than which of the two rules happened to fire.

## Tier 2 - the play queue (in flight)

Route B is **confirmed on hardware**: the player re-reads `LIST_SONG_0` as it advances, so we
can edit its live queue with no rebuild and no IPC tag. See `docs/QUEUE_DESIGN.md`.

- [x] Write layer - append / insert-after / remove / clear, with `tests/queuecheck.c`.
- [x] **Scope ownership** - `3d3b5f9`. The `Q:` sentinel marks the list as the user's, so nothing
  sends a rebuild over it; only an explicit "play something new" replaces the queue.
- [x] **UI** - done, commit `3d3b5f9`: long-press a song -> *Play next* / *Add to queue*; the
  Queue screen reads `LIST_SONG_0` directly and gained remove + clear. Album, artist and genre
  rows now hold for *Play all* / *Add to queue* too. Queue rows hold for *Move up* / *Move down* /
  *Remove* - buttons rather than drag, because on a 360px round screen a vertical reorder drag is
  indistinguishable from a scroll at the moment it starts.
- [ ] **Fix "play all by artist"** [device to verify]. It still sends the player `list_type 2` +
  a name, and the player filters `WHERE ARTIST=?`, so an album-artist row plays fewer tracks
  than the UI lists. The MECHANISM to fix it now exists - `mdb_queue_append_ids()` builds the
  exact list - and *Add to queue* on an artist row already uses it, so the workaround is one
  gesture away today.
  Deliberately NOT switched over yet: doing so would replace `LIST_SONG_0` wholesale and then
  send a bare position jump, and we have only verified that the player re-reads the table as it
  ADVANCES - not that it re-reads a wholesale replacement under a jump. Changing the most-used
  action in the UI on an unverified assumption is the wrong trade. One device test settles it.
- [ ] **Device pass** [device]. Add while playing without glitching the current track; reorder
  ahead of the playhead; reboot and confirm resume still lands correctly.

## Tier 3 - features a DAP should have

- [ ] **Folder browser** [device first]. `0408` = file/folder browse is string-verified. Probe
  it before building anything - the player may already do the work.
- [x] **Gapless** - user-confirmed working on device 2026-09-05. `0647` does what the setting says.
- [ ] **AirPlay** [device]. `mq_player` embeds a working stack; needs its trigger tag pinned.
  `docs/RE_CATALOGUE.md` names the exact next step. Genuinely useful, and already paid for.
- [ ] **Resume correctness** [device]. `MEMORY_PLAY.POSITION` is never updated during playback
  (probe-confirmed), so resume-to-exact-position may not work at all. Worth checking before
  promising it.

Deliberately NOT planned: cross-device sync, a "radio"/auto-queue, multi-select, themes beyond
light/dark, and anything social. Each adds surface without making the device better at playing
music.

## Tier 4 - iteration speed (this gates everything above)

- [x] **On-device update path** - built and host-tested; **one device reboot still needed to
  confirm it**. `tools/diskos-deploy.sh --persist` arms an update slot in `/usr/data`; S97 adopts
  it on the next boot, records its SHA-256 in an adopted manifest, and verifies against that from
  then on. Off by default behind **Settings -> System -> On-Device Updates**, because it means the
  rootfs is no longer the sole root of trust - see `docs/DEV_WORKFLOW.md` section 5a.
  The fail-closed contract is intact in both directions: shape (ELF32-LE/MIPS) is checked whatever
  blessed the binary, the slot is one-shot, the previous binary is kept at `/usr/data/mq_ui.prev`,
  turning the setting back off reinstalls the FLASHED build rather than dropping to stock, and a
  reflash always wins over a standing adoption (the adopted manifest records the rootfs it was
  adopted against, so a flash invalidates it - otherwise `/usr/data` surviving the flash would hand
  you back the build you already had).
  `python3 tests/s97check.py` runs the real S97 against a fake root across thirteen scenarios.
  **CONFIRMED ON HARDWARE 2026-09-06.** A build differing from the flashed one (`6aa3008e` vs the
  rootfs manifest's `66ece70f`) was pushed with `--persist` and survived two reboots. The device log
  shows both branches, which is why a differing binary was used rather than the same one:
  `pending update adopted (sha 6aa3008e..., 3493432 bytes)` then `verified against ADOPTED update
  manifest` on the adopting boot, and again on the NEXT boot via the fast path with no slot present.
  The adopted manifest records `BASE=66ece70f`, so the reflash-wins guard is armed. **UI changes no
  longer cost a reflash.**
  One defect found doing it: `mq_ui.prev` ends up identical to the installed binary, because
  `--persist` hot-swaps before the reboot, so the "outgoing" copy is already the new build. The
  rollback is a copy of itself - see `docs/FEATURE_PLAN.md`.

## Tier 5 - power

Measured on hardware 2026-09-05, which changed the priorities - see the verdict block at the
top of `docs/POWER_OPTIMIZATION_PLAN.md`.

- [x] BT auto-route `popen` on the LVGL thread every 3s - fixed in `2db5f3c`.
- [x] **Wi-Fi policy** - Settings -> Network -> Wi-Fi Auto-Off (Never / 5 / 15 / 30 min,
  default 15). Suspends the radio after that long with the screen off and restores it on wake.
  Suspension is separate from the user's on/off intent, so the keepalive cannot undo it.
  UNMEASURED: the benefit is inferred from the radio being a real draw, not from a battery test.
- [x] **Stop rendering to a dark panel** - done: `ui_screen_is_off()` now gates the 10s clock
  push and the 1 Hz analog-saver hands, matching how `ui_vinyl_spin` was already gated.
- [ ] **Index `SONG(PATH)`** [host]. **ANSWERED ON DEVICE 2026-09-06: there is no index.**
  `PRAGMA index_list(SONG)` returns nothing and `EXPLAIN QUERY PLAN` says `SCAN TABLE SONG`, against
  a library of **4821 rows**. The scanner does one such lookup PER FILE in `UPD_SONG`'s merge, so a
  rescan is roughly 23 million row visits. One `CREATE INDEX IF NOT EXISTS` in the scanner's schema
  setup is the whole fix, and it is now a measured win rather than a guess.
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

1. **Confirm the update path** - turn on On-Device Updates, deploy with `--persist`, reboot.
   Everything below ships faster once that is proven, and it is a two-minute test. Run
   `tools/diskos-probe.sh` on the same trip: it is read-only and now also answers the
   `SONG(PATH)` index question for free.
2. **Tier 0** key semantics - the biggest felt-quality win, and one RE session.
3. **Tier 2 device pass** - queue while playing, reorder ahead of the playhead, reboot and resume.
4. Tier 3 and the rest of Tier 5 as they earn it.

**Host-side work remaining**: none. Everything left on this page needs hardware.

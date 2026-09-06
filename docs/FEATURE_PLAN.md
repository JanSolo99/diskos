# diskOS feature plan - wallpaper, wireless music, remote library

Written 2026-09-06, after the on-device update path landed. Every constraint below was
MEASURED on the device that day, not assumed; the numbers are what make most of these
decisions obvious, so they come first.

---

## The box we are working in

| | Measured | Why it matters |
|---|---|---|
| RAM | 120 MB total, ~60 MB available | Anything resident competes with `mq_player`. A memory stall in the audio path is a glitch or a reboot, not a slow UI. |
| CPU | Ingenic XBurst II, BogoMIPS 2387 | Fine for UI, poor for crypto or image decoding in a loop. |
| `/usr/data` | 67.6 MB, **51.8 MB free** | Where our binary and any new payload live. This is the hard ceiling on shipped code. |
| SD card | 59.4 GB, 14.7 GB free, exfat, mounted **rw** at `/tmp/sdcard` | Music and wallpapers go here. Writable while the player holds it. |
| Library | **4821 songs** | Big enough that O(N^2) work in the scanner is felt. |
| `/dev/net/tun` | **ABSENT**, no module tree, no `iptables` | Decides the Tailscale question outright - see below. |
| busybox | `wget` (with TLS), `tftp`, `unzip`, `udhcpc`. **No `httpd`, no `nc`, no `timeout`** | We cannot lean on stock tools for a server; we can for HTTPS fetching. |
| LVGL build | `lodepng`, `tjpgd`, `qrcode` already compiled in | PNG/JPEG decode and QR rendering are free. |

Two things already in the tree do most of the heavy lifting for what follows:

- **`lastfm.c` contains a complete transient HTTP server** - binds to `wlan0` only (never
  `INADDR_ANY`), unguessable path token from secure randomness, non-blocking `accept()` on
  a worker thread, explicit start/stop tied to a screen, QR of the URL shown on the panel.
  That is exactly the shape "send music from the PC" needs, already reviewed and shipping.
- **HTTPS works** - Last.fm scrobbles POST to `https://ws.audioscrobbler.com/` through
  busybox `wget`, so TLS is available without linking anything.

---

## 1. Wallpaper on Home when nothing is playing

**Verdict: easy, low risk, do it first.** Nothing here is unknown.

Home already paints a blurred album-art backdrop while playing (`home_set_backdrop`).
The gap is only the idle case.

**Design**

- Wallpapers live on the SD at `/Wallpapers` (JPEG or PNG), same "user drops files on the
  card" contract as `Fonts/`.
- Settings -> Display -> Wallpaper opens a picker modelled on `fontpick.c`, which already
  scans an SD directory, lists what it finds and persists the choice. Options: **None**
  (today's behaviour), **a chosen image**, **Shuffle** (a different one each idle).
- Decode ONCE to a 360x360 buffer and cache it, exactly as `artcache.c` does for album
  art. A decoded 360x360 ARGB8888 frame is 518 KB - fine once, ruinous per frame on this
  CPU, so it must never touch `ui_update()`.
- Reuse the existing backdrop slot so the wallpaper and the now-playing backdrop cannot
  both be live: playing shows art, idle shows wallpaper, and the switch is one assignment.

**Watch for**

- The panel is a 360px CIRCLE. A photo with its subject at the edge loses it; the picker
  should preview in a circular mask so what you choose is what you get.
- Legibility. The clock and pills sit on top, so the wallpaper needs a scrim - reuse
  `th_scrim()` rather than inventing a dimming constant.
- Cache invalidation when the user swaps the file for a same-named one: key the cache on
  size+mtime, not on the path.

**Effort:** small. Entirely host-testable except the final look.

---

## 2. Send music to the device from the PC, wirelessly

**Verdict: very tractable, and the highest day-to-day value of the three.** The server
pattern already exists in `lastfm.c`; this is mostly extraction plus an upload handler.

**Design**

- Menu -> **Receive Music**. The screen shows a **QR code** of
  `http://<wlan0-ip>:<port>/<token>/` plus the URL in text, and a live count of files
  received. `lv_qrcode` is already compiled in.
- The PC opens that URL and gets a small drag-and-drop page served from the device. Files
  upload over plain HTTP on the LAN.
- The server exists ONLY while that screen is open - same lifecycle as the Last.fm setup
  server. No always-on listener, no new attack surface when you are not using it.
- Uploads **stream straight to the SD card**. With 60 MB of headroom and albums that can
  exceed it, buffering a file in RAM is not an option; write as it arrives, to a
  `.part` name, and rename on completion so a half-transfer is never indexed.
- On finish, offer **Scan now** rather than scanning automatically - the user may be
  sending twenty files and should not trigger twenty rescans.

**Security, deliberately**

- Bind to the `wlan0` address, never `0.0.0.0` (the Last.fm server already does this; the
  stock firmware, for what it is worth, listens on `0.0.0.0:111` and `:53`).
- Unguessable path token per session, from the same secure-randomness helper.
- Refuse paths containing `..` or a leading `/`, and write only under the music root.
  Uploads name files; a traversal bug here writes anywhere the player can.
- Cap the filename length and reject extensions we do not index.

**Watch for**

- exfat is mounted `rw` and the player holds the card. Writing while it plays should be
  fine, but the FIRST device test should be "upload a 300 MB album while music is
  playing" and listen for dropouts.
- No `httpd` and no `nc` on the device, so this must be our own code. That is already the
  plan, but it rules out a five-line shortcut.

**Effort:** medium. The riskiest part - a correct, bounded, non-blocking socket server on
a worker thread - is the part already written and shipping in `lastfm.c`.

---

## 3. Tailscale, and downloading from your home music server

**Verdict on Tailscale ON THE DEVICE: don't.** This is a measurement, not an opinion.

- `/dev/net/tun` is **absent**, `/proc/misc` has no `tun`, there is no `/lib/modules` tree
  and no `tun.ko` anywhere. Standard `tailscaled` cannot work.
- We do **not own the kernel** - it is stock FiiO 4.4.94, not rebuilt - so adding TUN means
  taking on the kernel, which is a different and much larger project.
- Tailscale's userspace mode (`--tun=userspace-networking`) does avoid TUN, but: it is Go,
  upstream ships no MIPS32 build, the combined binary is roughly 30 MB against 51.8 MB
  free, and `tailscaled` commonly sits at 30-60 MB RSS against ~60 MB available. It would
  be competing for memory with the audio engine. The failure mode is not "slow" - it is
  the player being starved.

**But you can have the whole outcome you actually asked for, without any of that.**

The goal is "browse my music server from the device and download straight onto it". The
device does not need to be on the tailnet for that. It needs to reach ONE HTTPS endpoint.

- **At home** the device is already on your LAN (it is on `192.168.1.x` right now). Plain
  HTTP to your server, no VPN, nothing to install.
- **Away**, put the tailnet on the SERVER side and expose just the portal:
  **Tailscale Funnel** (or a Cloudflare Tunnel) gives your music portal a public HTTPS URL,
  and the device fetches it with `wget` plus a bearer token. We know that works, because
  Last.fm scrobbling already does HTTPS through the same `wget`.

That is strictly better for this device: no VPN daemon, no TUN, no Go runtime, no memory
pressure, and it works on any network including tethering - not just when a VPN is up.

**Design**

- Settings -> Network -> **Music Server**: a base URL and a token, entered with the
  on-screen keyboard or handed over by the same QR-page trick `lastfm.c` uses for
  credentials (far less painful than typing a token on a round screen).
- A **Browse Server** screen: your portal returns a small JSON index (artist / album /
  track / size). Tap to download; it streams to the SD with a progress row, exactly like
  the upload path in reverse.
- Downloads land in a staging name and are renamed on completion, then offer **Scan now**.
- The portal is yours to write on the PC side, so keep the device contract tiny and
  versioned: one JSON listing endpoint, one file endpoint, a token header. Anything
  cleverer belongs on the server where it is easy to change.

**Watch for**

- Token on a device you carry. Make it revocable server-side and scope it to
  read-only music, so a lost player is a revoke and not an incident.
- 14.7 GB free on the card is not unlimited. Show free space before a download and refuse
  cleanly rather than filling the card.
- Long downloads with the screen off must not be killed by the screensaver path, and must
  not hold a wake-lock that ruins battery. Bound them and make them resumable.

**Effort:** medium on the device, and most of the interesting work is your server portal.
Phase it: LAN + plain HTTP first (proves the browse/download/scan loop), then the token +
HTTPS + Funnel step for remote, which is config rather than new device code.

---

## Other things genuinely missing

Ordered by value per unit of work, not by ambition.

1. **Update over the air.** Today's work made the device accept an adopted binary; the
   only manual part left is a PC running `--persist`. A "Check for updates" that fetches
   `mq_ui` plus its SHA-256 into `/usr/data/diskos_update/` and asks for a reboot reuses
   the entire mechanism that is now proven. This is the cheapest large win on the page.
2. **Index `SONG(PATH)`.** Measured today: 4821 rows, `PRAGMA index_list(SONG)` empty, and
   `EXPLAIN QUERY PLAN` says `SCAN TABLE SONG`. The scanner does one such lookup PER FILE,
   so a rescan is ~23 million row visits. One `CREATE INDEX IF NOT EXISTS` in the scanner's
   schema setup. See the defect note below.
3. **Resume position for long tracks.** `MEMORY_PLAY.POSITION` is never updated by the
   player (probe-confirmed), so an audiobook, DJ mix or podcast restarts from zero. We own
   our own database - store a per-track position and seek on resume. The single most
   glaring functional gap for anything over about twenty minutes.
4. **Key lock.** There is no hardware hold switch, and the volume keys are live in a
   pocket. A long-press to lock, shown on the screensaver, prevents both accidental
   track skips and an accidental full-volume blast.
5. **Volume limit.** Hearing safety, and it bounds the damage of number 4 happening anyway.
6. **Ship a static `timeout`.** S97 logs `no 'timeout' applet -> SD ops are UNBOUNDED` on
   every single boot. That is the one residual in its own fail-closed contract that we can
   actually close, and it is a small static binary in `payload/`.
7. **Folder browser.** Already on the roadmap; `0408` is string-verified and should be
   probed before anything is built.

Still deliberately NOT planned: cross-device sync, auto-generated "radio", multi-select,
themes beyond light/dark, anything social.

---

## Two defects found while confirming the update path

**`mq_ui.prev` does not hold what the docs claim.** Measured after the adoption:

```
installed:  6aa3008e...
mq_ui.prev: 6aa3008e...   <- identical, so there is nothing to roll back TO
```

Not an S97 bug in isolation - it faithfully kept "the outgoing binary". The problem is the
`--persist` flow: `diskos-deploy.sh` hot-swaps the new binary into `/usr/data/mq_ui`
BEFORE the reboot, so by the time S97 adopts the slot, the outgoing copy is already the
new build. The rollback is a copy of itself.

Fix: only move the outgoing binary aside when it actually differs from the incoming one.
That costs one SHA-256 of ~3.5 MB on an adopting boot and makes `.prev` mean what
`docs/DEV_WORKFLOW.md` says it means.

**No index on `SONG(PATH)`** - confirmed above. Previously unknowable from the source,
because our `CREATE TABLE IF NOT EXISTS` is a no-op against the stock player's table.

---

## Suggested order

1. The two defects above - both small, both measured, and one of them is a safety net that
   currently is not there.
2. **Wallpaper** - self-contained, visible, and the lowest-risk way to exercise the new
   over-the-air update loop in anger.
3. **Receive Music** - biggest day-to-day gain, and it builds the streaming-to-SD and
   scan-after-write plumbing that the download feature then reuses.
4. **Music Server browse/download** - LAN first, then token + HTTPS for remote.
5. **OTA updates**, once there is somewhere to publish builds.
6. Resume position, key lock, volume limit as they earn it.

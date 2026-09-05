# diskOS UI dev workflow

How to build, deploy, and iterate on the diskOS UI (`mq_ui`) on real hardware. For iterating on
UI changes you do **not** need the installer or a reflash. Build, push over SSH, hot-reload. By
default a reboot reverts to the flashed build, which is what you want while experimenting; when a
build is good, `--persist` (section 5) keeps it without a reflash.

## 1. Build the binary

Use the pinned Docker builder (works on Linux and macOS, Intel or Apple Silicon):

```sh
cd ui
docker build -t diskos-ui-builder .
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" diskos-ui-builder
```

This produces a static `mq_ui` for `mipsel-linux-musl`. LVGL is vendored, so nothing else is needed.
See [`ui/README.md`](../ui/README.md) for the toolchain details and a from-scratch (non-Docker) build.

## 2. Get a shell on the device

Enable **Debug Mode** on the device (Settings -> System). It shows the device IP and a one-time SSH
password. The password is regenerated on every enable and does **not** survive a reboot.

```sh
ssh root@<device-ip>      # password from Debug Mode
```

## 3. Push the binary

Stream the freshly built `mq_ui` to a staging path, verify it, then move it into place:

```sh
scp mq_ui root@<device-ip>:/usr/data/mq_ui.new
ssh root@<device-ip> 'chmod 755 /usr/data/mq_ui.new && md5sum /usr/data/mq_ui.new'
# confirm the md5 matches your local build, then:
ssh root@<device-ip> 'mv /usr/data/mq_ui.new /usr/data/mq_ui'
```

You do **not** need to touch the boot manifest for a hot-reload. The manifest and the S97 check only
run at boot; for a live reload they are irrelevant.

## 4. Hot-reload (the important part)

This is where most people get bitten. `fiio_init.sh` runs a watchdog that does `pgrep -x mq_ui`, and
busybox `pgrep -x` matches the full `argv[0]`, not the process `comm`. If you kill `mq_ui` and do not
immediately relaunch a **detached** replacement, the watchdog respawns the **stock**
`/usr/bin/mq_ui`. That is the "it fell back to the stock FiiO UI" you may have seen.

Two rules:

### Never kill `mq_player`

`mq_player` is the stock audio engine. Killing it frees the SD card, and the hardware MCU reboots the
whole device (about 10 seconds). Only ever touch `mq_ui`. If a restart caused a full reboot rather than
just the stock UI appearing, an errant `mq_player` kill is the usual cause.

### Relaunch detached, from `/usr/data`, in one step

```sh
killall -9 mq_ui; setsid /usr/data/mq_ui </dev/null >/usr/data/diskos_boot.log 2>&1 &
```

The `setsid </dev/null` is what matters. Without it the new process is a child of your SSH session and
dies when the session closes, so the watchdog wins the race and respawns stock. Then confirm your build
(not stock) is the one running:

```sh
for p in $(pidof mq_ui); do
  [ "$(readlink /proc/$p/exe)" = /usr/bin/mq_ui ] && kill -9 $p    # prune only a stock instance
done
for p in $(pidof mq_ui); do readlink /proc/$p/exe; done    # should print /usr/data/mq_ui
```

The binary built from this source already normalizes its own `argv[0]` to bare `mq_ui`, so once it is
relaunched from `/usr/data/mq_ui` the watchdog is satisfied and leaves it alone.

The snippet above is the quick manual form; it prunes stock right away and relies on the watchdog to
recover if your build does not start. For scripted use prefer [`../tools/diskos-deploy.sh`](../tools/diskos-deploy.sh),
which does the same thing but polls until your build is confirmed running before pruning stock, and
fails safely (leaving the stock UI as a fallback) if your build never comes up.

To go back to stock without a reboot, just `killall mq_ui` and let the watchdog respawn the stock UI.

## 5. Make it permanent

By default a hand-deployed binary reverts on the next reboot: S97 verifies `/usr/data/mq_ui` against
`/etc/diskos_manifest`, which is baked into the read-only rootfs at flash time, and quarantines
anything that does not match. There are two ways to keep a build.

### 5a. On-device update (seconds, one reboot)

**One-time cost first.** S97 lives in the read-only rootfs, so a device flashed before this existed
has an S97 that does not know about update slots and will ignore one. Getting the update path onto
a device therefore takes exactly one more reflash (5b) - and then no more, for UI changes. Flash the
newest `mq_ui` at the same time so that reflash is not wasted. `--persist` refuses to run against an
older S97 rather than arming a slot that would silently do nothing.

Turn on **Settings -> System -> On-Device Updates** on the device, once. Then:

```sh
DISKOS_IP=<device-ip> DISKOS_PW=<debug-password> tools/diskos-deploy.sh --persist ui/mq_ui
```

That does a normal deploy and then arms an update slot. On the next boot S97 adopts the binary and
records its hash, so every boot after that verifies against the adopted hash and runs your build.

What the flag actually changes, in one line: without it S97 accepts **only** the binary the flasher
blessed; with it S97 **also** accepts one whose SHA-256 matches a manifest sitting in `/usr/data`.
Anyone who can write that manifest already has root on the device, so it grants no new capability to
an attacker who is already there - but the rootfs stops being the sole root of trust, which is why
it is a switch the user throws rather than a default. `diskos-deploy.sh` will not create the flag
for you.

Details worth knowing:

- **The slot is written last**, after the binary has been confirmed running on the device. A build
  that fails to start exits earlier and leaves no slot behind, so "permanent" is never granted to
  something that has not run here at least once.
- **The slot is one-shot.** S97 clears it whether it verifies or not, so a bad drop cannot make the
  device retry it on every boot forever.
- **The previous binary is kept** at `/usr/data/mq_ui.prev` (a rename, so it costs no flash). One
  rollback deep; to use it, `mv` it back and reboot.
- **Turning the setting off is not a trap.** The adopted binary stops verifying, so S97 quarantines
  it and reinstalls the copy embedded in the rootfs at `/opt/diskos/mq_ui`. The device comes back on
  the **flashed diskOS build**, not on stock.
- **A reflash always wins.** The adopted manifest records which rootfs manifest it was adopted
  against; a flash replaces that, so the adoption is dropped and the freshly flashed UI installs.
  Without this, `/usr/data` surviving the flash would leave the old adopted binary in place, still
  verifying, and a 90-minute reflash would silently give you back the build you already had.
- **Shape is checked regardless.** A non-ELF or non-MIPS file is refused before anything is made
  executable, whichever manifest blessed it. `--persist` also refuses a non-MIPS binary host-side,
  because the native fast check writes x86-64 to the exact path the deploy script pushes.

The whole mechanism is exercised by `python3 tests/s97check.py`, which runs the real S97 against a
fake root - thirteen scenarios including a corrupt slot, a wrong-architecture slot, the flag being
switched back off after an adoption, and a reflash landing on top of one. Run it after touching
`payload/S97diskos_install`; the failure modes are otherwise only discoverable by rebooting a
device, where each wrong answer costs another flash to undo.

### 5b. Full reflash

Needed when the change is not just the UI binary (rootfs, kernel hooks, the manifest itself), or to
re-establish the flashed build as the baseline:

```sh
python3 -m diskos_installer install --stock <stock_rootfs.squashfs> --ui path/to/mq_ui --variant public
```

`--ui` overrides the bundled UI binary, so you can flash your own build. This is a mask-ROM write and
takes about 15 minutes. The installer prompts for confirmation before it writes; add `-y` only when you
deliberately want to skip that prompt (for example in a script), since it rewrites the root filesystem.

### macOS note

The installer looks for host-native tools under `vendor/<host-tag>/` (for example
`vendor/macos-arm64/`), not on your `PATH`. The repo ships only the prebuilt Linux x86-64 tools, so on
a Mac you build the native tools once:

```sh
cd installer
./vendor/setup-macos.sh
```

It builds `usbboot`, `mksquashfs`, and `unsquashfs` (with load paths rewritten) into
`vendor/macos-arm64/` from `src/usbboot` plus Homebrew deps (`libusb squashfs lzo dylibbundler`) and the
Xcode command line tools. After that the installer finds them and the `[E102]` error goes away. The end
user of a released build needs none of this.

## Notes

- **Screenshots:** the device ships `fbshot`, which writes the framebuffer to `/usr/data/fb.raw`
  (360x360, 32bpp BGRA, panel rotated 180 degrees). Pull it and convert to view.
- **Player logs:** `/usr/data/fiio/log/fiio_player.log`. **PCM state:**
  `/proc/asound/card*/pcm*p/sub*/status` (`RUNNING` = playing).
- **Raw player frames** (for debugging IPC): `/usr/data/psend <FRAME>` sends a raw `/player` command.

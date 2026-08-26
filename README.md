# diskOS

**diskOS** is a custom player UI/firmware for the **FiiO Snowsky Disc** digital audio player
(Ingenic X2000). It replaces the stock interface, and ships with an installer (run from source with
your own Python; a setup script sets up a local virtual environment) that flashes it onto the device
over the chip's mask-ROM USB mode - building the image **from your own stock firmware**, so no FiiO
rootfs is redistributed. The installer tooling in this repo is open
source; the diskOS UI itself (`payload/mq_ui`) currently ships as a **binary-only component**
(see [License](#license)).

> ⚠️ **Unsupported beta tool - read this.** Installing **erases and rewrites the device's main
> root filesystem**. Power loss, host sleep, a bad cable, a software defect, an unsupported
> DRAM/NAND variant, or running it on the wrong X2000 device can leave the player unbootable, lose
> data, or require hardware recovery. Mask-ROM reflashing recovered the tested unit(s), but
> **recovery is not guaranteed** for every unit or every failure. This may void your warranty.
> Not affiliated with or endorsed by FiiO, Snowsky, or Ingenic. **Proceed at your own risk; no
> warranty or support is promised.** Back up your data and keep the installer's saved stock image
> on separate storage.

> 🧪 **BETA.** Validated on the **Winbond W63AH6NKB (LPDDR3)** DRAM - the chip the Snowsky Disc
> uses. The Disc's own stock bootloader initializes only this chip, so every Disc that runs stock
> firmware has it; this is the Disc's DRAM, not one of several variants. We have flashed and booted
> our own unit(s), but wide field-testing is still ongoing, so treat it as beta. In the unlikely
> event a future hardware revision ever ships different DRAM, the flash fails safe at memory init
> (the device stays mask-ROM-recoverable) rather than completing. Report your results.

## Documentation

| Doc | What's in it |
|---|---|
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Live-probed hardware capability map: SoC, the quad CS43131 balanced DACs, display planes, power ICs, wireless, USB, and the stock-vs-hardware gap list. |
| [`docs/COMMAND_MAP.md`](docs/COMMAND_MAP.md) | The player IPC command surface: ~221 `mq_player` tags, reply frames, and the MCU/SPI command set - what you drive to build features. |
| [`docs/RE_CATALOGUE.md`](docs/RE_CATALOGUE.md) | Reverse-engineering of the `mq_player`/`mq_ui` binaries: structure, tables, string maps, network receivers. |
| [`NOTICE.md`](NOTICE.md) | Third-party components and their licenses (GPL usbboot, squashfs-tools, the SPL, etc.). |
| [`SPL_SOURCE.md`](SPL_SOURCE.md) | GPL corresponding source + build recipe for the stage-1 DRAM bring-up bootloader. |
| [`DEPENDENCY_INVENTORY.md`](DEPENDENCY_INVENTORY.md) | Libraries a *self-built* onefile would bundle + their obligations (the source release does not bundle these). |
| [`docs/PRIVACY.md`](docs/PRIVACY.md) | What the installer and the on-device UI send over the network. |
| [`agents/`](agents/) | A project brief you can drop into Claude Code or Codex to work on diskOS with an AI agent. |
| [`SECURITY.md`](SECURITY.md) / [`CONTRIBUTING.md`](CONTRIBUTING.md) | How to report vulnerabilities; how to contribute safely. |
| [`licenses/`](licenses/) | Verbatim license texts for the shipped third-party components. |

## First-time setup

The installer runs from source with your own Python. A one-time setup script builds a local
virtual environment and installs the two Python dependencies into it (nothing is installed
system-wide):

```bash
./install.sh
```

You need **Python 3.8+**. The script also checks for two optional system components and prints the
exact package to install if either is missing:
- **Tk / tkinter** - only for the *graphical* installer (the command line works without it).
  Debian/Ubuntu: `sudo apt install python3-tk`.
- **libusb-1.0** - to detect the device in mask-ROM mode.
  Debian/Ubuntu: `sudo apt install libusb-1.0-0`.

After setup, run everything through `./diskos-installer` (it uses the `.venv` automatically).

## Install (graphical)

1. Launch the graphical installer: `./diskos-installer gui`.
2. Choose **Install diskOS**, pick your FiiO firmware `.zip`, and a variant:
   - **Public** - no *always-on* root shell (recommended). Debug Mode can still enable SSH on
     demand from the UI (opt-in, off by default - see below).
   - **Dev** - adds an always-on USB-serial **root shell**. ⚠️ **This is a passwordless root shell
     available to anyone with physical USB access, on every boot; it bypasses normal device
     security.** Only use it on a development device you control, never an everyday or untrusted one.
3. Put the device in **mask-ROM**: power it **off**, hold **Volume-Down**, and plug in USB (the
   screen stays **black** - that's correct).
4. Click **Install**, tick the acknowledgement, and **Begin**. Don't disconnect or let the computer
   sleep during the flash (~**60-90 minutes**).
5. When it verifies, **power-cycle** the device. The UI is embedded in the flashed image, so first
   boot installs diskOS automatically - **no microSD step needed**.

## Install (command line)

```bash
./install.sh                                   # one-time: build the .venv + install Python deps
./diskos-installer doctor                      # check host + bundled tools + device
./diskos-installer install --firmware SNOWSKY_DISC_update_*.zip --variant public
./diskos-installer restore-stock               # deactivate diskOS -> reflash your saved stock rootfs
./diskos-installer remove                      # delete the installer's saved files from this computer
```

## Requirements

- Your device's **official FiiO firmware** as a `.zip` (we never ship FiiO's rootfs - you supply
  it; download it from FiiO's firmware page for the Snowsky Disc). The installer decrypts and
  extracts the stock rootfs from it locally.
- A USB cable and ~**60-90 minutes** for the flash.
- **Python 3.8+** and the two pip dependencies (installed into a local `.venv` by `./install.sh`).
- USB access to the mask-ROM device (`a108:eaef`). **Don't run the installer as root** - it keeps your
  saved recovery image and state under your home directory, and `sudo` would misplace them. Instead
  install the bundled udev rule once so your normal user has access:
  ```bash
  sudo cp udev/70-diskos-maskrom.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
  (The whole build+save+flash runs as one user process; there is no separate "flash step" to elevate.)
- **Linux (x86_64):** this is the released platform - the tarball ships the native flash tools
  prebuilt in `vendor/linux-x86_64/`.
- **macOS (Apple Silicon + Intel):** no release artifact yet; it's a **build-from-source** path -
  clone the repo and run `build/build-macos.sh` first (`libusb` via `brew install libusb`). See
  *For developers*. Treat macOS as unverified until built and flashed.

## Remove diskOS / restore stock

`restore-stock` reflashes the stock rootfs the installer built and saved during install - a
checksum-verified reconstruction of your firmware's rootfs (not a dump of the device's original
partition). This **deactivates diskOS**; inert diskOS files under `/usr/data` remain until you
delete them - it is not a factory wipe. You can also switch back temporarily:
**Settings → System → Default UI → Stock**, or hold **Vol-Up at power-on** to boot the other UI once.

If a flash fails or is interrupted, the main rootfs may be **partially written** and the device
may not boot normally. In the tested cases you can return to mask-ROM (power off, hold Vol-Down,
replug) and re-flash or restore your saved stock image. Mask-ROM is normally reachable because it
lives in on-chip ROM and is entered by a button combo before any flashed code runs, but recovery
is **not guaranteed** on untested hardware or every failure mode.

## What's proven vs. beta (honest)

- ✅ **Flash mechanism + image build** - proven; it's how diskOS was flashed to real hardware,
  including the bad-block-aware writer skipping factory bad blocks and verifying every block.
- ✅ **Firmware extraction** - reproduces the stock rootfs byte-for-byte from FiiO's zip.
- ✅ **Linux** - the app builds and flashes end-to-end.
- ⚠️ **macOS** - code is macOS-aware and the build recipe is provided, but the macOS artifact has
  not yet been produced/tested on Apple hardware. Treat as unverified until built + flashed.
- ⚠️ Each stock firmware version should be flash-tested before it's declared supported
  (V2.09 and V2.28 so far).

## Known issues (this beta)

diskOS is an early beta. What we know about:

- **Firmware coverage.** Only **V2.09** and **V2.28** are flash-tested. Newer stock versions use
  different internal command codes and are refused by default (override at your own risk).
- **~90-minute flash.** A full install writes **and verifies** the whole root filesystem over USB, so
  it takes roughly **60-90 minutes**. Don't disconnect or let the host sleep. (Most of that time is a
  conservative fixed wait; a faster writer is planned.)
- **No on-device updates.** Updating diskOS means re-flashing with the installer; there is no
  over-the-air path yet.
- **microSD at cold boot.** The card is auto-mounted a few seconds into boot. If your library is
  empty right after a **cold** boot, reinsert the card once and it should mount.
- **Output/work modes.** USB-DAC, Bluetooth-receiver, and USB-storage modes are wired but lightly
  tested; local playback is the well-worn path.
- **USB-serial debug is unreliable.** The optional serial shell can be finicky (a USB CDC-ACM
  flow-control quirk); prefer **Debug Mode's SSH over WiFi** for remote access (see below).
- **Last.fm scrobbling is unverified beta.** The building blocks (HTTPS transport, request signing,
  parsing) are tested, but the full connect-and-scrobble round-trip against a live Last.fm account has
  not yet been run end-to-end. It is **off by default** and needs your own Last.fm API key; treat it as
  experimental. Credential setup transfers your API key over your **local network in plain HTTP** - see
  [`docs/PRIVACY.md`](docs/PRIVACY.md).

Found something else? Open an issue with your device's **firmware version** and any on-screen
**error code**.

## Debug Mode (optional remote access)

For development or troubleshooting, diskOS can expose a remote shell **on demand** - it is **OFF by
default**. Open **Settings → System → Debug Mode → Enable Debug**. The screen then shows:

- an **SSH** command (`ssh root@<device-ip>`) reachable over WiFi, and
- a **fresh random password**, generated per-enable and shown on that screen.

> ⚠️ **This grants root access to the device over your network while it is on.** Only enable it on a
> network you trust, and turn it **off** when you are done. The password is random and rotates each
> time you enable it; the device's stock password is never used or exposed.
>
> **Where the password lives:** while Debug Mode is on, the current password is also written in
> **plaintext** to `/usr/data/sshd/current_pw` (mode 0600, root-only) so the screen can show it again
> after a UI restart. Disabling Debug Mode deletes it. A **reboot while it is still on** can leave a
> stale copy - harmless, because a reboot drops the SSH overlay (so that password no longer works
> until you re-enable), but you can delete the file manually if you want it gone. It is only as
> protected as physical/root access to the device's storage.
>
> **SSH server:** Debug Mode uses **Dropbear 2022.83**, which predates the CVE-2023-48795 "Terrapin"
> Strict-KEX mitigation (an update is planned). Because Debug Mode is opt-in and short-lived, exposure
> is limited, but keep it off on untrusted networks. Serial (USB) is available only on **dev** builds
> and is unreliable (see the known issue above); public builds expose no serial shell.

## How it works (in brief)

diskOS runs by a small hook in the stock `fiio_init.sh` that launches our UI (`mq_ui`) instead of
the stock one. Because the rootfs is a **read-only squashfs**, enabling that hook means rewriting
the rootfs partition (mtd2) - hence the flash. The UI itself is embedded in the image and installed
to the writable `/usr/data` on first boot, verified against a baked SHA-256 manifest; if it does
not match, the stock UI runs instead (**fail-closed**). You **cannot** install from the device's own
"System updates" menu - that path checks a signature we can't forge, so mask-ROM USB is the only
way in. See [`docs/HARDWARE.md`](docs/HARDWARE.md) for the partition map and the rest of the hardware.

## For developers

The installer already **runs from source** - `./install.sh` then `./diskos-installer` (see
*First-time setup*). The release tarball ships the native flash tools prebuilt in
`vendor/<os>-<arch>/`; a fresh **git clone** does not include them (they are large binaries kept out
of git), so build them once:

```bash
# Build the native flash tools into vendor/<os>-<arch>/ :
bash build/build-usbboot-static.sh     # static usbboot   (Linux)
bash build/build-squashfs-static.sh    # static mksquashfs/unsquashfs (Linux)
bash build/build-macos.sh              # usbboot + libusb (run on a Mac)
```

Optionally, you can package everything into a single self-contained binary (this is **not** how the
release ships, and it bundles ~90 system libraries - see
[`licenses/THIRD_PARTY_BUNDLED.md`](licenses/THIRD_PARTY_BUNDLED.md) before redistributing one):

```bash
bash build/build.sh                    # -> build/dist/diskos-installer (optional onefile)
```

See [`build/README-vendor.md`](build/README-vendor.md) for how the native tools are produced and the
portability requirements. Working on diskOS with an AI agent? Start with
[`agents/AGENTS.md`](agents/AGENTS.md).

## Error codes (for bug reports)

If the installer stops with an error, it prints a short code like `[E301]`. **Quote that code**
when reporting an issue - it tells us exactly where it stopped. In the tested cases the device
stays reachable in mask-ROM and your saved stock image can be re-flashed, but recovery is not
guaranteed on untested hardware or every failure mode.

| Code | Meaning |
|---|---|
| **E1xx** | **Environment / preflight - nothing was written to the device** |
| E101 | Unsupported host OS/arch (Linux/macOS only) |
| E102 | A bundled component is missing - re-download the installer |
| E103 | A bundled tool can't run (its directory is mounted `noexec`, or a missing library) |
| E110 | No device in mask-ROM mode (power off, hold Vol-Down, plug USB) |
| E111 | More than one device in mask-ROM mode - unplug the others |
| E112 | Can't confirm exactly one device (USB permissions / no libusb) |
| E120 / E121 / E122 | Image not found / wrong size / not a squashfs |
| E140 / E141 | No firmware `.zip` provided / no saved stock image to restore |
| E142 | `DISKOS_INSTALLER_HOME` points at a non-empty, non-state directory |
| **E2xx** | **Firmware extract & image build** |
| E201 / E202 | Input isn't a zip / unsafe zip (path escape or bomb) |
| E210 | FiiO OTA manifest missing, ambiguous, or unsafe |
| E211 | AES-decrypt failed (wrong key or not a FiiO OTA) |
| E212 / E213 | Rootfs chunks missing/duplicate / assembled image invalid |
| E220 / E221 | Not a Snowsky Disc rootfs / untested firmware version |
| E222 / E223 | diskOS UI binary invalid / boot-hook anchor problem |
| E224 | Stock rootfs doesn't match the known-good pinned hash (modified/corrupt firmware) |
| E230 / E231 / E232 | squashfs pack/unpack failed / image too large / output failed validation |
| E233 | A file in the stock rootfs resolves outside it via a symlink (crafted/corrupt firmware) |
| E240 | Stock image larger than the partition (refusing to truncate) |
| E250 | Invalid variant (must be `public` or `dev`) |
| **E3xx** | **Flashing (host side)** |
| E301 | No result read back - flash outcome UNKNOWN (assume failed, re-flash) |
| E302 | Short/truncated result readback - flash outcome UNKNOWN (assume failed, re-flash) |
| E303 | **Flash timed out - the device stopped responding (likely reset mid-flash)** |
| E310 | Flash verify failed - see the device code below |
| **F1xx** | **Device writer aborted and reported a coded reason.** It fails closed rather than committing a bad block mapping, but blocks erased/written before the abort may already be modified - the rootfs can be partially written. Return to mask-ROM and re-flash or restore stock. |
| F101…F106 | init/ECC, out-of-space, block-write fail, too many bad blocks, ECC re-enable, bad-block marker |

Exit codes: `0` success, `1` error, `2` usage/preflight refusal (unsupported host, missing
`--firmware`, bad arguments, or Tk unavailable for the GUI), `3` you cancelled at a prompt,
`130` interrupted.

## Support

diskOS is a solo, open-source hobby project. If it's useful to you and you'd like to help keep the
work going, you can leave a tip:

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20diskOS-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/b0hemia)

**[ko-fi.com/b0hemia](https://ko-fi.com/b0hemia)** - entirely optional, and hugely appreciated.

## License

- **Original diskOS installer files** in this repo (the Python installer, build scripts, docs)
  are **MIT** - see [`LICENSE`](LICENSE). SPDX: `MIT`.
- **Third-party components** retain their own licenses and ship with corresponding source: the native
  flash tools **usbboot** and **squashfs-tools** (**GPL-2.0**), the libraries they statically link
  (**liblzo2** GPL-2.0, **libusb-1.0** LGPL-2.1, zlib/liblzma), and the stage-1 **SPL** (**GPL-2.0**).
  The corresponding source for all of these ships in [`corresponding-source/`](corresponding-source/)
  (SPL also in [`spl-src/`](spl-src/) + [`SPL_SOURCE.md`](SPL_SOURCE.md)); see
  [`NOTICE.md`](NOTICE.md) for the full mapping and the relink path for the LGPL libusb.
- **The diskOS UI (`payload/mq_ui`)** currently ships as a **binary-only** component. It is not
  covered by the MIT license above and its source is not yet published; treat it as a redistributable
  binary whose provenance, version, and third-party content are still being documented. If/when the
  UI source is published this note will be updated.

> **Do not redistribute the `diskos_*.bin` images the installer builds** - they contain FiiO's
> rootfs. The installer produces them locally from *your* firmware; that is fine for your own use,
> but sharing them would redistribute FiiO's software. Share the installer, not the built image.

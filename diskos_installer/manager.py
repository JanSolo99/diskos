"""diskOS manager - the front door.

One place to answer the three questions you actually have when you own a Disc:

    Is my device there, and what mode is it in?
    Can I get back to stock if I want to?
    Install / revert.

Everything destructive delegates to the SAME proven service functions the installer
uses (`service.do_install` / `service.do_restore`) under the SAME single-run lock.
This module adds no new way to write to the device; what it adds is the things the
installer never had:

  * detection that also covers the device in its NORMAL (running) mode. Only the
    mask-ROM identity a108:eaef is known for certain, so the manager LEARNS the
    running identity from the user's own device rather than guessing at one.
  * a restore point you can create, verify and export BEFORE you change anything,
    instead of one that appears as a side effect part-way through a 90-minute flash.
  * `watch`, because putting the Disc into mask-ROM mode is a blind button-hold and
    the only feedback today is running a command afterwards to see if it worked.

A word on the limits, since a program called "backup" invites an assumption: this
backs up what is needed to put the STOCK FIRMWARE back. It is not a bit-for-bit dump
of the device's NAND. `usbboot` does carry a `--dump-partition` path, but nothing in
this project has ever exercised it on a Disc, and an unverified read offered as
"your backup" is worse than no backup at all. See `status` output, which says this
out loud rather than leaving it to be discovered.
"""

import json
import os
import shutil
import time

from diskos_installer import (__version__, bundle, imagebuild, platform_probe,
                              service, state, ui)
from diskos_installer.reporter import CLIReporter
from diskos_installer.runlock import RunLock

MASKROM_ID = (platform_probe.MASKROM_VID, platform_probe.MASKROM_PID)


# ---------------------------------------------------------------- device identity

def _devices_path():
    return os.path.join(state.state_dir(), "devices.json")


def load_devices():
    try:
        with open(_devices_path(), "r", encoding="utf-8") as f:
            d = json.load(f)
            return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_devices(d):
    path = _devices_path()
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=2, sort_keys=True)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def _fmt_id(vid, pid):
    return f"{vid:04x}:{pid:04x}"


def normal_mode_id():
    """The learned running-mode (vid, pid), or None if we have not learned one."""
    d = load_devices().get("normal") or {}
    try:
        return int(d["vid"]), int(d["pid"])
    except (KeyError, TypeError, ValueError):
        return None


def probe():
    """A single snapshot of what is attached.

    Returns a dict. `usb` is None when USB cannot be enumerated at all, which is a
    genuinely different answer from "nothing is attached" and is reported as such -
    the most common cause is a missing libusb, not a missing device."""
    devs = platform_probe.usb_list()
    out = {"usb": devs, "maskrom": 0, "normal": None, "learned": normal_mode_id()}
    if devs is None:
        out["maskrom"] = platform_probe.maskrom_count()   # may still work via lsusb
        return out
    out["maskrom"] = sum(1 for v, p, _ in devs if (v, p) == MASKROM_ID)
    if out["learned"]:
        for v, p, label in devs:
            if (v, p) == out["learned"]:
                out["normal"] = (v, p, label)
                break
    return out


def describe(pr):
    """One human line about the device, plus a hint when there is something to do."""
    if pr["usb"] is None and pr["maskrom"] < 0:
        return (ui.yellow("cannot enumerate USB"),
                "install libusb-1.0 (Debian/Ubuntu: sudo apt install libusb-1.0-0), then retry")
    if pr["maskrom"] == 1:
        return (ui.green("in mask-ROM mode - ready to flash"), None)
    if pr["maskrom"] > 1:
        return (ui.yellow(f"{pr['maskrom']} devices in mask-ROM mode"),
                "unplug the other Ingenic devices - exactly one must be connected")
    if pr["normal"]:
        v, p, label = pr["normal"]
        return (ui.green(f"connected and running ({_fmt_id(v, p)}{' ' + label if label else ''})"),
                "to flash: power it off, hold Volume-Down, plug USB in (the screen stays black)")
    if pr["learned"]:
        return (ui.dim("not connected"), None)
    return (ui.dim("not in mask-ROM mode"),
            "if it is plugged in and running, teach the tool to recognise it: diskos-manager detect --learn")


# ---------------------------------------------------------------- restore point

def restore_point():
    """What we could put back on the device today, as a dict for display."""
    st = state.load()
    bin_path, sha_path = state.stock_paths()
    info = {
        # stock_image_exists(), NOT have_stock_image(): the latter re-hashes the whole
        # 76 MB image on every call, and this runs on every status print and every
        # redraw of the menu loop. Presence is what a status line can honestly claim;
        # proving it is still intact is what `backup --verify` is for.
        "present": state.stock_image_exists(),
        "path": bin_path,
        "version": st.get("stock_version"),
        "sha256": st.get("stock_sha256"),
        "source": st.get("stock_source"),
        "saved_at": st.get("stock_saved_at"),
        "size": os.path.getsize(bin_path) if os.path.exists(bin_path) else 0,
    }
    if info["sha256"] is None and os.path.exists(sha_path):
        # Saved by an older version that recorded the digest only in the sidecar.
        try:
            with open(sha_path, "r", encoding="utf-8") as f:
                info["sha256"] = f.read().split()[0]
        except (OSError, IndexError):
            pass
    return info


def verify_restore_point(rep=None):
    """Re-hash the saved image and check it against the digest recorded beside it.

    `have_stock_image()` already compares the two, but it does so silently as a
    precondition. A backup you cannot ask "are you still good?" is not one you can
    rely on, so this reports what it found."""
    rep = rep or CLIReporter()
    bin_path, sha_path = state.stock_paths()
    if not os.path.exists(bin_path):
        ui.err("no restore point saved.")
        return False
    if not os.path.exists(sha_path):
        ui.err("restore point has no checksum beside it - cannot verify.")
        return False
    with open(sha_path, "r", encoding="utf-8") as f:
        want = f.read().split()[0].strip()
    ui.info(f"hashing {bin_path} ({os.path.getsize(bin_path)} bytes)…")
    got = state.sha256_file(bin_path, progress=lambda r, t: rep.progress(r, t))
    if got != want:
        ui.err(f"CHECKSUM MISMATCH - the saved image is corrupt.\n  expected {want}\n  got      {got}")
        ui.info("re-create it from your firmware zip:  diskos-manager backup --firmware <zip>")
        return False
    ui.ok(f"restore point verified (sha256={got[:16]}…)")
    return True


MANIFEST = "diskos-restore-point.json"


def export_restore_point(dest_dir):
    """Copy the restore point somewhere durable, with a manifest, and verify the copy.

    The saved image lives under the tool's own state directory, which `remove` deletes
    and a reinstalled OS takes with it. Being able to put a copy on another disk is
    the difference between a backup and a cache."""
    info = restore_point()
    if not info["present"]:
        ui.err("no restore point to export. Create one first: diskos-manager backup --firmware <zip>")
        return False
    bin_path, sha_path = state.stock_paths()
    tag = f"{info['version'] or 'unknown'}-{time.strftime('%Y%m%d')}"
    out = os.path.join(dest_dir, f"diskos-restore-point-{tag}")
    os.makedirs(out, exist_ok=True)

    ui.info(f"copying to {out}")
    shutil.copy2(bin_path, os.path.join(out, "stock.bin"))
    shutil.copy2(sha_path, os.path.join(out, "stock.sha256"))
    with open(os.path.join(out, MANIFEST), "w", encoding="utf-8") as f:
        json.dump({
            "kind": "diskos-restore-point",
            "tool_version": __version__,
            "stock_version": info["version"],
            "sha256": info["sha256"],
            "source": info["source"],
            "saved_at": info["saved_at"],
            "exported_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "size": info["size"],
        }, f, indent=2, sort_keys=True)

    # Verify the COPY, not the original: a bad cable or a full disk is exactly the
    # kind of thing that produces a file that looks right and is not.
    ui.info("verifying the copy…")
    got = state.sha256_file(os.path.join(out, "stock.bin"))
    want = info["sha256"]
    if want and got != want:
        ui.err(f"the copy does not match ({got[:16]}… != {want[:16]}…) - do not rely on it.")
        return False
    ui.ok(f"exported and verified: {out}")
    return True


def import_restore_point(src_dir, rep=None):
    """Adopt a previously exported restore point as the active one."""
    rep = rep or CLIReporter()
    src_bin = os.path.join(src_dir, "stock.bin")
    if not os.path.exists(src_bin):
        # Also accept being pointed straight at the .bin.
        if os.path.isfile(src_dir) and src_dir.endswith(".bin"):
            src_bin = src_dir
        else:
            ui.err(f"no stock.bin in {src_dir}")
            return False

    man = {}
    man_path = os.path.join(os.path.dirname(src_bin), MANIFEST)
    if os.path.exists(man_path):
        try:
            with open(man_path, "r", encoding="utf-8") as f:
                man = json.load(f)
        except (OSError, ValueError):
            man = {}

    if man.get("sha256"):
        ui.info("checking the exported copy against its manifest…")
        got = state.sha256_file(src_bin, progress=lambda r, t: rep.progress(r, t))
        if got != man["sha256"]:
            ui.err("the exported image does not match its manifest - refusing to import it.")
            return False

    # Validate it really is a genuine, supported Disc rootfs BEFORE it replaces a
    # restore point that may currently be good. Same gate do_install and do_restore use.
    version = imagebuild.validate_stock_rootfs(src_bin, rep)
    digest = state.save_stock_image(src_bin, progress=lambda r, t: rep.progress(r, t))
    st = state.load()
    st.update({
        "stock_version": version,
        "stock_sha256": digest,
        "stock_source": man.get("source") or os.path.basename(src_bin),
        "stock_saved_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    })
    state.save(st)
    ui.ok(f"imported restore point (stock {version}).")
    return True


# ---------------------------------------------------------------- commands

def cmd_status(args):
    ui.step(f"diskOS manager {__version__}")
    pr = probe()
    line, hint = describe(pr)
    ui.info(f"device        : {line}")
    if hint:
        ui.info(ui.dim(f"                {hint}"))

    rp = restore_point()
    if rp["present"]:
        detail = f"stock {rp['version']}" if rp["version"] else "saved"
        when = f", saved {rp['saved_at']}" if rp["saved_at"] else ""
        ui.ok(f"restore point : {detail}{when}")
        if rp["sha256"]:
            ui.info(ui.dim(f"                sha256 {rp['sha256'][:16]}…  {rp['path']}"))
    else:
        ui.warn("restore point : NONE - you could not put stock firmware back")
        ui.info(ui.dim("                create one:  diskos-manager backup --firmware <FiiO update .zip>"))

    st = state.load()
    if st.get("installed"):
        ui.info(f"diskOS        : installed by this tool ({st.get('variant')}, {st.get('installed_at')})")
    else:
        ui.info("diskOS        : no install recorded by this tool")

    ui.info(f"state dir     : {state.state_dir()}")
    ui.info(ui.dim("note: the restore point is your STOCK FIRMWARE, not a dump of the device's"))
    ui.info(ui.dim("      NAND. Files you keep on the player (/usr/data) are not part of it."))
    ui.info(ui.dim("run 'diskos-installer doctor' for the bundled-tool check."))
    return 0


def cmd_detect(args):
    if args.learn:
        return _learn()
    if args.watch:
        return _watch(args.watch)
    pr = probe()
    line, hint = describe(pr)
    ui.step("Device")
    ui.info(line)
    if hint:
        ui.info(ui.dim(hint))
    if args.verbose and pr["usb"]:
        ui.info("attached USB devices:")
        for v, p, label in sorted(pr["usb"]):
            mark = "  <- mask-ROM" if (v, p) == MASKROM_ID else (
                   "  <- your Disc" if pr["learned"] == (v, p) else "")
            ui.info(f"  {_fmt_id(v, p)}  {label}{mark}")
    return 0 if (pr["maskrom"] == 1 or pr["normal"]) else 1


def new_devices(before, after):
    """Devices in `after` that were not in `before`, as a multiset difference.

    A multiset, not a set: hubs and identical peripherals mean the same (vid, pid) can
    legitimately appear more than once, and plugging in a second one of something must
    still register as new. The mask-ROM identity is filtered out - that is the flashing
    mode, not the running mode we are trying to learn."""
    seen = {}
    for d in before:
        seen[d[:2]] = seen.get(d[:2], 0) + 1
    out = []
    for d in after:
        key = d[:2]
        if seen.get(key, 0) > 0:
            seen[key] -= 1
        elif key != MASKROM_ID:
            out.append(d)
    return out


def _learn():
    """Learn the running-mode USB id by watching what appears when the user plugs in.

    The project only ever recorded the mask-ROM identity, so rather than guess at a
    vendor id, ask the device. Diffing across a plug-in is exact and needs no
    database to go stale."""
    ui.step("Teach the tool to recognise your Disc")
    ui.info("This just watches which USB device appears - nothing is written to the player.")
    if platform_probe.usb_list() is None:
        ui.err("cannot enumerate USB on this host, so there is nothing to compare.")
        ui.info("install libusb-1.0 (Debian/Ubuntu: sudo apt install libusb-1.0-0) and retry.")
        return 1

    try:
        input("  1. UNPLUG the Disc, then press Enter… ")
    except EOFError:
        print(); ui.info("cancelled."); return 1
    before = platform_probe.usb_list() or []
    try:
        input("  2. Plug it in, powered ON (normal mode), then press Enter… ")
    except EOFError:
        print(); ui.info("cancelled."); return 1
    time.sleep(1.5)                      # let the host finish enumerating
    after = platform_probe.usb_list() or []

    new = new_devices(before, after)
    if not new:
        ui.err("nothing new appeared.")
        ui.info("Check the cable carries data (some are charge-only), and that the player is on.")
        return 1
    if len(new) > 1:
        ui.warn("more than one device appeared:")
        for i, (v, p, label) in enumerate(new, 1):
            ui.info(f"  {i}. {_fmt_id(v, p)}  {label}")
        try:
            pick = int(input("  which one is the Disc? [1] ") or "1")
        except (ValueError, EOFError):
            pick = 1
        if not 1 <= pick <= len(new):
            ui.err("no such choice.")
            return 1
        chosen = new[pick - 1]
    else:
        chosen = new[0]

    v, p, label = chosen
    d = load_devices()
    d["normal"] = {"vid": v, "pid": p, "label": label,
                   "learned_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    save_devices(d)
    ui.ok(f"learned: {_fmt_id(v, p)}{' ' + label if label else ''}")
    ui.info("'diskos-manager status' will now recognise it when it is plugged in and running.")
    return 0


def _watch(timeout):
    """Wait for the device to enter mask-ROM mode.

    Getting into mask-ROM is a blind button-hold with a black screen, so today the
    only way to know it worked is to run something afterwards and read an error.
    This just tells you the moment it appears."""
    ui.step("Waiting for mask-ROM mode")
    ui.info("Power the Disc OFF, hold Volume-Down, and plug in USB. The screen stays black.")
    ui.info(ui.dim(f"(waiting up to {timeout}s; Ctrl-C to stop)"))
    spin = "|/-\\"
    start = time.time()
    i = 0
    try:
        while time.time() - start < timeout:
            n = platform_probe.maskrom_count()
            if n == 1:
                print("\r" + " " * 40 + "\r", end="")
                ui.ok("device is in mask-ROM mode - ready to flash.")
                return 0
            if n > 1:
                print("\r" + " " * 40 + "\r", end="")
                ui.warn(f"{n} devices in mask-ROM mode - unplug the others.")
                return 1
            print(f"\r  {spin[i % len(spin)]} waiting… {int(time.time() - start)}s ", end="", flush=True)
            i += 1
            time.sleep(0.5)
    except KeyboardInterrupt:
        print()
        ui.info("stopped.")
        return 130
    print()
    ui.err("timed out - the device never appeared in mask-ROM mode.")
    ui.info("Try again: fully power OFF first, hold Volume-Down BEFORE plugging in, and")
    ui.info("check the cable carries data (some are charge-only).")
    return 1


def cmd_backup(args):
    if args.export:
        return 0 if export_restore_point(args.export) else 1
    if args.import_from:
        with RunLock():
            return 0 if import_restore_point(args.import_from) else 1
    if args.verify:
        return 0 if verify_restore_point() else 1
    if not args.firmware and not args.stock:
        # No action given: report rather than silently doing nothing.
        rp = restore_point()
        if rp["present"]:
            ui.ok(f"restore point present (stock {rp['version'] or '?'}, saved {rp['saved_at'] or '?'})")
            ui.info("  verify it:  diskos-manager backup --verify")
            ui.info("  copy it:    diskos-manager backup --export <folder>")
            return 0
        ui.err("no restore point, and no firmware given to make one from.")
        ui.info("  diskos-manager backup --firmware <FiiO official update .zip>")
        return 2

    ui.step("Creating a restore point")
    ui.info("Reads your firmware zip and saves the stock rootfs it contains.")
    ui.info("The device is not touched, and nothing is flashed.")
    with RunLock():
        r = service.do_backup({"firmware": args.firmware, "stock": args.stock}, CLIReporter())
    if not r.get("ok"):
        return 1
    ui.info("Keep a copy somewhere other than this machine:")
    ui.info("  diskos-manager backup --export /path/to/somewhere")
    return 0


# ---------------------------------------------------------------- interactive menu

_MENU = [
    ("Check my device", "status"),
    ("Wait for mask-ROM mode (before flashing)", "watch"),
    ("Teach the tool to recognise my Disc", "learn"),
    ("Back up: save a restore point from my firmware zip", "backup"),
    ("Verify my restore point", "verify"),
    ("Export my restore point to a folder", "export"),
    ("Install diskOS", "install"),
    ("Restore stock firmware", "restore"),
    ("Quit", "quit"),
]


def _ask(prompt, default=""):
    """Read one answer. Returns None on EOF (Ctrl-D, or piped input running out).

    None has to be distinct from "": the menu loops until told to quit, so treating
    a closed stdin as an empty answer meant re-prompting forever against an input
    that can never produce another line."""
    try:
        v = input(f"  {prompt}").strip()
    except EOFError:
        print()
        return None
    return v or default


def menu(args=None):
    """Interactive front end. Every destructive choice goes through the same service
    call, the same confirmation summary and the same lock as the CLI - the menu is a
    way to reach them, not a second implementation of them."""
    while True:
        print()
        ui.step(f"diskOS manager {__version__}")
        pr = probe()
        line, _ = describe(pr)
        rp = restore_point()
        ui.info(f"device: {line}")
        ui.info("restore point: " + (
            ui.green(f"stock {rp['version']}" if rp["version"] else "saved")
            if rp["present"] else ui.yellow("none")))
        print()
        for i, (label, _key) in enumerate(_MENU, 1):
            print(f"   {i}. {label}")
        print()
        choice = _ask("choose [1]: ", "1")
        if choice is None:              # Ctrl-D / closed stdin: the same as choosing Quit
            return 0
        try:
            key = _MENU[int(choice) - 1][1]
        except (ValueError, IndexError):
            ui.err("no such choice.")
            continue

        try:
            if key == "quit":
                return 0
            if key == "status":
                cmd_status(None)
            elif key == "watch":
                _watch(180)
            elif key == "learn":
                _learn()
            elif key == "verify":
                verify_restore_point()
            elif key == "export":
                d = _ask("folder to copy it to: ")
                if d:
                    export_restore_point(os.path.expanduser(d))
            elif key == "backup":
                fw = _ask("path to your FiiO update .zip: ")
                if fw:
                    cmd_backup(_Args(firmware=os.path.expanduser(fw), stock=None,
                                     export=None, import_from=None, verify=False))
            elif key == "install":
                _menu_install()
            elif key == "restore":
                _menu_restore()
        except KeyboardInterrupt:
            print()
            ui.info("cancelled.")
        except Exception as e:            # a menu must never exit on one bad action
            ui.err(str(e))


class _Args:
    """Tiny stand-in so the menu can call the same cmd_* functions as the CLI."""
    def __init__(self, **kw):
        self.__dict__.update(kw)


def _menu_confirm(summary):
    ui.step("Ready - please confirm")
    for k in ("action", "variant", "image", "duration", "consequence"):
        if summary.get(k):
            ui.info(f"  {k}: {summary[k]}")
    return ui.confirm("Proceed?", default=False)


def _menu_install():
    if not restore_point()["present"]:
        ui.warn("You have no restore point yet.")
        ui.info("Installing will create one from the firmware zip you provide, but making it")
        ui.info("first (option 4) lets you verify and copy it somewhere safe beforehand.")
        if not ui.confirm("Continue to install anyway?", default=False):
            return
    fw = _ask("path to your FiiO update .zip: ")
    if not fw:
        return
    variant = _ask("variant [public]/dev: ", "public")
    if variant not in ("public", "dev"):
        ui.err("variant must be 'public' or 'dev'.")
        return
    if variant == "dev":
        ui.warn("'dev' adds an ALWAYS-ON PASSWORDLESS ROOT SHELL over USB - anyone with")
        ui.warn("physical access gets root on every boot. Dev devices only.")
        if not ui.confirm("Really use the dev variant?", default=False):
            return
    with RunLock():
        service.do_install({"firmware": os.path.expanduser(fw), "stock": None,
                            "ui_binary": None, "variant": variant},
                           CLIReporter(), _menu_confirm)


def _menu_restore():
    if not restore_point()["present"]:
        ui.err("no saved restore point - nothing to restore from.")
        ui.info("If you have an exported one:  diskos-manager backup --import <folder>")
        return
    with RunLock():
        service.do_restore({"firmware": None}, CLIReporter(), _menu_confirm)

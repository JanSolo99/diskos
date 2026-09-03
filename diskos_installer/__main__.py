"""diskOS installer CLI.

Commands:
  doctor         Report host, bundled tools, device presence, and saved state.
  install        Build a diskOS image from YOUR stock firmware and flash it.
  restore-stock  Deactivate diskOS: reflash your saved stock-rootfs image (leaves /usr/data files).
  remove         Delete everything this tool created (its full uninstall footprint).

Design: the whole tool is Python; the bricking-sensitive steps delegate to bundled
proven native binaries. Nothing is installed system-wide on Linux/macOS.
"""

import argparse
import os
import sys
import tempfile

from diskos_installer import (__version__, bundle, diag, errors, flasher, imagebuild,
                              manager, platform_probe, service, state, ui)
from diskos_installer.reporter import CLIReporter
from diskos_installer.runlock import RunLock


def _set_phase(st, phase):
    st["phase"] = phase
    state.save(st)


def cmd_doctor(args):
    ui.step("diskOS installer - doctor")
    o, a = platform_probe.host()
    ui.info(f"version    : {__version__}")
    ui.info(f"host       : {o}-{a} ({'supported' if platform_probe.is_supported() else 'UNSUPPORTED'})")
    ui.info(f"state dir  : {state.state_dir()}")

    ui.info("bundled tools:")
    all_ok = True
    for name in ("usbboot", "mksquashfs", "unsquashfs", "my_write5_dram.bin", "disc_spl_lpddr3.bin"):
        p = bundle.native(name, required=False)
        (ui.ok if p else ui.err)(f"  {name}: {p or 'MISSING'}")
        all_ok = all_ok and bool(p)
    for name in ("mq_ui", "S97diskos_install", "S99usbserial", "diskos-debug.sh", "dropbearmulti"):
        p = bundle.data(name, required=False)
        (ui.ok if p else ui.err)(f"  {name}: {p or 'MISSING'}")
        all_ok = all_ok and bool(p)   # payload files are required to build a flashable image

    n = platform_probe.maskrom_count()
    if n < 0:
        ui.warn("device: cannot enumerate USB (pyusb/lsusb unavailable)")
    elif n == 0:
        ui.info("device: none in mask-ROM mode (normal unless you're about to flash)")
    else:
        (ui.ok if n == 1 else ui.warn)(f"device: {n} in mask-ROM mode")

    st = state.load()
    if st.get("installed"):
        ui.info(f"diskOS installed via this tool: yes (variant={st.get('variant')}, at {st.get('installed_at')})")
    else:
        ui.info("diskOS installed via this tool: no record")
    ui.info(f"saved bone-stock image (for restore): {'yes' if state.have_stock_image() else 'no'}")
    return 0 if all_ok else 1


def _cli_confirm(yes):
    """Build a service `confirm` callback for the CLI (prints the summary, then
    y/N - or auto-yes with --yes)."""
    def confirm(summary):
        ui.step("Ready - please confirm")
        for k in ("action", "variant", "image", "duration", "consequence"):
            if summary.get(k):
                ui.info(f"  {k}: {summary[k]}")
        if yes:
            return True
        return ui.confirm("Proceed?", default=False)
    return confirm


def cmd_install(args):
    if not platform_probe.is_supported():
        ui.err(f"[E101] unsupported host {platform_probe.host_tag()} - Linux/macOS only for now")
        return 2
    if not args.firmware and not args.stock:
        ui.err("[E140] need --firmware <FiiO official update .zip> (or --stock <rootfs.squashfs>).")
        return 2
    params = {"firmware": args.firmware, "stock": args.stock,
              "ui_binary": args.ui, "variant": args.variant}
    r = service.do_install(params, diag.TeeReporter(CLIReporter()), _cli_confirm(args.yes))
    if r.get("ok"):
        ui.info("The UI is embedded in the flashed image - just reboot the device and it installs")
        ui.info("diskOS automatically on first boot (no microSD step needed).")
        ui.info("To deactivate diskOS later (reflash your saved stock rootfs):  diskos-installer restore-stock")
        return 0
    return 3 if r.get("aborted") else 1


def cmd_restore_stock(args):
    r = service.do_restore({"firmware": args.firmware},
                          diag.TeeReporter(CLIReporter()), _cli_confirm(args.yes))
    return 0 if r.get("ok") else (3 if r.get("aborted") else 1)


def cmd_remove(args):
    def confirm(summary):
        ui.warn(summary.get("consequence", ""))
        return args.yes or ui.confirm("Delete the installer's files anyway?", default=False)
    r = service.do_remove({"force": args.force}, diag.TeeReporter(CLIReporter()), confirm)
    if r.get("ok"):
        if getattr(sys, "frozen", False):
            ui.info(f"To finish: delete this executable ({sys.executable}).")
        else:
            ui.info(f"To finish: delete the installer folder ({bundle.resource_root()}).")
        ui.info("Nothing was installed system-wide, so there is nothing else to clean up.")
        return 0
    if r.get("errors"):
        return 1   # partial-removal errors already reported by the service
    ui.info("aborted; nothing removed.")
    return 3


def build_parser():
    p = argparse.ArgumentParser(prog="diskos-installer",
                                description="Standalone diskOS installer for the FiiO Snowsky Disc.")
    p.add_argument("--version", action="version", version=f"diskos-installer {__version__}")
    # Global. Every run is logged either way; this decides what reaches the console.
    p.add_argument("--debug", action="store_true",
                   help="show tracebacks and log lines on the console (also DISKOS_DEBUG=1)")
    # no subcommand -> GUI (running ./diskos-installer with no arguments); subcommands = CLI
    sub = p.add_subparsers(dest="cmd", required=False)

    sub.add_parser("gui", help="launch the graphical installer (also the default with no arguments)")

    d = sub.add_parser("doctor", help="report host, bundled tools, device, and state")
    d.set_defaults(func=cmd_doctor)

    i = sub.add_parser("install", help="build from YOUR stock firmware and flash diskOS")
    i.add_argument("--firmware", help="FiiO official update .zip (your stock firmware)")
    i.add_argument("--stock", help="pre-extracted stock rootfs.squashfs (instead of --firmware)")
    i.add_argument("--ui", help="diskOS UI binary (default: bundled mq_ui)")
    i.add_argument("--variant", choices=["public", "dev"], default="public",
                   help="public (no always-on shell; Debug Mode enables SSH on demand) or dev (adds an ALWAYS-ON PASSWORDLESS ROOT SHELL over "
                        "USB - anyone with physical access gets root every boot; dev devices only)")
    i.add_argument("-y", "--yes", action="store_true", help="don't prompt before flashing")
    i.set_defaults(func=cmd_install)

    r = sub.add_parser("restore-stock", help="deactivate diskOS (reflash your saved stock rootfs)")
    r.add_argument("--firmware", help="FiiO update .zip (only needed if no stock image is saved)")
    r.add_argument("-y", "--yes", action="store_true", help="don't prompt before flashing")
    r.set_defaults(func=cmd_restore_stock)

    # ---- manager: device + restore-point front end (diskos_installer/manager.py) ----
    sub.add_parser("manager", help="interactive menu: device, backup, install, restore")

    stt = sub.add_parser("status", help="device, restore point, and what is installed")
    stt.set_defaults(func=manager.cmd_status)

    det = sub.add_parser("detect", help="is the Disc connected, and in which mode?")
    det.add_argument("--learn", action="store_true",
                     help="learn this Disc's running-mode USB id by watching it plug in")
    det.add_argument("--watch", type=int, nargs="?", const=180, metavar="SECS",
                     help="wait (default 180s) for the device to enter mask-ROM mode")
    det.add_argument("-v", "--verbose", action="store_true", help="list every attached USB device")
    det.set_defaults(func=manager.cmd_detect)

    bk = sub.add_parser("backup", help="save/verify/export a stock restore point (never touches the device)")
    bk.add_argument("--firmware", help="FiiO official update .zip to take the stock rootfs from")
    bk.add_argument("--stock", help="pre-extracted stock rootfs.squashfs instead of a zip")
    bk.add_argument("--verify", action="store_true", help="re-hash the saved restore point")
    bk.add_argument("--export", metavar="DIR", help="copy the restore point to DIR and verify the copy")
    bk.add_argument("--import", dest="import_from", metavar="DIR",
                    help="adopt a previously exported restore point")
    bk.set_defaults(func=manager.cmd_backup)

    rep = sub.add_parser("report", help="write a diagnostic report to attach to a bug report")
    rep.add_argument("-o", "--output", metavar="PATH", help="file or folder to write it to")
    rep.add_argument("--show", action="store_true", help="also print it to the terminal")
    rep.set_defaults(func=diag.cmd_report)

    rm = sub.add_parser("remove", help="delete the installer and everything it created")
    rm.add_argument("-y", "--yes", action="store_true", help="don't prompt")
    rm.add_argument("--force", action="store_true", help="remove even if diskOS is still on the device")
    rm.set_defaults(func=cmd_remove)
    return p


def _invoked_as_manager():
    """Was this started under the name 'diskos-manager'?

    One binary, two front doors. The frozen release is a single file, and adding a
    second PyInstaller target for what is the same program with a different default
    screen would double the build and the download for nothing. Dispatching on the
    program name instead means a copy (or a symlink) named diskos-manager opens the
    manager menu, and the same file named diskos-installer keeps opening the GUI -
    no build change, and no way for the two to drift apart."""
    name = os.path.basename(sys.argv[0] or "")
    if getattr(sys, "frozen", False):
        name = os.path.basename(sys.executable or name)
    return name.startswith("diskos-manager")


def _run(args):
    """Dispatch one parsed invocation. Wrapped by main() for logging."""
    # No subcommand: the manager menu when invoked as diskos-manager, else the GUI.
    if getattr(args, "cmd", None) is None and _invoked_as_manager():
        return manager.menu(args)
    if getattr(args, "cmd", None) in (None, "gui"):
        from diskos_installer import gui
        with RunLock():
            return gui.main()
    if args.cmd == "manager":
        return manager.menu(args)
    # Read-only commands take no lock: they must stay usable while a flash runs in
    # another window (checking on it is the obvious thing to want), and `report` in
    # particular has to work WHILE something is stuck.
    if args.cmd in ("doctor", "status", "detect", "report"):
        return args.func(args)
    with RunLock():
        return args.func(args)


def main(argv=None):
    args = build_parser().parse_args(argv)
    diag.set_debug(getattr(args, "debug", False))
    diag.session_start(sys.argv)
    rc = 1
    try:
        rc = _run(args)
    except errors.DiskOSError as e:   # any coded installer error (Build/Flash/Preflight)
        # Coded errors are expected failures with a plain-language action attached, so
        # the console gets the message - but the traceback still goes to the log, since
        # "which of the four call sites raised E220" is the first thing we will ask.
        tb = diag.log_exception(e, context=f"cmd={getattr(args, 'cmd', None)}")
        ui.err(str(e))
        if diag.debug_enabled():
            print(tb, file=sys.stderr)
        else:
            ui.info(ui.dim(f"details in {diag.log_path()}  (--debug to see them here)"))
        rc = 1
    except KeyboardInterrupt:
        diag.log("interrupted by user", level="warn")
        ui.err("interrupted.")
        rc = 130
    except Exception as e:            # noqa: BLE001 - an unexpected fault is exactly
        # what we most need recorded. Never let it escape as a bare traceback the user
        # has to copy by hand out of a terminal they are about to close.
        tb = diag.log_exception(e, context=f"cmd={getattr(args, 'cmd', None)} UNEXPECTED")
        ui.err(f"unexpected error: {e}")
        if diag.debug_enabled():
            print(tb, file=sys.stderr)
        ui.info(f"This is a bug. The full traceback is in {diag.log_path()}")
        ui.info("Please run 'diskos-installer report' and attach the file to an issue.")
        rc = 1
    finally:
        diag.session_end(rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())

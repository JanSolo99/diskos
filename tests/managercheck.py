#!/usr/bin/env python3
"""Host test for diskos_installer.manager - the parts that do not need a device.

The manager's destructive paths are the installer's own, already-proven service calls,
so what is worth testing here is everything AROUND them: does it tell the truth about
what is attached, does a restore point survive a round trip through export/import, and
does it refuse the copies it should refuse.

Runs against a scratch state dir (DISKOS_INSTALLER_HOME), so it touches nothing real.

    python3 tests/managercheck.py
"""
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

TMP = tempfile.mkdtemp(prefix="diskos-managercheck-")
os.environ["DISKOS_INSTALLER_HOME"] = os.path.join(TMP, "state")

from diskos_installer import manager, platform_probe, state   # noqa: E402

FAIL = []


def check(cond, msg):
    print(f"  {msg:<62} {'ok' if cond else 'FAIL'}")
    if not cond:
        FAIL.append(msg)


def fake_stock(path, size=1 << 16, fill=b"\xa5"):
    """A stand-in flashable image. save_stock_image() insists on squashfs magic, which
    is the only property of the contents this layer looks at."""
    with open(path, "wb") as f:
        f.write(b"hsqs")
        f.write(fill * (size - 4))
    return path


def main():
    print("\n-- device reporting -------------------------------------------------")
    # No USB backend at all must read as "cannot enumerate", never as "not connected":
    # the fix is to install libusb, and saying "no device" would send you hunting a cable.
    platform_probe.usb_list = lambda: None
    platform_probe.maskrom_count = lambda: -1
    line, hint = manager.describe(manager.probe())
    check("cannot enumerate" in line, "no USB backend reports 'cannot enumerate'")
    check(hint and "libusb" in hint, "...and points at libusb as the fix")

    platform_probe.usb_list = lambda: [(0x1d6b, 0x0002, "Linux Foundation hub")]
    platform_probe.maskrom_count = lambda: 0
    line, hint = manager.describe(manager.probe())
    check("not in mask-ROM" in line, "device absent reads as not in mask-ROM mode")
    check(hint and "--learn" in hint, "...and offers to learn the running-mode id")

    platform_probe.usb_list = lambda: [(0xa108, 0xeaef, "Ingenic")]
    line, _ = manager.describe(manager.probe())
    check("mask-ROM mode" in line and "ready to flash" in line, "mask-ROM device is recognised")

    platform_probe.usb_list = lambda: [(0xa108, 0xeaef, "a"), (0xa108, 0xeaef, "b")]
    line, hint = manager.describe(manager.probe())
    check("2 devices" in line, "two mask-ROM devices are reported as ambiguous")
    check(hint and "unplug" in hint, "...and refuse to guess between them")

    print("\n-- learning the running-mode id -------------------------------------")
    before = [(0x1d6b, 0x0002, "hub"), (0x046d, 0xc52b, "mouse")]
    after = before + [(0x2972, 0x0047, "FiiO Disc")]
    check(manager.new_devices(before, after) == [(0x2972, 0x0047, "FiiO Disc")],
          "a newly plugged device is detected")
    check(manager.new_devices(before, before) == [], "no change detects nothing")
    # mask-ROM is the FLASHING mode; learning it as the running mode would make the
    # tool report "connected and running" when it is actually sitting ready to flash.
    check(manager.new_devices(before, before + [(0xa108, 0xeaef, "Ingenic")]) == [],
          "mask-ROM is never learned as the running mode")
    # Identical peripherals are legitimate; a set difference would miss the second one.
    dup_before = [(0x046d, 0xc52b, "mouse")]
    dup_after = [(0x046d, 0xc52b, "mouse"), (0x046d, 0xc52b, "mouse")]
    check(len(manager.new_devices(dup_before, dup_after)) == 1,
          "a second identical device still counts as new")
    check(manager.new_devices([(1, 2, "x")], []) == [], "unplugging detects nothing new")

    print("\n-- restore point ----------------------------------------------------")
    check(manager.restore_point()["present"] is False, "reports absent when nothing is saved")
    check(manager.verify_restore_point() is False, "verify fails with nothing to verify")
    check(manager.export_restore_point(TMP) is False, "export refuses with nothing to export")

    src = fake_stock(os.path.join(TMP, "stock_src.bin"))
    digest = state.save_stock_image(src)
    st = state.load()
    st.update({"stock_version": "V2.28", "stock_sha256": digest,
               "stock_source": "fw.zip", "stock_saved_at": "2026-01-01T00:00:00Z"})
    state.save(st)

    rp = manager.restore_point()
    check(rp["present"] and rp["version"] == "V2.28", "reports the saved restore point")
    check(rp["sha256"] == digest, "...with the recorded digest")
    check(manager.verify_restore_point() is True, "verify passes on an intact image")

    print("\n-- export / import round trip ---------------------------------------")
    dest = os.path.join(TMP, "export")
    check(manager.export_restore_point(dest) is True, "export succeeds")
    made = [d for d in os.listdir(dest) if d.startswith("diskos-restore-point-")]
    check(len(made) == 1, "export creates one dated folder")
    exported = os.path.join(dest, made[0])
    check(os.path.exists(os.path.join(exported, "stock.bin")), "export contains the image")
    check(os.path.exists(os.path.join(exported, manager.MANIFEST)), "export contains a manifest")

    # An export whose bytes were damaged in transit must be refused BEFORE it can
    # overwrite a restore point that is currently good.
    with open(os.path.join(exported, "stock.bin"), "r+b") as f:
        f.seek(4096)
        f.write(b"\x00" * 64)
    check(manager.import_restore_point(exported) is False,
          "import refuses a copy that fails its manifest")
    check(manager.restore_point()["sha256"] == digest,
          "...and the existing restore point is untouched")

    check(manager.import_restore_point(os.path.join(TMP, "nope")) is False,
          "import refuses a folder with no image")

    print("\n-- corruption is noticed --------------------------------------------")
    bin_path, _ = state.stock_paths()
    with open(bin_path, "r+b") as f:
        f.seek(1024)
        f.write(b"\xff" * 32)
    check(manager.verify_restore_point() is False, "verify catches a corrupted saved image")
    check(manager.restore_point()["present"] is True,
          "...while presence stays cheap and does not re-hash")

    print()
    if FAIL:
        print(f"{len(FAIL)} CHECK(S) FAILED")
        for m in FAIL:
            print(f"  - {m}")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    finally:
        shutil.rmtree(TMP, ignore_errors=True)

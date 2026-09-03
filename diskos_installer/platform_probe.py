"""Host OS/arch detection and Snowsky-Disc device presence checks.

Device USB identities we care about:
  - Ingenic X2000 mask-ROM (flashing mode):  VID:PID 0a108:eaef  (a108:eaef)
  - normal running device (not used for flashing) is a different id.

We enumerate USB via pyusb (bundled) so we don't depend on `lsusb` being present.
"""

import platform
import sys

MASKROM_VID = 0x0a108 & 0xFFFF  # printed as a108 by lsusb; VID field is 0xa108
MASKROM_PID = 0xeaef
# lsusb shows "a108:eaef"; libusb reports idVendor=0xa108 idProduct=0xeaef
MASKROM_VID = 0xa108


def host():
    """Return (os_key, arch_key) e.g. ('linux','x86_64') / ('macos','arm64')."""
    sysname = platform.system().lower()
    if sysname == "darwin":
        os_key = "macos"
    elif sysname == "linux":
        os_key = "linux"
    else:
        os_key = sysname  # 'windows' etc. - unsupported for now
    machine = platform.machine().lower()
    arch = {
        "x86_64": "x86_64", "amd64": "x86_64",
        "arm64": "arm64", "aarch64": "arm64",
    }.get(machine, machine)
    return os_key, arch


def host_tag():
    o, a = host()
    return f"{o}-{a}"


def is_supported():
    o, _ = host()
    return o in ("linux", "macos")


def _bundled_libusb_backend():
    """A pyusb libusb1 backend pointed at OUR bundled libusb, so USB enumeration
    works in the frozen app even when the system has no libusb. Returns a backend
    or None (caller falls back to pyusb's default search)."""
    try:
        import glob
        import os
        import usb.backend.libusb1 as libusb1
        from . import bundle
        libdir = os.path.join(bundle.vendor_dir(), "lib")
        # macOS dylib or Linux .so, whatever we bundled
        for pat in ("libusb-1.0*.dylib", "libusb-1.0.so*", "libusb-1.0*"):
            hits = sorted(glob.glob(os.path.join(libdir, pat)))
            if hits:
                return libusb1.get_backend(find_library=lambda _n, _p=hits[0]: _p)
    except Exception:
        pass
    return None


def _maskrom_count_pyusb():
    """Count devices in Ingenic mask-ROM mode via pyusb. Returns int or None if
    pyusb/backend is unavailable."""
    try:
        import usb.core  # pyusb (bundled)
    except Exception:
        return None
    try:
        backend = _bundled_libusb_backend()   # prefer our bundled libusb
        devs = list(usb.core.find(find_all=True, idVendor=MASKROM_VID,
                                  idProduct=MASKROM_PID, backend=backend))
        return len(devs)
    except Exception:
        return None


def _maskrom_count_lsusb():
    """Fallback: parse `lsusb` if present (Linux)."""
    import shutil
    import subprocess
    if not shutil.which("lsusb"):
        return None
    try:
        out = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    return sum(1 for ln in out.splitlines() if "a108:eaef" in ln.lower())


def _usb_list_pyusb():
    """[(vid, pid, label)] for every attached USB device, or None if pyusb is unusable."""
    try:
        import usb.core
    except Exception:
        return None
    try:
        backend = _bundled_libusb_backend()
        out = []
        for d in usb.core.find(find_all=True, backend=backend):
            label = ""
            # Reading string descriptors needs the device to be open-able; on a machine
            # without the udev rule that fails for most devices. It is only cosmetic, so
            # never let it break enumeration.
            try:
                parts = [p for p in (d.manufacturer, d.product) if p]
                label = " ".join(parts).strip()
            except Exception:
                pass
            out.append((int(d.idVendor), int(d.idProduct), label))
        return out
    except Exception:
        return None


def _usb_list_lsusb():
    """Fallback for Linux hosts with no usable pyusb backend."""
    import re
    import shutil
    import subprocess
    if not shutil.which("lsusb"):
        return None
    try:
        out = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    devs = []
    for ln in out.splitlines():
        m = re.search(r"ID\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s*(.*)", ln)
        if m:
            devs.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
    return devs


def usb_list():
    """Every attached USB device as [(vid, pid, label)], or None if we cannot enumerate.

    Used to detect the Disc in its NORMAL (running) mode. Only the mask-ROM identity
    a108:eaef is known for certain; the running device presents a different one that
    this project has never recorded, so the manager learns it from the user's own
    device by diffing this list across a plug-in. None (cannot enumerate) is
    deliberately distinct from [] (nothing attached)."""
    devs = _usb_list_pyusb()
    if devs is not None:
        return devs
    return _usb_list_lsusb()


def maskrom_count():
    """How many devices are currently in mask-ROM mode. Prefers pyusb, falls back
    to lsusb, returns -1 if neither is available (caller should warn)."""
    n = _maskrom_count_pyusb()
    if n is not None:
        return n
    n = _maskrom_count_lsusb()
    if n is not None:
        return n
    return -1

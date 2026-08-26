"""Locate the bundled native binaries and data files.

Layout (in the source tree AND inside the PyInstaller bundle):
    <root>/vendor/<os>-<arch>/   usbboot, mksquashfs, unsquashfs,
                                 my_write5_dram.bin, disc_spl_lpddr3.bin,
                                 lib/ (bundled .so/.dylib for usbboot)
    <root>/payload/              mq_ui, S97diskos_install, S99usbserial,
                                 diskos_manifest templates, etc.

When frozen by PyInstaller, data is unpacked under sys._MEIPASS; in the source
tree it sits next to this package. `resource_root()` resolves both."""

import os
import stat
import sys

from . import platform_probe


def resource_root():
    """Directory that contains vendor/ and payload/.

    - Frozen (PyInstaller): sys._MEIPASS.
    - Source tree: the installer/ dir (parent of this package)."""
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        return sys._MEIPASS
    # this file is installer/diskos_installer/bundle.py -> installer/
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def vendor_dir():
    return os.path.join(resource_root(), "vendor", platform_probe.host_tag())


def payload_dir():
    return os.path.join(resource_root(), "payload")


def _ensure_exec(path):
    try:
        st = os.stat(path)
        os.chmod(path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass
    return path


def native(name, required=True):
    """Absolute path to a bundled native binary for this host, made executable.
    Raises FileNotFoundError if required and missing."""
    p = os.path.join(vendor_dir(), name)
    if os.path.exists(p):
        return _ensure_exec(p)
    if required:
        from .errors import PreflightError
        raise PreflightError(
            f"bundled tool '{name}' not found for {platform_probe.host_tag()}",
            code="E102",
            action="this build may not include binaries for your platform - "
                   "re-download the correct build")
    return None


def data(name, required=True):
    """Absolute path to a bundled data/payload file."""
    p = os.path.join(payload_dir(), name)
    if os.path.exists(p):
        return p
    if required:
        from .errors import PreflightError
        raise PreflightError(f"bundled payload '{name}' not found", code="E102",
                             action="the install is incomplete - re-download the installer")
    return None


def native_env():
    """Environment for running a bundled native binary, with its private lib dir
    on the loader path (so a bundled libusb is found without touching the system)."""
    env = dict(os.environ)
    libdir = os.path.join(vendor_dir(), "lib")
    if os.path.isdir(libdir):
        o, _ = platform_probe.host()
        var = "DYLD_LIBRARY_PATH" if o == "macos" else "LD_LIBRARY_PATH"
        env[var] = libdir + (os.pathsep + env[var] if env.get(var) else "")
    return env

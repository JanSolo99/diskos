"""Diagnostics: a run log, a traceback mode, and a one-file support report.

The tool talks to hardware the user can see and we cannot, over a 60-90 minute
operation, on a machine we have no access to. When something goes wrong the only
evidence that reaches us is whatever the person thought to copy out of a terminal -
and by then the terminal has usually been closed.

So three things:

  RUN LOG      Every invocation appends a transcript to <state>/logs/diskos.log:
               the command line, the host, every reporter event the engine emitted,
               and the full traceback of anything that failed. Always on - a problem
               you can only capture by asking the user to reproduce it with a flag is
               a problem you find out about twice. Rotated so it cannot grow forever.

  DEBUG MODE   --debug (or DISKOS_DEBUG=1) shows tracebacks and log lines on the
               console too. It changes what is DISPLAYED, never what is recorded.

  REPORT       `report` collects everything at once - host, Python, bundled tools,
               USB, udev, permissions, free space, state, restore point, and the tail
               of the logs - into a single file to attach to an issue. Paths under the
               user's home are redacted, and the file is plain text so it can be read
               before sending.

Nothing here may ever break the tool it is instrumenting: every write is best-effort
and swallowed on failure. A diagnostics layer that can abort an install is worse than
no diagnostics layer.
"""

import getpass
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import traceback

from . import bundle, platform_probe, state, ui
from .reporter import Reporter

MAX_LOG_BYTES = 1 << 20          # rotate at 1 MiB, keep one previous
_debug = bool(os.environ.get("DISKOS_DEBUG"))
_session = f"{int(time.time()) % 100000:05d}.{os.getpid()}"


def set_debug(on):
    global _debug
    _debug = bool(on) or bool(os.environ.get("DISKOS_DEBUG"))


def debug_enabled():
    return _debug


# ------------------------------------------------------------------ the run log

def log_dir():
    d = os.path.join(state.state_dir(), "logs")
    os.makedirs(d, exist_ok=True)
    return d


def log_path():
    return os.path.join(log_dir(), "diskos.log")


def _rotate(path):
    try:
        if os.path.exists(path) and os.path.getsize(path) > MAX_LOG_BYTES:
            os.replace(path, path + ".1")
    except OSError:
        pass


def log(msg, level="info"):
    """Append one line to the run log. Never raises.

    Best-effort by design: a read-only home or a full disk must not turn a working
    install into a failed one just because we could not write a log line."""
    line = f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {level:<5} [{_session}] {msg}"
    if _debug:
        ui.info(ui.dim(f"debug: {msg}"))
    try:
        p = log_path()
        _rotate(p)
        with open(p, "a", encoding="utf-8", errors="replace") as f:
            f.write(line + "\n")
    except OSError:
        pass


def log_exception(exc, context=""):
    """Record a full traceback. Returns the formatted traceback for optional display."""
    tb = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    log(f"EXCEPTION {context}: {exc!r}", level="error")
    for ln in tb.rstrip().splitlines():
        log("  " + ln, level="error")
    return tb


def session_start(argv):
    log("=" * 68)
    log(f"start: {' '.join(argv)}")
    try:
        from . import __version__
        log(f"version={__version__} host={platform_probe.host_tag()} "
            f"python={platform.python_version()} frozen={bool(getattr(sys, 'frozen', False))}")
    except Exception:      # noqa: BLE001 - never let the header break the run
        pass


def session_end(rc):
    log(f"exit: rc={rc}")


class TeeReporter(Reporter):
    """Wraps a reporter and mirrors every event into the run log.

    The engine already reports facts through this interface, which makes it the one
    place where a complete transcript can be captured without touching imagebuild,
    flasher or state at all. Before this, only the flash step wrote a log, so a
    failed extract or build left nothing behind."""

    def __init__(self, inner):
        self.inner = inner

    def phase(self, name, destructive=False):
        log(f"phase: {name}" + (" (destructive)" if destructive else ""))
        self.inner.phase(name, destructive)

    def status(self, message):
        log(f"status: {message}")
        self.inner.status(message)

    def log(self, line):
        log(f"log: {line}")
        self.inner.log(line)

    def progress(self, completed, total):
        self.inner.progress(completed, total)      # far too chatty for the log

    def indeterminate(self, active, note="", expect_secs=None):
        log(f"indeterminate: {'start' if active else 'stop'} {note}")
        self.inner.indeterminate(active, note, expect_secs)

    def warning(self, message):
        log(f"WARN: {message}", level="warn")
        self.inner.warning(message)

    def ok(self, message):
        log(f"ok: {message}")
        self.inner.ok(message)

    def error(self, message):
        log(f"ERROR: {message}", level="error")
        self.inner.error(message)


# ------------------------------------------------------------------ redaction

def redact(text):
    """Strip the thing that identifies a person: their home path.

    The report is meant to be pasted into a public issue tracker, so it should not
    need reading line by line first to be safe to share.

    The username is redacted ONLY where it appears as a path component. Replacing it
    everywhere looks safer and is actively harmful: usernames like root, max, sam,
    admin, test and user are ordinary English words that occur throughout this
    report's own prose. On a machine where the user is 'root' it turned the single
    most important line - "running as root: True" - into "running as <user>: True",
    redacting a diagnostic instead of a secret. A path component is the only place
    the name is genuinely identifying, and the only place we can rewrite it without
    changing the meaning of a sentence."""
    if not text:
        return text
    home = os.path.expanduser("~")
    if home and home not in ("", "/"):
        # Anchored at a path boundary. A plain replace() matches mid-path: for the
        # root account, home is "/root", and "/home/root/x" would come out as
        # "/home~/x" - a corrupted path that is now harder to read than the one we
        # were protecting.
        text = re.sub(r"(?<![\w/])" + re.escape(home) + r"(?![\w])", "~", text)
    try:
        user = getpass.getuser()
    except Exception:      # noqa: BLE001 - getuser raises with no passwd entry
        return text
    if user and len(user) > 1:
        # after a slash only: /home/bob, /media/bob/x, /run/user/bob - never prose
        text = re.sub(r"(?<=/)" + re.escape(user) + r"(?![\w])", "<user>", text)
    return text


# ------------------------------------------------------------------ the report

def _run(cmd, timeout=8):
    """Run a probe command for its output. Returns "" if it is not available."""
    if not shutil.which(cmd[0]):
        return ""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return (r.stdout or "") + (r.stderr or "")
    except Exception:      # noqa: BLE001 - a probe must never break the report
        return ""


def _section(title):
    return f"\n===== {title} " + "=" * max(0, 60 - len(title)) + "\n"


def _tools():
    out = []
    for name in ("usbboot", "mksquashfs", "unsquashfs", "my_write5_dram.bin", "disc_spl_lpddr3.bin"):
        p = bundle.native(name, required=False)
        out.append(f"  {name:<22} {p or 'MISSING'}")
    for name in ("mq_ui", "S97diskos_install", "S99usbserial", "diskos-debug.sh", "dropbearmulti"):
        p = bundle.data(name, required=False)
        sz = ""
        if p and os.path.exists(p):
            sz = f"  ({os.path.getsize(p)} bytes)"
        out.append(f"  {name:<22} {p or 'MISSING'}{sz}")
    return "\n".join(out)


def _usb():
    devs = platform_probe.usb_list()
    if devs is None:
        return ("  cannot enumerate USB (no pyusb backend and no lsusb)\n"
                "  -> this is usually a missing libusb-1.0, not a missing device")
    if not devs:
        return "  (no USB devices reported)"
    from . import manager
    learned = manager.normal_mode_id()
    lines = []
    for v, p, label in sorted(devs):
        mark = ""
        if (v, p) == (platform_probe.MASKROM_VID, platform_probe.MASKROM_PID):
            mark = "   <== mask-ROM (flash mode)"
        elif learned == (v, p):
            mark = "   <== learned as this Disc"
        lines.append(f"  {v:04x}:{p:04x}  {label}{mark}")
    return "\n".join(lines)


def _permissions():
    """The things that make the device invisible even when it is plugged in."""
    out = []
    out.append(f"  running as root      : {os.geteuid() == 0 if hasattr(os, 'geteuid') else '?'}")
    if sys.platform.startswith("linux"):
        rule = "/etc/udev/rules.d/70-diskos-maskrom.rules"
        out.append(f"  udev rule installed  : {os.path.exists(rule)}  ({rule})")
        out.append("  -> without it, the device is only reachable as root; see README 'USB permissions'")
    try:
        import ctypes.util
        out.append(f"  libusb-1.0           : {ctypes.util.find_library('usb-1.0') or 'NOT FOUND'}")
    except Exception:      # noqa: BLE001
        out.append("  libusb-1.0           : (probe failed)")
    try:
        import usb.core                                    # noqa: F401
        out.append("  pyusb                : importable")
    except Exception as e:                                  # noqa: BLE001
        out.append(f"  pyusb                : NOT importable ({e})")
    # A noexec temp mount is what E103 reports far too late - after the user has
    # already gone through the whole setup.
    tmp = os.environ.get("TMPDIR", "/tmp")
    out.append(f"  TMPDIR               : {tmp}")
    mounts = _run(["mount"])
    for ln in mounts.splitlines():
        if f" on {tmp} " in ln or ln.startswith(f"{tmp} "):
            out.append(f"    {ln.strip()}")
            if "noexec" in ln:
                out.append("    -> NOEXEC: bundled tools cannot run from here (installer error E103)")
    return "\n".join(out)


def _space():
    out = []
    for label, path in (("state dir", state.state_dir()), ("tmp", os.environ.get("TMPDIR", "/tmp"))):
        try:
            u = shutil.disk_usage(path)
            out.append(f"  {label:<10} {u.free // (1 << 20)} MiB free of {u.total // (1 << 20)} MiB   {path}")
        except OSError as e:
            out.append(f"  {label:<10} unavailable ({e})")
    # A build needs room for the extracted rootfs + the built image, both ~76 MB.
    out.append("  -> a build needs roughly 300 MiB free")
    return "\n".join(out)


def _state_dump():
    import json
    out = []
    try:
        out.append("  state.json:")
        for ln in json.dumps(state.load(), indent=2, sort_keys=True).splitlines():
            out.append("    " + ln)
    except Exception as e:                                  # noqa: BLE001
        out.append(f"  state.json unreadable: {e}")
    try:
        from . import manager
        rp = manager.restore_point()
        out.append(f"  restore point present: {rp['present']}")
        out.append(f"    version {rp['version']}  size {rp['size']}  saved {rp['saved_at']}")
        out.append(f"    sha256  {rp['sha256']}")
        out.append(f"    path    {rp['path']}")
        dev = manager.load_devices()
        out.append(f"  learned devices: {dev or '(none)'}")
    except Exception as e:                                  # noqa: BLE001
        out.append(f"  restore point unreadable: {e}")
    return "\n".join(out)


def _tail(path, lines=200):
    if not os.path.exists(path):
        return f"  (no {os.path.basename(path)})"
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = f.readlines()
        return "".join("  " + ln for ln in data[-lines:]).rstrip() or "  (empty)"
    except OSError as e:
        return f"  (unreadable: {e})"


def collect():
    """Build the whole diagnostic report as one redacted string."""
    from . import __version__
    o, a = platform_probe.host()
    parts = []
    parts.append("diskOS diagnostic report")
    parts.append(f"generated {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}")
    parts.append("Paths under your home directory and your username are replaced with ~ and <user>.")
    parts.append("Please read it before posting it anywhere public.")

    parts.append(_section("host"))
    parts.append(f"  tool version   : {__version__}")
    parts.append(f"  host           : {o}-{a} ({'supported' if platform_probe.is_supported() else 'UNSUPPORTED'})")
    parts.append(f"  platform       : {platform.platform()}")
    parts.append(f"  python         : {platform.python_version()} ({sys.executable})")
    parts.append(f"  frozen build   : {bool(getattr(sys, 'frozen', False))}")
    parts.append(f"  state dir      : {state.state_dir()}")

    parts.append(_section("bundled tools"))
    parts.append(_tools())

    parts.append(_section("usb devices"))
    parts.append(_usb())

    parts.append(_section("permissions and environment"))
    parts.append(_permissions())

    parts.append(_section("disk space"))
    parts.append(_space())

    parts.append(_section("saved state"))
    parts.append(_state_dump())

    parts.append(_section("run log (tail)"))
    parts.append(_tail(log_path()))

    sd = state.state_dir()
    for name in ("last-flash.log", "last-restore.log"):
        parts.append(_section(name))
        parts.append(_tail(os.path.join(sd, name), lines=120))

    return redact("\n".join(parts) + "\n")


def write_report(dest=None):
    """Write the report and return its path (None on failure)."""
    text = collect()
    if dest:
        path = os.path.expanduser(dest)
        if os.path.isdir(path):
            path = os.path.join(path, f"diskos-report-{time.strftime('%Y%m%d-%H%M%S')}.txt")
    else:
        path = os.path.join(log_dir(), f"diskos-report-{time.strftime('%Y%m%d-%H%M%S')}.txt")
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path
    except OSError as e:
        ui.err(f"could not write the report: {e}")
        return None


def cmd_report(args):
    ui.step("Collecting a diagnostic report")
    ui.info("Everything the tool can see about this machine, the device and what it has done.")
    path = write_report(getattr(args, "output", None))
    if not path:
        return 1
    ui.ok(f"written: {path}")
    ui.info("Attach it to a GitHub issue. Home paths and your username are already")
    ui.info("redacted, but read it first - it is plain text.")
    if getattr(args, "show", False):
        print()
        print(collect())
    return 0

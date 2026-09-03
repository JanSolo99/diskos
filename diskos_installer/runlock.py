"""The single-run lock.

Extracted from __main__ so every entry point - the CLI, the GUI and the manager -
takes the SAME lock rather than each growing its own. There is one physical device;
two tools writing to it at once is the failure mode this exists to prevent, so the
implementation must not be duplicated.
"""

import fcntl
import os

from diskos_installer import ui


# --- single-run lock so two installers can't touch the ONE physical device at once. It must be
# SHARED across all users on the host (a GUI as your user and a CLI under sudo must not both flash),
# so it lives at a FIXED /tmp path - NOT per-user. Advisory flock (auto-released on death -> no
# stale-lock hazard). Opened READ-ONLY so any user can take the flock on a 0644 file, and O_NOFOLLOW
# so a planted symlink can't redirect the open (we bail instead of following it). ---
class RunLock:
    def __init__(self):
        # A FIXED absolute path, not tempfile.gettempdir(): $TMPDIR differs between a normal run and a
        # sudo run (and per-user on macOS), which would hand them SEPARATE locks and defeat the whole
        # point. /tmp is world-accessible on every supported host (Linux/macOS); Windows is unsupported.
        self.path = "/tmp/diskos-installer.lock"
        self.fd = None

    def __enter__(self):
        import stat as _stat
        flags = os.O_CREAT | os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
        try:
            self.fd = os.open(self.path, flags, 0o644)
        except OSError as e:
            raise SystemExit(ui.red(
                f"could not open the run-lock at {self.path} ({e}). If it exists as a symlink, "
                "remove it and retry."))
        # A planted symlink is refused by O_NOFOLLOW above; also refuse a non-regular file.
        if not _stat.S_ISREG(os.fstat(self.fd).st_mode):
            os.close(self.fd); self.fd = None
            raise SystemExit(ui.red(f"run-lock {self.path} is not a regular file - refusing."))
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(self.fd)
            self.fd = None
            raise SystemExit(ui.red(
                "another diskOS installer is already running. Close it and retry "
                "(only one may touch the device at a time)."))
        return self

    def __exit__(self, *exc):
        try:
            if self.fd is not None:
                fcntl.flock(self.fd, fcntl.LOCK_UN)
                os.close(self.fd)
        except OSError:
            pass
        return False

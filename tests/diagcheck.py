#!/usr/bin/env python3
"""Host test for diskos_installer.diag - the run log, redaction and the report.

Diagnostics are the code you only find out is broken at the exact moment you need
it, so it gets tested like anything else. Two properties matter most:

  * Redaction must remove home paths WITHOUT damaging the report. Blind username
    replacement looks safer and is not: usernames like root, max, sam and admin are
    ordinary words, and on a root account it rewrote "running as root: True" - the
    single most useful line in the file - into "running as <user>: True".
  * Logging must never raise. A diagnostics layer that can abort an install is worse
    than no diagnostics layer.

    python3 tests/diagcheck.py
"""
import getpass
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

TMP = tempfile.mkdtemp(prefix="diskos-diagcheck-")
os.environ["DISKOS_INSTALLER_HOME"] = os.path.join(TMP, "state")

from diskos_installer import diag, platform_probe          # noqa: E402
from diskos_installer.reporter import Reporter             # noqa: E402

FAIL = []


def check(cond, msg):
    print(f"  {msg:<64} {'ok' if cond else 'FAIL'}")
    if not cond:
        FAIL.append(msg)


class Spy(Reporter):
    def __init__(self):
        self.events = []

    def phase(self, name, destructive=False):
        self.events.append(("phase", name))

    def status(self, message):
        self.events.append(("status", message))

    def log(self, line):
        self.events.append(("log", line))

    def progress(self, completed, total):
        self.events.append(("progress", completed))

    def indeterminate(self, active, note="", expect_secs=None):
        self.events.append(("indeterminate", active))

    def warning(self, message):
        self.events.append(("warning", message))

    def ok(self, message):
        self.events.append(("ok", message))

    def error(self, message):
        self.events.append(("error", message))


def main():
    user = getpass.getuser()
    home = os.path.expanduser("~")

    print("\n-- redaction ---------------------------------------------------------")
    check(diag.redact(f"running as {user}: True") == "running as root: True"
          if user == "root" else
          diag.redact(f"running as {user}: True") == f"running as {user}: True",
          "prose containing the username is left alone")
    check(diag.redact(f"only reachable as {user}; see README")
          == f"only reachable as {user}; see README",
          "the username mid-sentence is left alone")
    # This used to assert on /home/<user>/thing.bin, which fails for every account
    # whose home IS /home/<username> - the home-collapse rule runs first and yields
    # "~/thing.bin". That output is equally redacted, so the implementation was
    # right and the expectation was wrong; it only ever passed for root, whose home
    # is /root. Test the rule with a path that is never the home directory (the
    # implementation calls out /media/<user>/x), then assert the property that
    # actually matters for both.
    check(diag.redact(f"/media/{user}/card") == "/media/<user>/card",
          "the username as a path component is redacted")
    check(user not in diag.redact(f"/home/{user}/thing.bin"),
          "a path under home leaks no username, whichever rule fires")
    check(diag.redact(f"{home}/state/x") == "~/state/x",
          "the home directory collapses to ~")
    check(diag.redact(f"{home}x/not-home") == f"{home}x/not-home",
          "a directory merely starting with the home path is untouched")
    check(diag.redact(f"/media/{user}/usb and /run/user/{user}")
          == "/media/<user>/usb and /run/user/<user>",
          "every path component is redacted")
    check(diag.redact("") == "" and diag.redact(None) is None,
          "empty input is handled")

    print("\n-- the run log -------------------------------------------------------")
    diag.log("hello from the test")
    p = diag.log_path()
    check(os.path.exists(p), "the log file is created on first write")
    body = open(p, encoding="utf-8").read()
    check("hello from the test" in body, "the message is recorded")
    check("info" in body.splitlines()[-1], "lines carry a level")

    try:
        raise ValueError("a deliberate fault")
    except ValueError as e:
        tb = diag.log_exception(e, context="unit test")
    body = open(p, encoding="utf-8").read()
    check("a deliberate fault" in body, "an exception is recorded")
    check("Traceback" in tb and "ValueError" in tb, "the traceback is returned for display")
    check("diagcheck.py" in body, "the traceback frames reach the log, not just the message")

    print("\n-- logging never breaks the caller -----------------------------------")
    # The single most important property: if the log cannot be written - read-only
    # home, full disk, permissions - the tool must carry on regardless.
    saved = diag.log_path
    diag.log_path = lambda: "/proc/definitely/not/writable/x.log"
    try:
        diag.log("this must not raise")
        ok = True
    except Exception:                                        # noqa: BLE001
        ok = False
    diag.log_path = saved
    check(ok, "a failing log write is swallowed")

    print("\n-- TeeReporter -------------------------------------------------------")
    spy = Spy()
    tee = diag.TeeReporter(spy)
    tee.phase("Building", destructive=True)
    tee.status("doing a thing")
    tee.warning("careful")
    tee.error("it broke")
    tee.ok("done")
    tee.progress(5, 10)
    check([e[0] for e in spy.events] == ["phase", "status", "warning", "error", "ok", "progress"],
          "every event still reaches the wrapped reporter")
    body = open(diag.log_path(), encoding="utf-8").read()
    check("phase: Building (destructive)" in body, "phases are logged, with the destructive flag")
    check("WARN: careful" in body and "ERROR: it broke" in body, "warnings and errors are logged")
    check("progress" not in body, "progress is NOT logged (it would drown everything else)")

    print("\n-- the report --------------------------------------------------------")
    platform_probe.usb_list = lambda: [(0xa108, 0xeaef, "Ingenic")]
    text = diag.collect()
    for section in ("host", "bundled tools", "usb devices", "permissions and environment",
                    "disk space", "saved state", "run log"):
        check(f"===== {section} " in text, f"the report has a '{section}' section")
    check("mask-ROM (flash mode)" in text, "a mask-ROM device is called out in the report")
    check(home not in text, "the report contains no raw home path")

    out = diag.write_report(TMP)
    check(out and os.path.exists(out), "the report writes to a directory")
    check(out and out.endswith(".txt"), "...with a .txt name")
    named = os.path.join(TMP, "explicit.txt")
    check(diag.write_report(named) == named and os.path.exists(named),
          "the report writes to an explicit filename")

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

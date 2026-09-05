#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Functional tests for payload/S97diskos_install - the fail-closed boot installer.

S97 is the one piece of diskOS that runs as root before anything else and decides
whether the device boots OUR UI or the stock one. Getting it wrong is expensive in
both directions: too strict and a good build is quarantined into stock-UI-forever;
too loose and an unverified binary runs as root. It is also the hardest thing here
to test by hand, because every wrong answer costs a 60-90 minute reflash to undo.

So the script is run for real, under `sh`, against a FAKE ROOT: every absolute path
it touches (/usr/data, /etc/diskos_manifest, /opt/diskos, the SD mountpoint and the
mmcblk device nodes) is rewritten to a temp tree first. Nothing outside that tree is
read or written, and no SD device node exists inside it, so the SD fallback finds
nothing and logs that - which is exactly the "no card inserted" case anyway.

The binaries are fakes: verify_shape only reads e_ident[0..5] and e_machine, and
verify_ui only hashes, so a 64-byte buffer with the right header bytes is
indistinguishable from a real one as far as this script is concerned.

Run from the repo root:  python3 tests/s97check.py
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S97 = os.path.join(ROOT, "payload", "S97diskos_install")

FAILURES = []


# ---------------------------------------------------------------- fake binaries

def elf(machine="mips", tag=b"\x00"):
    """A 64-byte buffer with the exact header bytes verify_shape inspects."""
    b = bytearray(64)
    b[0:4] = b"\x7fELF"
    if machine == "mips":
        b[4], b[5] = 0x01, 0x01           # ELFCLASS32, ELFDATA2LSB
        b[18], b[19] = 0x08, 0x00         # EM_MIPS, little-endian
    elif machine == "x86_64":
        b[4], b[5] = 0x02, 0x01           # ELFCLASS64
        b[18], b[19] = 0x3E, 0x00         # EM_X86_64
    elif machine == "i386":
        b[4], b[5] = 0x01, 0x01           # ELFCLASS32, ELFDATA2LSB - passes the class
        b[18], b[19] = 0x03, 0x00         # EM_386 - so the MACHINE gate is what refuses it
    else:
        raise ValueError(machine)
    b[32:32 + len(tag)] = tag             # vary the payload -> vary the sha
    return bytes(b)


def sha(data):
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------- the fake root

class Fixture:
    """A temp tree standing in for the device's filesystem, plus a rewritten S97."""

    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="s97_")
        for d in ("usr/data", "etc", "opt/diskos", "tmp", "dev"):
            os.makedirs(os.path.join(self.root, d), exist_ok=True)
        self.script = os.path.join(self.root, "S97")
        src = open(S97, "rb").read().decode("utf-8").replace("\r\n", "\n")
        # Order matters only in that every rewritten prefix must be absolute and
        # distinct; none of these is a prefix of another.
        for p in ("/usr/data", "/etc/diskos_manifest", "/opt/diskos",
                  "/tmp/diskos_sd", "/dev/mmcblk"):
            src = src.replace(p, self.root + p)
        open(self.script, "w", newline="\n").write(src)
        os.chmod(self.script, 0o755)

    # -- paths
    def p(self, rel):
        return os.path.join(self.root, rel.lstrip("/"))

    # -- setup helpers
    def manifest(self, data):
        open(self.p("etc/diskos_manifest"), "w", newline="\n").write(
            "SHA256=%s\nSIZE=%d\n" % (sha(data), len(data)))

    def write(self, rel, data, mode=0o644):
        path = self.p(rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        os.chmod(path, mode)

    def touch(self, rel):
        self.write(rel, b"")

    def link_player(self):
        lp = self.p("usr/data/mq_player")
        if os.path.lexists(lp):
            os.remove(lp)
        os.symlink(self.p("usr/data/mq_ui"), lp)

    # -- inspection
    def exists(self, rel):
        return os.path.lexists(self.p(rel))

    def read(self, rel):
        with open(self.p(rel), "rb") as f:
            return f.read()

    def log(self):
        try:
            return open(self.p("usr/data/diskos_install.log"), errors="replace").read()
        except OSError:
            return ""

    def booted_diskos(self):
        """The fail-closed contract: the boot hook runs our UI iff BOTH exist."""
        return self.exists("usr/data/mq_ui") and self.exists("usr/data/mq_player")

    def run(self):
        return subprocess.run(["sh", self.script], capture_output=True, text=True,
                              timeout=120)

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


# ---------------------------------------------------------------- assertions

def check(name, cond, detail=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s %s" % (name, detail))
        FAILURES.append(name)


def scenario(title):
    print("\n== %s" % title)


# ---------------------------------------------------------------- the tests

FLASHED = elf(tag=b"flashed")
LOCAL = elf(tag=b"locally-built")
LOCAL2 = elf(tag=b"second-local-build")
X86 = elf("x86_64", tag=b"wrong-arch")
I386 = elf("i386", tag=b"wrong-machine")


def t_fastpath():
    scenario("A. no updates, installed UI matches the rootfs manifest -> fast path")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", FLASHED)
        f.link_player()
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("binary untouched", f.read("usr/data/mq_ui") == FLASHED)
        check("took the fast path", "already installed + verified" in f.log(), f.log())
        check("no adoption happened", not f.exists("usr/data/diskos_adopted_manifest"))
    finally:
        f.cleanup()


def t_update_ignored_when_disabled():
    scenario("B. update slot present but the flag is OFF -> slot ignored entirely")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", FLASHED)
        f.link_player()
        f.write("usr/data/diskos_update/mq_ui", LOCAL)
        f.write("usr/data/diskos_update/mq_ui.sha256", (sha(LOCAL) + "  mq_ui\n").encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("still the FLASHED binary", f.read("usr/data/mq_ui") == FLASHED)
        check("slot left alone (not consumed)", f.exists("usr/data/diskos_update/mq_ui"))
        check("nothing adopted", not f.exists("usr/data/diskos_adopted_manifest"))
    finally:
        f.cleanup()


def t_adopt():
    scenario("C. flag ON, good slot -> adopted, published, previous kept")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", FLASHED)
        f.link_player()
        f.write("opt/diskos/mq_ui", FLASHED)          # the flashed copy is present too
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_update/mq_ui", LOCAL)
        f.write("usr/data/diskos_update/mq_ui.sha256", (sha(LOCAL) + "  mq_ui\n").encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("installed binary is the UPDATE", f.read("usr/data/mq_ui") == LOCAL)
        check("update did not lose to the embedded copy",
              f.read("usr/data/mq_ui") != FLASHED)
        check("previous kept at mq_ui.prev",
              f.exists("usr/data/mq_ui.prev") and f.read("usr/data/mq_ui.prev") == FLASHED)
        check("no redundant mq_ui.rejected copy", not f.exists("usr/data/mq_ui.rejected"))
        check("slot consumed", not f.exists("usr/data/diskos_update"))
        man = f.read("usr/data/diskos_adopted_manifest").decode()
        check("adopted manifest records the new hash", sha(LOCAL) in man, man)
        check("adopted manifest records the size", "SIZE=%d" % len(LOCAL) in man, man)
        check("adopted manifest records the rootfs it was adopted against",
              "BASE=%s" % sha(FLASHED) in man, man)
        check("mq_ui is executable", os.access(f.p("usr/data/mq_ui"), os.X_OK))
        check("symlink resolves to mq_ui",
              os.readlink(f.p("usr/data/mq_player")) == f.p("usr/data/mq_ui"))
    finally:
        f.cleanup()


def t_next_boot_after_adopt():
    scenario("D. next boot after an adoption, no slot -> fast path via adopted manifest")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", LOCAL)
        f.link_player()
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_adopted_manifest",
                ("SHA256=%s\nSIZE=%d\nBASE=%s\n"
                 % (sha(LOCAL), len(LOCAL), sha(FLASHED))).encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("adopted binary still installed", f.read("usr/data/mq_ui") == LOCAL)
        check("verified against the adopted manifest",
              "ADOPTED update manifest" in f.log(), f.log())
        check("not quarantined", not f.exists("usr/data/mq_ui.rejected"))
    finally:
        f.cleanup()


def t_sha_mismatch():
    scenario("E. flag ON, slot sha does not match its own file -> discarded, install untouched")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", FLASHED)
        f.link_player()
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_update/mq_ui", LOCAL)
        f.write("usr/data/diskos_update/mq_ui.sha256", (sha(LOCAL2) + "  mq_ui\n").encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("installed binary untouched", f.read("usr/data/mq_ui") == FLASHED)
        check("slot discarded (one-shot)", not f.exists("usr/data/diskos_update"))
        check("nothing adopted", not f.exists("usr/data/diskos_adopted_manifest"))
        check("logged the mismatch", "sha mismatch" in f.log(), f.log())
    finally:
        f.cleanup()


def t_wrong_arch():
    scenario("F. flag ON, slot is an x86-64 binary with a matching sha -> refused on shape")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", FLASHED)
        f.link_player()
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_update/mq_ui", X86)
        f.write("usr/data/diskos_update/mq_ui.sha256", (sha(X86) + "  mq_ui\n").encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("installed binary untouched", f.read("usr/data/mq_ui") == FLASHED)
        check("slot discarded", not f.exists("usr/data/diskos_update"))
        check("nothing adopted", not f.exists("usr/data/diskos_adopted_manifest"))
        check("logged the arch refusal", "not a MIPS" in f.log(), f.log())
    finally:
        f.cleanup()


def t_malformed_adopted():
    scenario("G. adopted manifest is malformed -> ignored, rootfs manifest still governs")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", FLASHED)
        f.link_player()
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_adopted_manifest", b"garbage\n")
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("logged the malformed manifest",
              "adopted manifest malformed" in f.log(), f.log())
        check("still the flashed binary", f.read("usr/data/mq_ui") == FLASHED)
    finally:
        f.cleanup()


def t_flag_removed_reverts_to_flashed():
    scenario("H. user turns updates OFF after adopting -> reinstalls the FLASHED UI")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", LOCAL)              # the adopted build is installed
        f.link_player()
        f.write("opt/diskos/mq_ui", FLASHED)          # the flashed copy is still in the rootfs
        f.write("usr/data/diskos_adopted_manifest",
                ("SHA256=%s\nSIZE=%d\nBASE=%s\n"
                 % (sha(LOCAL), len(LOCAL), sha(FLASHED))).encode())
        # no diskos_updates_enabled -> the adopted manifest must not be honoured
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("still boots diskOS (not dumped to stock)", f.booted_diskos())
        check("reverted to the FLASHED binary", f.read("usr/data/mq_ui") == FLASHED)
        check("adopted build set aside, not deleted",
              f.exists("usr/data/mq_ui.rejected") and f.read("usr/data/mq_ui.rejected") == LOCAL)
    finally:
        f.cleanup()


def t_reflash_beats_an_adoption():
    scenario("L. reflash with a NEWER UI while an adoption stands -> the flashed build wins")
    f = Fixture()
    try:
        # This is the sequence that would otherwise silently waste a 90-minute reflash:
        # /usr/data survives the flash, so the old adopted binary is still sitting at
        # /usr/data/mq_ui and still hashes to what the adopted manifest says.
        f.manifest(LOCAL2)                           # the NEW rootfs blesses LOCAL2
        f.write("opt/diskos/mq_ui", LOCAL2)          # and carries it embedded
        f.write("usr/data/mq_ui", LOCAL)             # but /usr/data still holds the old adopt
        f.link_player()
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_adopted_manifest",  # adopted against the PREVIOUS rootfs
                ("SHA256=%s\nSIZE=%d\nBASE=%s\n"
                 % (sha(LOCAL), len(LOCAL), sha(FLASHED))).encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("installed the FRESHLY FLASHED build", f.read("usr/data/mq_ui") == LOCAL2)
        check("adoption dropped", not f.exists("usr/data/diskos_adopted_manifest"))
        check("logged why", "rootfs reflashed" in f.log(), f.log())
    finally:
        f.cleanup()


def t_legacy_adopted_manifest_dropped():
    scenario("M. adopted manifest with no BASE line (written by an older S97) -> dropped")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("opt/diskos/mq_ui", FLASHED)
        f.write("usr/data/mq_ui", LOCAL)
        f.link_player()
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_adopted_manifest",
                ("SHA256=%s\nSIZE=%d\n" % (sha(LOCAL), len(LOCAL))).encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("boots diskOS", f.booted_diskos())
        check("fell back to the flashed build", f.read("usr/data/mq_ui") == FLASHED)
        check("adoption dropped", not f.exists("usr/data/diskos_adopted_manifest"))
    finally:
        f.cleanup()


def t_no_manifest_fails_closed():
    scenario("I. no rootfs manifest -> quarantine, stock UI (unchanged behaviour)")
    f = Fixture()
    try:
        f.write("usr/data/mq_ui", LOCAL)
        f.link_player()
        f.touch("usr/data/diskos_updates_enabled")
        f.write("usr/data/diskos_update/mq_ui", LOCAL2)
        f.write("usr/data/diskos_update/mq_ui.sha256", (sha(LOCAL2) + "  mq_ui\n").encode())
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("does NOT boot diskOS", not f.booted_diskos())
        check("binary moved aside", f.exists("usr/data/mq_ui.rejected"))
        check("update slot never consulted", f.exists("usr/data/diskos_update/mq_ui"))
    finally:
        f.cleanup()


def t_unverified_local_build_quarantined():
    scenario("J. hand-deployed binary, updates OFF, no rootfs copy -> stock (the old behaviour)")
    f = Fixture()
    try:
        f.manifest(FLASHED)
        f.write("usr/data/mq_ui", LOCAL)     # what diskos-deploy.sh leaves behind
        f.link_player()
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("does NOT boot diskOS", not f.booted_diskos())
        check("binary moved aside", f.exists("usr/data/mq_ui.rejected"))
    finally:
        f.cleanup()


def t_shape_gate_still_applies_to_rootfs_path():
    scenario("K. updates OFF: a shape check now runs before the hash - still fails closed")
    f = Fixture()
    try:
        f.manifest(I386)                    # manifest blesses a 32-bit LE non-MIPS binary
        f.write("usr/data/mq_ui", I386)     # (impossible in practice - here the hash MATCHES,
                                            #  so only the machine gate can refuse it)
        f.link_player()
        r = f.run()
        check("exit 0", r.returncode == 0, r.stderr)
        check("does NOT boot diskOS", not f.booted_diskos())
        check("refused on architecture", "not MIPS" in f.log(), f.log())
    finally:
        f.cleanup()


def main():
    if not os.path.exists(S97):
        print("cannot find %s" % S97)
        return 1
    print("s97check: exercising payload/S97diskos_install against a fake root")
    for t in (t_fastpath, t_update_ignored_when_disabled, t_adopt,
              t_next_boot_after_adopt, t_sha_mismatch, t_wrong_arch,
              t_malformed_adopted, t_flag_removed_reverts_to_flashed,
              t_no_manifest_fails_closed, t_unverified_local_build_quarantined,
              t_shape_gate_still_applies_to_rootfs_path,
              t_reflash_beats_an_adoption, t_legacy_adopted_manifest_dropped):
        t()
    print("")
    if FAILURES:
        print("s97check: %d FAILED" % len(FAILURES))
        for n in FAILURES:
            print("  - %s" % n)
        return 1
    print("s97check: all scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

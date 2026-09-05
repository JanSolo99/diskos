#!/usr/bin/env bash
# diskos-deploy.sh - push a locally built mq_ui to the device and hot-reload it, safely.
#
# This is for iterating on UI changes. By default a hand-deployed binary reverts to the flashed
# build on the next reboot - that is S97's fail-closed contract, and it is what makes this safe to
# experiment with. Pass --persist to ALSO leave the build in the on-device update slot, so S97
# adopts it on the next boot and the change survives without a 60-90 minute reflash.
#
# Usage:
#   DISKOS_IP=<device-ip> DISKOS_PW=<debug-mode-password> ./diskos-deploy.sh [--persist] [path/to/mq_ui]
#
# Get the IP and a one-time SSH password from Debug Mode on the device (Settings > System). The
# password is regenerated on every enable and does not survive a reboot.
#
# --persist requires On-Device Updates to be ON (Settings > System) - the flag file it checks for is
# the user's consent to a weaker boot guarantee, so this tool will not create it for you.
#
# Requires: sshpass, ssh, scp, md5sum, sha256sum. Default binary path: ./mq_ui
set -eu

IP="${DISKOS_IP:?set DISKOS_IP to the device IP (shown in Debug Mode)}"
export SSHPASS="${DISKOS_PW:?set DISKOS_PW to the Debug Mode SSH password}"

PERSIST=0
BIN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --persist) PERSIST=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 1 ;;
    *)  BIN="$1" ;;
  esac
  shift
done
BIN="${BIN:-mq_ui}"
[ -f "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 1; }

# Refuse to persist an obviously wrong binary. The device would refuse it too (S97 checks the ELF
# header), but finding that out means a reboot, and the failure reads as "my update did nothing".
if [ "$PERSIST" = 1 ] && command -v file >/dev/null 2>&1; then
  case "$(file -b "$BIN")" in
    *MIPS*) ;;
    *) echo "refusing --persist: $BIN is not a MIPS binary ($(file -b "$BIN"))" >&2
       echo "   the native fast-check writes x86-64 to this exact path - rebuild with Docker" >&2
       exit 1 ;;
  esac
fi

# password via SSHPASS (sshpass -e), not the command line, so it is not visible in the process list
SSH=(sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8)
MD5="$(md5sum "$BIN" | cut -d' ' -f1)"
echo ">> local mq_ui md5=$MD5 -> root@$IP"

# 1. stream to a staging path and verify the exact md5 before touching the live binary
REMOTE_MD5="$("${SSH[@]}" "root@$IP" \
  'cat > /usr/data/mq_ui.new && chmod 755 /usr/data/mq_ui.new && md5sum /usr/data/mq_ui.new | cut -d" " -f1' \
  < "$BIN")"
[ "$REMOTE_MD5" = "$MD5" ] || { echo "md5 mismatch (device=$REMOTE_MD5) - aborting, live binary untouched" >&2; exit 1; }

# 2. swap in and hot-reload, in a failure-aware order.
#    IMPORTANT: only ever touch mq_ui. NEVER kill mq_player - that frees the SD card and the
#    hardware MCU reboots the whole device (~10s).
#    The fiio_init watchdog does `pgrep -x mq_ui` (matches the full argv[0]); if we kill mq_ui
#    without immediately relaunching a DETACHED replacement it respawns the STOCK /usr/bin/mq_ui.
#    We relaunch with setsid </dev/null, then POLL until our build is confirmed alive, and only
#    THEN prune any stock instance. If our build never comes up (a bad binary), we do NOT prune -
#    the watchdog restores the stock UI as a working fallback.
# The remote block deliberately does NOT use `set -e`: it relies on `[ test ] && cmd` idioms and poll
# loops where a false test is normal, and `set -e` would abort the script on the first false test.
# Every step that must not fail is checked explicitly instead.
"${SSH[@]}" "root@$IP" 'sh -s' <<'REMOTE'
mv -f /usr/data/mq_ui.new /usr/data/mq_ui || { echo "ERROR: could not swap in the new binary" >&2; exit 1; }
killall -9 mq_ui 2>/dev/null
setsid /usr/data/mq_ui </dev/null >/usr/data/diskos_boot.log 2>&1 &
# poll up to ~10s. Sleep FIRST, then check, so a check is always the last action before we decide
# (no trailing sleep that could miss a process which came up in the final second).
ok=0
i=0
while [ "$i" -lt 10 ]; do
  sleep 1
  for p in $(pidof mq_ui 2>/dev/null); do
    if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/data/mq_ui ]; then ok=1; fi
  done
  if [ "$ok" = 1 ]; then break; fi
  i=$((i + 1))
done
if [ "$ok" != 1 ]; then
  echo "ERROR: /usr/data/mq_ui did not come up - NOT pruning; the watchdog will restore stock" >&2
  exit 1
fi
# our build is confirmed running -> prune ONLY a genuine stock instance (exe is EXACTLY /usr/bin/mq_ui)
# that the watchdog may have raced in. Never kill a PID we cannot classify: an empty readlink means the
# PID vanished (or was reused), so leaving it alone avoids killing an unrelated process.
for p in $(pidof mq_ui 2>/dev/null); do
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/bin/mq_ui ]; then kill -9 "$p" 2>/dev/null; fi
done
# verify: no stock instance survived the prune (a failed kill must not report success)
stock=0
for p in $(pidof mq_ui 2>/dev/null); do
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/bin/mq_ui ]; then stock=1; fi
done
if [ "$stock" = 1 ]; then
  echo "ERROR: a stock mq_ui is still running after prune (check the device)" >&2
  exit 1
fi
if pidof mq_player >/dev/null 2>&1; then
  echo ">> deployed OK: /usr/data/mq_ui running; mq_player alive"
else
  echo ">> deployed: /usr/data/mq_ui running, but mq_player is NOT running (check the device)" >&2
fi
REMOTE

# 3. --persist: fill the on-device update slot so S97 adopts this build on the next boot.
#
# Deliberately LAST. The slot is only written after the binary has been confirmed running on the
# device, so "permanent" is never granted to something that has not started at least once here.
# A build that fails to come up exits above and leaves no slot behind.
#
# The slot is filled by copying /usr/data/mq_ui on the DEVICE rather than re-uploading: those bytes
# were just md5-verified end to end and are the ones actually running, so the copy is both cheaper
# and a stronger guarantee than a second transfer would be. The sha256 written alongside it is
# computed HERE, on the host, from the source file - a device-side hash of the device-side copy
# would be tautological and could not catch a corrupt copy.
if [ "$PERSIST" = 1 ]; then
  SHA="$(sha256sum "$BIN" | cut -d' ' -f1)"
  echo ">> persisting: sha256=$SHA"
  "${SSH[@]}" "root@$IP" "sh -s $SHA" <<'REMOTE'
SHA="$1"
# The boot installer lives in the READ-ONLY rootfs, so a device flashed before the update path
# existed simply ignores the slot - the deploy would look like it worked and then silently revert.
# Check the S97 that will actually run, not the one in the repo.
S97=/etc/init.d/S97diskos_install
if [ ! -f "$S97" ] || ! grep -q diskos_updates_enabled "$S97" 2>/dev/null; then
  echo "ERROR: the boot installer on this device predates the on-device update path." >&2
  echo "       It would ignore the update slot, so --persist would silently do nothing." >&2
  echo "       This needs ONE more reflash to install the new S97; after that, never again." >&2
  echo "       (The build you just pushed is running now; it will revert on the next reboot.)" >&2
  exit 1
fi
if [ ! -f /usr/data/diskos_updates_enabled ]; then
  echo "ERROR: On-Device Updates is OFF on this device." >&2
  echo "       Turn it on in Settings > System > On-Device Updates, then re-run with --persist." >&2
  echo "       (The build you just pushed is running now; it will revert on the next reboot.)" >&2
  exit 1
fi
D=/usr/data/diskos_update
rm -rf "$D" && mkdir -p "$D" || { echo "ERROR: could not create $D" >&2; exit 1; }
# Stage under a dot-name and rename in, so S97 can never see a half-written slot: it requires BOTH
# mq_ui and mq_ui.sha256, and the sha file is the LAST thing written.
cp -f /usr/data/mq_ui "$D/.mq_ui.part" || { echo "ERROR: could not stage the slot copy" >&2; rm -rf "$D"; exit 1; }
GOT="$(sha256sum "$D/.mq_ui.part" | cut -d' ' -f1)"
if [ "$GOT" != "$SHA" ]; then
  echo "ERROR: slot copy hashes $GOT, expected $SHA - discarding" >&2
  rm -rf "$D"
  exit 1
fi
mv -f "$D/.mq_ui.part" "$D/mq_ui" || { echo "ERROR: could not publish the slot" >&2; rm -rf "$D"; exit 1; }
printf '%s  mq_ui\n' "$SHA" > "$D/mq_ui.sha256" || { echo "ERROR: could not write the slot sha" >&2; rm -rf "$D"; exit 1; }
sync 2>/dev/null
echo ">> update slot armed - S97 will adopt this build on the next boot"
echo "   (previous binary will be kept at /usr/data/mq_ui.prev)"
REMOTE
else
  echo ">> note: this build reverts on the next reboot. Re-run with --persist to keep it."
fi

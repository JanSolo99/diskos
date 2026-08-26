#!/bin/bash
# build-usbboot-static.sh - build a portable, fully-static `usbboot` for Linux.
#
# The distro's libusb-1.0.a is built WITH udev, so linking against it drags in
# libudev + libcap dynamically - which defeats portability (missing/old libudev
# on other distros). This rebuilds libusb with --disable-udev --enable-static so
# usbboot links fully static: no libusb.so, no libudev, no glibc-version pin.
# udev is only needed for hotplug events; usbboot does a one-shot vid/pid open,
# which uses libusb's sysfs backend and works without udev.
#
# Output: installer/vendor/linux-x86_64/usbboot  (statically linked, stripped).
# Run on an x86_64 Linux host with build-essential + apt source access.
set -euo pipefail
cd "$(dirname "$0")/.."                       # installer/
ROOT=$(pwd)
SRC="$ROOT/src/usbboot/usbboot.c"
OUT="$ROOT/vendor/linux-x86_64/usbboot"
[ -f "$SRC" ] || { echo "ERROR: $SRC missing" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "==> fetching libusb source"
apt-get source libusb-1.0 >/dev/null 2>&1 || {
  echo "ERROR: 'apt-get source libusb-1.0' failed - enable deb-src or fetch libusb 1.0.x manually." >&2
  exit 3; }
cd libusb-1.0-*/

echo "==> configuring libusb (static, no udev)"
./configure --disable-udev --enable-static --disable-shared \
  --disable-examples-build --disable-tests-build CFLAGS="-O2" >/dev/null
echo "==> building libusb"
make -j"$(nproc)" >/dev/null
LIBA=$(find "$PWD" -name libusb-1.0.a | head -1)
[ -f "$LIBA" ] || { echo "ERROR: libusb-1.0.a not produced" >&2; exit 4; }
INC=$(dirname "$LIBA")/../.. ; INC="$PWD/libusb"

echo "==> linking usbboot (fully static)"
gcc -O2 -std=c99 -I"$INC" -static -o usbboot "$SRC" "$LIBA" -lpthread
strip usbboot
file usbboot | grep -q "statically linked" || { echo "ERROR: not static" >&2; exit 5; }

install -m 0755 usbboot "$OUT"
echo "==> installed: $OUT"
echo "    sha256: $(sha256sum "$OUT" | cut -d' ' -f1)"
echo "    $(file -b "$OUT")"
echo "NOTE: confirm a mask-ROM GET_CPU_INFO handshake on a real device before shipping."

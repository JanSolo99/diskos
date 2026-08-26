#!/bin/bash
# build-squashfs-static.sh - build portable, fully-static mksquashfs + unsquashfs
# for Linux, with the compressors the installer actually uses.
#
# The installer repacks the rootfs with `-comp lzo -b 131072` and unpacks the
# stock (lzo) rootfs, so LZO is mandatory; gzip + xz are included for reading
# other firmware. Built static (liblzo2.a/libz.a/liblzma.a) so there are no
# liblzo2/libz/liblzma/glibc-version runtime deps on the user's machine.
#
# Output: installer/vendor/linux-x86_64/{mksquashfs,unsquashfs} (static, stripped).
# A cosmetic getpwuid NSS warning at link time is expected (owner-name display in
# the summary only; packing uses numeric uid/gid) and does not affect output.
set -euo pipefail
cd "$(dirname "$0")/.."                       # installer/
OUTDIR="$(pwd)/vendor/linux-x86_64"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
echo "==> fetching squashfs-tools source"
apt-get source squashfs-tools >/dev/null 2>&1 || {
  echo "ERROR: 'apt-get source squashfs-tools' failed - enable deb-src or fetch squashfs-tools 4.x." >&2
  exit 3; }
cd squashfs-tools-*/squashfs-tools

echo "==> building static (gzip+lzo+xz)"
make clean >/dev/null 2>&1 || true
make GZIP_SUPPORT=1 LZO_SUPPORT=1 XZ_SUPPORT=1 \
     EXTRA_CFLAGS="-static" LDFLAGS="-static" -j"$(nproc)" mksquashfs unsquashfs >/dev/null
strip mksquashfs unsquashfs

for b in mksquashfs unsquashfs; do
  file "$b" | grep -q "statically linked" || { echo "ERROR: $b not static" >&2; exit 5; }
done

# prove the installer's exact LZO path round-trips before installing
t="$WORK/smoke"; mkdir -p "$t/src"; echo diskos > "$t/src/f"
./mksquashfs "$t/src" "$t/o.sqsh" -comp lzo -b 131072 -noappend >/dev/null 2>&1
./unsquashfs -d "$t/u" "$t/o.sqsh" >/dev/null 2>&1
diff -r "$t/src" "$t/u" >/dev/null || { echo "ERROR: LZO round-trip failed" >&2; exit 6; }
echo "==> LZO round-trip OK"

for b in mksquashfs unsquashfs; do
  install -m 0755 "$b" "$OUTDIR/$b"
  echo "==> installed $OUTDIR/$b  ($(sha256sum "$OUTDIR/$b" | cut -d' ' -f1))"
done

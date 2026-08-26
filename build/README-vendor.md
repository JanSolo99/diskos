# Vendored native tools - portability requirements

The installer bundles native binaries under `vendor/<os>-<arch>/` and calls them
via subprocess. For a distributable build these MUST be portable across the user's
machines - not just copies of the build host's dynamically-linked binaries.

| tool | why bundled | portability requirement |
|---|---|---|
| `usbboot` | Ingenic mask-ROM USB loader (the flasher) | Build **static** against libusb compiled `--disable-udev` (drops libudev + libcap; libusb falls back to sysfs enumeration). `usbboot` has **no direct udev symbols** - they were only transitive via libusb - so this yields a binary needing only libc. Prefer full-static (musl) or, at minimum, `$ORIGIN/lib` rpath + a bundled `libusb-1.0.so` in `vendor/<tag>/lib/`. Do **not** ship arbitrary host glibc `.so`s or rely on `LD_LIBRARY_PATH` as the primary strategy. |
| `mksquashfs`, `unsquashfs` | build/extract the rootfs image | Ship **static** squashfs-tools (with lzo support - the stock image uses `-comp lzo`). |
| `my_write5_dram.bin` | the DRAM NAND writer run on-device | Architecture-neutral blob (runs on the device, not the host). Copy as-is. |
| `disc_spl_lpddr3.bin` | X2000 SPL | Same - device blob, copy as-is. |

## Status

- `vendor/linux-x86_64/` holds **fully static** `usbboot`, `mksquashfs`, and
  `unsquashfs`, produced by `build/build-usbboot-static.sh` and
  `build/build-squashfs-static.sh` (each verifies the binary is `statically linked`;
  the squashfs build additionally runs an LZO round-trip self-test). They are static
  against glibc (built with `gcc -static`), so no runtime `.so` is needed; note the
  static glibc/liblzo2/libusb licensing in `../NOTICE.md` and the corresponding source
  in `../corresponding-source/`. `my_write5_dram.bin` and `disc_spl_lpddr3.bin` are
  device blobs, copied as-is.
- `vendor/macos-*/` is produced on a Mac by **`vendor/setup-macos.sh`** (compiles
  `usbboot` from `src/usbboot/usbboot.c` against Homebrew libusb, gathers
  lzo-capable squashfs-tools, copies the device blobs, and runs `dylibbundler` so
  every tool is self-contained under `vendor/macos-<arch>/lib/` - no Homebrew at
  runtime). `usbboot.c` is a single libusb-only file (no udev / no Linux-isms), so
  it builds on macOS as-is. Then `build/build-macos.sh` packages the app.

## macOS: Intel + Apple Silicon coverage

`usbboot.c` and pyusb are portable; only the *build* is per-arch. Options:

1. **Native per-arch (most robust):** run `build/build-macos.sh` on an Intel Mac
   and on an Apple Silicon Mac → two binaries. Recommended.
2. **Single file via Rosetta:** build **x86_64** only (on an Intel Mac, or with an
   x86_64 Homebrew under `arch -x86_64`). The x86_64 app runs natively on Intel and
   under Rosetta 2 on M-series (macOS offers to install Rosetta on first run).
   One file covers both, at a small speed cost - fine for a flasher.
3. **Universal2 (advanced):** build both arches, `lipo` the vendor tools into fat
   binaries, and set `target_arch='universal2'` in the spec with a universal2
   Python. One native file for both, most work.

## Building portable usbboot (sketch)

1. Get the Ingenic `usbboot` source (the X2000 burn tool).
2. Build libusb static: `./configure --disable-udev --enable-static --disable-shared`
   → `libusb-1.0.a`.
3. Link `usbboot` against the static libusb; confirm with `ldd` that libudev/libcap
   are gone and only libc (or nothing, if fully static) remains.
4. Verify it still enters mask-ROM and flashes on a real unit before shipping.

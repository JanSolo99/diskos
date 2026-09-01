# diskOS UI build environment - pins the musl mipsel cross-toolchain so `mq_ui` builds reproducibly.
#
# The published binary builds with a static mipsel-linux-musl GCC 11.2.x (from musl.cc). SQLite's
# amalgamation compiles cleanly against it (verified with and without _GNU_SOURCE / _FILE_OFFSET_BITS).
# The "musl fighting SQLite on fcntl64" failures people hit are a TOOLCHAIN mismatch, not a flag
# problem - this image removes the variable by pinning the exact toolchain (by SHA-256).
#
# Build the image:   docker build -t diskos-ui-builder .
# Build the UI:       docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" diskos-ui-builder
#   Run from the ui/ directory. The `-u` keeps the built mq_ui/*.o owned by you, not root.
#   LVGL is vendored at ui/lvgl, so no extra checkout is needed; the static mipsel `mq_ui` lands here.
#
# The toolchain host binaries are 32-bit x86 (i386), so this is an amd64 image - the FROM pins that.
# On Apple Silicon / ARM, Docker runs it under emulation automatically (slower, but works).

FROM --platform=linux/amd64 debian:12-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates make \
    && rm -rf /var/lib/apt/lists/*

# Static musl mipsel cross-toolchain (GCC 11.2.1, i386 host binaries, statically linked - the exact
# family diskOS is built with). Pinned by SHA-256: if musl.cc changes the tarball the build fails
# loudly instead of drifting - update the checksum here when you intentionally bump the toolchain.
ARG TC_URL=https://musl.cc/mipsel-linux-musl-cross.tgz
ARG TC_SHA256=82626533bf7e677c225e7cbedf1d5b0d6bc60c3daaf28249e54f0eb805d89b13
RUN curl -fsSL "$TC_URL" -o /tmp/tc.tgz \
    && echo "$TC_SHA256  /tmp/tc.tgz" | sha256sum -c - \
    && tar -xzf /tmp/tc.tgz -C /opt \
    && rm /tmp/tc.tgz
ENV CROSS=/opt/mipsel-linux-musl-cross/bin/mipsel-linux-musl-

WORKDIR /src
# Default: build mq_ui from the mounted ui/ tree. Fails clearly if the source isn't mounted.
CMD ["sh", "-c", "test -f Makefile || { echo 'mount the ui/ source at /src (see the Dockerfile header)'; exit 1; }; make CROSS=$CROSS && ls -l mq_ui"]

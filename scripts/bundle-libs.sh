#!/bin/bash
# Copy the transitive shared-library closure of the given ELF files into a lib/ dir,
# as real files named by SONAME (the SD card is exFAT: no symlinks), plus the glibc
# dynamic loader. Then point every given executable at the bundled loader and lib dir.
#
# Usage: bundle-libs.sh <device-prefix> <out-dir> <elf>...
#   device-prefix: absolute install path on the device (e.g. /mnt/SDCARD/Apps/WPE)
#   out-dir:       local staging dir that mirrors device-prefix
set -euo pipefail

DEVICE_PREFIX=$1; OUT=$2; shift 2
LIBDIR="$OUT/lib"
mkdir -p "$LIBDIR"

# build/ffmpeg first: the minimal FFmpeg (scripts/build-ffmpeg.sh) replaces Debian's for gst-libav.
# runtime/mpv/lib last: only for what nothing else has, libcedarc (its h264_cedar decoder links it)
SEARCH=(/work/build/ffmpeg/lib /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu /usr/aarch64-linux-gnu/lib "$OUT/lib"
        /work/runtime/mpv/lib)

# Provided by the device (PowerVR GPU stack, TrimUI SDL2): never bundle these.
skip() {
    case "$1" in
        libEGL.so*|libGLESv2.so*|libGLESv1_CM.so*|libGLdispatch.so*|libGLX*|libOpenGL.so*) return 0 ;;
        libSDL2-2.0.so*|libSDL2_ttf-2.0.so*) return 0 ;;
        libasound.so*) return 0 ;;  # the device's alsa-lib matches its asound.conf and plugins
        libGL.so.1) return 0 ;;     # a stub built by package.sh (scripts/libgl-stub.c)
        *) return 1 ;;
    esac
}

find_lib() {
    for d in "${SEARCH[@]}"; do
        [ -f "$d/$1" ] && { readlink -f "$d/$1"; return 0; }
    done
    return 1
}

declare -A seen
queue=("$@")
while [ ${#queue[@]} -gt 0 ]; do
    f=${queue[0]}; queue=("${queue[@]:1}")
    for need in $(aarch64-linux-gnu-readelf -d "$f" 2>/dev/null | awk '/NEEDED/{gsub(/[\[\]]/,"",$5); print $5}'); do
        [ -n "${seen[$need]:-}" ] && continue
        seen[$need]=1
        skip "$need" && continue
        src=$(find_lib "$need") || { echo "WARN: $need not found (needed by $f)" >&2; continue; }
        if [ "$src" != "$(readlink -f "$LIBDIR/$need" 2>/dev/null)" ]; then
            cp -f "$src" "$LIBDIR/$need"
        fi
        queue+=("$LIBDIR/$need")
    done
done

cp -f "$(readlink -f /lib/aarch64-linux-gnu/ld-linux-aarch64.so.1)" "$LIBDIR/ld-linux-aarch64.so.1"
# Always ship the full glibc set: device blobs (PowerVR) link libdl/libpthread/librt/libm,
# and mixing the device's glibc 2.33 pieces with our 2.36 loader breaks (GLIBC_PRIVATE ABI).
for g in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libutil.so.1 libresolv.so.2 libanl.so.1; do
    cp -f "$(readlink -f /lib/aarch64-linux-gnu/$g)" "$LIBDIR/$g"
done
aarch64-linux-gnu-strip --strip-unneeded "$LIBDIR"/*.so* 2>/dev/null || true

# Bundled libs first; then device GPU blobs (/usr/lib) and TrimUI SDL2 (/usr/trimui/lib).
# --force-rpath writes DT_RPATH, which (unlike RUNPATH) also applies to transitive deps.
for exe in "$@"; do
    if aarch64-linux-gnu-readelf -l "$exe" | grep -q "program interpreter"; then
        patchelf --set-interpreter "$DEVICE_PREFIX/lib/ld-linux-aarch64.so.1" "$exe"
    fi
    patchelf --force-rpath --set-rpath "$DEVICE_PREFIX/lib:/usr/trimui/lib:/usr/lib:/lib64" "$exe"
done
echo "Bundled $(ls "$LIBDIR" | wc -l) libs into $LIBDIR"

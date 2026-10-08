#!/bin/bash
# A minimal FFmpeg for GStreamer's gst-libav plugin (in-page <video>/<audio>): decoders only, no
# external libraries, LGPL. Debian's FFmpeg links ~40 codec/encoder/filter libraries (x264, x265,
# aom, rav1e, codec2, flite, librsvg...: 150+ MB) that a browser never uses. Same release as
# Debian's (5.1.9: same sonames and symbol versions), so Debian's gst-libav loads it unchanged.
# Installs into build/ffmpeg/lib; package.sh bundles it instead of Debian's.
# Runs inside wpe-tsp-builder:
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work wpe-tsp-builder scripts/build-ffmpeg.sh
set -euo pipefail

VERSION=5.1.9
SRC=/work/src/ffmpeg-$VERSION
OUT=/work/build/ffmpeg

if [ ! -d "$SRC" ]; then
    mkdir -p /work/src
    TARBALL=/work/src/ffmpeg-$VERSION.tar.xz
    [ -f "$TARBALL" ] || wget -O "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    tar -xf "$TARBALL" -C /work/src
fi

BUILD=/tmp/ffmpeg-build
rm -rf "$BUILD" "$OUT" && mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
    --prefix="$OUT" --arch=aarch64 --cpu=cortex-a53 --target-os=linux \
    --enable-cross-compile --cross-prefix=aarch64-linux-gnu- \
    --enable-shared --disable-static --enable-pic \
    --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-swscale --disable-swresample --disable-postproc \
    --disable-everything \
    --enable-decoder=h264,vp8,vp9,mpeg4,aac,aac_latm,mp3,mp3float,opus,vorbis,flac \
    --enable-parser=h264,vp8,vp9,mpeg4video,aac,aac_latm,mpegaudio,opus,vorbis,flac \
    --enable-filter=buffer,buffersink,yadif
make -j"$(nproc)"
make install
echo "=== FFmpeg $VERSION installed into $OUT ==="
ls -la "$OUT/lib"

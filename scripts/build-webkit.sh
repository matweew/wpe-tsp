#!/bin/bash
# Configure + build WPE WebKit 2.54 for the TrimUI Smart Pro. Runs inside wpe-tsp-builder.
# Usage (host): docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work \
#                   wpe-tsp-builder scripts/build-webkit.sh [jobs]
# Downloads the WPE WebKit release tarball into src/ if needed and applies patches/*.patch.
set -euo pipefail

WEBKIT_VERSION=2.54.0
SRC=/work/src/wpewebkit-${WEBKIT_VERSION}
BUILD=/work/build/webkit
# Install prefix == location on the device, so WebKit finds its helper processes
# (WPEWebProcess, WPENetworkProcess) at the compiled-in libexec path.
PREFIX=/mnt/SDCARD/Apps/WPE
STAGE=/work/build/stage
JOBS=${1:-10}

# Fetch + unpack the release tarball on first use, then apply our patches once.
if [ ! -d "$SRC" ]; then
    mkdir -p /work/src
    TARBALL=/work/src/wpewebkit-${WEBKIT_VERSION}.tar.xz
    [ -f "$TARBALL" ] || wget -O "$TARBALL" "https://wpewebkit.org/releases/wpewebkit-${WEBKIT_VERSION}.tar.xz"
    tar -xf "$TARBALL" -C /work/src
fi

if [ ! -f "$SRC/.patched" ]; then
    for p in /work/patches/*.patch; do
        echo "Applying $p"
        patch -d "$SRC" -p1 --forward < "$p"
    done
    touch "$SRC/.patched"
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/toolchain-aarch64.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -g0" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -g0" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DPORT=WPE \
    -DENABLE_WPE_PLATFORM=ON \
    -DENABLE_WPE_PLATFORM_HEADLESS=ON \
    -DENABLE_WPE_PLATFORM_DRM=OFF \
    -DENABLE_WPE_PLATFORM_WAYLAND=OFF \
    -DENABLE_WPE_LEGACY_API=OFF \
    -DENABLE_WPE_QT_API=OFF \
    -DUSE_GBM=OFF \
    -DUSE_LIBDRM=OFF \
    -DENABLE_GPU_PROCESS=OFF \
    -DENABLE_DOCUMENTATION=OFF \
    -DENABLE_INTROSPECTION=OFF \
    -DENABLE_JOURNALD_LOG=OFF \
    -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
    -DENABLE_WEBDRIVER=OFF \
    -DENABLE_SPELLCHECK=OFF \
    -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DUSE_GSTREAMER=OFF \
    -DENABLE_VIDEO=OFF \
    -DENABLE_WEB_AUDIO=OFF \
    -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_MEDIA_RECORDER=OFF \
    -DENABLE_MEDIA_SESSION=OFF \
    -DENABLE_WEB_CODECS=OFF \
    -DENABLE_ENCRYPTED_MEDIA=OFF \
    -DENABLE_GAMEPAD=OFF \
    -DENABLE_WEBGL=ON \
    -DENABLE_MINIBROWSER=OFF \
    -DENABLE_API_TESTS=OFF \
    -DUSE_ATK=OFF \
    -DUSE_AVIF=OFF \
    -DUSE_JPEGXL=OFF \
    -DUSE_LIBHYPHEN=OFF \
    -DUSE_LIBBACKTRACE=OFF \
    -DUSE_SYSPROF_CAPTURE=OFF

ninja -C "$BUILD" -j"$JOBS" -k "${KEEP_GOING:-1}"
DESTDIR="$STAGE" ninja -C "$BUILD" install
echo "=== WebKit installed into $STAGE$PREFIX ==="

#!/bin/sh
# Recreate sysroot-device/: the device's own SDL2 (2.30.x, PowerVR GE8300 build) and SDL2_ttf
# libraries to link the app against, plus the SDL_ttf header. At runtime the app uses the
# copies in /usr/trimui/lib on the device; these are only needed at build time.
# Usage: scripts/fetch-device-sdl.sh            (device from .device; DEVICE=root@<ip> overrides)
set -e
. "$(dirname "$0")/device.sh"
cd "$(dirname "$0")/.."

mkdir -p sysroot-device/lib sysroot-device/include
scp "$DEVICE:/usr/trimui/lib/libSDL2-2.0.so.0" "$DEVICE:/usr/trimui/lib/libSDL2_ttf-2.0.so.0" sysroot-device/lib/
# Linker names (real copies: the project may live on a filesystem without symlinks)
cp sysroot-device/lib/libSDL2-2.0.so.0 sysroot-device/lib/libSDL2.so
cp sysroot-device/lib/libSDL2_ttf-2.0.so.0 sysroot-device/lib/libSDL2_ttf.so
wget -q -O sysroot-device/include/SDL_ttf.h \
    https://raw.githubusercontent.com/libsdl-org/SDL_ttf/release-2.0.15/SDL_ttf.h
echo "sysroot-device/ ready"

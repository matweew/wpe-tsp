#!/bin/bash
# Build the browser app against the staged WebKit and assemble dist/WPE for the device.
# Runs inside wpe-tsp-builder:  docker run --rm -v "$PWD":/work wpe-tsp-builder scripts/package.sh
set -euo pipefail

DEVICE_PREFIX=/mnt/SDCARD/Apps/WPE
STAGE=/work/build/stage$DEVICE_PREFIX
OUT=/work/dist/WPE
MULTIARCH=/usr/lib/aarch64-linux-gnu

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share/fonts"

# --- pkg-config view of the staged install (its .pc files point at the device prefix)
PCDIR=/tmp/stage-pc
rm -rf "$PCDIR" && mkdir -p "$PCDIR"
for pc in "$STAGE"/lib/pkgconfig/*.pc; do
    sed "s#$DEVICE_PREFIX#$STAGE#g" "$pc" > "$PCDIR/$(basename "$pc")"
done
export PKG_CONFIG_PATH=$PCDIR

# --- app
aarch64-linux-gnu-gcc-12 -O2 -Wall -Wextra -mcpu=cortex-a53 \
    -o "$OUT/bin/wpe-tsp" /work/app/browser.c /work/app/osk.c /work/app/menu.c /work/app/downloads.c /work/app/player.c /work/app/dmabuf.c /work/app/ytdlp.c /work/app/hid.c \
    -I/opt/SDL2-2.26.1/include -I/work/sysroot-device/include -D_REENTRANT \
    $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-2.0) \
    -L/work/sysroot-device/lib -lSDL2 -lSDL2_ttf -lxkbcommon -lm -ldl \
    -Wl,--allow-shlib-undefined

# --- WebKit runtime: libs (as real files named by SONAME; exFAT has no symlinks)
for lib in "$STAGE"/lib/*.so.*; do
    soname=$(aarch64-linux-gnu-readelf -d "$lib" | awk '/SONAME/{gsub(/[\[\]]/,"",$5); print $5}')
    [ -n "$soname" ] && cp -f "$(readlink -f "$lib")" "$OUT/lib/$soname"
done
cp -r "$STAGE/libexec" "$OUT/"
[ -d "$STAGE/lib/wpe-webkit-2.0" ] && cp -r "$STAGE/lib/wpe-webkit-2.0" "$OUT/lib/"

# --- TLS for libsoup: glib-networking GnuTLS module
mkdir -p "$OUT/lib/gio/modules"
cp "$MULTIARCH/gio/modules/libgiognutls.so" "$OUT/lib/gio/modules/"

# --- GStreamer plugins for <video>/<audio> (Debian's; the libraries come with the closure below).
# Only what playback needs: demuxers (MP4, WebM, Ogg, MP3, WAV), parsers, FFmpeg decoders
# (gst-libav: H.264, VP9, AAC...), Opus/Vorbis, conversion, GL upload (WebKit's video sink),
# ALSA output (the device's libasound and asound.conf: dmix + its volume control). WebKit also
# asks for scaletempo (audiofx: playback speed), videobalance (videofilter), deinterlace and the
# WebVTT encoder (subenc: subtitles).
GST_PLUGINS="coreelements typefindfunctions playback app gio audioconvert audioresample audiomixer
    videoconvertscale volume opengl opus opusparse vorbis ogg alsa autodetect audioparsers isomp4
    matroska id3demux mpg123 wavparse interleave libav videoparsersbad audiofx videofilter deinterlace
    subenc"
mkdir -p "$OUT/lib/gstreamer-1.0"
for plugin in $GST_PLUGINS; do
    cp "$MULTIARCH/gstreamer-1.0/libgst$plugin.so" "$OUT/lib/gstreamer-1.0/"
done

# GStreamer's GL library links libGL.so.1 for X11 only: a stub with the GLX entry points it names
aarch64-linux-gnu-gcc-12 -O2 -shared -fPIC -Wl,-soname,libGL.so.1 -o "$OUT/lib/libGL.so.1" /work/scripts/libgl-stub.c

# --- ad blocking: EasyList + EasyPrivacy domain rules -> WebKit content-blocker JSON.
# The lists are cached in build/adblock; ADBLOCK_REFRESH=1 downloads fresh copies.
ADBLOCK=/work/build/adblock
mkdir -p "$ADBLOCK" "$OUT/share/adblock"
for list in easylist easyprivacy; do
    if [ "${ADBLOCK_REFRESH:-0}" = 1 ] || [ ! -s "$ADBLOCK/$list.txt" ]; then
        wget -q -O "$ADBLOCK/$list.txt" "https://easylist.to/easylist/$list.txt"
    fi
done
python3 /work/scripts/make-adblock.py "$OUT/share/adblock/rules.json" "$ADBLOCK/easylist.txt" "$ADBLOCK/easyprivacy.txt"
# Stable mtime (the browser recompiles when size or mtime change): the newest list's date
touch -r "$(ls -t "$ADBLOCK"/*.txt | head -1)" "$OUT/share/adblock/rules.json"

# --- data: fonts, keyboard layouts, XKB
cp /opt/runtime-data/usr/share/fonts/truetype/dejavu/*.ttf "$OUT/share/fonts/"
mkdir -p "$OUT/share/mime"
cp /opt/runtime-data/usr/share/mime/mime.cache "$OUT/share/mime/"   # file:// content types
# Every XKB layout/variant, compiled (physical + on-screen keyboards, KEYBOARD_LAYOUTS). Cached:
# compiling ~580 keymaps takes a while; delete build/xkb-keymaps.bin to redo it.
[ -s /work/build/xkb-keymaps.bin ] && [ /work/build/xkb-keymaps.bin -nt /work/scripts/xkb-keymaps.py ] \
    || python3 /work/scripts/xkb-keymaps.py /opt/runtime-data/usr/share/X11/xkb /work/build/xkb-keymaps.bin
cp /work/build/xkb-keymaps.bin "$OUT/share/xkb-keymaps.bin"
# Only the XKB files for the one keymap WPE compiles (evdev/pc105/us): ~0.4 MB instead of 3.8 MB
python3 /work/scripts/xkb-minimal.py /opt/runtime-data/usr/share/X11/xkb "$OUT/share/xkb"

# --- video: mpv (its own libraries, built for the device's glibc: not bundled/patched) and
# yt-dlp (updated on the device by the browser; deploy.sh only installs it if missing)
cp -r /work/runtime/mpv "$OUT/mpv"
cp /work/runtime/yt-dlp "$OUT/bin/yt-dlp"
chmod 755 "$OUT/mpv/mpv" "$OUT/bin/yt-dlp"

# --- launcher
cp /work/app/launch.sh /work/app/config.json /work/app/settings.conf "$OUT/"
[ -f /work/app/icon.png ] && cp /work/app/icon.png "$OUT/"
chmod +x "$OUT/launch.sh"

# --- resolve shared-lib closure, bundle glibc, set interpreter + rpath
ELFS=("$OUT/bin/wpe-tsp" "$OUT"/libexec/wpe-webkit-2.0/* "$OUT"/lib/*.so.* "$OUT/lib/gio/modules/libgiognutls.so"
      "$OUT"/lib/gstreamer-1.0/*.so)
while IFS= read -r -d '' f; do ELFS+=("$f"); done < <(find "$OUT/lib/wpe-webkit-2.0" -name '*.so' -print0 2>/dev/null)
/work/scripts/bundle-libs.sh "$DEVICE_PREFIX" "$OUT" "${ELFS[@]}"

aarch64-linux-gnu-strip --strip-unneeded "$OUT/bin/wpe-tsp" "$OUT"/libexec/wpe-webkit-2.0/* "$OUT"/lib/*.so* \
    "$OUT"/lib/gstreamer-1.0/*.so 2>/dev/null || true

# exFAT can't store symlinks: fail loudly if any slipped in.
if find "$OUT" -type l | grep -q .; then
    echo "ERROR: symlinks in $OUT:"; find "$OUT" -type l; exit 1
fi
du -sh "$OUT"; du -sh "$OUT"/lib

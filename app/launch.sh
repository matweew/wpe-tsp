#!/bin/sh
# TrimUI app launcher for the WPE browser.

progdir="$(dirname "$(readlink -f "$0")")"
cd "$progdir"

# User settings live in settings.conf (kept across updates). The browser has its own default
# for every setting that isn't there (HOME_URL has none). Variables
# already set in the environment win (used by test tooling).
[ -f "$progdir/settings.conf" ] && . "$progdir/settings.conf"
for key in HOME_URL SEARCH_URL SCALE PAGE_MEMORY_LIMIT_MB POINTER_HIDE_SECONDS HISTORY_SIZE \
           DOWNLOAD_DIR PLAY_YOUTUBE_IN_MPV USER_AGENT AD_BLOCK WEBGL START_PAGE \
           KEYBOARD_LAYOUTS; do
    eval "value=\${$key-} env=\${WPE_TSP_$key-}"
    [ -z "$env" ] && [ -n "$value" ] && export "WPE_TSP_$key=$value"
done

# Fonts: bundled DejaVu first; the device's Source Han Sans (/usr/trimui/res/full.ttf) as
# fallback for characters DejaVu lacks (Chinese, Japanese, Korean). regular.ttf in the same
# directory only duplicates the CJK coverage, so it is ignored.
cat > "$progdir/fonts.conf" << EOF
<?xml version="1.0"?>
<fontconfig>
  <dir>$progdir/share/fonts</dir>
  <dir>/usr/trimui/res</dir>
  <selectfont><rejectfont><glob>/usr/trimui/res/regular.ttf</glob></rejectfont></selectfont>
  <cachedir>/tmp/wpe-fc-cache</cachedir>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family><family>Source Han Sans SC</family></prefer></alias>
  <alias><family>serif</family><prefer><family>DejaVu Serif</family><family>Source Han Sans SC</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family><family>Source Han Sans SC</family></prefer></alias>
</fontconfig>
EOF
export FONTCONFIG_FILE="$progdir/fonts.conf"

# TLS for libsoup (glib-networking/GnuTLS module) and XKB data for the keymap.
export GIO_MODULE_DIR="$progdir/lib/gio/modules"
export XKB_CONFIG_ROOT="$progdir/share/xkb"
# MIME database (share/mime/mime.cache): content types of file:// pages; the device has none.
export XDG_DATA_DIRS="$progdir/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

# Profile (cookies, local storage) and HTTP cache on the internal ext4 partition:
# WebKit's disk cache needs hard links, which the exFAT SD card doesn't support.
datadir=/mnt/UDISK/wpe-browser
export HOME="$datadir"
export XDG_DATA_HOME="$datadir/data"
export XDG_CACHE_HOME="$datadir/cache"
mkdir -p "$XDG_DATA_HOME" "$XDG_CACHE_HOME"

# GStreamer (<video>/<audio>): only the bundled plugins; the registry is scanned in-process
# (no gst-plugin-scanner helper) and cached with the HTTP cache. AV1 has no hardware support
# and is too slow in software here: hide FFmpeg's AV1 decoder, so sites pick H.264 or VP9.
export GST_PLUGIN_SYSTEM_PATH="$progdir/lib/gstreamer-1.0"
export GST_PLUGIN_PATH=
export GST_REGISTRY_FORK=no
export GST_REGISTRY="$XDG_CACHE_HOME/gstreamer-registry.bin"
export GST_PLUGIN_FEATURE_RANK="avdec_av1:NONE,${GST_PLUGIN_FEATURE_RANK:-}"
# GStreamer's GL (video frames to the GPU) asks EGL for desktop OpenGL first; the PowerVR driver
# only has GLES ("Failed to bind OpenGL API: EGL_BAD_PARAMETER" and no picture).
export GST_GL_API=gles2
export GST_GL_PLATFORM=egl

# Rendering tuning for the PowerVR GE8300: 4x MSAA is costly on this GPU, Skia's
# analytic anti-aliasing looks the same for page content.
export WEBKIT_SKIA_MSAA_SAMPLE_COUNT=${WEBKIT_SKIA_MSAA_SAMPLE_COUNT:-0}
# One web process at a time: no per-site process swapping and no cache of suspended
# processes (each costs 100-200 MB; several at once exhaust the 1 GB RAM).
export WEBKIT_DISABLE_PSON=1
export WEBKIT_DISABLE_WEB_PROCESS_CACHE=1
# glibc creates up to 8 malloc arenas per core for threaded processes; WebKit's network
# process (libsoup/GnuTLS threads) wastes ~50 MB that way. Two arenas are enough here.
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
# Zero-copy frames: the web process renders into DMA-bufs from ION heap 0 (sys_user, ordinary
# pages: the GPU has an MMU) that the browser shows directly (WebKit patch 0008). Without
# /dev/ion, or if a buffer can't be created, WebKit copies frames instead.
# WPE_TSP_ZERO_COPY=0 forces copied frames (diagnostics).
if [ "${WPE_TSP_ZERO_COPY:-1}" = 1 ]; then
    export WEBKIT_DMABUF_ION=0x1
fi

# Keep the device awake while browsing. (CPU speed is left to TrimUI's FN switch:
# standard 1.0-2.0 GHz or performance at 2.0 GHz.)
echo wpe-browser > /sys/power/wake_lock 2>/dev/null

./bin/wpe-tsp "$@" > "$progdir/wpe-tsp.log" 2>&1

echo wpe-browser > /sys/power/wake_unlock 2>/dev/null

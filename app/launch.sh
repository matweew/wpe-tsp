#!/bin/sh
# TrimUI app launcher for the WPE browser.

progdir="$(dirname "$(readlink -f "$0")")"
cd "$progdir"

# User settings live in settings.conf (the browser's Settings page edits it). Updates ship
# settings.default.conf, so unpacking a release over the folder never touches the user's file: the
# first start copies it, later starts add the settings an update introduced (comments + default),
# never changing existing values.
if [ ! -f "$progdir/settings.conf" ]; then
    cp "$progdir/settings.default.conf" "$progdir/settings.conf"
elif [ -f "$progdir/settings.default.conf" ]; then
    have=" $(grep -o '^[A-Z_]*=' "$progdir/settings.conf" | tr -d '=' | tr '\n' ' ') "
    new=$(awk -v have="$have" '
        /^#/ || /^$/ { block = block $0 "\n"; next }
        /^[A-Z_]+=/ { key = $0; sub(/=.*/, "", key)
                      if (index(have, " " key " ") == 0) printf "%s%s\n", block, $0
                      block = ""; next }
    ' "$progdir/settings.default.conf")
    if [ -n "$new" ]; then
        [ -n "$(tail -c 1 "$progdir/settings.conf")" ] && echo >> "$progdir/settings.conf" # ends a last line
        printf '%s\n' "$new" >> "$progdir/settings.conf"
    fi
fi
# The browser has its own default for every setting that isn't there (HOME_URL has none).
# Variables already set in the environment win (used by test tooling).
keys="HOME_URL SEARCH_URL SCALE PAGE_MEMORY_LIMIT_MB POINTER_HIDE_SECONDS HISTORY_SIZE DOWNLOAD_DIR
      PLAY_YOUTUBE_IN_MPV USER_AGENT AD_BLOCK AD_BLOCK_EXCEPTIONS WEBGL START_PAGE KEYBOARD_LAYOUTS
      HW_VIDEO_DECODE"
for key in $keys; do
    eval "caller_$key=\${WPE_TSP_$key-}"
done
caller_gst_rank=${GST_PLUGIN_FEATURE_RANK:-}
# Read settings.conf: at start, and again when the browser restarts itself to apply settings
load_settings() {
    for key in $keys; do
        unset "$key"
    done
    [ -f "$progdir/settings.conf" ] && . "$progdir/settings.conf"
    for key in $keys; do
        eval "value=\${$key-} caller=\${caller_$key}"
        if [ -n "$caller" ]; then
            export "WPE_TSP_$key=$caller"
        elif [ -n "$value" ]; then
            export "WPE_TSP_$key=$value"
        else
            unset "WPE_TSP_$key"
        fi
    done
    # H.264 on the Allwinner Cedar hardware decoder (FFmpeg's h264_cedar, which falls back to
    # software by itself when the hardware can't be used): ranked above the software avdec_h264.
    # HW_VIDEO_DECODE=0 hides it.
    if [ "${WPE_TSP_HW_VIDEO_DECODE:-1}" = 0 ]; then
        cedar_rank=avdec_h264_cedar:NONE
    else
        cedar_rank=avdec_h264_cedar:257 # PRIMARY (256) + 1
    fi
    export GST_PLUGIN_FEATURE_RANK="$cedar_rank,$caller_gst_rank"
}
load_settings

# Model: TrimUI's MainUI names it ("Trimui Smart Pro", "Trimui Brick", "Trimui Brick Pro"); the
# browser picks its default page scale from it, and on the Brick (no sticks) the d-pad moves the
# pointer. Without the string the browser guesses from the screen size.
if [ -z "${WPE_TSP_DEVICE:-}" ]; then
    case "$(strings /usr/trimui/bin/MainUI 2>/dev/null | grep -m 1 '^Trimui')" in
        "Trimui Brick Pro") export WPE_TSP_DEVICE=brickpro ;;
        "Trimui Brick") export WPE_TSP_DEVICE=brick ;;
        "Trimui Smart Pro") export WPE_TSP_DEVICE=smartpro ;;
    esac
fi

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
# (no gst-plugin-scanner helper) and cached with the HTTP cache. The FFmpeg build has no AV1
# decoder (no hardware support, too slow in software), so sites pick H.264 or VP9.
export GST_PLUGIN_SYSTEM_PATH="$progdir/lib/gstreamer-1.0"
export GST_PLUGIN_PATH=
export GST_REGISTRY_FORK=no
# The registry is only rescanned when a plugin file changes, not when the FFmpeg gst-libav loads does
# (it lists FFmpeg's decoders, e.g. the Cedar one): one registry per libavcodec build
avcodec_id=$(stat -c %Y-%s "$progdir/lib/libavcodec.so.59" 2>/dev/null)
export GST_REGISTRY="$XDG_CACHE_HOME/gstreamer-registry-$avcodec_id.bin"
for old in "$XDG_CACHE_HOME"/gstreamer-registry*.bin; do
    [ "$old" != "$GST_REGISTRY" ] && rm -f "$old"
done
# (GST_PLUGIN_FEATURE_RANK, the Cedar decoder's rank: load_settings above)

# Rendering tuning for the PowerVR GE8300: 4x MSAA is costly on this GPU, Skia's
# analytic anti-aliasing looks the same for page content.
export WEBKIT_SKIA_MSAA_SAMPLE_COUNT=${WEBKIT_SKIA_MSAA_SAMPLE_COUNT:-0}
# Layers composited by TextureMapper instead of Skia (tiles are still painted by Skia): Skia
# draws a 720p video frame in ~10 ms of GPU time (TextureMapper: ~4 ms), which held 720p60 at
# ~44 fps; with TextureMapper it shows ~58. Page scrolling is the same (59 fps either way).
export WEBKIT_USE_SKIA_FOR_COMPOSITION=${WEBKIT_USE_SKIA_FOR_COMPOSITION:-0}
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

: > "$progdir/wpe-tsp.log"
# Exit code 75: the Settings page restarts the browser to apply settings read at start. It reopens
# the page the settings were opened from.
while :; do
    ./bin/wpe-tsp "$@" >> "$progdir/wpe-tsp.log" 2>&1
    [ $? = 75 ] || break
    uri=$(cat /tmp/wpe-tsp-restart-uri 2>/dev/null)
    rm -f /tmp/wpe-tsp-restart-uri
    if [ -n "$uri" ]; then set -- "$uri"; else set --; fi
    load_settings
done

echo wpe-browser > /sys/power/wake_unlock 2>/dev/null

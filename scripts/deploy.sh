#!/bin/sh
# Copy dist/WPE to the device, sending only files whose checksum differs (one tar stream over
# ssh; the device has no rsync). Stops the browser first if anything changed, since running
# binaries can't be overwritten. settings.conf and bin/yt-dlp are only installed if the device
# has none, so user settings and yt-dlp updates made by the browser survive deploys.
#
# Usage: scripts/deploy.sh [--delete] [--dry-run]
#   --delete   also remove files on the device that are not in dist/WPE (settings, logs kept)
#   --dry-run  only list what would change
set -e
. "$(dirname "$0")/device.sh"
cd "$(dirname "$0")/.."

SRC=dist/WPE
DEST=/mnt/SDCARD/Apps/WPE
DELETE=0; DRY=0
for arg in "$@"; do
    case "$arg" in
        --delete) DELETE=1 ;;
        --dry-run) DRY=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done
[ -x "$SRC/bin/wpe-tsp" ] || { echo "$SRC is missing or incomplete (failed build?): run scripts/package.sh" >&2; exit 1; }

# Files kept on the device as they are (user settings, generated at launch, logs, yt-dlp and
# its updater's files: .version, .checked, .bak)
KEEP='^\./(settings\.conf|fonts\.conf|wpe-tsp\.log|bin/yt-dlp(\.[a-z]+)?)$'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

(cd "$SRC" && find . -type f | sort | xargs md5sum) | grep -Ev "  ${KEEP#^}" > "$tmp/local" || true
# An unreachable device must not look like an empty one (that would resend everything,
# including settings.conf): stop if the listing fails.
if ! ssh -o ConnectTimeout=10 "$DEVICE" "mkdir -p $DEST && cd $DEST && find . -type f | sort | xargs -r md5sum" \
        2>/dev/null > "$tmp/remote"; then
    echo "Can't reach $DEVICE (or list $DEST) — nothing was sent." >&2
    exit 1
fi

# Changed or new: lines of the local manifest that aren't in the remote one
grep -vxFf "$tmp/remote" "$tmp/local" | sed 's/^[0-9a-f]*  //' > "$tmp/changed" || true
# settings.conf only if the device doesn't have one yet
grep -q '  \./settings\.conf$' "$tmp/remote" || echo "./settings.conf" >> "$tmp/changed"
# yt-dlp only if the device doesn't have one (the browser keeps it up to date)
grep -q '  \./bin/yt-dlp$' "$tmp/remote" || echo "./bin/yt-dlp" >> "$tmp/changed"
# On the device but not in the package
sed 's/^[0-9a-f]*  //' "$tmp/local" > "$tmp/local.names"
sed 's/^[0-9a-f]*  //' "$tmp/remote" | grep -Ev "$KEEP" | grep -vxFf "$tmp/local.names" > "$tmp/extra" || true

n_changed=$(grep -c . "$tmp/changed" || true)
n_extra=$(grep -c . "$tmp/extra" || true)
size=$(cd "$SRC" && cat "$tmp/changed" | xargs -r du -cb 2>/dev/null | tail -1 | cut -f1)
echo "$DEVICE: $n_changed file(s) to send (${size:-0} bytes), $n_extra file(s) only on the device"
sed 's/^/  + /' "$tmp/changed"
[ "$DELETE" = 1 ] && sed 's/^/  - /' "$tmp/extra"
[ "$DRY" = 1 ] && exit 0
[ "$n_changed" = 0 ] && { [ "$DELETE" = 0 ] || [ "$n_extra" = 0 ]; } && { echo "Up to date."; exit 0; }

ssh "$DEVICE" 'killall wpe-tsp WPEWebProcess WPENetworkProcess 2>/dev/null; sleep 1; true' 2>/dev/null
if [ "$n_changed" != 0 ]; then
    tar cf - -C "$SRC" -T "$tmp/changed" | ssh "$DEVICE" "cd $DEST && tar xf -"
fi
if [ "$DELETE" = 1 ] && [ "$n_extra" != 0 ]; then
    (cd "$tmp" && sed "s|^\./|$DEST/|" extra) | ssh "$DEVICE" 'xargs rm -f'
    # Drop directories left empty (on exFAT each one still occupies a cluster)
    ssh "$DEVICE" "find $DEST -depth -mindepth 1 -type d -empty -exec rmdir {} \\;" 2>/dev/null || true
fi
# New settings: append blocks (comments + KEY=default) for keys the device's settings.conf
# doesn't have yet; existing values are never touched.
remote_keys=$(ssh "$DEVICE" "grep -o '^[A-Z_]*=' $DEST/settings.conf 2>/dev/null" | tr -d '=')
awk -v have=" $(echo $remote_keys) " '
    /^#/ || /^$/ { block = block $0 "\n"; next }
    /^[A-Z_]+=/ { key = $0; sub(/=.*/, "", key)
                  if (index(have, " " key " ") == 0) printf "%s%s\n", block, $0
                  block = ""; next }
' "$SRC/settings.conf" > "$tmp/new-settings"
if [ -s "$tmp/new-settings" ] && [ -n "$remote_keys" ]; then
    echo "Adding new settings to the device's settings.conf:"
    grep -o '^[A-Z_]*=.*' "$tmp/new-settings" | sed 's/^/  /'
    ssh "$DEVICE" "cat >> $DEST/settings.conf" < "$tmp/new-settings"
fi
echo "Done."

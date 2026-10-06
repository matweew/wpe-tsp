#!/bin/bash
# Send keyboard and mouse input to the browser on the device: a virtual USB keyboard + mouse
# (tools/fakehid.c, built static for aarch64 into build/fakehid on first use and copied to the
# device's /tmp when it changed). Steps are documented in tools/fakehid.c, e.g.
#   scripts/device-input.sh C:38 't:wikipedia.org' k:28      Ctrl+L, type, Enter
#   scripts/device-input.sh m:-300,-200 c w:-5                move the mouse, click, scroll down
# The device appears, the steps run, and it's unplugged again (~3 s plus the steps).
set -e
. "$(dirname "$0")/device.sh"
cd "$(dirname "$0")/.."

BIN=build/fakehid
if [ ! -x "$BIN" ] || [ tools/fakehid.c -nt "$BIN" ]; then
    mkdir -p build
    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work -w /work wpe-tsp-builder \
        aarch64-linux-gnu-gcc-12 -O2 -Wall -static -o "$BIN" tools/fakehid.c
fi
local_sum=$(md5sum < "$BIN" | cut -d' ' -f1)
remote_sum=$(ssh "$DEVICE" 'md5sum < /tmp/fakehid 2>/dev/null' | cut -d' ' -f1)
[ "$local_sum" = "$remote_sum" ] || ssh "$DEVICE" 'cat > /tmp/fakehid && chmod +x /tmp/fakehid' < "$BIN"

# Quote each step for the device's shell (steps may contain spaces: 't:hello world')
args=""
for step in "$@"; do
    args="$args '$(printf %s "$step" | sed "s/'/'\\\\''/g")'"
done
ssh "$DEVICE" "/tmp/fakehid$args"

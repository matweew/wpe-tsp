#!/bin/bash
# Launch the browser on the device the way MainUI does: queue it in /tmp/cmd_to_run.sh
# (run by /usr/trimui/bin/runtrimui.sh) and close MainUI, which is restarted afterwards.
# If the browser is already running it is closed first (the launcher deletes the command file
# when an app exits, so we wait for MainUI to be back before queueing the new one).
# Usage: scripts/run-on-device.sh [NAME=value ...] [url]
#   NAME=value: environment for this run, e.g. WPE_TSP_START_PAGE=address, WPE_TSP_CONSOLE=1
#   (any setting as WPE_TSP_<NAME> overrides settings.conf); url: opened first (also data: URLs)
# Device from .device; DEVICE=root@<ip> overrides.
. "$(dirname "$0")/device.sh"

quote() { printf "'%s'" "$(printf %s "$1" | sed "s/'/'\\\\''/g")"; }
env_args="" url=""
for arg in "$@"; do
    case "$arg" in
        [A-Z_]*=*) env_args="$env_args $(quote "$arg")" ;;
        *) url=$(quote "$arg") ;;
    esac
done
cmd="cd /mnt/SDCARD/Apps/WPE && env$env_args ./launch.sh${url:+ $url}"

# The command goes over stdin, so long URLs and any characters in it survive unchanged
printf '%s\n' "$cmd" | ssh "$DEVICE" '
    cat > /tmp/wpe-tsp-cmd
    if pidof wpe-tsp >/dev/null; then
        killall wpe-tsp
        for i in $(seq 1 30); do pidof MainUI >/dev/null && break; sleep 0.5; done
        sleep 1
    fi
    mv /tmp/wpe-tsp-cmd /tmp/cmd_to_run.sh && killall -9 MainUI'

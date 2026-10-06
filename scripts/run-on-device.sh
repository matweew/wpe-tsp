#!/bin/sh
# Launch the browser on the device the way MainUI does: queue it in /tmp/cmd_to_run.sh
# (run by /usr/trimui/bin/runtrimui.sh) and close MainUI, which is restarted afterwards.
# If the browser is already running it is closed first (the launcher deletes the command file
# when an app exits, so we wait for MainUI to be back before queueing the new one).
# Usage: scripts/run-on-device.sh [url]     (device from .device; DEVICE=root@<ip> overrides)
. "$(dirname "$0")/device.sh"
URL=${1:-}
ssh "$DEVICE" "
    if pidof wpe-tsp >/dev/null; then
        killall wpe-tsp
        for i in \$(seq 1 30); do pidof MainUI >/dev/null && break; sleep 0.5; done
        sleep 1
    fi
    echo 'cd /mnt/SDCARD/Apps/WPE && ./launch.sh $URL' > /tmp/cmd_to_run.sh && killall -9 MainUI"

# Sourced by the device scripts: sets DEVICE (ssh target) from $DEVICE or the .device file.
# Put "root@<device-ip>" in <repo>/.device once; override per command with DEVICE=root@<ip>.
if [ -z "$DEVICE" ]; then
    DEVICE=$(cat "$(dirname "$0")/../.device" 2>/dev/null)
fi
if [ -z "$DEVICE" ]; then
    echo "No device configured: echo root@<device-ip> > .device  (or set DEVICE=...)" >&2
    exit 1
fi

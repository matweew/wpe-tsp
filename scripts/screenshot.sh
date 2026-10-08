#!/bin/sh
# Grab the device framebuffer (visible page) into a PNG.  Usage: scripts/screenshot.sh out.png
. "$(dirname "$0")/device.sh"
OUT=${1:-screen.png}
# The panel size (1280x720 Smart Pro, 1024x768 Brick) from the framebuffer's mode, e.g. "U:1280x720p-63"
size=$(ssh "$DEVICE" 'cat /sys/class/graphics/fb0/modes' | head -1 | sed 's/^[^:]*://; s/p.*//')
W=${size%x*} H=${size#*x}
ssh "$DEVICE" "off=\$(cut -d, -f2 /sys/class/graphics/fb0/pan); dd if=/dev/fb0 bs=\$(cat /sys/class/graphics/fb0/stride) skip=\$off count=$H 2>/dev/null" > /tmp/wpe-fb.raw
python3 - "$OUT" "$W" "$H" <<'PY'
import struct, sys, zlib
w, h = int(sys.argv[2]), int(sys.argv[3])
d = open('/tmp/wpe-fb.raw', 'rb').read()
stride = len(d) // h  # bytes per framebuffer row (may be padded)
rows = bytearray()
for y in range(h):
    rows.append(0)
    line = d[y * stride:y * stride + w * 4]
    for i in range(0, len(line), 4):
        rows += bytes((line[i + 2], line[i + 1], line[i]))
def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(rows))) + chunk(b'IEND', b''))
PY

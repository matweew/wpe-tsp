#!/usr/bin/env python3
"""Compile every XKB layout and variant (rules evdev, model pc105) into one file for the browser's
physical and on-screen keyboards (KEYBOARD_LAYOUTS picks from it). One file instead of the XKB
tree (hundreds of small files: 128 KB each on the exFAT SD card), each keymap zlib-compressed on
its own, so the browser only inflates the layouts it uses.

Format: "XKBKEYMAPS 1\\n", then one "name<TAB>offset<TAB>size\\n" line per keymap ("us",
"ua(phonetic)"), an empty line, then the compressed keymaps (offsets from the end of the index).
Usage: xkb-keymaps.py <xkb-root> <out-file>"""
import os
import subprocess
import sys
import zlib
from concurrent.futures import ThreadPoolExecutor

XKB, OUT = sys.argv[1], sys.argv[2]
TOOLS = "/opt/xkbtools"
COMPILE = TOOLS + "/usr/libexec/xkbcommon/xkbcli-compile-keymap"
ENV = dict(os.environ, LD_LIBRARY_PATH=TOOLS + "/usr/lib/x86_64-linux-gnu")


def sections(lst):
    """layout names and (layout, variant) pairs from rules/evdev.lst"""
    layouts, variants, section = [], [], None
    for line in open(lst, encoding="utf-8"):
        if line.startswith("!"):
            section = line[1:].strip()
        elif line.strip() and section == "layout":
            layouts.append(line.split()[0])
        elif line.strip() and section == "variant":
            name, rest = line.split(None, 1)
            variants.append((rest.split(":")[0], name))
    return layouts, variants


def compile_one(job):
    layout, variant = job
    args = [COMPILE, "--include", XKB, "--rules", "evdev", "--model", "pc105", "--layout", layout]
    if variant:
        args += ["--variant", variant]
    r = subprocess.run(args, env=ENV, capture_output=True)
    name = f"{layout}({variant})" if variant else layout
    return name, r.stdout if r.returncode == 0 and r.stdout.startswith(b"xkb_keymap") else None


layouts, variants = sections(os.path.join(XKB, "rules", "evdev.lst"))
jobs = [(l, None) for l in layouts] + variants
with ThreadPoolExecutor(os.cpu_count()) as pool:
    results = [r for r in pool.map(compile_one, jobs) if r[1]]

index, blobs, offset = [], [], 0
for name, keymap in sorted(results):
    blob = zlib.compress(keymap, 9)
    index.append(f"{name}\t{offset}\t{len(blob)}\n")
    blobs.append(blob)
    offset += len(blob)
with open(OUT, "wb") as f:
    f.write(b"XKBKEYMAPS 1\n" + "".join(index).encode() + b"\n")
    for blob in blobs:
        f.write(blob)
print(f"xkb keymaps: {len(results)} of {len(jobs)} layouts/variants, {os.path.getsize(OUT) // 1024} KB")

#!/usr/bin/env python3
"""Lists the C files of Linux's nouveau driver that kbuild would build, from
its Kbuild files, for the configuration air/OS builds (x86_64; no ACPI, debugfs,
backlight, SVM, LEDs, platform driver or 32-bit compat).
    tools/nouveau-sources.py <linux tree> > haiku/build/sources-nouveau.txt"""
import os, re, sys

ENABLED = {"CONFIG_X86"}
root = sys.argv[1]
base = "drivers/gpu/drm/nouveau"
out = []

def parse(path):
    skip = []
    for raw in open(os.path.join(root, path)):
        line = raw.split("#")[0].strip()
        if not line:
            continue
        m = re.match(r"ifdef (\S+)", line)
        if m:
            skip.append(m.group(1) not in ENABLED)
            continue
        if line == "endif":
            skip.pop()
            continue
        if any(skip):
            continue
        m = re.match(r"include \$\(src\)/(\S+)", line)
        if m:
            parse(os.path.join(base, m.group(1)))
            continue
        m = re.match(r"(\w+)-(y|\$\((\w+)\))\s*\+?:?=\s*(.*)", line)
        if not m:
            continue
        if m.group(3) and m.group(3) not in ENABLED:
            continue
        # the add-on is nouveau.o; ch7006.o and sil164.o are modules of their own
        if m.group(1) not in ("nouveau", "nvkm", "nvif"):
            continue
        for word in m.group(4).split():
            if word.startswith("$("):
                continue
            if word.endswith(".o"):
                out.append(os.path.join(base, word[:-2] + ".c"))

parse(os.path.join(base, "Kbuild"))
seen = set()
for f in out:
    if f not in seen and os.path.exists(os.path.join(root, f)):
        seen.add(f)
        print(f)
    elif f not in seen:
        print("missing: " + f, file=sys.stderr)

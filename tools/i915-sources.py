#!/usr/bin/env python3
"""Lists the C files of Linux's i915 driver that kbuild would build, from its
Makefile, for the configuration air/OS builds (x86_64 with ACPI; no debugfs,
perf events, hwmon, fbdev emulation, 32-bit compat, PXP, GVT, DP tunnels,
error capture or self tests).
    tools/i915-sources.py <linux tree> > haiku/build/sources-i915.txt"""
import os, re, sys

ENABLED = {"CONFIG_X86", "CONFIG_ACPI"}
# Not built, haiku/i915/stubs.c or the headers' stubs stand in:
# - the ACPI glue of intel_acpi.c (CONFIG_ACPI is on for the OpRegion, which
#   holds the VBT)
# - the GMCH GGTT of GPUs before Sandy Bridge (AGP's intel-gtt)
# - the private tmpfs mount (gemfs; shmem objects need none here)
# - the GSC (discrete and Meteor Lake GPUs' security controller, an MEI
#   auxiliary device)
# - LPE audio (Valleyview and Cherryview HDMI audio, a platform device)
EXCLUDE = {"display/intel_acpi.o", "gt/intel_ggtt_gmch.o", "gem/i915_gemfs.o",
	"gt/intel_gsc.o", "display/intel_lpe_audio.o"}
root = sys.argv[1]
base = "drivers/gpu/drm/i915"
text = open(os.path.join(root, base, "Makefile")).read()
text = re.sub(r"\\\n", " ", text)
lists = {}
order = []
for raw in text.splitlines():
    line = raw.split("#")[0].strip()
    m = re.match(r"([a-z0-9_]+)-(y|\$\((CONFIG_\w+)\))\s*\+?:?=\s*(.*)", line)
    if not m:
        continue
    name, cond, words = m.group(1), m.group(3), m.group(4).split()
    if cond and cond not in ENABLED:
        continue
    out = lists.setdefault(name, [])
    for w in words:
        v = re.fullmatch(r"\$\(([a-z0-9_]+)-y\)", w)
        if v:
            out.extend(lists.get(v.group(1), []))
        elif w.endswith(".o") and w not in EXCLUDE:
            out.append(w)
seen = set()
for o in lists["i915"]:
    f = os.path.join(base, o[:-2] + ".c")
    if f in seen:
        continue
    seen.add(f)
    if os.path.exists(os.path.join(root, f)):
        print(f)
    else:
        print("missing: " + f, file=sys.stderr)

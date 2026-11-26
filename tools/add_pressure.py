#!/usr/bin/env python3
"""add_pressure.py - 1.4: adds "pad pressure opens the filter" (aftertouch -> filter 1 cutoff, 0.35) to the factory
patches in Content/Patches, in each patch's first free modulation slot, leaving everything else as it is.
tools/make_factory.cpp has the same routing in its house style, for patches made from now on. Running it twice
changes nothing. Part of Warpline. GPLv3 (see LICENSE).
"""
import glob
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AMOUNT = 0.3499999940395355    # 0.35f, as make_factory writes it (0.18 in the first 1.4 build: too subtle)
LAST_SLOT = 48                 # slots 49-64 belong to Warpline's mod matrix

changed = 0
files = sorted(glob.glob(os.path.join(ROOT, "Content", "Patches", "*", "*.vital")))
for path in files:
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    s = data["settings"]
    mods = s["modulations"]
    have = [i for i, m in enumerate(mods) if m.get("source") == "aftertouch" and m.get("destination") == "filter_1_cutoff"]
    if have:
        key = "modulation_%d_amount" % (have[0] + 1)
        if s.get(key) == AMOUNT:
            continue
        s[key] = AMOUNT   # an earlier amount: update it
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(json.dumps(data, separators=(",", ":"), sort_keys=True, ensure_ascii=False))
        changed += 1
        continue
    free = next(i for i, m in enumerate(mods[:LAST_SLOT]) if not m.get("source"))
    mods[free] = {"destination": "filter_1_cutoff", "source": "aftertouch"}
    n = free + 1
    s["modulation_%d_amount" % n] = AMOUNT
    for k in ("bipolar", "bypass", "power", "stereo"):
        s["modulation_%d_%s" % (n, k)] = 0.0
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(json.dumps(data, separators=(",", ":"), sort_keys=True, ensure_ascii=False))
    changed += 1
print("%d of %d patches given pad pressure" % (changed, len(files)))

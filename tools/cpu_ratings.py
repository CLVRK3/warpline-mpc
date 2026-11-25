#!/usr/bin/env python3
"""cpu_ratings.py - rates every factory patch Light / Medium / Heavy for the browser (Warpline 1.4), into Content/Cpu.txt.

Runs tools/bench_patches (build it first, see its header) over Content/Patches a few times, keeps each patch's fastest
run, and rates it against the median of all of them, so the result does not depend on the machine:
Light <= 0.8 x median, Heavy >= 1.4 x median, Medium between. Bench settings: 8 voices, unison 8, Eco (as shipped).

    python3 tools/cpu_ratings.py ./bench_patches [runs]
Part of Warpline. GPLv3 (see LICENSE).
"""
import os
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCHES = os.path.join(ROOT, "Content", "Patches")
OUT = os.path.join(ROOT, "Content", "Cpu.txt")
LIGHT, HEAVY = 0.8, 1.4


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bench = sys.argv[1]
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    files = sorted(os.path.join(d, f) for d, _, fs in os.walk(PATCHES) for f in fs if f.endswith(".vital"))
    best = {}
    for r in range(runs):
        out = subprocess.run([bench, "8", "8"] + files, capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            pct, name = line.split("%", 1)
            name = name.strip()
            best[name] = min(best.get(name, 1e9), float(pct))
        print("run %d: %d patches" % (r + 1, len(best)))
    if len(best) != len(files):
        sys.exit("bench measured %d of %d patches" % (len(best), len(files)))
    median = statistics.median(best.values())
    counts = {"L": 0, "M": 0, "H": 0}
    lines = ["# Warpline factory patch CPU ratings (tools/cpu_ratings.py): L light, M medium, H heavy.",
             "# Relative to the median patch (%.2f%% of a desktop core here): L <= %.1fx, H >= %.1fx." % (median, LIGHT, HEAVY)]
    for name in sorted(best, key=str.lower):
        ratio = best[name] / median
        rating = "L" if ratio <= LIGHT else ("H" if ratio >= HEAVY else "M")
        counts[rating] += 1
        lines.append("%s.vital|%s" % (name, rating))
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("median %.2f%%: %d light, %d medium, %d heavy -> %s" % (median, counts["L"], counts["M"], counts["H"], OUT))


if __name__ == "__main__":
    main()

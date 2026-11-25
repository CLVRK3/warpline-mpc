# Rebuilding the factory patches

`make_factory.cpp` builds every factory patch with the Vital engine itself, loudness-matches it, and writes a `.vital` file.
It is a desktop tool (not part of the plugin).

1. Get the CC0 wavetables: `git clone https://github.com/atsushieno/open-vital-resources`
2. From the plugin folder (`plugins/Warpline`), build with any C++17 compiler. Linux/macOS example (paths from `build-extra.json`):

```
INC=$(python3 -c "import json;print(' '.join('-I'+d for d in json.load(open('build-extra.json'))['includeDirs']))")
c++ -std=c++17 -O2 $INC -Isrc tools/make_factory.cpp src/VitalCore.cpp src/VitalEngine.cpp -lpthread -o make_factory
./make_factory ../../open-vital-resources Content/Patches
```

Edit the `defs` list in `make_factory.cpp` to change or add patches. Control names are Vital's own (`filter_1_cutoff`, ...),
and modulation sources and destinations use Vital's names too (`env_2`, `lfo_1`, `macro_control_1`, `velocity`, ...).

Note (1.4): some shipped patches were tuned by hand after generation, so a full `make_factory` run is not a drop-in
replacement for `Content/Patches`: generate into another folder and copy only the patches you changed (`FACTORY_ONLY`
builds a subset). After changing patches, run `python3 tools/add_pressure.py` (pad pressure), `python3 tools/genres.py`
and `python3 tools/cpu_ratings.py ./bench_patches` (CPU marks; build `bench_patches.cpp` like make_factory).

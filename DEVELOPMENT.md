# Warpline: notes for further development

Read this first in a new chat. It covers what the code does and what was learnt on the MPC. `README.md` is for users;
`RELEASING.md` covers releases.

The plugin was called Vitalium during development and was renamed Warpline before the first release (the name clashed
with DISTRHO's Vital fork, and "Vital" is a trademark). The uid `CvVm` was kept, so projects saved with the old test
builds still load. Your own patch folder moved from `<drive>\Vitalium\Patches` to `<drive>\Warpline\Patches`.

## Layout

- `vital/`: Vital's engine source (synthesis + parts of common), GPLv3. Changes from upstream:
  - `tuning` has no file loading.
  - `load_save.h` is a minimal stand-in.
  - `load_save_update.cpp` is extracted from Vital.
  - The kissfft include in `fourier_transform.h` moved outside the namespace.
  - `kMaxPolyphony` is 17 (was 33), to save memory.
  - `poly_values.h` defines `NEON_ARM32` on 32-bit ARM.
- `shim/JuceHeader.h`: the few JUCE pieces the engine needs (no JUCE dependency).
- `src/VitalEngine.cpp`: unity build of the engine files.
- `src/VitalCore.*`: engine wrapper. It handles patch loading (JSON, version upgrade, modulations, wavetables, LFOs),
  rendering in chunks, unison/voice/oversampling limits, warm-up, saving, and `liveNormalised()` (modulated value for Live View).
- `src/Warpline.cpp`: the MPC plugin.
  - Parameters, and the patch scan with two-letter type codes.
  - A worker thread for patch loads, which stops audio briefly during each load.
  - State saving, Live View, and the scrollable patch list/browser.
- `tools/make_factory.cpp`: generates the 300 factory patches.
  - It uses CC0 wavetables from atsushieno/open-vital-resources.
  - It sets loudness (target -16 dBFS at velocity 0.8) and velocity sensitivity by type.
  - Run: `make_factory <open-vital-resources> Content/Patches` (takes about 5 minutes).
- `tools_build_skin.py`: writes `skins/` (the custom touchscreen design, 6 pages + browser overlay).
  - It needs `build/params.json` from a native build.
  - When `skins/` exists the SDK copies it as-is; `ui.json` is not used.
- `tools_preview_skin.py <state.json> <outdir>`: renders PNG previews of every page.

## Rules that must not break

- Never change the uid `CvVm`, the .so names or parameter ids. Only add new parameters at the END of the list (older ones
  keep their order). Projects store parameters by position.
- State loading handles older projects:
  - Ones saved with 92 parameters (before Vel Sens) only restore the engine controls.
  - The saved patch file path wins over the stored index, because the list grows when patches are added.
- Parameter 0 (Patch) is nudged by the host on load; patch loads are debounced 80 ms.
- Nothing slow on the audio thread. Patch loads happen on the worker, with audio paused through a handshake.

## What the MPC does (learnt on hardware)

- The MPC's own double-tap / Enter menu shows plugin lists with NO text. Warpline therefore opens no MPC menus.
  - The PATCH/TYPE boxes use plain Q-Link focus (data wheel) instead.
  - The browser and the PATCH-page list are built from:
    - row parameters whose display text is a patch name (rows 1-12), tapped as momentary buttons;
    - `List Scroll` (1 = top), with a film-strip thumb and an invisible vertical slider;
    - `List Current`, which outlines the playing patch's row;
    - `Browser`, which shows or hides the overlay.
- After the list changes, the plugin calls `requestDisplayUpdate()` so the MPC re-reads all texts.
- In a skin component, the touch goes to the LAST child: buttons must sit above labels.
- Non-automatable display parameters can be pushed with `notifyHostParameterChanged` (used by Live View and the list).
  The MPC does not record those as automation.
- The compressor flattens velocity, so played instruments in the factory set have it off.

## Testing

- Native build: `python tools/build.py plugins/Warpline --target native --toolchain gcc` (on the PC, `BUILD.bat`
  builds both ARM targets with zig).
- Host checks: `.sdk-tools/host_sim <so> test --notes 60,64,67 --block 32|128|512`.
  - Expect PASS.
  - At block 512 the parameter sweep sometimes warns about denormals inside Vital's engine; that warning is harmless.
- Always try a build on the MPC before releasing: every page, the Q-Links, the browser, save and reload a project.

## User patches

- `<drive>\Warpline\Patches`, where `<drive>` is the parent of the folder holding the plugin: `VST2 Plugins` (hand install)
  or `Synths` (Open MPC installer / Plugin Manager, e.g. `/nvme/Synths` -> `/nvme/Warpline/Patches`). Also the
  WARPLINE_USER_PATCHES env var. The `Synths` case was added in 1.0.0 and still needs a hardware check.
- Scanned recursively; up to 6000 patches in total.
- Type code from the name's own label (BA, PD, SEQ, PLK...), then Vital's preset_style, then keywords.
- The plugin writes the full patch list into `Plugin Skins/TUI.json` on load (harmless leftover from the menu attempt).

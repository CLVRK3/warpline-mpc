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

## 1.1.0 additions

- All new parameters come after `Browser` (index 113), in this order:
  - favourites/search: Favourite, Fav State, Fav Filter, Search, 29 keys;
  - Panel, the three chip texts;
  - arp: 9 settings, 16 steps, 16 step taps, Arp Position;
  - 16 x mod source/dest/amount, 4 LFO shapes, 16 Vital LFO 5-8 controls;
  - 14 GRAINZ parameters;
  - List Touch, List Drag, List Drag Page.
  275 + 3 = 278 in total.
  `first_v11_index_` splits `parameterChanged`/`formatValue`.
- **Panels:** one choice parameter `Panel` (Closed, Arp, Granulator, Mod 1-8, Mod 9-16, LFOs).
  - Each overlay is shown with `IndexedEnabling/<k>/6/Parameter <Panel>`.
  - It starts with a see-through blocker button that sets Panel to its own value, like the browser's.
  - Opening a panel closes the browser and the other way round (in `parameterChanged`).
  - `tools/skin_check.py` reports every page control under a blocker as TOUCH. It doesn't model invalidating handles,
    so that is expected.
- **Arp** (`Arp.h`): runs on the audio thread and feeds Warpline's engine directly.
  - The render loop splits at MIDI events and at `samplesToNextEvent()`.
  - With the transport playing it resyncs to `ppqPos` (only when drift > 32 samples).
- **GRAINZ** (`Granulator.h`): after the engine, on the full block.
  - 1.5 s ring buffer, 40 grains, 6 allpasses for SPACE.
  - Grains that are pitched up are shortened, so they never pass the write head.
- **Mod matrix:**
  - Slot s is Vital modulation `48 + s` (0-based). Patches only get 0-47; a patch's own modulations at 48+ are dropped and counted in the status.
  - `VitalCore` keeps the routings across patch loads and Init.
  - Routing changes happen on the worker with audio paused. Amounts are set on the audio thread (`modulation_49_amount` ...).
  - **Gotcha:** Vital's `disableUnnecessaryModSources()` switches *every* LFO/envelope/random source off, and connecting switches a source on.
    After disconnecting, `setUserModulation` switches on again the source of every connection that is still there. Without that,
    changing a slot silenced the patch's own LFOs.
  - The matrix LFOs are Vital LFO 5-8. A patch's LFO 5-8 settings (4 controls, shape) are saved when a slot first uses one
    and given back when none does (`syncMatrixLocked`).
- **Search/favourites:**
  - `passesFilter()` checks type, query (lower case, without the type code) and favourites, under `filter_mutex_`.
  - Favourites are file stems ("Init" for Init), in `<drive>/Warpline/Favourites.txt` or the plugin folder (env `WARPLINE_FAVOURITES` for tests).
    The worker writes them as `.new`, then renames.
- **Drag scrolling (needs a device check):**
  - An invisible vertical slider over the rows (`List Drag`, `List Drag Page`), shown while `List Touch` = Drag.
  - Events less than 250 ms apart are one gesture: the list follows the value change. A gesture with a single event is a tap at
    that position and loads the row (worker, `checkListTap`).
  - This assumes MPC sends the absolute touch position on touch-down (the scroll bar's slider seems to). If not, switch to TAP.
  - The SDK's state loader restores every parameter by id *before* `loadExtraState`, which looks like a one-event gesture.
    `loadExtraState` therefore clears the gesture, or each project load would "tap" a row and change the patch
    (found offline, 2026-11-18).

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

- 1.1 feature test: `tools/test_v11.cpp` (build line at the top) is a small VST2 host. It drives the arp, GRAINZ, the matrix,
  search, favourites, drag and a state reload through `setParameter` and prints what it measures. Run it under ASan.

## User patches

- `<drive>\Warpline\Patches`, where `<drive>` is the parent of the folder holding the plugin: `VST2 Plugins` (hand install)
  or `Synths` (Open MPC installer / Plugin Manager, e.g. `/nvme/Synths` -> `/nvme/Warpline/Patches`). Also the
  WARPLINE_USER_PATCHES env var. The `Synths` case was added in 1.0.0 and still needs a hardware check.
- Scanned recursively; up to 6000 patches in total.
- Type code from the name's own label (BA, PD, SEQ, PLK...), then Vital's preset_style, then keywords.
- The plugin writes the full patch list into `Plugin Skins/TUI.json` on load (harmless leftover from the menu attempt).

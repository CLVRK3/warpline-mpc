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
  - (1.2) `SynthVoiceHandler` registers two idle mono `cr::Value` mod sources, `gen_1` and `gen_2`. `VitalCore::setGenValue` sets them.
- `shim/JuceHeader.h`: the few JUCE pieces the engine needs (no JUCE dependency).
- `src/VitalEngine.cpp`: unity build of the engine files.
- `src/VitalCore.*`: engine wrapper. It handles patch loading (JSON, version upgrade, modulations, wavetables, LFOs),
  rendering in chunks, unison/voice/oversampling limits, warm-up, saving, and `liveNormalised()` (modulated value for Live View).
- `src/Warpline.cpp`: the MPC plugin.
  - Parameters, and the patch scan with two-letter type codes.
  - A worker thread for patch loads, which stops audio briefly during each load.
  - State saving, Live View, and the scrollable patch list/browser.
- `tools/render_demo.cpp`: renders a WAV demo of .vital patches through VitalCore (chords, or an 808 line for Bass) to listen
  to factory patches without a device. Build line in its header.
- `tools/bench_patches.cpp`: ranks .vital patches by engine CPU (relative, x86) to find patches too heavy for Gen 1. Build line in its header.
- `tools/cpu_ratings.py`: runs bench_patches over the factory set and writes `Content/Cpu.txt` (the browser's CPU marks). Rerun after changing patches.
- `tools/add_pressure.py`: the 1.4 pad-pressure routing for factory patches (safe to rerun).
- `tools/state_dump.cpp`: a state.json for the skin previews, from the running plugin.
- `tools/genres.py`: writes `Content/Genres.txt`, the factory patches' genres. Run it after adding patches.
- `tools/make_factory.cpp`: generates the 519 factory patches (a table named "@n" is Warpline's own built-in table n).
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
  - The list moves by the change between events. A pause over 250 ms, or a jump over 0.2 in one event, starts a new
    touch. Device, 2026-11-19 (Live III): measuring from the first event made quick repeated flicks snap back after
    about two, so the list seemed to stop scrolling.
  - The strip's value follows the finger's movement from where it was, not the finger's position (Live III, 2026-11-19:
    with only the delta fix it still stopped after about two drags, once the value reached the top or bottom). So
    after every touch (no event for 120 ms) the plugin puts both strips back at 0.5 (`recentreDragStrips`, guarded
    by `recentring_`). A single event at 0.5 doesn't count as a tap.
  - The strip doesn't say where the finger is, so taps can't come from it (Live III, 2026-11-19: with one strip over
    the rows, tapping a row did nothing). In DRAG mode each row is now its own component (`comp_row_drag`):
    - Touch-down presses the row (Mouse Down -> Toggle Switch on `Data` = List n).
    - A see-through vertical film-strip Knob on handle `Drag` (remapped to List Drag / List Drag Page) scrolls.
    - The plugin loads the row 220 ms after the press, if no drag event came in between (`pending_tap_patch_`,
      `checkListTap`). TAP mode shows the plain button rows, which load at once.
  - If tapping a row loads the wrong patch on some MPC, switch to TAP.
- **Horizontal drags** (mod boxes and AMOUNT bars, GRAINZ columns, every `comp_box`): a see-through film-strip `Knob`
  with `dragOrientation: Horizontal`, as the last child of the component. A `Slider` (ColourBar, horizontal) didn't
  drag on a Live III (2026-11-19). The vertical ColourBar scroll bars do work.
  - The SDK's state loader restores every parameter by id *before* `loadExtraState`, which looks like a one-event gesture.
    `loadExtraState` therefore clears the gesture, or each project load would "tap" a row and change the patch
    (found offline, 2026-11-18).

## GRAINZ texture modes (2026-11-19, offline)
- **Parameters:** `Grain Mode` (Grainz, Particle, Stretch, Loop, Spectral) and `Grain Texture`, appended after `List Drag Page`.
  Mode 0 is our `Granulator`. Modes 1-4 are `GrainzClouds`, built on the vendored processor in `third_party/mutable`.
  `GrainzClouds.cpp` is its only translation unit (it defines `TEST` for the portable code paths); `build-extra.json` adds
  `third_party/mutable` to the include path.
- **Rates:** the processor runs at 32 kHz on 32-frame blocks.
  - Host to 32 kHz: a 4th-order Butterworth low-pass at 13.5 kHz, then Hermite interpolation.
  - The output comes back the same way, through a 48-frame primed FIFO.
  - Only the wet signal comes out. Warpline mixes dry and wet equal-power at the host rate.
- **Mapping:** size = (GRAIN − 10) / 490, density = SCATTER, reverb = SPACE, position = POSITION / 1000, pitch, spread, feedback,
  freeze, texture. SYNC sends a trigger at each grid line (block accuracy).
- **Upstream bug fixed:** `Window::Start()` never cleared `done_`, so STRETCH was silent. Listed in `third_party/mutable/README.md`.
- **CPU, x86 native, 256-frame blocks at 44.1 kHz, the whole plugin with one note:** dry 22 µs; GRAINZ 69; PARTICLE 79; STRETCH 98;
  LOOP 89; SPECTRAL 141 µs.
- **Device check, 2026-11-20, MPC Live III, MPC OS 3.10.1:** all five GRAINZ modes work. With chords playing, the MPC's CPU
  meter showed about 20-25%, close to where Warpline already sat without GRAINZ. The Windows native build needs
  `_USE_MATH_DEFINES` (MSVC) for the processor's `M_PI`.

## 1.2.0 additions (2026-11-20, offline)

- **New parameters**, after `Grain Texture`: Osc1-3 Table; Arp Chance, Arp Deja Vu, Arp Loop; Gen Rate, Gen Spread, Gen Bias,
  Gen Steps, Gen Deja Vu, Gen Length; List User 1-12, Genre and Gen 1 Out, Gen 2 Out (screen only). 307 in total. `kModSources` gains Gen 1, Gen 2
  (16, 17, bipolar) and `kPanels` gains Gen (6), both at the end: the SDK's state loader keeps the plain value when a range
  grows, so older projects keep their choices.
- **Wavetable picker:** `VitalCore::setWavetable(osc, table)` builds the table with `WavetableCreator` (one `WaveSource` group
  of time-domain frames; Basic Shapes = `initPredefinedWaves()`), or restores the patch's own, which every patch load and Init
  stores as JSON first (`rememberPatchTables`). Changes go to the worker with audio paused (`tables_dirty_`). `loadPatchIndex`
  resets the picks (`resetTablesFor`: Patch, or Basic Shapes on OSC 1 for Init) before loading; `loadIntoCore` applies the
  current picks, so a project restore gets its saved ones. Projects saved before 1.2 have fewer parameters, so `loadExtraState`
  puts their tables back to Patch. A new instance sets OSC 1 to Basic Shapes.
- **Arp generate:** `Arp::roll()` gives each played step a gate die and a note die (Random mode). Deja vu below 0.5 = chance of
  replaying the die from LOOP steps ago; at or above 0.5 always replays, and above it may jump to another place in the loop.
- **Gen** (`Gen.h`): two outputs, ticked by the rate (transport position while playing, free otherwise). It runs only when a
  slot uses Gen 1/2 (`gen_in_use_`), then the render loop splits into 64-frame chunks and sets the values before each.
  Values go through SPREAD (narrow/even/extremes) and BIAS (a power curve), and glide over (1 − STEPS) of a tick.
  Saved .vital files leave out connections from `gen_*`.
- **Tables menu, USER tags (2026-11-22, offline):** the Table box opens `Panel` 7-9 ("Osc1-3 Tables": ten tiles bound to
  OscN Table). A pick while that panel is open closes it (`close_tables_menu_`, done by the worker). `Fav Filter` gained
  "User" (index >= `factory_count_`). The screen params List User 1-12 (Factory/User) are kept in step with the rows by
  `updateUserMarks()` in the worker; the skin shows a USER chip per row with IndexedEnabling. 60 new factory patches
  (`addMore4`), 360 in all.
- **Genres (2026-11-22, offline):** `Content/Genres.txt` (made by `tools/genres.py`, which fails if a factory patch has no
  genre) and `<drive>/Warpline/Genres.txt` give each `PatchEntry` a genre bit mask; `WARPLINE_USER_GENRES` points the test
  host at another file. The screen param Genre (after List User 1-12) filters in `passesFilter`; `Panel` 10 is the GENRE menu,
  closed by a pick. List User N only changes when its row's patch changes (`user_marks_`), so a host's state round trip keeps
  what it set.
- **Skin:** a Table box in each OSC header (MAIN, OSC pages). The wavetable display has one copy per table
  (`IndexedEnabling/<t>/10/Parameter <OscN Table>`); single-wave tables use a 1-frame picture and the morphing ones 16 frames, to keep
  image memory down (all images decoded: about 288 MB with the tables and genre menu tiles; 1.1 was about 203 MB). The MOD panel has a fourth tab, GEN, with two meters
  and the shared controls. The ARP panel's hint box became the GENERATE knobs.

## 1.3.0 (2026-11-24, offline)

- Rage set reworked in `addMore4` (`kRageBus`: compressor on, fully wet, band +12 / high +12 dB; `kRageSpread`; `kRagePoly`;
  `kSub`/`kSubLight`: OSC 3 sine at -24 straight to Direct Out; `kCoins`: the sample source's white noise into Filter 2 as a
  comb at high resonance, its level and cutoff stepped by Random 1 and 2 at 1/32) plus 34 new patches. The loudness match
  measures 808s, Slime Reese Bass and the addMore5 basses (`g_low_names`) on a low note (A1). Listened to as `render_demo` renders by the user before shipping.
- `addMore5`: 125 UK garage, DnB and jungle patches, written by a generator script from family templates (kept in the
  patches, not the repo). Genre gains "UK Garage" and "DnB / Jungle" (appended to `kGenres`); the GENRE menu is 4 x 3 tiles.
- CPU, after a Gen 1 user report (about 10 % loaded and idle, 50-130 % playing, Gated Choir the worst):
  - Idle skip in `process()`: after `kIdleSeconds` (1 s) with the output below `kIdlePeak` (-100 dBFS), no active voices and
    the arp not running, `core_->render` is skipped (zeros out) until a channel MIDI message arrives or the arp runs. System
    messages (clock) do not wake it. `tools/test_v13.cpp` checks it: block time 10.4 → 1.7 µs idle on x86, the first note
    after idle has its attack, a latched arp stays awake.
  - The 32-bit build (`__arm__`, Gen 1) defaults Max Voices to 6 (`kDefaultMaxVoices`); saved projects keep theirs.
  - `tools/bench_patches.cpp` ranks patches by engine CPU (4-note chord, x86, relative only; `BENCH_OFF=reverb_on,...`
    switches controls off). Factory median 3.9 %, heaviest 11.6 % at 8 voices. Unison 1 vs 16 made no measurable
    difference; voices (1 → 4 doubles it) and FX (Clavinet Pop: 10 % → 3 % without them) do.
  - Gated Choir: patch polyphony 4 and amp release 0.7 (was 8 and 1.3), so its arp does not pile up voices.
- Skin: page controls below the header get a `Panel == Closed` invalidating handle (`Page.add`), so they hide while a panel
  is open and a focused control's orange Focus outline no longer draws over the panel (reported on a device; fix checked
  only in the offline preview so far).

## 1.4.0 (2026-11-25, offline)

- Chord mode (`Chord`, `Chord Voicing`, appended): `handleMidi` turns a played key into `chordNotes(root, chord, voicing)`
  before the arp. `chord_held_` remembers each key's notes (so a chord change while holding releases the right ones) and
  `chord_note_refs_` counts keys per note (overlapping chords share notes). All-notes-off and `reset()` clear both.
- Polyphonic pressure (`0xA0`) goes to the note (`VitalCore::polyPressure` -> `SoundEngine::setAftertouch`), and to every
  note of a chord. Factory patches: `tools/add_pressure.py` added aftertouch -> filter 1 cutoff (0.35; 0.18 at first was too
  subtle on a device) in each patch's first
  free slot (everything else unchanged); `make_factory`'s house style has the same routing. A full `make_factory` run is NOT
  a drop-in replacement for Content/Patches any more: 41 patches had been tuned by hand afterwards (volume, levels, drive).
- CPU ratings: `tools/cpu_ratings.py <bench_patches> [runs]` writes `Content/Cpu.txt` (`<file>|L/M/H`, relative to the median
  patch: L <= 0.8x, H >= 1.4x). `List CPU 1-12` (screen) drive a 1-3 bar meter on the list rows; `Fav Filter` gains `Light`.
- `Random Patch` (momentary): a random entry of `filteredPatches()` other than the current one.
- `Macro 1-4 Name` (screen, text only): `PatchReport::macro_names` from the patch's `macro1`..`macro4`; the skin's macro
  captions are value labels of these.
- Your own WAV wavetables: `src/WavTable.h` parses Serum-style WAVs (`clm ` chunk, else 2048-sample cycles, else one cycle up
  to 4096 samples); `VitalCore::setUserWavetable` resamples each cycle to 2048 and builds a linear-interpolated table.
  `Osc1-3 Table` now always has 10 + 64 choices (built-ins, then `<drive>/Warpline/Wavetables` files or "WAV n"
  placeholders), so the skin's `IndexedEnabling` counts and saved values don't depend on the files. Files are read on the
  worker before the audio pause (`cacheUserTables`), parsed once (`user_table_cache_`). `Osc1-3 WAV Prev/Next` step through them.
- Strum (`Chord Strum` 0-250 ms, `Strum Direction` Up/Down/Alt, appended): chord notes after the first wait in `pending_`
  and start from the render loop (`firePending`, which also splits the render at their sample). A key let go before its strum
  is done drops its pending notes; `chord_note_started_` keeps a note that never started from getting a note-off.
- `Pressure In` (screen, text): the last pressure value, published by the worker; the MACROS box on MAIN shows it.
- Engine thread (`Engine Thread`, on by default since the Live III test on 2026-11-26, which showed much lower MPC CPU): `process()` either renders inline (`renderBlock`) or queues the block
  (frames, MIDI, transport) for `engineLoop()` and copies audio rendered earlier from a ring, `kEngineAhead` (2) blocks
  behind. The engine thread pins itself with `fast_cores.h` (Gen 2: the A76s minus core 7; Gen 1: no pin) and asks for
  `SCHED_FIFO` 15, below MPC's audio threads. A late block plays silence for the missing frames and counts in
  `engine_underruns_` (logged to stderr every ~5 s when it rose: `Warpline: engine thread late N times`). `reset()` is now
  deferred to the next rendered block (`reset_request_`), so the audio thread never touches the engine while the engine
  thread owns it. Switching off drains the queue first.
  `tools/test_v14.cpp`: the threaded note onset is exactly 2 blocks later than inline (two Init instances differ sample by
  sample anyway: random phase), toggling while playing leaves no hanging notes; paced 2.9 ms blocks on a 2-core x86 box:
  audio-thread time per block 174 -> 133 µs mean. Needs a device test in MPC (EMULATOR_PORTS.md section 4).
- Not done: profile-guided optimisation. The SDK's `tools/build.py` fixes `-O2` and takes no extra compile flags, zig 0.17's
  ARM targets ship no profile runtime (an `-fprofile-instr-generate` .so links without one), and `-O3` measured no faster
  on x86 (2026-11-25).
- Patch name in the PATCH boxes (1.4, after a device report that the box stayed on Init and the data wheel sometimes picked
  the wrong patch): the boxes show `Patch Name` (screen, text = the patch `parameterChanged(kPatch)` asked for, set right
  there) instead of the skin's list, and `Patch` always has Init + factory + `kUserPatchSlots` (1000) choices, so its length
  equals the skin's list from the first start (a length mismatch made MPC's steps land on the wrong patch). Empty slots
  ("User n") snap back to the last patch.
- Skin patch list: the shipped TUI.json carries the factory names (`tools/patch_names.json`, made with
  `state_dump --choices Patch` on a native build) so the PATCH box shows names on the first start; at load the plugin rewrites
  the list with your own patches added (`writeSkinLists`), which MPC shows after its next restart. Before, the shipped list was
  only "Init", and a fresh install showed Init in the PATCH box until MPC restarted (reported 2026-11-26). Write failures go
  to stderr.
- `tools/state_dump.cpp`: plays the plugin like the tests, applies `Param=normalised` / `Param~choice text` / `note=` /
  `wait=` steps and prints a state.json for `tools_preview_skin.py` (how the 1.4 screenshots were made).

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

- 1.4 test: `tools/test_v14.cpp` (chord, pressure, RANDOM, LIGHT, macro names, WAV tables with `WARPLINE_USER_TABLES`,
  engine thread), run under ASan.
- 1.3 idle test: `tools/test_v13.cpp` (build line at the top), run under ASan.
- 1.1 feature test: `tools/test_v11.cpp` (build line at the top) is a small VST2 host. It drives the arp, GRAINZ, the matrix,
  search, favourites, drag and a state reload through `setParameter` and prints what it measures. Run it under ASan.

## User patches

- `<drive>\Warpline\Patches`, where `<drive>` is the parent of the folder holding the plugin: `VST2 Plugins` (hand install)
  or `Synths` (Open MPC installer / Plugin Manager, e.g. `/nvme/Synths` -> `/nvme/Warpline/Patches`). Also the
  WARPLINE_USER_PATCHES env var. The `Synths` case was added in 1.0.0 and still needs a hardware check.
- Scanned recursively; up to 1000 of your own (`kUserPatchSlots`, 1.4).
- Type code from the name's own label (BA, PD, SEQ, PLK...), then Vital's preset_style, then keywords.
- The plugin writes the full patch list into `Plugin Skins/TUI.json` on load (harmless leftover from the menu attempt).

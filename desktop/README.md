# Warpline desktop (beta)

Warpline as an **Audio Unit** and **VST3** for macOS (Apple Silicon and Intel), for testing the sound engine, the 519 factory
patches, the arp, CHORD and STRUM in a desktop DAW. It is the same engine as the MPC plugin, built for the Mac.

**Beta, for testing.** There is no plugin window: the MPC screen only exists on MPC hardware, so your DAW shows its own list of
Warpline's parameters instead (Logic: the plugin header's "Controls" view). Patches are picked with the **Patch** parameter.

## Install (macOS)

1. Unzip. Copy `Warpline.component` to `~/Library/Audio/Plug-Ins/Components` (Audio Unit: Logic, GarageBand, MainStage...)
   and/or `Warpline.vst3` to `~/Library/Audio/Plug-Ins/VST3` (Ableton Live, Reaper, Bitwig...).
2. The build is not notarised by Apple, so macOS blocks it at first. In Terminal:
   ```
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Warpline.component
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Warpline.vst3
   ```
3. Logic: restart it; if Warpline does not show, open Logic > Settings > Plug-in Manager and "Reset & Rescan Selection".
   It is under Instruments > AU Instruments > CLVRKE > Warpline.

## What to test

- Patches (the **Patch** parameter), notes, chords, the arp (Arp On, Mode, Rate), CHORD, Chord Voicing, Chord Strum.
- Saving and reopening a project (Warpline restores its patch and settings).
- CPU in the DAW, and any crashes or silence. Please report the Mac model, macOS version and DAW.

## Not in this beta

No plugin window, no `Warpline/Patches` folder for your own .vital patches, no own WAV wavetables (both look for the MPC's
drive folders), no installer, no notarisation.

## Building it yourself

`cmake -S desktop -B build/desktop -G Xcode && cmake --build build/desktop --config Release` (CMake 3.26+, Xcode; JUCE 8.0.15
is downloaded). The GitHub workflow `.github/workflows/desktop-macos.yml` does the same on a GitHub Mac, signs ad hoc, runs
Apple's `auval`, attaches the zip to the run and, when the repo has the secrets `GDRIVE_TOKEN` (from
`rclone authorize "drive"`) and `GDRIVE_FOLDER_ID`, copies it into that Google Drive folder. On Linux the same CMake project builds a VST3 (passed pluginval at strictness
5 on 2026-11-27, offline).

Desktop wrapper: `desktop/WarplineDesktop.cpp` (JUCE 8) loads the unchanged MPC plugin, built as
`Contents/Resources/VST/warpline.dylib`, and drives it through the VST2 calls MPC uses. `desktop/sdk/` holds the MIT-licensed
MPC plugin SDK headers it is built with.

## Licence

The desktop binaries combine Warpline (GPLv3) with JUCE 8, which is licensed under the AGPLv3, so they are distributed under
the AGPLv3; the full source is this repository. The MPC plugin does not use JUCE and stays GPLv3.

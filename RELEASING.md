# Releasing Warpline on Open MPC

Notes so any future session (or you) can ship a new version the same way.
Open MPC catalog: https://open-mpc.github.io/ (release rules: https://open-mpc.github.io/workflow.html)

## Fixed values: never change these

- Plugin uid: `CvVm` (plugin.json). Projects find the plugin by uid.
- .so names: `VST/warpline-arm32.so`, `VST/warpline-arm64.so`.
- Catalog id: `warpline` (once listed).
- License: GPL-3.0-or-later (Vital's engine is GPLv3). Keep `LICENSE` in the repo root.
- Do not ship anyone else's presets (your own collection in `<drive>\Warpline\Patches` stays private).
  The 394 factory patches are ours, made from CC0 wavetables, so they are fine to ship.

## Version numbers (X.Y.Z)

- Z: fixes only
- Y: new parameters or pages (add new parameters at the end of the list)
- X: parameter positions change (breaks saved projects, so avoid)

## Each release

1. Bump the version, then run `BUILD.bat` (makes `build\CLVRKE - VST - Warpline\` with both ARM builds).
2. Test on the MPC: load on a track, play, go through every page and Q-Link, save and reload a project.
3. Package with Open MPC's tools (repo `open-mpc/mpc-vst-plugins`):
   ```
   python3 tools/sdk_release.py "<path>/CLVRKE - VST - Warpline" --version X.Y.Z --id warpline --repo CLVRK3/warpline-mpc --license GPL-3.0-or-later -o dist
   python3 tools/catalog_check.py dist/Warpline-X.Y.Z-mpc-armv7.zip --catalog
   ```
4. Publish a GitHub release on the plugin repo, tag `vX.Y.Z`, with the zips from `dist` attached.
5. Optional: add a line to `tested.json` in the repo root, for example
   `{ "version": "X.Y.Z", "device": "MPC Live II", "firmware": "3.x", "date": "YYYY-MM-DD" }`.

The catalog picks up new releases by itself every night. A failed check opens an issue on the catalog repo.

## First listing (one time)

Open a pull request to `open-mpc/mpc-vst-plugins` adding `catalog/plugins/warpline.json`:

```json
{ "id": "warpline", "name": "Warpline", "author": "CLVRKE",
  "repo": "CLVRK3/warpline-mpc", "kind": "instrument", "license": "GPL-3.0-or-later",
  "summary": "Vital wavetable synth engine for standalone MPC: plays .vital patches, 300 factory patches with type filter, velocity, six touchscreen pages.",
  "style": "synth", "tags": ["wavetable", "vital"],
  "screenshot": "https://raw.githubusercontent.com/CLVRK3/warpline-mpc/<tag>/docs/screenshots/warpline.png" }
```

No versions or checksums go in that file. To pull a bad release, add `"warpline@X.Y.Z"` to `catalog/yanked.json` by pull request.

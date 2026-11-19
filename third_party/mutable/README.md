# Vendored: Emilie Gillet's granular processor and stmlib (MIT)

GRAINZ's texture modes (PARTICLE, STRETCH, LOOP, SPECTRAL) run this code. Warpline builds it in
`src/GrainzClouds.cpp` with `TEST` defined, which selects the portable code paths.

- `clouds/dsp/**`, `clouds/resources.*`, `clouds/drivers/debug_pin.h`: from https://github.com/pichenettes/eurorack
  at `f4cfa7697095a28d292bff758beb51deb49b5a86`.
- `stmlib/**`: from https://github.com/pichenettes/stmlib at `d18def816c51d1da0c108236928b2bbd25c17481` (only the files the processor uses).
- Licence: MIT, copyright Emilie Gillet (`LICENSE`, and the header of every file). This applies to this folder
  only. Warpline as a whole is GPL-3.0-or-later.
- "Mutable Instruments" is a registered trademark, and its module names aren't used for this plugin or its controls.

## Changes from upstream
- `clouds/dsp/window.h`: `Window::Start()` now clears `done_` and `half_`. Without that, `Init()`'s
  `done_ = true` was never cleared, so every window returned at once and the stretch mode stayed silent
  (found offline, 2026-11-19).
- `clouds/dsp/grain.h`: the window lookup clamps its position to just under 1.0. At exactly 1.0 it read one float
  past the table's end (AddressSanitizer, offline, 2026-11-19). For the same reason, `src/GrainzClouds.cpp` keeps
  every 0..1 control just under 1.0.
- `clouds/dsp/correlator.cc`: no shift by 32 when the bit offset is 0. That shift is undefined in C++. It gave 0 on the
  Cortex-M4 but not on x86 or AArch64, which skewed STRETCH's window matching (UBSan, offline, 2026-11-19).

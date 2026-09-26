# mpc-vst-monomodule

Porting [Monomodule](https://github.com/shnolk/monomodule) (chip-level Elektron Monomachine
emulation) to an Akai Force/MPC OS VST2 plugin, via
[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).

**Status: blocked on CPU, investigating a fix.** See `HANDOFF.md` for exactly where this stands and
`libs/dsp56300/docs/ARM32_JIT.md` for the technical plan. Short version: the Force is 32-bit ARM
(Cortex-A17), and dsp56300's JIT (the thing that makes Monomachine emulation fast enough for
realtime) only supports x86-64 and 64-bit ARM. The interpreter-only fallback is ~2-3x too slow. We
forked dsp56300 (`sd88me/dsp56300`, branch `arm32`) to attempt a 32-bit ARM JIT backend rather than
extend upstream (unsupported there) or write a new emulator from scratch.

## Layout

- `libs/dsp56300` — our fork of the DSP56300 emulator (submodule, `arm32` branch). Where the JIT
  work happens; useful to any dsp56300-based product on 32-bit ARM, not just this one.
- `libs/monomodule` — upstream Monomodule (submodule, unmodified). We use its engine core
  (`src/core/`) only; its JUCE plugins/app aren't built.
- Engine glue (once started): follows the pattern in
  [legsmechanical/schwung-monomodule](https://github.com/legsmechanical/schwung-monomodule)'s
  `src/engine/`.
- The plugin itself, once the JIT investigation clears its bail-out gates: a `vst/` directory per
  the `mpc-vst-plugins` `PORTING.md` checklist.

Not affiliated with Elektron. Monomodule is GPLv3-linked (dsp56300, GPLv3) / AGPLv3 (Monomodule
itself); this port inherits those terms.

# Handoff

Read this first in any new session picking up this port. Keep it current — update it at every
checkpoint, not just at the end.

## Where things live

- **This repo** (`sd88me/mpc-vst-monomodule`): the port itself once it exists (VST wrapper, skin,
  vst.json). Currently just scaffolding + submodules.
- **`libs/dsp56300`** = `sd88me/dsp56300` fork, branch `arm32`. Our 32-bit ARM JIT work happens
  here. See `libs/dsp56300/docs/ARM32_JIT.md` for the plan and stage checkpoints — that file is the
  source of truth for where we are in the JIT investigation, more current than this section.
- **`libs/monomodule`** = upstream `shnolk/monomodule`, unmodified, pinned. Its engine core
  (`src/core/`) is what we vendor into the port; its JUCE plugins/app are not built.
- Engine glue pattern to follow: `legsmechanical/schwung-monomodule`'s `src/engine/` (DspEngine.cpp,
  MonoVoice.cpp) shadows upstream's own — same approach here once we get that far.
- Session scratchpad (this machine only, not committed anywhere):
  `/tmp/claude-1000/-home-sam/9e196c7c-f6d4-4616-ad6a-e844f7e1e6e1/scratchpad/` — has the stage-0
  instrumented dsp56300 copy (`dsp56300-stats/`), the opcode-histogram script (`opstats.py`), the
  x86/armhf docker build images (`mnm-armhf-builder`, `mnm-x86-builder`), and raw stage-0 output
  (`opstats/dsp_*.txt`). Not durable — if it's gone, stage 0 is cheap to redo (a few hours, all on
  x86, see docs/ARM32_JIT.md's Stage 0 section for the method).
- Memory: `[[monomodule-force-feasibility]]` in the user's Claude memory has the full numbers.

## Status as of 2026-09-26

- Feasibility investigated: Monomodule's engine (via schwung-monomodule's glue) cross-compiles for
  the Force; interpreter-only load is 150-285% of one core (too slow to ship as-is).
- Decided to attempt a 32-bit ARM JIT backend for dsp56300 rather than abandon the port. Forked
  dsp56300 for this (`sd88me/dsp56300`, branch `arm32`) instead of extending upstream (they don't
  support 32-bit) or writing a new emulator (would lose the interpreter/opcode tables/peripherals
  that already work).
- Stage 0 (instruction-mix histogram) done and passed — see `libs/dsp56300/docs/ARM32_JIT.md`.
- **Stage 1 in progress** — make the interpreter bit-exact against the JIT (`mnm-golden` hash match
  on every machine). dsp56300#8 (out-of-range-read divergence) is fixed and pushed. Still open:
  10/22 machines still mismatch (FM+ STAT/PAR/DYN, GND SIN, SWAVE SAW/PULS, DPRO WAVE, REVERB,
  RINGMOD, PHASER). Narrowed to an exact, cheap, deterministic repro: dump the 8192-entry sine
  table the kernel builds at init (Y:$14A000, no audio pipeline needed) and diff interpreter vs.
  JIT — they agree exactly through index 0x1800 (both -1.0 exactly), then every entry from 0x1801
  onward has the interpreter's value sign-flipped vs. the JIT's. Two plausible causes were tested
  and ruled out (one real-but-inert saturation-logic bug fixed anyway, one accumulator-add bug
  proven to be a no-op after masking); the actual cause is still open. See
  `libs/dsp56300/docs/ARM32_JIT.md`'s Stage 1 section for the full detail, what's ruled out, and the
  recommended next step (a from-scratch minimal reproducer feeding the loop's exact instruction
  sequence to a bare DSP instance, rather than debugging inside the full kernel).
- Xenia (Microwave XT) parked: same dsp56300 core, much heavier DSP load (~100MHz-class vs.
  Monomodule's ~21M instr/s) — check with Gearmulator's `virusTestConsole`-style instruction-rate
  measurement before assuming this JIT makes Xenia viable too.

## Resuming

1. Read `libs/dsp56300/docs/ARM32_JIT.md`'s stage list for the current bail-out gate and next step.
2. If the scratchpad instrumented build is gone, Stage 0's dsp56300-stats/ patch is described in
   that doc's Stage 0 section (interpreter counts executed (PC, opcode) pairs to a file, dumped on
   destructor exit if `MNM_OPSTATS=<dir>` is set) — small, rebuild in ~30min.
- Ask the user before restarting MPC on the Force (192.168.1.44) or touching its `MPC.settings`,
  per the `mpc-vst-plugin` skill.

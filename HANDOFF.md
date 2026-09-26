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
- **Stage 1 done (2026-09-26)** — the interpreter is now bit-exact against the JIT: `mnm-golden`
  hash-matches on all 22 machines. Root cause was `decode_LLL_read`'s case 4/5 (plain
  `move a,l:(rN)` / `move b,l:(rN)`) missing the 48-bit transfer saturation the JIT applies there —
  found by capturing the exact register state at the real divergence point (Monomodule's
  sine-table-build loop, right where the accumulator crosses -1.0) and replaying the loop body in
  both engines, diffing full registers after every instruction rather than tracing instruction
  counts (which doesn't survive JIT/interpreter hardware-loop batching — see
  `libs/dsp56300/docs/ARM32_JIT.md` for what didn't work, for next time). Also fixed along the way:
  dsp56300#8 (out-of-range-read divergence).
- **Stage 2 blocked (2026-09-26)** — not by anything in Stage 2 itself. The calling shape
  (hand-written Thumb-2 blocks calling into existing interpreter opcode handlers), its ABI, and the
  actual block compiler (walks P memory, resolves each instruction the way `op_ResolveCache` does,
  emits the call sequence) are all built and correct — verified decoding Stage 1's sine-table loop
  body correctly on x86. But running *any* armhf binary built against this fork — including plain
  `mnm-golden`, Stage 1's own already-passing tool, completely unmodified — segfaults on the real
  Force with a wild branch (PC == fault address) inside `MemoryBuffer`'s constructor (upstream
  code, untouched by any patch here). Reproduces natively over SSH, not a qemu-user artifact, and
  not an optimizer bug (`-O0` still crashes). This contradicts this project's own recorded Force
  interpreter benchmarks (150-285% load numbers in `[[monomodule-force-feasibility]]`), which imply
  this exact memory setup worked on this hardware before — so the leading theory is a toolchain
  regression (this session's cross-compiler/flags don't match whatever built those numbers), not a
  newly-discovered 32-bit bug, but that's unconfirmed. Full detail, the crash's exact signature, and
  next steps (find the original toolchain, or get a real backtrace — no gdb available on-device or
  in this session's containers) are in `libs/dsp56300/docs/ARM32_JIT.md`'s Stage 2 section. Bail-out
  gate unchanged, not yet reachable: >=1.3x speedup or stop, once this unblocks.
- Xenia (Microwave XT) parked: same dsp56300 core, much heavier DSP load (~100MHz-class vs.
  Monomodule's ~21M instr/s) — check with Gearmulator's `virusTestConsole`-style instruction-rate
  measurement before assuming this JIT makes Xenia viable too.

- **2026-09-26 later:** step 1 of the resume found the old/new CMake caches point at different
  dsp56300 source trees (not just different build types). Bisect from `a750f285`; see
  `libs/dsp56300/tools/arm32jit_prototype/toolchain-diff/README.md`. Session-local build dirs and
  Docker images are gone. Rebuilding needs Docker Desktop running with WSL integration enabled.

- **2026-09-26 final: Stage 2 gate FAILED; the arm32 JIT effort is stopped.** The compiled block ran
  at only 1.12-1.17x the interpreter's speed on the Force (the gate is 1.3x). The "crash" was a
  missing `MNM_DSP_INTERP=1`: without it `DspEngine` takes the JIT path, and on armv7 that goes
  through a NULL table. Also found: the armhf interpreter differs from x86 on the 7 effect machines
  (deterministic, reproduces under qemu). Details are in `libs/dsp56300/docs/ARM32_JIT.md`, "Stage 2
  result". With the JIT ruled out, this port needs the interpreter itself to get faster (150-285% of
  one core today), or it gets shelved.

## Resuming

1. Read `libs/dsp56300/docs/ARM32_JIT.md`'s stage list for the current bail-out gate and next step.
2. If the scratchpad instrumented build is gone, Stage 0's dsp56300-stats/ patch is described in
   that doc's Stage 0 section (interpreter counts executed (PC, opcode) pairs to a file, dumped on
   destructor exit if `MNM_OPSTATS=<dir>` is set) — small, rebuild in ~30min.
- Ask the user before restarting MPC on the Force (192.168.1.44) or touching its `MPC.settings`,
  per the `mpc-vst-plugin` skill.

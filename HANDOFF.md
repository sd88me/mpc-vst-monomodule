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

- **2026-09-26: SHELVED.** The on-device perf profile of the interpreter is flat (the largest symbol
  is 10.5%, MAC/MPY about 18% in total, per-instruction overhead about 31%), so the best case from
  optimizing the interpreter is about 1.45x against the ~2.5-4x needed. See ARM32_JIT.md,
  "Interpreter profile on the Force".

- **2026-09-26: UN-SHELVED, static recompilation is promising.** The DSP loop, translated ahead of time
  to C++ that calls the interpreter's handlers with constant opcodes, runs 1.84-2.06x faster on the
  Force, with registers matching exactly. That isn't enough yet (~120% of a core). Next gate: inline
  memory access and reach >= 3x on the same loop. See ARM32_JIT.md, "Static recompilation gate test".

- **2026-09-26: recompilation gate 2 passed, 3.73-3.83x** on the Force (pinned), with `flatten` on the
  generated block. That projects to ~64% of one core. Next: whole-program recompiler, gated on
  `mnm-golden` matching on all 22 machines and `mnm-bench` <= 100%. See ARM32_JIT.md.

- **2026-09-26: whole-program static recompiler works.** It hash-matches the x86 reference on all 22
  machines on the Force, and load is 225% -> 113% of one core. The gate is <= 100%. See
  ARM32_JIT.md, "Whole-program static recompiler". Build steps are in
  `libs/dsp56300/tools/arm32jit_prototype/recomp/`. The generated `.inl` holds firmware words: never commit it.

- **2026-09-27 (overnight Stage 3): real-time feasible.** With the static recompiler plus Stage 3 optimisations
  (dead-flag elimination, whole-loop functions, and hot-helper fixes found with an on-device source-line
  profiler), mnm-bench is at 59% average (interpreter ~225%). Paced at real-time priority on the Force, with its
  normal background load (JV-880 emulator, MockbaMod capture script), every machine's p99 is under 100%. The
  heaviest, DPRO DDRW, is at mean 66% / p99 93%. Bit-exact with the x86 reference on all 22 machines. The VST
  wrapper should run the DSP on its own SCHED_FIFO thread. Details and the step table are in ARM32_JIT.md.

- **2026-09-27: Stage 3 DONE.** Final: mnm-bench 57.6-58.4% average (normal priority). Paced at real-time
  priority, the heaviest machine (DPRO DDRW) is at mean 67% / p99 93%, and every machine's p99 is under 100%.
  Bit-exact with x86 on all 22 machines. **Next is the port itself** (VST wrapper, skin, vst.json). Before
  shipping: widen discovery coverage and decide the distribution model. The generated code contains firmware
  words, so users must build it from their own OS `.syx`. See ARM32_JIT.md "Stage 3 conclusion" and
  `libs/dsp56300/tools/arm32jit_prototype/recomp/README.md` (pipeline).

- **2026-09-27: coexistence measured.** One Monomodule instance = one engine = one core. On the Force, N engines
  work on N separate cores (heaviest machine ~66% of its core, typical ~46%, light ~38%), but **the DSP thread
  must be SCHED_FIFO above MPC's AudioWorkers (RR 20)**: below them even 25% other load causes misses. Running
  above them, typical machines coexist with ~50% other work on the same core; the heavy ones (DPRO DDRW/DENS,
  RINGMOD, REVERB, SID) need a core mostly to themselves (other work fine to ~20%). JV-880 (`jv880-emu`, FIFO 45,
  cores 0-2) is already on the device at ~20% of a core. Core 3 has no JV-880. Open: how MPC calls plugins,
  and real project loads. Tables and pitfalls in ARM32_JIT.md "Coexistence with the rest of MPC".

- **Decision (2026-09-27): further speed-ups are parked for the next major revision.** That means block chaining,
  SR mode-bit specialisation, idle-voice skipping and the register-allocating code generator; the ideas and their
  estimates are in ARM32_JIT.md ("Stage 3 conclusion"). **Next task: the VST wrapper, skin and vst.json**, following
  the other mpc-vst ports and the `mpc-vst-plugin` skill. Design requirements from the measurements: one engine per
  instance on its own core, DSP thread SCHED_FIFO above priority 20 with a bailout when behind, ~62 MB per instance,
  and a build step that generates the recompiled code from the user's own OS `.syx`.

- **2026-09-27: VST wrapper spike built (`vst/`).** `mnm_engine.cpp` = in-process `mpc_engine()` (one voice; the
  DSP on its own SCHED_FIFO 30 thread, `MNM_FIFO`/`MNM_CPU` env overrides; 2-block ring; silence on underrun; OS
  loaded on that thread so create() returns at once; forces `MNM_DSP_INTERP=1`). 59 params (machine, level, SYN/AMP/
  FILT/EFX 8 each, LFO1-3 8 each; raw 0..127). `build_so.sh` cross-builds the .so (needs the scratchpad glue tree,
  the dsp56300 arm32 tree and a dir with the user's generated `dsp56k_recomp.inl`). `smoke.cpp` drives it paced on
  the Force: SWAVE SAW 46%, DPRO DDRW 69% of the DSP thread's core, 0 underruns. Auto-layout skin only; the exact
  upstream (LCD-look) skin is next, and it must be generated at install time from the user's OS file (fonts/dials/
  icons come from it, see upstream RomArt.h). **User decisions: One only (no Six), FX later as a separate effect
  plugin; skin comes after the spike runs on the device.** Staged on the Force (`/sdcard/vst/monomodule_one.so`,
  `/sdcard/vst/monomodule/<OS>.syx`, skin folder in /sdcard/Synths) but NOT registered: needs the user's OK to
  restart MPC and edit MPC.settings. Known mpc-vst gotcha: an auto-layout with a popup fails unless the layout is
  copied to a real layout.conf first (done).

- **2026-09-27 later: Q-Links work again (user report), cause unconfirmed.** It coincided with pinning `mnm-dsp` off core 0 and
  parking it when idle, which fits the theory that a FIFO-30 thread using ~46% of the UI core disturbed MPC's UI thread
  (but the JV-880 test then had no Monomodule instance inserted, so treat as unproven). Reopen only if it returns.
- **2026-09-27: Q-Link bug PARKED until the VST is ready (user decision).** On the Force, Monomodule One (and the
  JV-880) Q-Links on 0..127 params climb 1,2,3 then restart near 0 (LEVEL 100 -> 101 -> ~1). Traced: MPC sets exact
  k/128 steps and restarts from ~1/128 each touch even though getParameter returns the right (shadow) value; the
  wrapper fix eb2ee55 is in both deployed .so files and is not enough. Not a Monomodule engine issue (the engine
  reads back what it is given). Next steps when picked up: trace every call MPC makes around a Q-Link touch on the
  JV-880, and compare with a stock MPC instrument (Bassline). Also unrelated but real: `mnm-dsp` was unpinned on
  core 0 (MPC UI core) at FIFO 30 and ~46% idle load: pin it and add idle-skip (see the design notes above).
  The debug trace (`/tmp/mnm_trace.on` -> `/tmp/mnm_trace.log`) is still in `mnm_engine.cpp`; harmless when the file is absent.
- **TODO (user request, 2026-09-27): later, also try the alternative where the DSP runs inside MPC's own audio
  callback (no plugin thread).** Simpler and no starvation risk, but the heaviest machines (~69% of a core) would
  then be inside the AudioWorker's deadline; needs a measurement with the recompiled code first.

- **2026-09-27: exact upstream skin built and running on the Force (user: "looks great for first pass, controls fine").**
  `vst/skin/`: `mnm_artdump.cpp` dumps the LCD art + UI spec from the user's OS file (built against upstream RomArt/SpecData),
  `mk_skin.py` draws the One editor at its native 3x scale as an MPC skin (static background; 128-frame filmstrip per cell
  *kind*; per-machine SYN grid/bar overlays via IndexedEnabling on `machine`; LEV = 4 stacked strips; picker = panel + one
  button per machine behind `machine__open`; LFO2|LFO3 tabs via `lfo23tab`, DEST names via `lfoN_pagesel`/`lfo23dest`).
  `build_skin.sh <os.syx>` runs it (Docker); `ink=`/`paper=` args recolour (upstream's INVERTED / LOW CONTRAST presets).
  Params now 64 (append-only: 58 knobs, machine__open, lfo1-3_pagesel, lfo23tab, lfo23dest); `vst.json` has
  `custom_skin: true` (new gen_vst option in mpc-vst). Engine: DSP thread pinned to the least busy non-UI core, parks after 2 s silence.
  Not done: preset strip, BPM/host tempo, menu/skin dialog, install flow from the user's OS file, wider discovery workload.

- **2026-09-27: skin at 4x with three tabs, colour presets, preset strip (user request).** Tabs: SYN+AMP, FILT+EFX,
  LFO1+LFO2|3 (each tab has its own background; the machine block, LEV and preset strip are on all of them; Q-Link
  pages: SYN/AMP, FILT/EFX, LFO1/LFO2, LFO3/level+machine). Colours are a build option in `vst/skin/skin.conf`
  (`skin=default|inverted|lowcontrast`, `ink=`/`paper=`; a runtime switch would triple the image memory). Preset strip =
  PREV / PRESET selector / NEXT (upstream's save and library buttons are omitted); the name is MPC live text
  (`preset_name`, Titillium), prev/next are momentary params. Presets = Init per machine + every synth sound of
  `.syx` kit dumps in `/sdcard/vst/monomodule/dumps/` (parsed with upstream MnmDump); stepping is within the current
  machine; a `*` marks a modified sound. Upstream's `arrowH()` draws its PREV arrow pointing right; the skin mirrors it.
  LFO defaults now match upstream (PAGE 0, DEST 64, TRIG 0, WAVE 0, MULT 1, SPD 64, INTL 0, DPTH 0). Params: 67.
  Next: a preset list (tap the selector), the install flow from the user's own OS file, wider discovery coverage.

## Resuming

1. Read `libs/dsp56300/docs/ARM32_JIT.md`'s stage list for the current bail-out gate and next step.
2. If the scratchpad instrumented build is gone, Stage 0's dsp56300-stats/ patch is described in
   that doc's Stage 0 section (interpreter counts executed (PC, opcode) pairs to a file, dumped on
   destructor exit if `MNM_OPSTATS=<dir>` is set) — small, rebuild in ~30min.
- Ask the user before restarting MPC on the Force (192.168.1.44) or touching its `MPC.settings`,
  per the `mpc-vst-plugin` skill.

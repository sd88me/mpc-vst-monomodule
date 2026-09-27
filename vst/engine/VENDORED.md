# Vendored: schwung-monomodule engine glue

`MonoVoice.{h,cpp}` and `dsp/DspEngine.{h,cpp}` are vendored from
[legsmechanical/schwung-monomodule](https://github.com/legsmechanical/schwung-monomodule), commit
`bcbff13` (`main`), with one local change on top (below). License: AGPLv3 (`LICENSE` in this
directory), same as upstream Monomodule's own.

## What this is

Upstream [shnolk/monomodule](https://github.com/shnolk/monomodule) (`libs/monomodule` in this repo)
already ships its own `MonoVoice`/`DspEngine` (`libs/monomodule/src/core/`). This directory's copies
**shadow** those — same class interfaces, same header names, built instead of upstream's — because
schwung-monomodule's engine glue (written for an Ableton Move port of the same DSP core) carries real
performance and lifecycle work upstream's own copies don't have:

- **A much cheaper block handoff.** Upstream writes/reads the whole 52-word parameter block and the
  32-word audio block through the DSP's HI08 host port, one word at a time. This glue writes the 51
  words the DSP doesn't need to synchronise on straight to DSP memory, and sends only one "go" word
  and receives only one "done" word over HI08 — an order of magnitude fewer host-port transfers per
  block.
- **`resetKeepCode()`**: returns a `DspEngine` to a fresh-instance state without invalidating any
  compiled JIT code (only internal X/Y RAM is cleared, program memory is untouched), so `prewarm()`
  can compile every machine once and then reset to a clean voice at zero recompilation cost.
- **`prewarm()`** (`MonoVoice`): renders every machine once so its code is compiled ahead of the first
  real note (avoiding a 5–30 ms stall on first use of a machine), then resets to a fresh voice.
- **`skip()`** (`MonoVoice`): advances the host model's timing state without running the DSP, for an
  idle voice that's been silent — this port's DSP-thread parking (see `HANDOFF.md`) depends on it.
- **Idle-loop skipping** (`skipIdleLoops()`): several machines' kernel code spends a large fraction of
  its instructions in `DO` loops whose body is nothing but padding NOPs (up to ~50% of GND SIN's
  instructions). Since a NOP-only `DO` body changes no register or memory and the loop restores
  `LA`/`LC`/`SR` when it ends, these are safely patched to jump straight past — bit-exact, but without
  burning the emulated cycles.

None of this is required for correctness (upstream's own `MonoVoice`/`DspEngine` work fine); it's here
because this port's static-recompilation performance budget (see `libs/dsp56300/docs/ARM32_JIT.md`)
needed the lower per-block overhead and the idle-parking hooks.

## Local change on top of `bcbff13`

`DspEngine::runUntilTx()` polls `hi.txData().size()` in a tight loop from the DSP's own thread. On
32-bit ARM, `RingBuffer::size()` takes an acquire barrier (a `dmb`) on every call; `sizeSameThread()`
(added to this port's `dsp56300` fork, `libs/dsp56300`) does the same read without one, safe because
the poller and the writer are the same thread here. Changed to use it.

## Not vendored

Everything else in schwung-monomodule (its Move-specific `plugin_api_v2` wrapper, the child-process
supervisor, its own CMake project) is Move-specific and not used here; this port's own CMake
(`CMakeLists.txt` at the repo root) builds `mnmcore` from upstream Monomodule's core plus just these
two shadowed files, and links this port's own `vst/mnm_engine.cpp` on top.

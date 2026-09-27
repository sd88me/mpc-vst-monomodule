// Glue: MIDI + parameters -> HostModel blocks -> DspEngine -> float stereo audio (44.1 kHz native).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "dsp/DspEngine.h"
#include "host/HostModel.h"

namespace mnm {

class MonoVoice {
public:
    explicit MonoVoice(const fw::Firmware& fw);

    host::HostModel& host() { return m_host; }
    dsp::DspEngine& engine() { return *m_engine; }

    // Renders exactly one 16-frame block into the internal FIFO (advances the host tick).
    void renderBlock();
    // Pull n frames; renders blocks as needed. Events should be applied via host() between calls
    // (they take effect at the next block boundary, like the hardware's control rate).
    void process(float* left, float* right, int n);
    // Effect mode: host audio in (may alias out), processed in 16-frame blocks.
    void processFx(const float* inL, const float* inR, float* outL, float* outR, int n);
    int framesBuffered() const { return m_avail; }
    // Snaps the host words to their targets (HostModel::settle) and renders `blocks` blocks so the DSP
    // side is initialised; the FIFO is left empty.
    void warmUp(int blocks);
    // Back to the state of a freshly constructed voice: the DSP re-initialised (delay lines, envelopes and
    // every other kernel state cleared), a new HostModel, the FIFO empty. Used between offline renders.
    void reset();
    // Schwung: renders every machine once so the JIT compiles all their code now (the first block of a
    // machine otherwise stalls for 5-30 ms on a CM5), then returns to the state of a fresh voice.
    void prewarm();
    // Schwung: an idle engine skips the DSP. Advances the host model (frame counter, LFOs, slew) by
    // `frames` (a multiple of 16) as if blocks had been rendered, and outputs silence. The FIFO keeps
    // its position (so an FX engine's input/output alignment survives), filled with zeros. Not
    // bit-exact on wake: the DSP's free-running state (oscillator phases) is where it stopped.
    void skip(int frames);

private:
    host::HostModel m_host;
    std::unique_ptr<dsp::DspEngine> m_engine;
    std::array<int32_t, 32> m_fifo{};
    std::array<int32_t, 32> m_in{};
    int m_inPos = 0;
    int m_pos = 0, m_avail = 0;
};

} // namespace mnm

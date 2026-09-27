// Bit-exactness check: renders a fixed script on every machine and prints a hash of the output samples.
// Run it on the unmodified engine and on an optimised one; identical hashes = identical audio.
//
//   mnm-golden <os.syx> [raw-out-dir]      (MNM_PREWARM=1: pre-warm every voice first; must not change a hash)
//
// The script per machine: settle, note on, parameter sweeps on every page, an LFO, a retrigger, note off
// and a tail (synths); noise with a level ramp and parameter sweeps (FX machines). With raw-out-dir, the
// float output is also written as <machine>.f32 (interleaved L/R) for diffing.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "host/Machines.h"

using namespace mnm;

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: mnm-golden <os.syx> [raw-out-dir]\n"); return 2; }
    const auto fw = fw::loadFirmware(argv[1]);
    const bool prewarm = std::getenv("MNM_PREWARM") != nullptr;   // the output must not change
    const char* rawDir = argc > 2 ? argv[2] : nullptr;
    constexpr int kChunk = 128, kChunks = 44100 * 3 / kChunk;   // ~3 s per machine

    uint64_t all = 0xcbf29ce484222325ull;
    for (const auto& def : host::kMachineDefs) {
        MonoVoice v(fw);
        if (prewarm) v.prewarm();
        auto& h = v.host();
        h.setMachine(def.machine);
        const bool fx = host::isFxMachine(def.machine);
        h.setRouting(fx ? host::dspInputBits(host::FxInput::InpAB) : 0u);
        v.warmUp(8);
        h.setLfoParam(0, 0, 0); h.setLfoParam(0, 1, 0); h.setLfoParam(0, 6, 64); h.setLfoParam(0, 7, 90);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> noise(-0.6f, 0.6f);
        std::vector<float> L(kChunk), R(kChunk), inL(kChunk), inR(kChunk), raw;
        uint64_t hsh = 0xcbf29ce484222325ull;
        h.noteOn(fx ? 60 : 45);
        for (int c = 0; c < kChunks; ++c) {
            // parameter automation: one page/param step every 8 chunks, sweeping values
            if (c % 8 == 0) {
                const int step = c / 8;
                const auto page = host::Page(step % 4);
                const int k = (step / 4) % 8;
                if (!(page == host::Page::AMP && k >= 2 && k <= 5 && fx))   // keep FX envelope open
                    h.setParam(page, k, (step * 37) % 128);
            }
            if (!fx) {
                if (c == kChunks / 3) h.noteOn(52);
                if (c == kChunks / 2) h.noteOn(57);
                if (c == 2 * kChunks / 3) h.noteOff();
            }
            if (fx) {
                const float g = c < kChunks / 2 ? 1.f : 0.f;   // input stops halfway: tails
                for (int i = 0; i < kChunk; ++i) { inL[i] = g * noise(rng); inR[i] = g * noise(rng); }
                v.processFx(inL.data(), inR.data(), L.data(), R.data(), kChunk);
            } else {
                v.process(L.data(), R.data(), kChunk);
            }
            for (int i = 0; i < kChunk; ++i) {
                hsh = fnv(hsh, &L[i], 4); hsh = fnv(hsh, &R[i], 4);
                if (rawDir) { raw.push_back(L[i]); raw.push_back(R[i]); }
            }
        }
        std::printf("%-11s %016llx%s\n", def.name, (unsigned long long)hsh, v.engine().faulted() ? "  FAULTED" : "");
        all = fnv(all, &hsh, 8);
        if (rawDir) {
            std::string name = def.name; for (auto& ch : name) if (ch == ' ' || ch == '+') ch = '_';
            if (FILE* f = std::fopen((std::string(rawDir) + "/" + name + ".f32").c_str(), "wb")) {
                std::fwrite(raw.data(), 4, raw.size(), f); std::fclose(f);
            }
        }
    }
    std::printf("ALL         %016llx\n", (unsigned long long)all);
    return 0;
}

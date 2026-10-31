// Six-voice feasibility bench: N MonoVoice engines (one per track), each on its own thread, paced at real time
// (128-frame chunks at 44.1 kHz). Reports each thread's load (busy time / chunk time), the worst chunk, and the chunks that
// missed their deadline.
//
//   mnm-six-bench <os.syx> [seconds] [machine-index ...]   machine indexes into host::kMachineDefs (0..21); one voice each,
//                                                          default 6 x SWAVE SAW (6). Fewer than 6 indexes = that many voices.
//   MNM_FIFO=n   run the threads SCHED_FIFO priority n.   MNM_ONE=1  run the voices one after another on one thread (sum of load).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <algorithm>
#include <sched.h>
#include <pthread.h>

#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "host/Machines.h"

using namespace mnm;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: mnm-six-bench <os.syx> [seconds] [machine-index ...]\n"); return 2; }
    const auto fw = fw::loadFirmware(argv[1]);
    const int secs = argc > 2 ? std::atoi(argv[2]) : 20;
    std::vector<int> idx;
    for (int i = 3; i < argc; ++i) idx.push_back(std::atoi(argv[i]));
    if (idx.empty()) idx.assign(6, 6);
    const int n = int(idx.size());
    constexpr int kChunk = 128;
    const double chunkUs = kChunk * 1e6 / 44100.0;
    const int chunks = int(secs * 44100.0 / kChunk);
    const int fifo = std::getenv("MNM_FIFO") ? std::atoi(std::getenv("MNM_FIFO")) : 0;

    std::vector<std::unique_ptr<MonoVoice>> v;
    for (int t = 0; t < n; ++t) {
        v.push_back(std::make_unique<MonoVoice>(fw));
        v[t]->prewarm();
        const auto& def = host::kMachineDefs[idx[t] % host::kNumMachineDefs];
        v[t]->host().setMachine(def.machine);
        if (host::isFxMachine(def.machine)) v[t]->host().setRouting(host::dspInputBits(host::FxInput::InpAB));
        v[t]->warmUp(8);
        std::printf("voice %d: %s\n", t + 1, def.name);
    }
    struct Res { double busyUs = 0, worstUs = 0; int miss = 0; };
    std::vector<Res> res(n);
    auto work = [&](int t) {
        if (fifo) { sched_param sp{}; sp.sched_priority = fifo; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }
        std::vector<float> L(kChunk), R(kChunk), iL(kChunk), iR(kChunk);
        const bool fx = host::isFxMachine(host::kMachineDefs[idx[t] % host::kNumMachineDefs].machine);
        unsigned rs = 1234u + unsigned(t);
        auto next = clk::now();
        for (int c = 0; c < chunks; ++c) {
            if (c % 344 == 0) { v[t]->host().noteOn(40 + 4 * t); }          // a note about every second, a chord across the voices
            if (c % 344 == 250) v[t]->host().noteOff();
            const auto t0 = clk::now();
            if (fx) { for (int i = 0; i < kChunk; ++i) { rs = rs * 1664525u + 1013904223u; iL[i] = (float(int(rs >> 8)) / 8388608.f - 1.f) * 0.3f; iR[i] = iL[i]; } v[t]->processFx(iL.data(), iR.data(), L.data(), R.data(), kChunk); }
            else v[t]->process(L.data(), R.data(), kChunk);
            const double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
            res[t].busyUs += us; res[t].worstUs = std::max(res[t].worstUs, us);
            if (us > chunkUs) ++res[t].miss;
            next += std::chrono::microseconds(int64_t(chunkUs));
            if (clk::now() < next) std::this_thread::sleep_until(next); else next = clk::now();   // behind: no catching up, as a real callback
        }
    };
    if (std::getenv("MNM_ONE")) {   // sequential on one thread: total cost of n voices per chunk
        std::vector<float> L(kChunk), R(kChunk);
        double busy = 0, worst = 0; int miss = 0; auto next = clk::now();
        for (int c = 0; c < chunks; ++c) {
            for (int t = 0; t < n; ++t) { if (c % 344 == 0) v[t]->host().noteOn(40 + 4 * t); if (c % 344 == 250) v[t]->host().noteOff(); }
            const auto t0 = clk::now();
            for (int t = 0; t < n; ++t) v[t]->process(L.data(), R.data(), kChunk);
            const double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
            busy += us; worst = std::max(worst, us); if (us > chunkUs) ++miss;
            next += std::chrono::microseconds(int64_t(chunkUs));
            if (clk::now() < next) std::this_thread::sleep_until(next); else next = clk::now();
        }
        std::printf("one thread, %d voices: load %.0f%% of a core, worst chunk %.0f%%, missed %d / %d\n", n, 100 * busy / (chunks * chunkUs), 100 * worst / chunkUs, miss, chunks);
        return 0;
    }
    std::vector<std::thread> th;
    for (int t = 0; t < n; ++t) th.emplace_back(work, t);
    for (auto& t : th) t.join();
    double sum = 0; int miss = 0;
    for (int t = 0; t < n; ++t) {
        std::printf("voice %d: load %.0f%%  worst chunk %.0f%%  missed %d / %d\n", t + 1, 100 * res[t].busyUs / (chunks * chunkUs), 100 * res[t].worstUs / chunkUs, res[t].miss, chunks);
        sum += res[t].busyUs / (chunks * chunkUs); miss += res[t].miss;
    }
    std::printf("TOTAL %.2f cores over %d threads, %d missed chunks\n", sum, n, miss);
}

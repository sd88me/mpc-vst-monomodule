// Drives mpc_engine() like the host does: 128-frame blocks paced to real time. Prints readiness time, peak,
// underruns and the DSP thread's CPU share. usage: mnm-vst-smoke <data-dir> [machine-slot] [seconds]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <dirent.h>
#include <unistd.h>
extern "C" {
#include "engine.h"
}
using Clock = std::chrono::steady_clock;
static double dspCpuSeconds()
{
    double best = 0;
    if (DIR* d = opendir("/proc/self/task")) {
        while (auto* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char p[128], name[64] = "", line[512];
            snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
            if (FILE* f = fopen(p, "r")) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
            if (strcmp(name, "mnm-dsp")) continue;
            snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
            if (FILE* f = fopen(p, "r")) {
                if (fgets(line, sizeof line, f)) { unsigned long u = 0, s = 0; const char* q = strrchr(line, ')'); sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &u, &s); best = double(u + s) / double(sysconf(_SC_CLK_TCK)); }
                fclose(f);
            }
        }
        closedir(d);
    }
    return best;
}
int main(int argc, char** argv)
{
    const mpc_engine_t* e = mpc_engine();
    const auto t0 = Clock::now();
    void* in = e->create(argc > 1 ? argv[1] : ".");
    char buf[32];
    while (true) { if (e->get_param(in, "ready", buf, sizeof buf) > 0 && buf[0] == '1') break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); if (Clock::now() - t0 > std::chrono::seconds(60)) { puts("engine never became ready"); return 1; } }
    printf("ready after %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    if (argc > 2) { e->set_param(in, "machine", argv[2]); }
    const int secs = argc > 3 ? atoi(argv[3]) : 10;
    static int16_t out[128 * 2];
    int peak = 0; const uint8_t on[3] = {0x90, 45, 100}, off[3] = {0x80, 45, 0};
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (getenv("SMOKE_IDLE")) {   // no note: the DSP should park after 2 s and cost ~nothing
        const double c0 = dspCpuSeconds(); const auto ti = Clock::now();
        static int16_t junk[128 * 2];
        for (int b = 0; b < 6 * 44100 / 128; ++b) { std::this_thread::sleep_for(std::chrono::microseconds(2902)); e->render(in, junk, 128); }
        e->get_param(in, "parked", buf, sizeof buf);
        printf("idle 6 s: dsp-thread cpu %.1f%% parked=%s\n", 100.0 * (dspCpuSeconds() - c0) / std::chrono::duration<double>(Clock::now() - ti).count(), buf);
    }
    e->get_param(in, "core", buf, sizeof buf); printf("core %s\n", buf);
    const bool fx = e->process != nullptr;   // an effect: feed noise for the first three quarters, then silence (tail)
    if (!fx) e->midi(in, on, 3);
    const double cpu0 = dspCpuSeconds(); const auto t1 = Clock::now();
    auto next = t1;
    const int blocks = secs * 44100 / 128;
    for (int b = 0; b < blocks; ++b) {
        next += std::chrono::nanoseconds(int64_t(128.0 / 44100.0 * 1e9));
        std::this_thread::sleep_until(next);
        if (fx) {
            static int16_t src[128 * 2];
            for (int i = 0; i < 256; ++i) src[i] = b < blocks * 3 / 4 ? int16_t((rand() % 16000) - 8000) : 0;
            e->process(in, src, out, 128);
        } else {
            e->render(in, out, 128);
        }
        for (int i = 0; i < 256; ++i) { int v = out[i] < 0 ? -out[i] : out[i]; if (v > peak) peak = v; }
        if (!fx && b == blocks * 3 / 4) e->midi(in, off, 3);
    }
    const double wall = std::chrono::duration<double>(Clock::now() - t1).count();
    e->get_param(in, "underruns", buf, sizeof buf);
    printf("peak %d underruns %s dsp-thread cpu %.1f%%\n", peak, buf, 100.0 * (dspCpuSeconds() - cpu0) / wall);
    e->destroy(in);
    return 0;
}

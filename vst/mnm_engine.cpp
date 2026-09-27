// mpc_engine() (mpc-vst-plugins wrapper/engine.h) for Monomodule One: one voice, one machine per instance.
//
// The emulated DSP runs on its own SCHED_FIFO thread and renders 128-frame blocks ahead of the host into a
// small ring; render() (the host's audio callback) only copies a finished block out, or outputs silence when
// the DSP thread is behind (an underrun is counted, never waited for). Loading the OS file and warming the
// DSP happen on that thread too, so create() returns at once and the host never blocks.
//
// Parameters are raw 0..127 kit bytes, held in atomics; the DSP thread diffs them against what it applied.
// Keys: machine (option index), level, syn0-7 (A-H, meaning follows the machine), amp0-7, filt0-7, efx0-7,
// lfo1_0-7, lfo2_0-7, lfo3_0-7.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "host/Machines.h"

extern "C" {
#include "engine.h"
}

namespace {

using namespace mnm;

constexpr int kFrames = 128;
constexpr int kRing = 4;           // blocks
constexpr int kAhead = 2;          // blocks the DSP thread keeps rendered ahead
constexpr int kNumMachines = 15;
constexpr host::Machine kMachines[kNumMachines] = {
    host::Machine::GND, host::Machine::SIN, host::Machine::NOIS, host::Machine::SID, host::Machine::SAW,
    host::Machine::PULS, host::Machine::ENS,
    host::Machine::WAVE, host::Machine::BBOX, host::Machine::DDRW, host::Machine::DENS,
    host::Machine::FM_STAT, host::Machine::FM_PAR, host::Machine::FM_DYN, host::Machine::VO6};

// parameter slots: 0 machine, 1 level, 2.. = 4 pages x 8, then 3 LFOs x 8
constexpr int kSlotMachine = 0, kSlotLevel = 1, kSlotPages = 2, kSlotLfo = 34, kNumSlots = 58;

int slotOf(const char* key)
{
    if (!std::strcmp(key, "machine")) return kSlotMachine;
    if (!std::strcmp(key, "level")) return kSlotLevel;
    static const char* pages[4] = {"syn", "amp", "filt", "efx"};
    for (int p = 0; p < 4; ++p) {
        const size_t n = std::strlen(pages[p]);
        if (!std::strncmp(key, pages[p], n) && key[n] >= '0' && key[n] <= '7' && !key[n + 1]) return kSlotPages + p * 8 + (key[n] - '0');
    }
    if (!std::strncmp(key, "lfo", 3) && key[3] >= '1' && key[3] <= '3' && key[4] == '_' && key[5] >= '0' && key[5] <= '7' && !key[6])
        return kSlotLfo + (key[3] - '1') * 8 + (key[5] - '0');
    return -1;
}

void defaultsFor(int machineSlot, int* out /*[32] SYN AMP FILT EFX*/)
{
    const auto* d = host::machineDef(kMachines[machineSlot]);
    for (int k = 0; k < 8; ++k) {
        out[k] = d ? d->defaults[size_t(k)] : 0;
        out[8 + k] = host::kDefaultAmp[size_t(k)];
        out[16 + k] = host::kDefaultFilt[size_t(k)];
        out[24 + k] = host::kDefaultEfx[size_t(k)];
    }
}

struct NoteEv { int8_t type; int8_t note; };   // 1 on, 2 off, 3 all off

struct Inst {
    std::string osPath;
    std::atomic<int> param[kNumSlots];
    std::atomic<bool> stop{false}, ready{false};
    std::atomic<uint32_t> underruns{0}, blocks{0};
    // note queue (host thread -> DSP thread)
    NoteEv notes[256];
    std::atomic<uint32_t> nWrite{0}, nRead{0};
    // audio ring (DSP thread -> host)
    alignas(64) int16_t ring[kRing][kFrames * 2];
    std::atomic<uint32_t> rWrite{0}, rRead{0};
    std::thread th;
    FILE* trace = nullptr;   // /tmp/mnm_trace.on present at create: log every set/get to /tmp/mnm_trace.log

    Inst()
    {
        for (auto& p : param) p.store(0);
        param[kSlotLevel].store(100);
        int d[32];
        defaultsFor(4, d);   // SWAVE SAW, as upstream One
        param[kSlotMachine].store(4);
        for (int i = 0; i < 32; ++i) param[kSlotPages + i].store(d[i]);
    }
    void pushNote(int8_t t, int8_t n)
    {
        const uint32_t w = nWrite.load(std::memory_order_relaxed);
        if (w - nRead.load(std::memory_order_acquire) >= 256) return;
        notes[w & 255] = {t, n};
        nWrite.store(w + 1, std::memory_order_release);
    }
    void run();
};

std::unique_ptr<fw::Firmware> loadFw(const std::string& path)
{
    return std::make_unique<fw::Firmware>(fw::loadFirmware(path));
}

void Inst::run()
{
    pthread_setname_np(pthread_self(), "mnm-dsp");
    std::unique_ptr<fw::Firmware> firmware;
    std::unique_ptr<MonoVoice> voice;
    try {
        firmware = loadFw(osPath);
        voice = std::make_unique<MonoVoice>(*firmware);
        voice->host().setMachine(kMachines[param[kSlotMachine].load()]);
        voice->warmUp(8);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[monomodule] engine failed: %s\n", ex.what());
        return;   // stays silent
    }
    // realtime only once booted, above MPC's AudioWorkers (SCHED_RR 20); MNM_FIFO=0 keeps SCHED_OTHER
    int prio = 30;
    if (const char* e = std::getenv("MNM_FIFO")) prio = std::atoi(e);
    if (prio > 0) { sched_param sp{}; sp.sched_priority = prio; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }
    if (const char* c = std::getenv("MNM_CPU")) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(std::atoi(c), &s); sched_setaffinity(0, sizeof s, &s); }

    auto& h = voice->host();
    int applied[kNumSlots];
    for (int i = 0; i < kNumSlots; ++i) applied[i] = -1;
    int machineSlot = -1;
    int held[16], nHeld = 0;
    bool muted = false;
    std::vector<float> L(kFrames), R(kFrames);
    ready.store(true);

    while (!stop.load(std::memory_order_acquire)) {
        const uint32_t w = rWrite.load(std::memory_order_relaxed);
        if (int32_t(w - rRead.load(std::memory_order_acquire)) >= kAhead) {
            struct timespec ts{0, 400000};
            nanosleep(&ts, nullptr);
            continue;
        }
        // machine first: it loads that machine's page defaults, which the shadow already holds
        const int ms = std::clamp(param[kSlotMachine].load(std::memory_order_relaxed), 0, kNumMachines - 1);
        if (ms != machineSlot) {
            machineSlot = ms;
            h.setMachine(kMachines[ms]);
            if (nHeld == 0) { h.noteOff(); muted = true; }   // the assign's init must not sound
            for (int i = 0; i < 32; ++i) applied[kSlotPages + i] = h.param(host::Page(i / 8), i % 8);
        }
        for (int i = 0; i < 32; ++i) {
            const int v = std::clamp(param[kSlotPages + i].load(std::memory_order_relaxed), 0, 127);
            if (v != applied[kSlotPages + i]) { applied[kSlotPages + i] = v; h.setParam(host::Page(i / 8), i % 8, v); }
        }
        for (int i = 0; i < 24; ++i) {
            const int v = std::clamp(param[kSlotLfo + i].load(std::memory_order_relaxed), 0, 127);
            if (v != applied[kSlotLfo + i]) { applied[kSlotLfo + i] = v; h.setLfoParam(i / 8, i % 8, v); }
        }
        const int lv = std::clamp(param[kSlotLevel].load(std::memory_order_relaxed), 0, 127);
        if (lv != applied[kSlotLevel]) { applied[kSlotLevel] = lv; h.setLevel(lv); }

        uint32_t r = nRead.load(std::memory_order_relaxed);
        const uint32_t nw = nWrite.load(std::memory_order_acquire);
        for (; r != nw; ++r) {
            const NoteEv e = notes[r & 255];
            if (e.type == 1) {
                muted = false;
                int n = 0;
                for (int i = 0; i < nHeld; ++i) if (held[i] != e.note) held[n++] = held[i];
                nHeld = n;
                if (nHeld < 16) held[nHeld++] = e.note;
                h.noteOn(e.note);
            } else if (e.type == 2) {
                const bool cur = nHeld > 0 && held[nHeld - 1] == e.note;
                int n = 0;
                for (int i = 0; i < nHeld; ++i) if (held[i] != e.note) held[n++] = held[i];
                nHeld = n;
                if (nHeld == 0) h.noteOff();
                else if (cur) h.noteOn(held[nHeld - 1]);
            } else { nHeld = 0; h.noteOff(); }
        }
        nRead.store(r, std::memory_order_release);

        voice->process(L.data(), R.data(), kFrames);
        int16_t* out = ring[w % kRing];
        for (int i = 0; i < kFrames; ++i) {
            const float l = muted ? 0.f : L[size_t(i)], rr = muted ? 0.f : R[size_t(i)];
            out[2 * i] = int16_t(std::lrint(std::clamp(l, -1.f, 1.f) * 32767.f));
            out[2 * i + 1] = int16_t(std::lrint(std::clamp(rr, -1.f, 1.f) * 32767.f));
        }
        rWrite.store(w + 1, std::memory_order_release);
        blocks.fetch_add(1, std::memory_order_relaxed);
        if (voice->engine().faulted()) { std::fprintf(stderr, "[monomodule] DSP fault: %s\n", voice->engine().faultReason().c_str()); return; }
    }
}

void* eCreate(const char* dataDir)
{
    // the JIT path has no 32-bit ARM backend; the recompiled interpreter is the only one that works there
    setenv("MNM_DSP_INTERP", "1", 0);
    auto* in = new Inst();
    if (access("/tmp/mnm_trace.on", F_OK) == 0) in->trace = std::fopen("/tmp/mnm_trace.log", "a");
    if (const char* p = std::getenv("MNM_OS")) in->osPath = p;
    else in->osPath = std::string(dataDir && *dataDir ? dataDir : ".") + "/Elektron_SFX6-60_OS1.32B.syx";
    in->th = std::thread([in] { in->run(); });
    return in;
}
void eDestroy(void* p)
{
    auto* in = static_cast<Inst*>(p);
    in->stop.store(true);
    if (in->th.joinable()) in->th.join();
    delete in;
}
void eMidi(void* p, const uint8_t* m, int len)
{
    auto* in = static_cast<Inst*>(p);
    if (len < 2) return;
    const int st = m[0] & 0xf0;
    if (st == 0x90 && len >= 3) in->pushNote(m[2] ? 1 : 2, int8_t(m[1] & 0x7f));
    else if (st == 0x80) in->pushNote(2, int8_t(m[1] & 0x7f));
    else if (st == 0xb0 && (m[1] == 123 || m[1] == 120)) in->pushNote(3, 0);
}
void eSet(void* p, const char* key, const char* val)
{
    auto* in = static_cast<Inst*>(p);
    const int s = slotOf(key);
    if (s < 0) return;
    const int v = int(std::lround(std::atof(val)));
    if (in->trace) { std::fprintf(in->trace, "set %s '%s' -> %d\n", key, val, v); std::fflush(in->trace); }
    if (s == kSlotMachine) {
        const int m = std::clamp(v, 0, kNumMachines - 1);
        if (m == in->param[kSlotMachine].load()) return;
        int d[32];
        defaultsFor(m, d);
        for (int i = 0; i < 32; ++i) in->param[kSlotPages + i].store(d[i]);
        in->param[kSlotMachine].store(m);
        return;
    }
    in->param[s].store(std::clamp(v, 0, 127));
}
int eGet(void* p, const char* key, char* buf, int len)
{
    auto* in = static_cast<Inst*>(p);
    const int s = slotOf(key);
    if (s >= 0) {
        const int n = std::snprintf(buf, size_t(len), "%d", in->param[s].load());
        if (in->trace) { std::fprintf(in->trace, "get %s = %s\n", key, buf); std::fflush(in->trace); }
        return n;
    }
    if (!std::strcmp(key, "underruns")) return std::snprintf(buf, size_t(len), "%u", in->underruns.load());
    if (!std::strcmp(key, "ready")) return std::snprintf(buf, size_t(len), "%d", int(in->ready.load()));
    return 0;
}
void eRender(void* p, int16_t* out, int frames)
{
    auto* in = static_cast<Inst*>(p);
    int done = 0;
    while (done < frames) {
        const int n = std::min(frames - done, kFrames);
        const uint32_t r = in->rRead.load(std::memory_order_relaxed);
        if (int32_t(in->rWrite.load(std::memory_order_acquire) - r) > 0 && n == kFrames) {
            std::memcpy(out + done * 2, in->ring[r % kRing], sizeof(int16_t) * kFrames * 2);
            in->rRead.store(r + 1, std::memory_order_release);
        } else {
            std::memset(out + done * 2, 0, sizeof(int16_t) * size_t(n) * 2);
            if (in->ready.load(std::memory_order_relaxed)) in->underruns.fetch_add(1, std::memory_order_relaxed);
        }
        done += n;
    }
}

const mpc_engine_t kEngine = {eCreate, eDestroy, eMidi, eSet, eGet, eRender};

} // namespace

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }

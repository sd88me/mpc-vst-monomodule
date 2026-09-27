// mpc_engine() (mpc-vst-plugins wrapper/engine.h) for Monomodule One: one voice, one machine per instance.
// Built twice: MNM_FX=0 -> Monomodule One (a synth: notes in, audio out); MNM_FX=1 -> Monomodule FX (the FX machines as an audio
// effect on the host's audio: process() instead of render()).
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
#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "library/MnmDump.h"
#include "host/Machines.h"

extern "C" {
#include "engine.h"
}

#ifndef MNM_FX
#define MNM_FX 0
#endif

namespace {

using namespace mnm;

constexpr int kFrames = 128;
constexpr int kRing = 4;           // blocks
constexpr int kAhead = 2;          // blocks the DSP thread keeps rendered ahead
constexpr int kDepth = 2;          // FX: blocks of latency between the host's input and the output it reads
#if MNM_FX
constexpr int kNumMachines = 7;
constexpr int kDefaultMachine = 1;   // REVERB
constexpr host::Machine kMachines[kNumMachines] = {host::Machine::THRU, host::Machine::REVERB, host::Machine::CHORUS,
    host::Machine::DYNAMIX, host::Machine::RINGMOD, host::Machine::PHASER, host::Machine::FLANGER};
#else
constexpr int kNumMachines = 15;
constexpr int kDefaultMachine = 4;   // SWAVE SAW, as upstream One
constexpr host::Machine kMachines[kNumMachines] = {
    host::Machine::GND, host::Machine::SIN, host::Machine::NOIS, host::Machine::SID, host::Machine::SAW,
    host::Machine::PULS, host::Machine::ENS,
    host::Machine::WAVE, host::Machine::BBOX, host::Machine::DDRW, host::Machine::DENS,
    host::Machine::FM_STAT, host::Machine::FM_PAR, host::Machine::FM_DYN, host::Machine::VO6};
#endif

// parameter slots: 0 machine, 1 level, 2.. = 4 pages x 8, then 3 LFOs x 8
constexpr int kSlotMachine = 0, kSlotLevel = 1, kSlotPages = 2, kSlotLfo = 34, kSlotTab = 58, kSlotTune = 59, kSlotLpk = 60, kSlotHpk = 61, kNumSlots = 62;
// lfoN_pagesel (N = 1..3) is the list index of that LFO's PAGE knob (raw 0..127 in 9 unequal buckets), so the skin
// can show DEST's names for the page; setting it moves PAGE to that list entry's middle.
constexpr int listIndex9(int raw) { return ((2 * raw + 1) * 9) >> 8; }
constexpr int listRawMid9(int idx) { return (idx * 256 + 128) / 18; }

int slotOf(const char* key)
{
    if (!std::strcmp(key, "machine")) return kSlotMachine;
    if (!std::strcmp(key, "level")) return kSlotLevel;
    static const char* pages[4] = {"syn", "amp", "filt", "efx"};
    for (int p = 0; p < 4; ++p) {
        const size_t n = std::strlen(pages[p]);
        if (!std::strncmp(key, pages[p], n) && key[n] >= '0' && key[n] <= '7' && !key[n + 1]) return kSlotPages + p * 8 + (key[n] - '0');
    }
    if (!std::strcmp(key, "master_tune")) return kSlotTune;   // global (not part of a preset): master tune in Hz, 400..440
    (void)0;
    if (!std::strcmp(key, "lpf_key")) return kSlotLpk;        // LPF / HPF track the key (KIT > ASSIGN > KEY)
    if (!std::strcmp(key, "hpf_key")) return kSlotHpk;
    if (!std::strcmp(key, "lfo23tab")) return kSlotTab;   // the LFO2 | LFO3 tab of the skin: skin state only
    if (!std::strncmp(key, "lfo", 3) && key[3] >= '1' && key[3] <= '3' && key[4] == '_' && key[5] >= '0' && key[5] <= '7' && !key[6])
        return kSlotLfo + (key[3] - '1') * 8 + (key[5] - '0');
    return -1;
}

constexpr int kLfoDefaults[8] = {0, 64, 0, 0, 1, 64, 0, 0};   // upstream kLfoParams: PAGE DEST TRIG WAVE MULT SPD INTL DPTH

void defaultsFor(int machineSlot, int* out /*[32] SYN AMP FILT EFX*/)
{
    const auto* d = host::machineDef(kMachines[machineSlot]);
    for (int k = 0; k < 8; ++k) {
        out[k] = d ? d->defaults[size_t(k)] : 0;
        out[8 + k] = MNM_FX ? host::kDefaultAmpFx[size_t(k)] : host::kDefaultAmp[size_t(k)];   // an FX track holds its envelope open
        out[16 + k] = host::kDefaultFilt[size_t(k)];
        out[24 + k] = host::kDefaultEfx[size_t(k)];
    }
}

// One engine per core, never the UI core (MPC's main thread lives on cpu0): take the least busy of cores 1..N-1
// (sampled from /proc/stat over 100 ms) that no other instance of this plugin already uses.
std::atomic<unsigned> g_usedCores{0};
int chooseCore()
{
    auto sample = [](unsigned long long* busy, unsigned long long* total, int n) {
        FILE* f = std::fopen("/proc/stat", "r");
        if (!f) return;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            int c; unsigned long long u, ni, s, id, io, ir, so, st;
            if (std::sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &ni, &s, &id, &io, &ir, &so, &st) == 9 && c >= 0 && c < n) {
                busy[c] = u + ni + s + ir + so + st; total[c] = busy[c] + id + io;
            }
        }
        std::fclose(f);
    };
    const int n = int(std::min<long>(sysconf(_SC_NPROCESSORS_ONLN), 8));
    if (n < 2) return -1;
    unsigned long long b0[8] = {}, t0[8] = {}, b1[8] = {}, t1[8] = {};
    sample(b0, t0, n);
    struct timespec ts{0, 100000000};
    nanosleep(&ts, nullptr);
    sample(b1, t1, n);
    int best = -1; double bestLoad = 2;
    const unsigned used = g_usedCores.load();
    for (int c = 1; c < n; ++c) {
        const double dt = double(t1[c] - t0[c]);
        double load = dt > 0 ? double(b1[c] - b0[c]) / dt : 0;
        if (used & (1u << c)) load += 1.0;   // another instance already runs there: last resort
        if (load < bestLoad) { bestLoad = load; best = c; }
    }
    if (best >= 0) g_usedCores.fetch_or(1u << best);
    return best;
}

struct NoteEv { int8_t type; int8_t note; };   // 1 on, 2 off, 3 all off

// Presets: an "Init" per machine, then every synth sound (kit track) of the .syx dumps in <data dir>/dumps, as upstream.
struct PresetSound {
    char name[28];
    int machine;    // host::Machine model
    int level;
    uint8_t params[56];   // SYN AMP FILT EFX (32) + LFO 1-3 (24), raw
};
struct Catalog { std::vector<PresetSound> sounds; };

Catalog* buildCatalog(const std::string& dir)
{
    auto* cat = new Catalog();
    std::vector<std::string> files;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const size_t n = std::strlen(e->d_name);
            if (n > 4 && strcasecmp(e->d_name + n - 4, ".syx") == 0) files.push_back(dir + "/" + e->d_name);
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    std::vector<std::string> seen;
    for (const auto& path : files) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && data.size() < (64u << 20)) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
        mnm::dump::Dump dump;
        try { dump = mnm::dump::parseDump(data.data(), data.size(), path); } catch (...) { continue; }
        for (const auto& kit : dump.kits) {
            if (kit.isEmptySlot()) continue;
            for (int t = 0; t < 6; ++t) {
                const auto& tr = kit.tracks[t];
                bool ours = false;
                for (auto m : kMachines) if (int(m) == tr.model) ours = true;
                if (!ours || (!MNM_FX && tr.model == int(host::Machine::GND))) continue;
                std::string key(reinterpret_cast<const char*>(tr.params), 56);
                key += char(tr.model); key += char(tr.level);
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
                seen.push_back(key);
                PresetSound snd{};
                std::snprintf(snd.name, sizeof snd.name, "%s %d", kit.name.c_str(), t + 1);
                snd.machine = tr.model;
                snd.level = std::min<int>(tr.level, 127);
                std::memcpy(snd.params, tr.params, 56);
                for (auto& b : snd.params) b = std::min<uint8_t>(b, 127);
                cat->sounds.push_back(snd);
            }
        }
    }
    return cat;
}

struct Inst {
    std::string osPath;
    std::atomic<int> param[kNumSlots];
    std::atomic<bool> stop{false}, ready{false}, parked{false};
    std::atomic<uint32_t> underruns{0}, blocks{0};
    // note queue (host thread -> DSP thread)
    NoteEv notes[256];
    std::atomic<uint32_t> nWrite{0}, nRead{0};
    // audio ring (DSP thread -> host)
    alignas(64) int16_t ring[kRing][kFrames * 2];
    std::atomic<uint32_t> rWrite{0}, rRead{0};
    std::thread th, catTh;
    alignas(64) int16_t inRing[kRing][kFrames * 2];   // FX: host audio waiting for the DSP thread
    std::atomic<uint32_t> inWrite{0}, inRead{0}, dropped{0};
    std::atomic<Catalog*> cat{nullptr};
    int presetIdx = 0;               // 0 = Init, k = the machine's k-th sound; control thread only
    int snap[kNumSlots] = {};        // the loaded preset's values (slots 1..57), to tell "modified"
    std::string dumpsDir;
    int core = -1;
    FILE* trace = nullptr;   // /tmp/mnm_trace.on present at create: log every set/get to /tmp/mnm_trace.log

    Inst()
    {
        for (auto& p : param) p.store(0);
        loadInit(kDefaultMachine);
        param[kSlotTune].store(440);
        param[kSlotLpk].store(1);   // the hardware's kit default: key tracking on
        param[kSlotHpk].store(1);
    }
    ~Inst() { delete cat.load(); }
    // Init of a machine: its page defaults, default LFOs and level 100.
    void loadInit(int machineSlot)
    {
        int d[32];
        defaultsFor(machineSlot, d);
        param[kSlotMachine].store(machineSlot);
        param[kSlotLevel].store(100);
        for (int i = 0; i < 32; ++i) param[kSlotPages + i].store(d[i]);
        for (int i = 0; i < 24; ++i) param[kSlotLfo + i].store(kLfoDefaults[i % 8]);
        presetIdx = 0;
        markLoaded();
    }
    void markLoaded() { for (int i = 1; i < kSlotTab; ++i) snap[i] = param[i].load(); }
    bool modified() const
    {
        for (int i = 1; i < kSlotTab; ++i) if (param[i].load() != snap[i]) return true;
        return false;
    }
    // the sounds for a machine, sorted by name
    std::vector<int> soundsFor(int model) const
    {
        std::vector<int> out;
        if (const Catalog* c = cat.load()) {
            for (size_t i = 0; i < c->sounds.size(); ++i) if (c->sounds[i].machine == model) out.push_back(int(i));
            std::sort(out.begin(), out.end(), [c](int a, int b) { return strcasecmp(c->sounds[size_t(a)].name, c->sounds[size_t(b)].name) < 0; });
        }
        return out;
    }
    void stepPreset(int dir)
    {
        const int ms = param[kSlotMachine].load();
        const auto list = soundsFor(int(kMachines[ms]));
        const int n = 1 + int(list.size());
        int idx = (presetIdx + dir) % n;
        if (idx < 0) idx += n;
        if (idx == 0) { loadInit(ms); return; }
        const Catalog* c = cat.load();
        const auto& snd = c->sounds[size_t(list[size_t(idx - 1)])];
        for (int i = 0; i < 32; ++i) param[kSlotPages + i].store(snd.params[i]);
        for (int i = 0; i < 24; ++i) param[kSlotLfo + i].store(snd.params[32 + i]);
        param[kSlotLevel].store(snd.level);
        presetIdx = idx;
        markLoaded();
    }
    std::string presetName() const
    {
        const int ms = param[kSlotMachine].load();
        std::string name = "INIT";
        if (presetIdx > 0) {
            const auto list = soundsFor(int(kMachines[ms]));
            const Catalog* c = cat.load();
            if (presetIdx - 1 < int(list.size())) name = c->sounds[size_t(list[size_t(presetIdx - 1)])].name;
        }
        for (auto& ch : name) ch = char(toupper(static_cast<unsigned char>(ch)));
        return modified() ? name + " *" : name;
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
#if MNM_FX
        voice->host().setRouting(host::dspInputBits(host::FxInput::InpAB));
        voice->host().noteOn(60);   // an FX machine runs with its envelope open, as upstream does
#endif
        voice->warmUp(8);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[monomodule] engine failed: %s\n", ex.what());
        return;   // stays silent
    }
    // realtime only once booted, above MPC's AudioWorkers (SCHED_RR 20); MNM_FIFO=0 keeps SCHED_OTHER
    int prio = 30;
    if (const char* e = std::getenv("MNM_FIFO")) prio = std::atoi(e);
    if (prio > 0) { sched_param sp{}; sp.sched_priority = prio; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }
    core = std::getenv("MNM_CPU") ? std::atoi(std::getenv("MNM_CPU")) : chooseCore();
    if (core >= 0) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s); sched_setaffinity(0, sizeof s, &s); }

    auto& h = voice->host();
    int applied[kNumSlots];
    for (int i = 0; i < kNumSlots; ++i) applied[i] = -1;
    int machineSlot = -1;
    int held[16], nHeld = 0;
    bool muted = false;
    uint32_t silentBlocks = 0;
    constexpr uint32_t kParkAfter = 44100 * 2 / kFrames;   // 2 s of exact silence with no note held
    std::vector<float> L(kFrames), R(kFrames);
#if MNM_FX
    {   // hand the host its two blocks of latency now, so its one-block-per-call reads never wait on the DSP thread
        const uint32_t w0 = rWrite.load(std::memory_order_relaxed);
        for (int i = 0; i < kDepth; ++i) std::memset(ring[(w0 + uint32_t(i)) % kRing], 0, sizeof ring[0]);
        rWrite.store(w0 + kDepth, std::memory_order_release);
    }
#endif
    ready.store(true);

    while (!stop.load(std::memory_order_acquire)) {
        const uint32_t w = rWrite.load(std::memory_order_relaxed);
#if MNM_FX
        // an effect can only work on audio that has arrived: one input block in, one output block out
        if (int32_t(inWrite.load(std::memory_order_acquire) - inRead.load(std::memory_order_relaxed)) <= 0) {
            struct timespec ts{0, 300000};
            nanosleep(&ts, nullptr);
            continue;
        }
#else
        if (int32_t(w - rRead.load(std::memory_order_acquire)) >= kAhead) {
            struct timespec ts{0, 400000};
            nanosleep(&ts, nullptr);
            continue;
        }
#endif
        // machine first: it loads that machine's page defaults, which the shadow already holds
        const int ms = std::clamp(param[kSlotMachine].load(std::memory_order_relaxed), 0, kNumMachines - 1);
        if (ms != machineSlot) {
            machineSlot = ms;
            h.setMachine(kMachines[ms]);
#if MNM_FX
            h.setRouting(host::dspInputBits(host::FxInput::InpAB));
            h.noteOn(60);
#else
            if (nHeld == 0) { h.noteOff(); muted = true; }   // the assign's init must not sound
#endif
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
        {   // globals: master tune, filter key tracking
            const int tune = std::clamp(param[kSlotTune].load(std::memory_order_relaxed), 400, 440);
            const int lpk = param[kSlotLpk].load(std::memory_order_relaxed) ? 1 : 0, hpk = param[kSlotHpk].load(std::memory_order_relaxed) ? 1 : 0;
            if (tune != applied[kSlotTune]) { applied[kSlotTune] = tune; h.setMasterTuneHz(double(tune)); }
            if (lpk != applied[kSlotLpk] || hpk != applied[kSlotHpk]) { applied[kSlotLpk] = lpk; applied[kSlotHpk] = hpk; h.setKeyTracking(lpk != 0, hpk != 0); }
        }
        const int lv = std::clamp(param[kSlotLevel].load(std::memory_order_relaxed), 0, 127);
        if (lv != applied[kSlotLevel]) { applied[kSlotLevel] = lv; h.setLevel(lv); }

#if !MNM_FX
        uint32_t r = nRead.load(std::memory_order_relaxed);
        const uint32_t nw = nWrite.load(std::memory_order_acquire);
        for (; r != nw; ++r) {
            const NoteEv e = notes[r & 255];
            if (e.type == 1) {
                muted = false; parked = false; silentBlocks = 0;
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
#endif

        int16_t* out = ring[w % kRing];
#if MNM_FX
        {
            const uint32_t ir = inRead.load(std::memory_order_relaxed);
            const int16_t* in_ = inRing[ir % kRing];
            bool inSilent = true;
            for (int i = 0; i < kFrames * 2; ++i) if (in_[i]) { inSilent = false; break; }
            if (parked && !inSilent) { parked = false; silentBlocks = 0; }
            if (parked) {   // idle: advance the host model only; the tail was silent for 2 s
                voice->skip(kFrames);
                std::memset(out, 0, sizeof(int16_t) * kFrames * 2);
            } else {
                for (int i = 0; i < kFrames; ++i) { L[size_t(i)] = float(in_[2 * i]) * (1.f / 32768.f); R[size_t(i)] = float(in_[2 * i + 1]) * (1.f / 32768.f); }
                std::vector<float> oL(kFrames), oR(kFrames);
                voice->processFx(L.data(), R.data(), oL.data(), oR.data(), kFrames);
                bool silent = inSilent;
                for (int i = 0; i < kFrames; ++i) {
                    out[2 * i] = int16_t(std::lrint(std::clamp(oL[size_t(i)], -1.f, 1.f) * 32767.f));
                    out[2 * i + 1] = int16_t(std::lrint(std::clamp(oR[size_t(i)], -1.f, 1.f) * 32767.f));
                    if (out[2 * i] || out[2 * i + 1]) silent = false;
                }
                if (silent) { if (++silentBlocks >= kParkAfter) parked = true; } else silentBlocks = 0;
            }
            inRead.store(ir + 1, std::memory_order_release);
            rWrite.store(w + 1, std::memory_order_release);
            blocks.fetch_add(1, std::memory_order_relaxed);
            if (voice->engine().faulted()) { std::fprintf(stderr, "[monomodule] DSP fault: %s\n", voice->engine().faultReason().c_str()); return; }
            continue;
        }
#endif
        if (parked) {   // idle: advance the host model only (the DSP's free-running state stops where it was)
            voice->skip(kFrames);
            std::memset(out, 0, sizeof(int16_t) * kFrames * 2);
            rWrite.store(w + 1, std::memory_order_release);
            blocks.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        voice->process(L.data(), R.data(), kFrames);
        for (int i = 0; i < kFrames; ++i) {
            const float l = muted ? 0.f : L[size_t(i)], rr = muted ? 0.f : R[size_t(i)];
            out[2 * i] = int16_t(std::lrint(std::clamp(l, -1.f, 1.f) * 32767.f));
            out[2 * i + 1] = int16_t(std::lrint(std::clamp(rr, -1.f, 1.f) * 32767.f));
        }
        bool silent = true;
        for (int i = 0; i < kFrames * 2; ++i) if (out[i]) { silent = false; break; }
        if (silent && nHeld == 0) { if (++silentBlocks >= kParkAfter) parked = true; } else silentBlocks = 0;
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
    in->dumpsDir = std::string(dataDir && *dataDir ? dataDir : ".") + "/dumps";
    in->catTh = std::thread([in] { in->cat.store(buildCatalog(in->dumpsDir)); });
    in->th = std::thread([in] { in->run(); });
    return in;
}
void eDestroy(void* p)
{
    auto* in = static_cast<Inst*>(p);
    in->stop.store(true);
    if (in->th.joinable()) in->th.join();
    if (in->catTh.joinable()) in->catTh.join();
    if (in->core >= 0) g_usedCores.fetch_and(~(1u << in->core));
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
    if (!MNM_FX && !std::strcmp(key, "randomize_syn") && std::atof(val) > 0.5) {
        for (int k = 0; k < 8; ++k) if (k != 7) in->param[kSlotPages + k].store(std::rand() % 128);   // SYN A-G (H is TUNE on every pitched machine)
        return;
    }
    if (!MNM_FX && !std::strcmp(key, "randomize_ampfilt") && std::atof(val) > 0.5) {
        for (int k = 0; k < 8; ++k) if (k != 5) in->param[kSlotPages + 8 + k].store(std::rand() % 128);   // AMP except VOL
        for (int k = 0; k < 8; ++k) in->param[kSlotPages + 16 + k].store(std::rand() % 128);              // FILT
        return;
    }
    if (!MNM_FX && !std::strcmp(key, "randomize_lfo") && std::atof(val) > 0.5) {
        for (int k = 0; k < 16; ++k) in->param[kSlotLfo + k].store(std::rand() % 128);                     // LFO1 + LFO2
        return;
    }
    if (!std::strcmp(key, "preset_prev") || !std::strcmp(key, "preset_next")) {
        if (std::atof(val) > 0.5) in->stepPreset(key[7] == 'n' ? 1 : -1);
        return;
    }
    if (!std::strncmp(key, "lfo", 3) && key[3] >= '1' && key[3] <= '3' && !std::strcmp(key + 4, "_pagesel")) {
        const int idx = std::clamp(int(std::lround(std::atof(val))), 0, 8);
        in->param[kSlotLfo + (key[3] - '1') * 8].store(listRawMid9(idx));
        return;
    }
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
        in->presetIdx = 0;   // a new machine starts from its Init
        in->markLoaded();
        return;
    }
    in->param[s].store(s == kSlotTune ? std::clamp(v, 400, 440) : s == kSlotLpk || s == kSlotHpk ? (v ? 1 : 0) : std::clamp(v, 0, 127));
}
int eGet(void* p, const char* key, char* buf, int len)
{
    auto* in = static_cast<Inst*>(p);
    if (!std::strncmp(key, "randomize_", 10)) return std::snprintf(buf, size_t(len), "0");   // momentary: always reads back off
    if (!std::strcmp(key, "preset_name")) return std::snprintf(buf, size_t(len), "%s", in->presetName().c_str());
    if (!std::strcmp(key, "preset_prev") || !std::strcmp(key, "preset_next")) return std::snprintf(buf, size_t(len), "0");
    if (!std::strcmp(key, "lfo23dest")) {   // DEST's view of the LFO2|LFO3 page: tab * 9 + that LFO's PAGE list index
        const int tab = std::clamp(in->param[kSlotTab].load(), 0, 1);
        return std::snprintf(buf, size_t(len), "%d", tab * 9 + listIndex9(in->param[kSlotLfo + (1 + tab) * 8].load()));
    }
    if (!std::strncmp(key, "lfo", 3) && key[3] >= '1' && key[3] <= '3' && !std::strcmp(key + 4, "_pagesel"))
        return std::snprintf(buf, size_t(len), "%d", listIndex9(in->param[kSlotLfo + (key[3] - '1') * 8].load()));
    const int s = slotOf(key);
    if (s >= 0) {
        const int n = std::snprintf(buf, size_t(len), "%d", in->param[s].load());
        if (in->trace) { std::fprintf(in->trace, "get %s = %s\n", key, buf); std::fflush(in->trace); }
        return n;
    }
    if (!std::strcmp(key, "underruns")) return std::snprintf(buf, size_t(len), "%u", in->underruns.load());
    if (!std::strcmp(key, "core")) return std::snprintf(buf, size_t(len), "%d", in->core);
    if (!std::strcmp(key, "parked")) return std::snprintf(buf, size_t(len), "%d", int(in->parked.load()));
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

// FX: the host's audio goes into a small ring; the DSP thread works through it and the host reads finished blocks back
void eProcess(void* p, const int16_t* inp, int16_t* out, int frames)
{
    auto* in = static_cast<Inst*>(p);
    if (frames != kFrames) { std::memset(out, 0, sizeof(int16_t) * size_t(frames) * 2); return; }
    const uint32_t w = in->inWrite.load(std::memory_order_relaxed);
    if (int32_t(w - in->inRead.load(std::memory_order_acquire)) < kRing) {
        std::memcpy(in->inRing[w % kRing], inp, sizeof(int16_t) * kFrames * 2);
        in->inWrite.store(w + 1, std::memory_order_release);
    } else {
        in->dropped.fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t r = in->rRead.load(std::memory_order_relaxed);
    if (int32_t(in->rWrite.load(std::memory_order_acquire) - r) > 0) {
        std::memcpy(out, in->ring[r % kRing], sizeof(int16_t) * kFrames * 2);
        in->rRead.store(r + 1, std::memory_order_release);
    } else {
        std::memset(out, 0, sizeof(int16_t) * kFrames * 2);
        if (in->ready.load(std::memory_order_relaxed)) in->underruns.fetch_add(1, std::memory_order_relaxed);
    }
}

const mpc_engine_t kEngine = {eCreate, eDestroy, eMidi, eSet, eGet, eRender, MNM_FX ? eProcess : nullptr};

} // namespace

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }

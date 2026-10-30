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
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
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

#include "skin_art.h"
#include "skin_recipe.h"

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
constexpr int kSlotMachine = 0, kSlotLevel = 1, kSlotPages = 2, kSlotLfo = 34, kSlotTab = 58, kSlotTune = 59, kSlotLpk = 60, kSlotHpk = 61, kSlotVoices = 62, kNumSlots = 63;
#ifdef DSP56K_RECOMP
constexpr int kMaxVoices = 1;   // Gen1: one engine already needs a core
#else
constexpr int kMaxVoices = 6;   // Gen2: POLY, up to six engines (bench: about 0.3 core each)
#endif
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
    if (!std::strcmp(key, "voices")) return kSlotVoices;      // POLY: 1 = the classic monophonic voice, 2..6 = notes spread over that many engines
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
    char bank[24];   // the .syx file it came from (basename, no extension), for the bank selector
    int machine;    // host::Machine model
    int level;
    uint8_t params[56];   // SYN AMP FILT EFX (32) + LFO 1-3 (24), raw
};
struct Catalog { std::vector<PresetSound> sounds; std::vector<std::string> banks; uint64_t signature = 0; };

// A cheap signature of every .syx in the watched folders (name, size, mtime): scanDumps() re-samples this every
// few seconds on a background thread, and only re-parses (buildCatalog) when it actually changes.
uint64_t scanDumps(const std::vector<std::string>& dirs, std::vector<std::string>* files)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    std::vector<std::string> found;
    for (const auto& dir : dirs) {
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                const size_t n = std::strlen(e->d_name);
                if (n > 4 && strcasecmp(e->d_name + n - 4, ".syx") == 0) found.push_back(dir + "/" + e->d_name);
            }
            closedir(d);
        }
    }
    std::sort(found.begin(), found.end());
    for (const auto& path : found) {
        for (char c : path) mix(uint8_t(c));
        struct stat st{};
        if (stat(path.c_str(), &st) == 0) { mix(uint64_t(st.st_size)); mix(uint64_t(st.st_mtime)); }
    }
    if (files) *files = found;
    return h;
}

Catalog* buildCatalog(const std::vector<std::string>& dirs, uint64_t sig)
{
    auto* cat = new Catalog();
    cat->signature = sig;
    std::vector<std::string> files;
    scanDumps(dirs, &files);
    std::vector<std::string> seen;
    for (const auto& path : files) {
        std::string bank = path.substr(path.find_last_of('/') + 1);
        const auto dot = bank.find_last_of('.');
        if (dot != std::string::npos) bank.resize(dot);
        for (auto& ch : bank) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        if (bank.size() > 23) bank.resize(23);
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
                std::snprintf(snd.bank, sizeof snd.bank, "%s", bank.c_str());
                snd.machine = tr.model;
                snd.level = std::min<int>(tr.level, 127);
                std::memcpy(snd.params, tr.params, 56);
                for (auto& b : snd.params) b = std::min<uint8_t>(b, 127);
                cat->sounds.push_back(snd);
            }
        }
        if (std::find(cat->banks.begin(), cat->banks.end(), bank) == cat->banks.end()) cat->banks.push_back(bank);
    }
    std::sort(cat->banks.begin(), cat->banks.end());
    return cat;
}

struct Inst {
    std::string osPath, dataDir;
    std::atomic<int> param[kNumSlots];
    std::atomic<bool> stop{false}, ready{false}, parked{false}, maxVoicesFailed{false};
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
    std::string bankName;            // "" = ALL (every watched bank pooled); else one bank from Catalog::banks
    int snap[kNumSlots] = {};        // the loaded preset's values (slots 1..57), to tell "modified"
    std::vector<std::string> dumpDirs;
    std::vector<Catalog*> retiredCats;   // freed at destroy, not on rescan: a concurrent get_param() may still hold the old pointer briefly
    int core = -1;
    FILE* trace = nullptr;   // /tmp/mnm_trace.on present at create: log every set/get to /tmp/mnm_trace.log

    Inst()
    {
        for (auto& p : param) p.store(0);
        loadInit(kDefaultMachine);
        param[kSlotTune].store(440);
        param[kSlotLpk].store(1);   // the hardware's kit default: key tracking on
        param[kSlotHpk].store(1);
        param[kSlotVoices].store(1);
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
    // MPC probes the machine parameter (set another value, then straight back to the original) around the autosampler and
    // similar; a machine change resets the pages, so a quick A -> B -> A would wipe the patch. Remember the sound that was
    // replaced and put it back if its machine returns right away.
    struct Stash { int machine = -1; int param[kSlotTab]; int snapv[kSlotTab]; int presetIdx = 0; std::chrono::steady_clock::time_point t; } stash;
    void stashSound()
    {
        stash.machine = param[kSlotMachine].load();
        for (int i = 1; i < kSlotTab; ++i) { stash.param[i] = param[i].load(); stash.snapv[i] = snap[i]; }
        stash.presetIdx = presetIdx;
        stash.t = std::chrono::steady_clock::now();
    }
    bool unstashSound(int machine)
    {
        using namespace std::chrono;
        if (stash.machine != machine || steady_clock::now() - stash.t > milliseconds(500)) return false;
        const Stash old = stash;
        stashSound();
        for (int i = 1; i < kSlotTab; ++i) { param[i].store(old.param[i]); snap[i] = old.snapv[i]; }
        presetIdx = old.presetIdx;
        param[kSlotMachine].store(machine);
        stash.machine = -1;
        return true;
    }
    bool modified() const
    {
        for (int i = 1; i < kSlotTab; ++i) if (param[i].load() != snap[i]) return true;
        return false;
    }
    // the sounds for a machine in the selected bank ("" = every bank), sorted by name
    std::vector<int> soundsFor(int model) const
    {
        std::vector<int> out;
        if (const Catalog* c = cat.load()) {
            for (size_t i = 0; i < c->sounds.size(); ++i)
                if (c->sounds[i].machine == model && (bankName.empty() || bankName == c->sounds[i].bank)) out.push_back(int(i));
            std::sort(out.begin(), out.end(), [c](int a, int b) { return strcasecmp(c->sounds[size_t(a)].name, c->sounds[size_t(b)].name) < 0; });
        }
        return out;
    }
    // bank names in cycle order: "" (ALL) first, then Catalog::banks alphabetically
    std::vector<std::string> bankList() const
    {
        std::vector<std::string> out{""};
        if (const Catalog* c = cat.load()) for (auto& b : c->banks) out.push_back(b);
        return out;
    }
    int bankIndex() const
    {
        const auto banks = bankList();
        for (size_t i = 0; i < banks.size(); ++i) if (banks[i] == bankName) return int(i);
        return 0;
    }
    void selectBank(int idx)
    {
        const auto banks = bankList();
        if (idx < 0 || idx >= int(banks.size())) return;
        bankName = banks[size_t(idx)];
        presetIdx = 0;   // the current selection may not exist in the new bank: fall back to Init, as a machine change does
        pageSeen = -1;
        markLoaded();
    }
    void stepBank(int dir)
    {
        const int n = int(bankList().size());
        selectBank(((bankIndex() + dir) % n + n) % n);
    }
    std::string bankLabel() const { return bankName.empty() ? "ALL" : bankName; }
    void stepPreset(int dir)
    {
        const int ms = param[kSlotMachine].load();
        const int n = 1 + int(soundsFor(int(kMachines[ms])).size());
        selectPreset((presetIdx + dir + n) % n);
    }
    // the preset list popup: kPickSlots entries per page, entry 0 of page 0 is Init
    static constexpr int kPickSlots = 32, kBankSlots = 24;   // entries per page: the preset list is 4 x 8, the bank list 3 x 8
    int pageNum = 0, pageSeen = 0, bankPageNum = 0, bankPageSeen = 0;
    std::mutex pickMu;
    std::vector<std::string> pickNames;   // INIT + the machine's sounds in the selected bank, upper case; rebuilt when the key changes
    const Catalog* pickCat = nullptr; int pickMachine = -1; std::string pickBank;
    const std::vector<std::string>& pickList()   // pickMu held
    {
        const int ms = param[kSlotMachine].load();
        const Catalog* c = cat.load();
        if (c != pickCat || ms != pickMachine || bankName != pickBank || pickNames.empty()) {
            pickCat = c; pickMachine = ms; pickBank = bankName;
            pickNames.assign(1, "INIT");
            for (int i : soundsFor(int(kMachines[ms]))) {
                std::string n = c->sounds[size_t(i)].name;
                for (auto& ch : n) ch = char(toupper(static_cast<unsigned char>(ch)));
                pickNames.push_back(n);
            }
        }
        return pickNames;
    }
    int pages() { std::lock_guard<std::mutex> g(pickMu); return (int(pickList().size()) + kPickSlots - 1) / kPickSlots; }
    int page()   // follows the current preset until the pager is used
    {
        if (presetIdx != pageSeen) { pageSeen = presetIdx; pageNum = presetIdx / kPickSlots; }
        return pageNum;
    }
    int bankPages() const { return (int(bankList().size()) + kBankSlots - 1) / kBankSlots; }
    int bankPage()
    {
        const int bi = bankIndex();
        if (bi != bankPageSeen) { bankPageSeen = bi; bankPageNum = bi / kBankSlots; }
        return bankPageNum;
    }
    void stepBankPage(int dir) { const int p = bankPage() + dir; if (p >= 0 && p < bankPages()) bankPageNum = p; }
    void stepPage(int dir) { const int p = page() + dir; if (p >= 0 && p < pages()) pageNum = p; }
    std::string pickEntry(int k)
    {
        std::lock_guard<std::mutex> g(pickMu);
        const auto& l = pickList();
        const size_t i = size_t(page()) * kPickSlots + size_t(k);
        return i < l.size() ? l[i] : std::string();
    }
    void pickPreset(int k)
    {
        std::unique_lock<std::mutex> g(pickMu);
        const int idx = page() * kPickSlots + k;
        const bool ok = k >= 0 && k < kPickSlots && idx < int(pickList().size());
        g.unlock();
        if (ok) selectPreset(idx);
    }
    void selectPreset(int idx)
    {
        const int ms = param[kSlotMachine].load();
        const auto list = soundsFor(int(kMachines[ms]));
        if (idx < 0 || idx > int(list.size())) return;
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
    // The whole instance for the host's project/program chunk: "v1|<all slots>|<snap 1..57>|<presetIdx>|<bank>".
    std::string saveState() const
    {
        std::string o = "v1|";
        for (int i = 0; i < kNumSlots; ++i) { if (i) o += ','; o += std::to_string(param[i].load()); }
        o += '|';
        for (int i = 1; i < kSlotTab; ++i) { if (i > 1) o += ','; o += std::to_string(snap[i]); }
        o += '|' + std::to_string(presetIdx) + '|' + bankName;
        return o;
    }
    void loadState(const char* str)
    {
        if (std::strncmp(str, "v1|", 3)) return;
        const char* c = str + 3;
        int vals[kNumSlots], sn[kNumSlots] = {};
        int nSlots = 1;   // an older state has one slot fewer (no voices); count the commas of the first field
        for (const char* q = c; *q && *q != '|'; ++q) if (*q == ',') ++nSlots;
        if (nSlots < kSlotVoices || nSlots > kNumSlots) return;
        for (int i = nSlots; i < kNumSlots; ++i) vals[i] = param[i].load();
        auto parse = [&c](int* dst, int n, int first) {
            for (int i = first; i < first + n; ++i) {
                char* e; dst[i] = int(std::strtol(c, &e, 10));
                if (e == c) return false;
                c = e; if (*c == ',') ++c;
            }
            return true;
        };
        if (!parse(vals, nSlots, 0) || *c++ != '|' || !parse(sn, kSlotTab - 1, 1) || *c++ != '|') return;
        char* e; const int idx = int(std::strtol(c, &e, 10));
        if (e == c || *e != '|') return;
        bankName = e + 1;
        vals[kSlotMachine] = std::clamp(vals[kSlotMachine], 0, kNumMachines - 1);
        vals[kSlotTune] = std::clamp(vals[kSlotTune], 400, 440);
        for (int i = 1; i < kNumSlots; ++i) if (i != kSlotTune) vals[i] = i == kSlotVoices ? std::clamp(vals[i], 1, kMaxVoices) : std::clamp(vals[i], 0, (i == kSlotLpk || i == kSlotHpk) ? 1 : 127);
        for (int i = 0; i < kNumSlots; ++i) param[i].store(vals[i]);
        for (int i = 1; i < kSlotTab; ++i) snap[i] = sn[i];
        presetIdx = std::max(idx, 0);
    }
    void pushNote(int8_t t, int8_t n)
    {
        const uint32_t w = nWrite.load(std::memory_order_relaxed);
        if (w - nRead.load(std::memory_order_acquire) >= 256) return;
        notes[w & 255] = {t, n};
        nWrite.store(w + 1, std::memory_order_release);
    }
    void run();
    void runVoices(MonoVoice& v0, const fw::Firmware& firmware, int prio);
};

// The skin, made on the device (the Gen2 download carries no Elektron art): <plugin folder>/skin-recipe/art.bin (vst/skin/skin_recipe.py)
// says which skin pixel follows which bit of the LCD artwork, and the user's OS file gives the bits (vst/skin_recipe.h). Writes the
// images of every Monomodule plugin folder found next to this one that has not got them yet, once (a marker file in each). MPC reads
// skins when it starts, so the new skin shows from the next start. No recipe: nothing to do (Gen1 packages carry the finished skin).
static void makeSkins(const std::string& dataDir, const std::string& osPath)
{
    const auto cut = [](const std::string& p) { const size_t s = p.find_last_of('/'); return s == std::string::npos ? std::string(".") : p.substr(0, s); };
    const std::string recipe = cut(dataDir) + "/skin-recipe/art.bin";   // <Synths>/<plugin folder>/skin-recipe (dataDir: .../monomodule)
    struct stat st{}, os{};
    if (stat(recipe.c_str(), &st) != 0 || stat(osPath.c_str(), &os) != 0) return;
    const std::string synths = cut(cut(dataDir));
    const std::string stamp = std::to_string(os.st_size) + " " + std::to_string(st.st_size) + " " + std::to_string(st.st_mtime);
    std::vector<std::string> made;
    try {
        const auto bits = mnm::skinart::bits(osPath);
        std::string err;
        const int n = mnm::skin::fill(recipe, bits, [&](const std::string&, const std::string& folder) -> std::string {
            const std::string dir = synths + "/" + folder;
            if (!mnm::skin::isDir(dir)) return {};   // that plugin is not installed
            std::vector<uint8_t> have;
            if (mnm::skin::readFile(dir + "/" + mnm::skin::kMarker, have) && std::string(have.begin(), have.end()) == stamp) return {};
            if (std::find(made.begin(), made.end(), dir) == made.end()) made.push_back(dir);
            return dir;
        }, err);
        if (n < 0) { std::fprintf(stderr, "[monomodule] skin: %s\n", err.c_str()); return; }
        for (const auto& dir : made)
            if (std::FILE* f = std::fopen((dir + "/" + mnm::skin::kMarker).c_str(), "w")) { std::fputs(stamp.c_str(), f); std::fclose(f); }
        if (n) std::fprintf(stderr, "[monomodule] skin: %d images made from your OS file (shown from MPC's next start)\n", n);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[monomodule] skin: %s\n", e.what());
    }
}

std::unique_ptr<fw::Firmware> loadFw(const std::string& path)
{
    return std::make_unique<fw::Firmware>(fw::loadFirmware(path));
}

// ---- the voice pool (instruments only): voice 0 is the classic voice and renders on the DSP thread; with VOICES > 1, up to five more
// MonoVoice engines are built in the background, each rendering on a thread of its own, and notes are spread over them.
namespace {
struct VSlot {
    MonoVoice* v = nullptr;
    int applied[kNumSlots];
    int machineSlot = -1;
    bool muted = false, parked = false;
    uint32_t silent = 0;
    int held[16], nHeld = 0;
    NoteEv pend[32], ops[32];       // pend: queued by the DSP thread; ops: what the render of the next block works through
    int nPend = 0, nOps = 0;
    float L[kFrames], R[kFrames];
    std::atomic<uint32_t> go{0}, done{0};
    std::atomic<int> lastUs{0}, cpu{-1};   // MNM_DEBUG
    std::unique_ptr<MonoVoice> own;
    std::thread th;
    int note = -1;                  // the allocator's: the note this voice plays, -1 = free
    uint32_t age = 0;
    VSlot() { for (auto& a : applied) a = -1; }
};
// The DSP thread is pinned to one core and a thread it creates inherits that: the voices and their builder run on any core but 0 (MPC's UI core).
void freeAffinity(int exceptCore)
{
    cpu_set_t cs; CPU_ZERO(&cs);
    const int ncpu = int(sysconf(_SC_NPROCESSORS_ONLN));
    for (int c = ncpu > 1 ? 1 : 0; c < ncpu; ++c) if (c != exceptCore || ncpu <= 2) CPU_SET(c, &cs);
    sched_setaffinity(0, sizeof cs, &cs);
}
constexpr uint32_t kParkAfterBlocks = 44100 * 2 / kFrames;   // 2 s of exact silence with no note held

// One voice's block: its parameters, its notes, then the DSP (or, parked, only the host model).
void renderSlot(Inst& in, VSlot& s)
{
    auto& h = s.v->host();
    const int ms = std::clamp(in.param[kSlotMachine].load(std::memory_order_relaxed), 0, kNumMachines - 1);
    if (ms != s.machineSlot) {
        s.machineSlot = ms;
        h.setMachine(kMachines[ms]);
        if (s.nHeld == 0) { h.noteOff(); s.muted = true; }   // the assign's init must not sound
        for (int i = 0; i < 32; ++i) s.applied[kSlotPages + i] = h.param(host::Page(i / 8), i % 8);
    }
    for (int i = 0; i < 32; ++i) {
        const int v = std::clamp(in.param[kSlotPages + i].load(std::memory_order_relaxed), 0, 127);
        if (v != s.applied[kSlotPages + i]) { s.applied[kSlotPages + i] = v; h.setParam(host::Page(i / 8), i % 8, v); }
    }
    for (int i = 0; i < 24; ++i) {
        const int v = std::clamp(in.param[kSlotLfo + i].load(std::memory_order_relaxed), 0, 127);
        if (v != s.applied[kSlotLfo + i]) { s.applied[kSlotLfo + i] = v; h.setLfoParam(i / 8, i % 8, v); }
    }
    {
        const int tune = std::clamp(in.param[kSlotTune].load(std::memory_order_relaxed), 400, 440);
        const int lpk = in.param[kSlotLpk].load(std::memory_order_relaxed) ? 1 : 0, hpk = in.param[kSlotHpk].load(std::memory_order_relaxed) ? 1 : 0;
        if (tune != s.applied[kSlotTune]) { s.applied[kSlotTune] = tune; h.setMasterTuneHz(double(tune)); }
        if (lpk != s.applied[kSlotLpk] || hpk != s.applied[kSlotHpk]) { s.applied[kSlotLpk] = lpk; s.applied[kSlotHpk] = hpk; h.setKeyTracking(lpk != 0, hpk != 0); }
    }
    const int lv = std::clamp(in.param[kSlotLevel].load(std::memory_order_relaxed), 0, 127);
    if (lv != s.applied[kSlotLevel]) { s.applied[kSlotLevel] = lv; h.setLevel(lv); }

    for (int k = 0; k < s.nOps; ++k) {
        const NoteEv e = s.ops[k];
        if (e.type == 1) {
            s.muted = false; s.parked = false; s.silent = 0;
            int n = 0;
            for (int i = 0; i < s.nHeld; ++i) if (s.held[i] != e.note) s.held[n++] = s.held[i];
            s.nHeld = n;
            if (s.nHeld < 16) s.held[s.nHeld++] = e.note;
            h.noteOn(e.note);
        } else if (e.type == 2) {
            const bool cur = s.nHeld > 0 && s.held[s.nHeld - 1] == e.note;
            int n = 0;
            for (int i = 0; i < s.nHeld; ++i) if (s.held[i] != e.note) s.held[n++] = s.held[i];
            s.nHeld = n;
            if (s.nHeld == 0) h.noteOff();
            else if (cur) h.noteOn(s.held[s.nHeld - 1]);
        } else { s.nHeld = 0; h.noteOff(); }
    }
    s.nOps = 0;

    if (s.parked) {   // idle: advance the host model only (the DSP's free-running state stops where it was)
        s.v->skip(kFrames);
        std::memset(s.L, 0, sizeof s.L); std::memset(s.R, 0, sizeof s.R);
        return;
    }
    s.v->process(s.L, s.R, kFrames);
    bool silent = true;
    for (int i = 0; i < kFrames; ++i) {
        if (s.muted) { s.L[i] = 0.f; s.R[i] = 0.f; }
        if (std::lrint(std::clamp(s.L[i], -1.f, 1.f) * 32767.f) || std::lrint(std::clamp(s.R[i], -1.f, 1.f) * 32767.f)) silent = false;
    }
    if (silent && s.nHeld == 0) { if (++s.silent >= kParkAfterBlocks) s.parked = true; } else s.silent = 0;
}
} // namespace

void Inst::runVoices(MonoVoice& v0, const fw::Firmware& firmware, int prio)
{
    const int maxV = std::clamp(std::getenv("MNM_MAXVOICES") ? std::atoi(std::getenv("MNM_MAXVOICES")) : kMaxVoices, 1, kMaxVoices);
    auto slots = std::make_unique<VSlot[]>(size_t(kMaxVoices));
    slots[0].v = &v0;
    std::atomic<int> built{1};
    std::thread builder;
    bool building = false;
    auto buildNext = [&] {
        const int k = built.load();
        try {
            auto v = std::make_unique<MonoVoice>(firmware);
#ifndef DSP56K_RECOMP
            if (!std::getenv("MNM_PREWARM") || std::atoi(std::getenv("MNM_PREWARM")) != 0) v->prewarm();
#endif
            v->host().setMachine(kMachines[param[kSlotMachine].load()]);
            v->warmUp(8);
            VSlot& s = slots[size_t(k)];
            s.own = std::move(v);
            s.v = s.own.get();
            s.th = std::thread([this, &s, prio] {
                pthread_setname_np(pthread_self(), "mnm-voice");
                freeAffinity(core);
                if (prio > 0) { sched_param sp{}; sp.sched_priority = prio > 1 ? prio - 1 : prio; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }   // just below the DSP thread, which must never wait for a voice
                while (!stop.load(std::memory_order_acquire)) {
                    const uint32_t g = s.go.load(std::memory_order_acquire);
                    if (g == s.done.load(std::memory_order_relaxed)) { struct timespec ts{0, 20000}; nanosleep(&ts, nullptr); continue; }
                    const auto w0 = std::chrono::steady_clock::now();
                    renderSlot(*this, s);
                    s.lastUs.store(int(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count())); s.cpu.store(sched_getcpu());
                    s.done.store(g, std::memory_order_release);
                }
            });
            built.store(k + 1, std::memory_order_release);
        } catch (const std::exception& ex) { std::fprintf(stderr, "[monomodule] voice %d failed: %s\n", k + 1, ex.what()); maxVoicesFailed.store(true); }
    };

    int lastNv = 1;
    uint32_t clock = 0;
    double sumUs = 0, sumV0 = 0, worstUs = 0; long nBlk = 0, nLate = 0, nSlow = 0;   // MNM_DEBUG
    const bool dbg = std::getenv("MNM_DEBUG") != nullptr;
    int afterNote = 0;
    auto lastIter = std::chrono::steady_clock::now();
    float gain = 1.f;
    while (!stop.load(std::memory_order_acquire)) {
        const uint32_t w = rWrite.load(std::memory_order_relaxed);
        if (dbg) { const auto n0 = std::chrono::steady_clock::now(); const double g = std::chrono::duration<double, std::milli>(n0 - lastIter).count(); if (g > 20) std::fprintf(stderr, "[monomodule] loop gap %.0f ms, ring ahead %d, cpu %d\n", g, int32_t(w - rRead.load()), sched_getcpu()); lastIter = n0; }
        if (int32_t(w - rRead.load(std::memory_order_acquire)) >= kAhead) {
            struct timespec ts{0, 400000};
            nanosleep(&ts, nullptr);
            continue;
        }
        // VOICES: build the extra engines in the background; the voices in play are as many as exist
        const int want = std::min(param[kSlotVoices].load(std::memory_order_relaxed), maxV);
        if (want > built.load(std::memory_order_acquire) && !maxVoicesFailed.load()) {
            if (!building) { building = true; builder = std::thread([&] { freeAffinity(core); while (built.load() < want && !stop.load() && !maxVoicesFailed.load()) buildNext(); }); }
        } else if (building && builder.joinable() && built.load() >= want) { builder.join(); building = false; }
        const int nv = std::min(want, built.load(std::memory_order_acquire));
        if (nv != lastNv) {
            for (int k = nv; k < lastNv; ++k) { slots[size_t(k)].note = -1; if (slots[size_t(k)].nPend < 32) slots[size_t(k)].pend[slots[size_t(k)].nPend++] = NoteEv{3, 0}; }
            lastNv = nv;
            gain = nv > 1 ? 1.f / std::sqrt(float(nv)) : 1.f;
        }

        uint32_t r = nRead.load(std::memory_order_relaxed);
        const uint32_t nw = nWrite.load(std::memory_order_acquire);
        auto post = [&](int k, int8_t type, int8_t note) { VSlot& s = slots[size_t(k)]; if (s.nPend < 32) s.pend[s.nPend++] = NoteEv{type, note}; };
        for (; r != nw; ++r) {
            const NoteEv e = notes[r & 255];
            if (nv == 1) { post(0, e.type, e.note); continue; }
            if (e.type == 1) {
                int best = -1;
                for (int k = 0; k < nv; ++k) if (slots[size_t(k)].note == e.note) { best = k; break; }   // the same note again: retrigger its voice
                for (int k = 0; k < nv && best < 0; ++k) if (slots[size_t(k)].note < 0 && (best < 0 || slots[size_t(k)].age < slots[size_t(best)].age)) best = k;
                if (best < 0) {   // all in use: steal the oldest
                    for (int k = 0; k < nv; ++k) if (best < 0 || slots[size_t(k)].age < slots[size_t(best)].age) best = k;
                    post(best, 2, int8_t(slots[size_t(best)].note));
                }
                slots[size_t(best)].note = e.note; slots[size_t(best)].age = ++clock;
                post(best, 1, e.note);
            } else if (e.type == 2) {
                int oldest = -1;
                for (int k = 0; k < nv; ++k) if (slots[size_t(k)].note == e.note && (oldest < 0 || slots[size_t(k)].age < slots[size_t(oldest)].age)) oldest = k;
                if (oldest < 0) continue;   // stolen earlier
                slots[size_t(oldest)].note = -1; slots[size_t(oldest)].age = ++clock;
                post(oldest, 2, e.note);
            } else {
                for (int k = 0; k < nv; ++k) { slots[size_t(k)].note = -1; post(k, 3, 0); }
            }
        }
        if (dbg && r != nRead.load(std::memory_order_relaxed)) afterNote = 40;
        nRead.store(r, std::memory_order_release);

        // hand each idle worker its notes and start it; voice 0 renders here meanwhile
        for (int k = 1; k < nv; ++k) {
            VSlot& s = slots[size_t(k)];
            if (s.done.load(std::memory_order_acquire) != s.go.load(std::memory_order_relaxed)) continue;   // still on the last block: its notes wait
            std::memcpy(s.ops, s.pend, sizeof(NoteEv) * size_t(s.nPend)); s.nOps = s.nPend; s.nPend = 0;
            s.go.store(s.go.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        }
        const auto tb = std::chrono::steady_clock::now();
        {
            VSlot& s = slots[0];
            std::memcpy(s.ops, s.pend, sizeof(NoteEv) * size_t(s.nPend)); s.nOps = s.nPend; s.nPend = 0;
            renderSlot(*this, s);
        }
        const auto tv0 = std::chrono::steady_clock::now();
        std::array<bool, kMaxVoices> got{}; got[0] = true;
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 1; k < nv; ++k) {
            VSlot& s = slots[size_t(k)];
            while (s.done.load(std::memory_order_acquire) != s.go.load(std::memory_order_relaxed)
                   && std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(2200)) {
                struct timespec ts{0, 10000}; nanosleep(&ts, nullptr);
            }
            got[size_t(k)] = s.done.load(std::memory_order_acquire) == s.go.load(std::memory_order_relaxed);   // a late voice is left out of this block
        }

        if (dbg && nv > 1) {
            const auto te = std::chrono::steady_clock::now();
            const double us = std::chrono::duration<double, std::micro>(te - tb).count();
            sumUs += us; worstUs = std::max(worstUs, us); sumV0 += std::chrono::duration<double, std::micro>(tv0 - tb).count(); ++nBlk;
            for (int k = 1; k < nv; ++k) if (!got[size_t(k)]) { ++nLate; break; }
            if (afterNote > 0) { --afterNote; std::fprintf(stderr, "[monomodule] after note: block %.0f us (voice 0 %.0f), ring ahead %d, nv %d\n", us, std::chrono::duration<double, std::micro>(tv0 - tb).count(), int32_t(w - rRead.load()), nv); }
            if (us > 2000 && nSlow++ < 40) { std::fprintf(stderr, "[monomodule] slow block %ld: %.0f us (voice 0 %.0f) late:", nBlk, us, std::chrono::duration<double, std::micro>(tv0 - tb).count()); for (int k = 1; k < nv; ++k) std::fprintf(stderr, " [v%d late=%d last=%dus cpu=%d]", k, got[size_t(k)] ? 0 : 1, slots[size_t(k)].lastUs.load(), slots[size_t(k)].cpu.load()); std::fprintf(stderr, " main cpu %d\n", sched_getcpu()); }
            if (nBlk % 300 == 0) std::fprintf(stderr, "[monomodule] poly: %d voices, block mean %.0f us (voice 0 %.0f), worst %.0f, late %ld / %ld\n", nv, sumUs / nBlk, sumV0 / nBlk, worstUs, nLate, nBlk);
        }
        if (dbg) { const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb).count(); if (t > 20) std::fprintf(stderr, "[monomodule] block took %.0f ms (voice 0 %.0f ms)\n", t, std::chrono::duration<double, std::milli>(tv0 - tb).count()); }
        int16_t* out = ring[w % kRing];
        for (int i = 0; i < kFrames; ++i) {
            float l = 0.f, rr = 0.f;
            for (int k = 0; k < nv; ++k) if (got[size_t(k)]) { l += slots[size_t(k)].L[i]; rr += slots[size_t(k)].R[i]; }
            out[2 * i] = int16_t(std::lrint(std::clamp(l * gain, -1.f, 1.f) * 32767.f));
            out[2 * i + 1] = int16_t(std::lrint(std::clamp(rr * gain, -1.f, 1.f) * 32767.f));
        }
        bool allParked = true;
        for (int k = 0; k < nv; ++k) allParked = allParked && slots[size_t(k)].parked;
        parked.store(allParked);
        rWrite.store(w + 1, std::memory_order_release);
        blocks.fetch_add(1, std::memory_order_relaxed);
        for (int k = 0; k < nv; ++k) if (slots[size_t(k)].v->engine().faulted()) { std::fprintf(stderr, "[monomodule] DSP fault: %s\n", slots[size_t(k)].v->engine().faultReason().c_str()); return; }
    }
    if (builder.joinable()) builder.join();
    for (int k = 1; k < kMaxVoices; ++k) if (slots[size_t(k)].th.joinable()) slots[size_t(k)].th.join();
}

void Inst::run()
{
    pthread_setname_np(pthread_self(), "mnm-dsp");
    std::unique_ptr<fw::Firmware> firmware;
    std::unique_ptr<MonoVoice> voice;
    try {
        makeSkins(dataDir, osPath);
        firmware = loadFw(osPath);
        voice = std::make_unique<MonoVoice>(*firmware);
#ifndef DSP56K_RECOMP
        // The JIT compiles a machine's code the first time it runs (5-30 ms, heard as a dropout at a machine's first note or when
        // the machine changes). Render every machine once, silently, before "ready", then back to a fresh voice with the code kept
        // (about 0.25 s per instance on x86; bit-exact, mnm-golden MNM_PREWARM=1). MNM_PREWARM=0 skips it.
        if (!std::getenv("MNM_PREWARM") || std::atoi(std::getenv("MNM_PREWARM")) != 0) voice->prewarm();
#endif
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
#if !MNM_FX
    runVoices(*voice, *firmware, prio);
    return;
#endif

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
#ifdef DSP56K_RECOMP
    // Gen1: the JIT has no 32-bit ARM backend; the recompiled interpreter is the only one that works there. Gen2 (aarch64) runs the JIT.
    setenv("MNM_DSP_INTERP", "1", 0);
#endif
    auto* in = new Inst();
    if (access("/tmp/mnm_trace.on", F_OK) == 0) in->trace = std::fopen("/tmp/mnm_trace.log", "a");
    if (const char* p = std::getenv("MNM_OS")) in->osPath = p;
    else in->osPath = std::string(dataDir && *dataDir ? dataDir : ".") + "/Elektron_SFX6-60_OS1.32B.syx";
    const std::string base = dataDir && *dataDir ? dataDir : ".";
    in->dataDir = base;
    // One OS file serves both plugins: when this plugin's folder has none, the other Monomodule plugin's folder may
    if (access(in->osPath.c_str(), R_OK) != 0 && !std::getenv("MNM_OS")) {
        const size_t sl = base.find_last_of('/'), sl2 = sl == std::string::npos ? sl : base.find_last_of('/', sl - 1);
        if (sl2 != std::string::npos) {
            const std::string synths = base.substr(0, sl2);
            for (const char* other : {"sd88me - VST - Monomodule One", "sd88me - VST - Monomodule FX"}) {
                const std::string cand = synths + "/" + other + "/" + base.substr(sl + 1) + "/Elektron_SFX6-60_OS1.32B.syx";
                if (access(cand.c_str(), R_OK) == 0) { in->osPath = cand; break; }
            }
        }
    }
    in->dumpDirs = {base + "/dumps", "/sdcard/Force Documents/Monomachine Dumps"};   // manual copy (SD card) and MPC's own Documents browser
    in->catTh = std::thread([in] {
        while (!in->stop.load(std::memory_order_acquire)) {
            const uint64_t sig = scanDumps(in->dumpDirs, nullptr);
            const Catalog* cur = in->cat.load(std::memory_order_relaxed);
            if (!cur || cur->signature != sig) {
                Catalog* next = buildCatalog(in->dumpDirs, sig);
                Catalog* old = in->cat.exchange(next, std::memory_order_release);
                if (old) in->retiredCats.push_back(old);
            }
            for (int i = 0; i < 30 && !in->stop.load(std::memory_order_acquire); ++i) {   // ~3 s between scans, checked often enough to exit quickly
                struct timespec ts{0, 100000000};
                nanosleep(&ts, nullptr);
            }
        }
    });
    in->th = std::thread([in] { in->run(); });
    return in;
}
void eDestroy(void* p)
{
    auto* in = static_cast<Inst*>(p);
    in->stop.store(true);
    if (in->th.joinable()) in->th.join();
    if (in->catTh.joinable()) in->catTh.join();
    for (auto* c : in->retiredCats) delete c;
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
    if (!std::strcmp(key, "state")) { in->loadState(val); return; }
    if (!std::strcmp(key, "preset_prev") || !std::strcmp(key, "preset_next")) {
        if (std::atof(val) > 0.5) in->stepPreset(key[7] == 'n' ? 1 : -1);
        return;
    }
    if (!std::strcmp(key, "bank_prev") || !std::strcmp(key, "bank_next")) {
        if (std::atof(val) > 0.5) in->stepBank(key[5] == 'n' ? 1 : -1);
        return;
    }
    if (!std::strcmp(key, "bank_pick")) {
        const int k = int(std::lround(std::atof(val)));
        if (k >= 0 && k < Inst::kBankSlots) in->selectBank(in->bankPage() * Inst::kBankSlots + k);
        return;
    }
    if (!std::strcmp(key, "bank_pgprev") || !std::strcmp(key, "bank_pgnext")) {
        if (std::atof(val) > 0.5) in->stepBankPage(key[7] == 'n' ? 1 : -1);
        return;
    }
    if (!std::strcmp(key, "preset_pick")) { in->pickPreset(int(std::lround(std::atof(val)))); return; }
    if (!std::strcmp(key, "preset_pgprev") || !std::strcmp(key, "preset_pgnext")) {
        if (std::atof(val) > 0.5) in->stepPage(key[9] == 'n' ? 1 : -1);
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
        if (in->unstashSound(m)) return;
        in->stashSound();
        int d[32];
        defaultsFor(m, d);
        for (int i = 0; i < 32; ++i) in->param[kSlotPages + i].store(d[i]);
        in->param[kSlotMachine].store(m);
        in->presetIdx = 0;   // a new machine starts from its Init
        in->pageSeen = -1;
        in->markLoaded();
        return;
    }
    in->param[s].store(s == kSlotTune ? std::clamp(v, 400, 440) : s == kSlotVoices ? std::clamp(v, 1, kMaxVoices) : s == kSlotLpk || s == kSlotHpk ? (v ? 1 : 0) : std::clamp(v, 0, 127));
}
int eGet(void* p, const char* key, char* buf, int len)
{
    auto* in = static_cast<Inst*>(p);
    if (!std::strcmp(key, "state")) return std::snprintf(buf, size_t(len), "%s", in->saveState().c_str());
    if (!std::strncmp(key, "randomize_", 10)) return std::snprintf(buf, size_t(len), "0");   // momentary: always reads back off
    if (!std::strcmp(key, "preset_name")) return std::snprintf(buf, size_t(len), "%s", in->presetName().c_str());
    if (!std::strcmp(key, "bank_name")) return std::snprintf(buf, size_t(len), "%s", in->bankLabel().c_str());
    if (!std::strcmp(key, "bank_prev") || !std::strcmp(key, "bank_next")) return std::snprintf(buf, size_t(len), "0");
    if (!std::strcmp(key, "bank_pick")) {
        const int k = in->bankIndex() - in->bankPage() * Inst::kBankSlots;
        return std::snprintf(buf, size_t(len), "%d", k >= 0 && k < Inst::kBankSlots ? k : Inst::kBankSlots);
    }
    if (!std::strcmp(key, "bank_page")) return std::snprintf(buf, size_t(len), "PAGE %d/%d", in->bankPage() + 1, std::max(in->bankPages(), 1));
    if (!std::strcmp(key, "bank_pgprev") || !std::strcmp(key, "bank_pgnext")) return std::snprintf(buf, size_t(len), "0");
    if (!std::strncmp(key, "bank_s", 6) && std::isdigit(static_cast<unsigned char>(key[6]))) {
        const auto banks = in->bankList();
        const size_t k = size_t(in->bankPage() * Inst::kBankSlots + std::atoi(key + 6));
        return std::snprintf(buf, size_t(len), "%s", k >= banks.size() ? "" : k == 0 ? "ALL" : banks[k].c_str());
    }
    if (!std::strcmp(key, "preset_pick")) {
        const int k = in->presetIdx - in->page() * Inst::kPickSlots;
        return std::snprintf(buf, size_t(len), "%d", k >= 0 && k < Inst::kPickSlots ? k : Inst::kPickSlots);
    }
    if (!std::strncmp(key, "preset_s", 8) && std::isdigit(static_cast<unsigned char>(key[8])))
        return std::snprintf(buf, size_t(len), "%s", in->pickEntry(std::atoi(key + 8)).c_str());
    if (!std::strcmp(key, "preset_page")) return std::snprintf(buf, size_t(len), "PAGE %d/%d", in->page() + 1, std::max(in->pages(), 1));
    if (!std::strcmp(key, "preset_pgprev") || !std::strcmp(key, "preset_pgnext")) return std::snprintf(buf, size_t(len), "0");
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

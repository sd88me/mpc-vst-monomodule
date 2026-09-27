#include "DspEngine.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/jit.h"

namespace mnm::dsp {

using namespace dsp56k;

namespace {
// External memory: X, Y and P alias the same physical memory above $100000 (the payload is stored
// as P records but read as X/Y by the kernel). Internal X/Y RAM 2K each, P 4K (OMR MS=0); we map
// everything below the bridge as plain RAM, which is a superset of the real chip.
constexpr TWord kBridge = 0x100000;
constexpr TWord kSizeP  = 0x180000;
constexpr TWord kSizeXY = 0x180000;
constexpr uint64_t kMaxInstrInit  = 50'000'000;
constexpr uint64_t kMaxInstrBlock = 4'000'000;

EMemArea area(fw::Space s) { return s == fw::Space::P ? MemArea_P : s == fw::Space::X ? MemArea_X : MemArea_Y; }
}

DspEngine::DspEngine(const fw::Firmware& fw) : m_fw(fw)
{
    // The JIT is the reference engine. The interpreter mis-limits accumulator-to-memory moves in the
    // overflow cases (e.g. a = -1.0-eps stores $7FFFFF instead of $800000), which corrupts the runtime
    // sine table (Y:$14A000) and every oscillator built on it. MNM_DSP_INTERP=1 / setUseJit(false)
    // selects the interpreter for debugging only.
    m_useJit = std::getenv("MNM_DSP_INTERP") == nullptr;
    m_trace = std::getenv("MNM_DSP_TRACE") != nullptr;
    m_validator = std::make_unique<DefaultMemoryValidator>();
    m_mem = std::make_unique<Memory>(*m_validator, kSizeP, kSizeXY, kBridge);
    if (!m_mem->hasMmuSupport()) throw std::runtime_error("dsp56300: MMU-backed memory allocation failed");
    m_periphX = std::make_unique<Peripherals56303>();
    m_periphY = std::make_unique<PeripheralsNop>();
    m_dsp = std::make_unique<DSP>(*m_mem, m_periphX.get(), m_periphY.get());
    if (!m_useJit) m_dsp->setInterpreterEnabled(true);   // Schwung dsp56300 patch: its opcode cache is ~75 MB
    {
        // Kernel A keeps its main-loop code at P:$0087..$00FF, inside the interrupt-vector area that the
        // JIT would otherwise compile as 2-word fast-interrupt blocks.
        auto cfg = m_dsp->getJit().getConfig();
        cfg.dynamicFastInterrupts = true;
        cfg.interruptRegionIsCode = true;   // MNM patch in ext/patches
        m_dsp->getJit().setConfig(cfg);
    }
    m_periphX->getHI08().setRXRateLimit(0);
    m_periphX->getHI08().setTransmitDataAlwaysEmpty(true);
    reset();
}

DspEngine::~DspEngine() = default;

void DspEngine::loadImage(const fw::DspImage& img)
{
    for (const auto& r : img.records)
        for (size_t k = 0; k < r.words.size(); ++k) {
            const TWord a = r.addr + TWord(k);
            // P space and everything in the bridged external range must go through memWriteP:
            // it is the only path that tells the JIT about program-memory changes.
            if (r.space == fw::Space::P || a >= kBridge) m_dsp->memWriteP(a, r.words[k]);
            else m_dsp->memWrite(area(r.space), a, r.words[k]);
        }
}

void DspEngine::installStub()
{
    Assembler as;
    auto emit = [&](TWord& pc, const char* text) {
        const auto r = as.assemble(text);
        if (!r.success()) throw std::runtime_error(std::string("harness stub: cannot assemble '") + text + "'");
        m_dsp->memWriteP(pc++, r.word[0]);
        if (r.wordCount > 1) m_dsp->memWriteP(pc++, r.word[1]);
    };
    auto jsrAbs = [&](TWord& pc, TWord target) {   // 'jsr >$xxxxxx' (assembler lacks this form): 0BF080 + address, as in the kernel
        m_dsp->memWriteP(pc++, 0x0BF080);
        m_dsp->memWriteP(pc++, target);
    };
    // ---- init: the parts of kernel A's $0066..$0086 that do not touch ESSI/DMA/HI08 interrupts
    TWord pc = kStubInit;
    emit(pc, "ori #$3,mr");
    emit(pc, "move #>$4d0d,omr");
    emit(pc, "move #>$080300,sr");
    jsrAbs(pc, 0x100077);                 // build 8192-entry sine table at Y:$14A000
    jsrAbs(pc, 0x100139);                 // clear X work RAM + external buffers, AAR3 check
    jsrAbs(pc, 0x145CCE);                 // init the three track structs at $500/$600/$700
    emit(pc, "movep #>$00c0de,x:<<$ffffc7"); // sentinel → host
    emit(pc, "andi #$fc,mr");               // enable interrupts, as the kernel does before its main loop
    emit(pc, "jmp $0c20");
    if (pc > kStubMain) throw std::runtime_error("harness stub: init overflow");
    // ---- main: wait for word 0 of the 52-word block over HI08, store it at Y:$500, run one track pass. The
    // host has already written words 1..51 to Y:$501..$533 directly (Schwung: the upstream stub copied all 52
    // over HI08 word by word, which cost about a tenth of an engine). r0/m0 as that copy loop left them.
    pc = kStubMain;
    const TWord waitPc = pc;
    {
        std::ostringstream o; o << "jclr #0,x:<<$ffffc3,$" << std::hex << waitPc;   // wait HRDF (absolute target)
        emit(pc, o.str().c_str());
    }
    emit(pc, "movep x:<<$ffffc6,y:$500");
    emit(pc, "move #>$534,r0");
    emit(pc, "move #>$ffffff,m0");
    emit(pc, "jmp $0092");               // the kernel's own track pass: pointer block, y:$123/$124, track 0 dispatch, render, chain → jmp $0143 (patched)
    if (pc > kStubOut) throw std::runtime_error("harness stub: main overflow");
    // ---- out: Y:$0..$1F (L/R interleaved, written by the chain tail at $0B45/$0B46) stays in place for the
    // host to read; one "done" word goes to HOTX. r0/m0 as the upstream copy loop left them.
    pc = kStubOut;
    emit(pc, "move #>$20,r0");
    emit(pc, "move #>$ffffff,m0");
    emit(pc, "movep #>$00d0e0,x:<<$ffffc7");
    emit(pc, "jmp $0c20");
    skipIdleLoops();
    // ---- patch: $0143 (start of the mixing step) → stub_out. 1-word short jmp; the 2nd word of the
    // overwritten 'move y:>$123,r6' is never reached.
    m_dsp->memWriteP(kPatchAddr, 0x0C0000 | kStubOut);
}

// Schwung: the kernel burns padding on the real chip — DO loops whose body is nothing but NOPs (listed by
// `mnm-disasm <os> --idle-loops`, each checked by hand to be code). They have no effect but time, which the
// emulation does not need: each DO (2 words) becomes 'jmp >end' (2 words). A NOP-only DO body changes no
// register or memory and the DO restores LA/LC/SR when it ends, so the audio is bit-exact. GND and REVERB
// spend ~37% of their instructions here, GND SIN ~50%. Only patched if the words match OS 1.32B.
void DspEngine::skipIdleLoops()
{
    struct Site { TWord pc, doWord, laWord, end; };
    static constexpr Site kSites[] = {
        {0x100162, 0x06808C, 0x100164, 0x100165},   // do #$c80 { nop }            (GND ---)
        {0x144D0E, 0x061080, 0x144D1F, 0x144D20},   // do #$10 { do #$18 { 13×nop } nop }  (GND SIN)
        {0x144E72, 0x067880, 0x144E74, 0x144E75},   // do #$78 { nop }
        {0x14765C, 0x064086, 0x14765F, 0x147660},   // do #$640 { nop nop }        (REVERB)
    };
    m_idleLoopsSkipped = 0;
    for (const auto& s : kSites) {
        if (m_mem->get(MemArea_P, s.pc) != s.doWord || m_mem->get(MemArea_P, s.pc + 1) != s.laWord) continue;
        m_dsp->memWriteP(s.pc, 0x0AF080);   // jmp >end
        m_dsp->memWriteP(s.pc + 1, s.end);
        ++m_idleLoopsSkipped;
    }
}

void DspEngine::reset(bool clearMemory)
{
    m_faulted = false; m_fault.clear(); m_stats = {};
    m_dsp->resetHW();
    if (clearMemory) {
        for (TWord a = 0; a < kSizeP; ++a) m_dsp->memWriteP(a, 0);           // P, and X/Y above the bridge (aliased)
        for (TWord a = 0; a < kBridge; ++a) { m_dsp->memWrite(MemArea_X, a, 0); m_dsp->memWrite(MemArea_Y, a, 0); }
    }
    loadImage(m_fw.kernelA);
    loadImage(m_fw.payload);
    installStub();
    m_periphX->write(0xFFFFF6, 0x1C0639);   // AAR3 as on a unit with the extra memory (MKII): keeps DDRW/DENS slots
    if (m_bank) uploadDigibank();
    m_periphX->write(0xFFFFEE, 0x000120);   // DMA0 destination (DDR0) parked at $120: the kernel then reads ADC frames from X:$100..$11F
    for (TWord a = 0x100; a < 0x140; ++a) m_dsp->memWrite(MemArea_X, a, 0);
    m_periphX->getHI08().clearRX();
    while (m_periphX->getHI08().hasTX()) m_periphX->getHI08().readTX();
    m_dsp->setPC(kStubInit);
    if (!runUntilTx(1, kMaxInstrInit)) throw std::runtime_error("DSP init did not complete: " + m_fault);
    const auto s = m_periphX->getHI08().readTX();
    if (s != kInitSentinel) throw std::runtime_error("DSP init: bad sentinel");
}

// Schwung: back to the state of a fresh engine without losing the JIT's compiled code. reset(true) would
// zero P, which invalidates every compiled block; here only internal X/Y RAM is zeroed (P untouched), and
// reset() then reloads the image (unchanged P words do not invalidate) and runs the kernel's own init, which
// clears the X work RAM and the external buffers.
void DspEngine::resetKeepCode()
{
    for (TWord a = 0; a < kBridge; ++a) { m_mem->set(MemArea_X, a, 0); m_mem->set(MemArea_Y, a, 0); }
    reset(false);
}

void DspEngine::setDigibank(std::shared_ptr<const Digibank> bank)
{
    m_bank = std::move(bank);
    if (m_bank) uploadDigibank();
}

// The host's HV $0B upload, all 64 slots at once: 2048 words per slot to Y:$150000 + slot*$800 (bridged external memory)
void DspEngine::uploadDigibank()
{
    for (int s = 0; s < Digibank::kSlots; ++s)
        for (int i = 0; i < Digibank::kSlotWords; ++i)
            m_dsp->memWriteP(Digibank::kBase + TWord(s) * Digibank::kSlotWords + TWord(i), TWord(uint32_t(m_bank->slot[size_t(s)][size_t(i)]) & 0xFFFFFF));
}

bool DspEngine::runUntilTx(size_t words, uint64_t maxInstr)
{
    auto& hi = m_periphX->getHI08();
    const uint64_t start = m_dsp->getInstructionCounter();
    const bool trace = m_trace;
    while (hi.txData().sizeSameThread() < words) {   // same thread as the DSP: no acquire barriers (dmb on armv7)
        if (trace) std::fprintf(stderr, "PC=%06x\n", m_dsp->getPC().toWord());
        if (m_useJit) m_dsp->exec(); else m_dsp->execInterpreter();
        if (m_useJit && m_dsp->getJit().hasFailed()) {   // no JIT memory (or code generation failed): silence and a status, never a crash
            fault("DSP JIT failed: " + m_dsp->getJit().failReason());
            return false;
        }
        if (m_dsp->getInstructionCounter() - start > maxInstr) {
            std::ostringstream o; o << "instruction budget exceeded (PC=$" << std::hex << m_dsp->getPC().toWord() << ", " << std::dec << hi.txData().size() << "/" << words << " words)";
            fault(o.str());
            return false;
        }
    }
    m_stats.instructions = m_dsp->getInstructionCounter() - start;
    m_stats.totalInstructions += m_stats.instructions;
    return true;
}

bool DspEngine::renderBlock(const uint32_t* block52, std::array<int32_t, 32>& out)
{
    if (m_faulted) { out.fill(0); return false; }
    auto& hi = m_periphX->getHI08();
    for (int k = 1; k < kBlockWords; ++k) m_mem->set(MemArea_Y, kBlockAddr + TWord(k), block52[k] & 0xFFFFFF);
    const TWord go = block52[0] & 0xFFFFFF;
    hi.writeRX(&go, 1);
    if (!runUntilTx(1, kMaxInstrBlock)) { out.fill(0); return false; }
    if (hi.readTX() != kDoneSentinel) { fault("protocol mismatch: bad done word"); out.fill(0); return false; }
    for (int k = 0; k < 32; ++k) {
        const TWord t = m_mem->get(MemArea_Y, TWord(k)) & 0xFFFFFF;
        out[size_t(k)] = int32_t(t << 8) >> 8;
    }
    ++m_stats.blocks;
    return true;
}

void DspEngine::setJitMaxInstructionsPerBlock(uint32_t n)
{
    auto cfg = m_dsp->getJit().getConfig();
    cfg.maxInstructionsPerBlock = n;
    m_dsp->getJit().setConfig(cfg);
}

void DspEngine::sendBlock(const uint32_t* block52)
{
    for (int k = 1; k < kBlockWords; ++k) m_mem->set(MemArea_Y, kBlockAddr + TWord(k), block52[k] & 0xFFFFFF);
    const TWord go = block52[0] & 0xFFFFFF;
    m_periphX->getHI08().writeRX(&go, 1);
}

void DspEngine::setInputFrames(const int32_t* lr32)
{
    for (int k = 0; k < 32; ++k) m_dsp->memWrite(MemArea_X, 0x100 + TWord(k), TWord(lr32[k]) & 0xFFFFFF);
}

void DspEngine::fault(const std::string& why) { m_faulted = true; m_fault = why; }

uint32_t DspEngine::peek(fw::Space s, uint32_t addr) const { return m_mem->get(area(s), addr); }
void DspEngine::poke(fw::Space s, uint32_t addr, uint32_t v)
{
    if (s == fw::Space::P || addr >= kBridge) m_dsp->memWriteP(addr, v); else m_dsp->memWrite(area(s), addr, v);
}

} // namespace mnm::dsp

// Extracts the factory kit bank out of a Monomachine OS file and writes it as an ordinary Monomachine kit-dump
// .syx, so it drops straight into a Monomodule One/FX install's dumps/ folder unchanged (buildCatalog() in
// mnm_engine.cpp already scans that folder). The factory bank isn't kept in Monomodule's own runtime; the OS
// file's flash image happens to hold it (section 4, undocumented and unused elsewhere in this tree) in exactly
// the field layout MnmDump.cpp's decodeKit() already produces after un-packing a kit sysex message -- confirmed
// empirically: named records recur every 698 bytes, which is precisely decodeKit()'s byte count from `name`
// through `splitRange` (11 + 6 + 6*72 + 6 + 6 + 1 + 2 + 1 + 1 + 6*6*2*3 + 1 + 1 + 1 + 6 + 1 + 1 + 1 + 1 + 1 + 1
// = 698), and the field that field offset 461 is named for (`unused461`) lands exactly there. Never distributed:
// this is Elektron's own factory content, extracted per user from their own OS file.
//   mnm-factorybank <os.syx> <out.syx> [max-kits]
#include <cstdio>
#include <cstring>
#include <vector>
#include "Firmware.h"
#include "MnmDump.h"

using namespace mnm;

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: mnm-factorybank <os.syx> <out.syx> [max-kits]\n"); return 2; }
    const int maxKits = argc > 3 ? std::atoi(argv[3]) : 1000;
    fw::Container c;
    try {
        c = fw::parseContainer(fw::parseSysex(fw::readFile(argv[1])));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "OS file: %s\n", ex.what());
        return 1;
    }
    if (c.sections.size() < 5) { std::fprintf(stderr, "OS file has no section 4 (factory bank not found)\n"); return 1; }
    const auto& d = c.sections[4].data;
    constexpr size_t kRec = 698;

    dump::Dump out;
    int pos = 0;
    for (size_t off = 40; off + kRec <= d.size() && int(out.kits.size()) < maxKits; off += kRec) {
        const uint8_t* p = &d[off];
        bool any = false;
        for (int i = 0; i < 11; ++i) if (p[i] && p[i] != 0xff) any = true;
        if (!any) continue;   // an empty slot

        dump::Kit kit;
        kit.position = pos++;
        size_t o = 0;
        auto get = [&](void* dst, size_t n) { std::memcpy(dst, p + o, n); o += n; };
        get(kit.nameRaw, 11);
        for (int i = 0; i < 11 && kit.nameRaw[i]; ++i) {
            if (kit.nameRaw[i] < 0x20 || kit.nameRaw[i] > 0x7e) break;
            kit.name.push_back(char(kit.nameRaw[i]));
        }
        uint8_t levels[6], params[6][72], models[6], types[6];
        get(levels, 6);
        get(&params[0][0], 6 * 72);
        get(models, 6);
        get(types, 6);
        kit.unused461 = p[o++];
        kit.patchBusIn = uint16_t(p[o] | (p[o + 1] << 8)); o += 2;
        kit.mirrorLR = p[o++];
        kit.mirrorUD = p[o++];
        uint8_t destPages[6][6][2], destParams[6][6][2], destRanges[6][6][2];
        get(&destPages[0][0][0], sizeof destPages);
        get(&destParams[0][0][0], sizeof destParams);
        get(&destRanges[0][0][0], sizeof destRanges);
        kit.lpKeyTrack = p[o++];
        kit.hpKeyTrack = p[o++];
        kit.trigPortamento = p[o++];
        get(kit.trigTracks, 6);
        kit.trigLegatoAmp = p[o++];
        kit.trigLegatoFilter = p[o++];
        kit.trigLegatoLFO = p[o++];
        kit.commonMultimode = p[o++];
        kit.commonTiming = p[o++];
        kit.splitKey = p[o++];
        kit.splitRange = p[o++];
        for (int t = 0; t < 6; ++t) {
            kit.tracks[t].model = models[t];
            kit.tracks[t].type = types[t];
            kit.tracks[t].level = levels[t];
            std::memcpy(kit.tracks[t].params, params[t], 72);
            for (int s = 0; s < 6; ++s)
                for (int slot = 0; slot < 2; ++slot) {
                    kit.tracks[t].destPage[s][slot] = destPages[t][s][slot];
                    kit.tracks[t].destParam[s][slot] = destParams[t][s][slot];
                    kit.tracks[t].destRange[s][slot] = int8_t(destRanges[t][s][slot]);
                }
        }
        // sanity: every track's model must be a real machine id, or this offset/stride guess is wrong
        bool ok = true;
        for (int t = 0; t < 6; ++t) {
            const uint8_t m = kit.tracks[t].model;
            if (!(m <= 19 || m == 32 || m == 33)) { ok = false; break; }
        }
        if (!ok) continue;   // implausible track model: not a kit record at this offset
        if (kit.name.empty()) continue;   // an unnamed slot (init/reserved), not a usable preset
        dump::Message msg;
        msg.id = dump::kKitId;
        msg.kitIndex = int(out.kits.size());
        out.kits.push_back(kit);
        out.messages.push_back(msg);
    }
    if (out.kits.empty()) { std::fprintf(stderr, "no kits decoded (layout guess was wrong)\n"); return 1; }

    const auto bytes = dump::encodeDump(out);
    FILE* f = std::fopen(argv[2], "wb");
    if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    std::fclose(f);
    std::printf("%zu kits -> %s (%zu bytes)\n", out.kits.size(), argv[2], bytes.size());
    return 0;
}

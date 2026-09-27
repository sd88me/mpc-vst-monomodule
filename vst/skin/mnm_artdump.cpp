// Dumps the LCD artwork of the user's own Monomachine OS file (fonts, dial, icon families, group logos) and
// upstream Monomodule's UI spec (machines, knob labels, value lists) as one JSON file, for mk_skin.py to draw
// the plugin's skin from. The OS file's artwork is Elektron's: the output is per-user build data, never committed
// or distributed. Built against upstream Monomodule's own RomArt.cpp / SpecData.cpp (no JUCE needed).
//   mnm-artdump <os.syx> <out.json>
#include <cstdio>
#include <string>
#include <vector>
#include "SpecData.h"

using namespace mnm::uispec;

static FILE* g_f;
static void bitmap(const Bitmap* b)
{
    if (!b || !b->rows) { std::fputs("null", g_f); return; }
    std::fprintf(g_f, "{\"w\":%d,\"h\":%d,\"rows\":[", b->w, b->h);
    for (int r = 0; r < b->h; ++r) std::fprintf(g_f, "%s\"%016llx\"", r ? "," : "", (unsigned long long)b->rows[r]);
    std::fputs("]}", g_f);
}
static void font(const char* name, const Font& f, bool last = false)
{
    std::fprintf(g_f, "\"%s\":{\"h\":%d,\"adv\":%d,\"glyphs\":{", name, f.h, f.adv);
    bool first = true;
    for (int c = 0; c < 128; ++c) {
        const Bitmap* g = f.glyph(c);
        if (!g) continue;
        std::fprintf(g_f, "%s\"%d\":", first ? "" : ",", c); first = false;
        bitmap(g);
    }
    std::fprintf(g_f, "}}%s", last ? "" : ",");
}
static void family(const char* name, const Bitmap* const* arr, int n)
{
    std::fprintf(g_f, "\"%s\":[", name);
    for (int i = 0; i < n; ++i) { if (i) std::fputc(',', g_f); bitmap(arr[i]); }
    std::fputs("],", g_f);
}
static void str(const char* s)
{
    std::fputc('"', g_f);
    for (; s && *s; ++s) { if (*s == '"' || *s == '\\') std::fputc('\\', g_f); std::fputc(*s, g_f); }
    std::fputc('"', g_f);
}
static void strs(const char* const* v, int n)
{
    std::fputc('[', g_f);
    for (int i = 0; i < n; ++i) { if (i) std::fputc(',', g_f); str(v[i]); }
    std::fputc(']', g_f);
}
static void param(const Param& p)
{
    std::fputs("{\"label\":", g_f); str(p.label);
    std::fprintf(g_f, ",\"display\":%d,\"tieRight\":%d,\"default\":%d,\"max\":%d,\"count\":%d,\"icons\":%d,\"values\":", int(p.display), int(p.tieRight), p.defaultRaw, p.maxRaw, p.valueCount, int(p.icons));
    if (p.values && (p.display == Display::List || p.display == Display::Readout)) strs(p.values, p.valueCount); else std::fputs("null", g_f);
    std::fputc('}', g_f);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: mnm-artdump <os.syx> <out.json>\n"); return 2; }
    std::string err;
    if (!ensureRomArt(argv[1], &err)) { std::fprintf(stderr, "OS artwork: %s\n", err.c_str()); return 1; }
    g_f = std::fopen(argv[2], "w");
    if (!g_f) return 1;
    std::fputs("{\"fonts\":{", g_f);
    font("bold8", kFontBold8); font("small4x5", kFontSmall4x5); font("tiny3x5", kFontTiny3x5);
    font("square5x5", kFontSquare5x5); font("digitsTop", kFontDigitsTop); font("digitsBottom", kFontDigitsBottom, true);
    std::fputs("},\"bitmaps\":{\"dialRing\":", g_f); bitmap(&kDialRing);
    std::fputs(",\"groupTie\":", g_f); bitmap(&kGroupTie);
    std::fputs(",\"ringPlain\":", g_f); bitmap(&kRingPlain);
    std::fputs(",", g_f);
    family("dialDot", kDialDot, 128);
    family("lfoDest", kIconLfoDest, 8); family("toggle", kIconToggle, 2); family("fmRatio", kIconFmRatio, 24);
    family("ensPitch", kIconEnsPitch, 33); family("fmDynFrq", kIconFmDynFrq, 128); family("sidWave", kIconSidWave, 5);
    family("dproSync", kIconDproSync, 3); family("dproWave", kIconDproWave, 32); family("voCons", kIconVoCons, 21);
    family("ddrwWave", kIconDdrwWave, 64); family("lfoPage", kIconLfoPage, 9); family("lfoWave", kIconLfoWave, 11);
    std::fputs("\"logos\":{", g_f);
    const char* groups[5] = {"SWAVE", "SID", "DPRO", "FM+", "VO"};
    for (int i = 0; i < 5; ++i) {
        std::fprintf(g_f, "%s\"%s\":{\"bitmap\":", i ? "," : "", groups[i]); bitmap(groupLogo(groups[i]));
        const auto* bp = groupLogo(groups[i]);
        if (bp) { const auto lb = litBounds(*bp); std::fprintf(g_f, ",\"lit\":[%d,%d,%d,%d]", lb.x, lb.y, lb.w, lb.h); }
        if (const char* const* w = groupLogoWords(groups[i])) { std::fputs(",\"words\":", g_f); strs(w, 2); }
        std::fputc('}', g_f);
    }
    std::fputs("}},\"spec\":{\"machines\":[", g_f);
    for (int m = 0; m < kNumMachines; ++m) {
        const auto& mm = kMachines[m];
        std::fprintf(g_f, "%s{\"index\":%d,\"group\":", m ? "," : "", mm.index); str(mm.group);
        std::fputs(",\"name\":", g_f); str(mm.name); std::fputs(",\"displayName\":", g_f); str(mm.displayName);
        std::fprintf(g_f, ",\"isFx\":%d,\"params\":[", int(mm.isFx));
        for (int k = 0; k < 8; ++k) { if (k) std::fputc(',', g_f); param(mm.params[k]); }
        std::fputs("]}", g_f);
    }
    std::fputs("],\"shared\":[", g_f);
    for (int s = 0; s < 3; ++s) {
        std::fprintf(g_f, "%s{\"name\":", s ? "," : ""); str(kSharedPages[s].name);
        std::fputs(",\"params\":[", g_f);
        for (int k = 0; k < 8; ++k) {   // as OneParams.h sharedPageParam()
            const auto& pg = kSharedPages[s];
            Param p{pg.labels[k], ((pg.bipolarMask >> k) & 1) ? Display::Bipolar : Display::Numeric, false, pg.defaults[k], 127, 128, Icons::None, nullptr};
            if (k) std::fputc(',', g_f);
            param(p);
        }
        std::fputs("]}", g_f);
    }
    std::fputs("],\"lfoParams\":[", g_f);
    for (int k = 0; k < 8; ++k) { if (k) std::fputc(',', g_f); param(kLfoParams[k]); }
    std::fputs("],\"lfoPageNames\":", g_f); strs(kLfoPageNames, 9);
    std::fputs(",\"lfoDestNames\":[", g_f);
    for (int p = 0; p < 9; ++p) { if (p) std::fputc(',', g_f); strs(kLfoDestNames[p], 8); }
    std::fputs("],\"lfoTrigNames\":", g_f); strs(kLfoTrigNames, 5);
    std::fputs(",\"lfoWaveNames\":", g_f); strs(kLfoWaveNames, 11);
    std::fputs(",\"lfoMultNames\":", g_f); strs(kLfoMultNames, 7);
    std::fputs("}}", g_f);
    std::fclose(g_f);
    return 0;
}

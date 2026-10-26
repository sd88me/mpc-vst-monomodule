// The LCD artwork of the user's own Monomachine OS file as one list of bits, in the order vst/skin/skin_recipe.py's art_bits() walks
// art.json (written by vst/skin/mnm_artdump.cpp from the same upstream accessors): fonts by name, their glyphs by code, then the
// bitmaps by name (a family in order; a missing entry, as in a logo that does not exist, is skipped); in each bitmap row by row, left
// to right. The plugin fills the skin recipe in with it (vst/skin_recipe.h). Elektron's artwork: read from the user's file at run time,
// never committed or shipped.
#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "RomArt.h"

namespace mnm::skinart
{
	using namespace mnm::uispec;

	// Reads the artwork of the OS file (installs it as upstream's artwork in use) and returns its bits.
	inline std::vector<uint8_t> bits(const std::string& osFile)
	{
		std::string err;
		if(!ensureRomArt(osFile, &err) || !art().fromRom) throw std::runtime_error("OS artwork: " + (err.empty() ? std::string("not a known OS image") : err));
		std::vector<uint8_t> out;
		const auto add = [&](const Bitmap* b)
		{
			if(!b || !b->rows) return;
			for(int y = 0; y < b->h; ++y)
				for(int x = 0; x < b->w; ++x) out.push_back(uint8_t((b->rows[y] >> (63 - x)) & 1));
		};
		const std::map<std::string, const Font*> fonts = {{"bold8", &kFontBold8}, {"small4x5", &kFontSmall4x5}, {"tiny3x5", &kFontTiny3x5},
			{"square5x5", &kFontSquare5x5}, {"digitsTop", &kFontDigitsTop}, {"digitsBottom", &kFontDigitsBottom}};	// sorted by name, as Python's sorted()
		for(const auto& [name, f] : fonts)
			for(int c = 0; c < 128; ++c) add(f->glyph(c));
		std::map<std::string, std::vector<const Bitmap*>> maps;
		const auto family = [&](const char* n, const Bitmap* const* a, int count) { maps[n].assign(a, a + count); };
		maps["dialRing"] = {&kDialRing};
		maps["groupTie"] = {&kGroupTie};
		maps["ringPlain"] = {&kRingPlain};
		family("dialDot", kDialDot, 128);
		family("lfoDest", kIconLfoDest, 8); family("toggle", kIconToggle, 2); family("fmRatio", kIconFmRatio, 24);
		family("ensPitch", kIconEnsPitch, 33); family("fmDynFrq", kIconFmDynFrq, 128); family("sidWave", kIconSidWave, 5);
		family("dproSync", kIconDproSync, 3); family("dproWave", kIconDproWave, 32); family("voCons", kIconVoCons, 21);
		family("ddrwWave", kIconDdrwWave, 64); family("lfoPage", kIconLfoPage, 9); family("lfoWave", kIconLfoWave, 11);
		// the logos, keyed by group; Python sorts "DPRO" < "FM+" < "SID" < "SWAVE" < "VO", and its "logos" key falls between "lfoWave" and "ringPlain"
		std::vector<const Bitmap*> logos;
		for(const char* g : {"DPRO", "FM+", "SID", "SWAVE", "VO"}) logos.push_back(groupLogo(g));
		maps["logos"] = logos;
		for(const auto& [name, list] : maps)
			for(const Bitmap* b : list) add(b);
		return out;
	}
}

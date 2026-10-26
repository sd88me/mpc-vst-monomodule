// The skin filled in on the device (the Gen2 download carries no Elektron art): the release ships skin-recipe/art.bin, made by
// vst/skin/skin_recipe.py, in which every skin pixel that comes from the LCD art is a reference to one or two bits of it; this fills
// those in from the user's own OS file (vst/skin_art.h) and writes the PNGs, pixel for pixel what vst/skin/mk_skin.py and
// draws. Image paths in art.bin are "<skin>/<skin folder>/...", skin = one or fx; the folders are the plugins' folders in the Synths
// folder.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "skin_png.h"

namespace mnm::skin
{
	constexpr const char* kMarker = ".mnm-skin";	// in a plugin folder: its images are made (holds the OS file's size)

	struct Reader
	{
		std::vector<uint8_t> d;
		size_t o = 0;
		bool ok = true;
		template<class T> T get()
		{
			T v{};
			if(o + sizeof(T) > d.size()) { ok = false; return v; }
			std::memcpy(&v, &d[o], sizeof(T));
			o += sizeof(T);
			return v;
		}
		std::string str(size_t n)
		{
			if(o + n > d.size()) { ok = false; return {}; }
			std::string s(reinterpret_cast<const char*>(&d[o]), n);
			o += n;
			return s;
		}
	};

	inline bool readFile(const std::string& path, std::vector<uint8_t>& out)
	{
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if(!f) return false;
		uint8_t buf[65536];
		size_t n;
		while((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
		std::fclose(f);
		return true;
	}

	inline bool isDir(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

	inline void makeDirs(const std::string& path)	// every parent folder of a file path
	{
		for(size_t i = 1; i < path.size(); ++i)
			if(path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
	}

	// Fills in art.bin with the art bits (mnm::skinart::bits) and writes the images. dest(skin, folder) gives the folder to write a skin's
	// images into, or "" to skip that skin (its plugin is not installed, or its images are made already). Returns the number of images
	// written, or -1 (err says why).
	template<class Dest> int fill(const std::string& artBin, const std::vector<uint8_t>& bits, Dest dest, std::string& err)
	{
		Reader r;
		if(!readFile(artBin, r.d)) { err = "cannot read " + artBin; return -1; }
		if(r.str(4) != "MNSK" || r.get<uint32_t>() != 2) { err = "not a skin recipe: " + artBin; return -1; }
		const uint32_t nbits = r.get<uint32_t>(), nimages = r.get<uint32_t>();
		if(nbits != bits.size()) { err = "the recipe was made for other art (" + std::to_string(nbits) + " bits, the OS has " + std::to_string(bits.size()) + ")"; return -1; }
		int written = 0;
		std::vector<uint8_t> px;
		for(uint32_t i = 0; i < nimages && r.ok; ++i)
		{
			const std::string rel = r.str(r.get<uint16_t>());
			const uint8_t mode = r.get<uint8_t>();
			const uint32_t w = r.get<uint32_t>(), h = r.get<uint32_t>();
			const uint32_t runs = r.get<uint32_t>();
			px.assign(size_t(w) * h * 4, 0);
			size_t p = 0;
			for(uint32_t k = 0; k < runs && r.ok; ++k)
			{
				const uint32_t n = r.get<uint32_t>();
				uint8_t c[4];
				for(auto& b : c) b = r.get<uint8_t>();
				for(uint32_t j = 0; j < n && p < size_t(w) * h; ++j, ++p) std::memcpy(&px[p * 4], c, 4);
			}
			const uint32_t ncolours = r.get<uint8_t>();
			uint8_t colours[256][4];
			for(uint32_t k = 0; k < ncolours; ++k)
				for(auto& v : colours[k]) v = r.get<uint8_t>();
			const auto varint = [&r]
			{
				uint32_t v = 0;
				for(int shift = 0; shift < 35 && r.ok; shift += 7)
				{
					const uint8_t b = r.get<uint8_t>();
					v |= uint32_t(b & 0x7f) << shift;
					if(!(b & 0x80)) break;
				}
				return v;
			};
			const uint32_t rects = r.get<uint32_t>();
			for(uint32_t k = 0; k < rects && r.ok; ++k)
			{
				const uint32_t x = r.get<uint16_t>(), y = r.get<uint16_t>(), rw = r.get<uint16_t>(), rh = r.get<uint16_t>();
				const uint32_t a = varint(), b1 = varint();
				const uint32_t ci = r.get<uint8_t>();
				if(!r.ok || ci >= ncolours || x + rw > w || y + rh > h) { r.ok = false; break; }
				if(!(a < bits.size() && (bits[a] || (b1 > 0 && b1 - 1 < bits.size() && bits[b1 - 1])))) continue;
				for(uint32_t yy = y; yy < y + rh; ++yy)
					for(uint32_t xx = x; xx < x + rw; ++xx) std::memcpy(&px[(size_t(yy) * w + xx) * 4], colours[ci], 4);
			}
			if(!r.ok) break;
			const size_t s1 = rel.find('/'), s2 = s1 == std::string::npos ? s1 : rel.find('/', s1 + 1);
			if(s2 == std::string::npos) { err = "bad path in the recipe: " + rel; return -1; }
			const std::string dir = dest(rel.substr(0, s1), rel.substr(s1 + 1, s2 - s1 - 1));
			if(dir.empty()) continue;
			const std::string out = dir + "/" + rel.substr(s2 + 1);
			makeDirs(out);
			if(!mnm::png::write(out, px.data(), w, h, mode == 1)) { err = "cannot write " + out; return -1; }
			++written;
		}
		if(!r.ok) { err = "the recipe is truncated: " + artBin; return -1; }
		return written;
	}
}

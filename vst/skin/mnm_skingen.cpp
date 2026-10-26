// mnm-skin-gen: fills in a skin recipe (vst/skin/skin_recipe.py) from an OS file, as the Gen2 plugin does on the device
// (vst/skin_recipe.h), writing every image to <out dir>/<its path in the recipe>. The test that the device writes the generator's
// skin: compare its images with mk_skin.py's (vst/skin/check_skin_gen.py).
//   mnm-skin-gen <Monomachine OS .syx> <art.bin> <out dir> [bits.txt]
#include <cstdio>
#include <stdexcept>

#include "skin_art.h"
#include "skin_recipe.h"

int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: mnm-skin-gen <Monomachine OS .syx> <art.bin> <out dir> [bits.txt]\n"); return 2; }
    try {
        const auto bits = mnm::skinart::bits(argv[1]);
        if (argc > 4)
            if (std::FILE* f = std::fopen(argv[4], "w")) { for (uint8_t b : bits) std::fputc('0' + b, f); std::fclose(f); }
        std::string err;
        const std::string out = argv[3];
        const int n = mnm::skin::fill(argv[2], bits, [&](const std::string& skin, const std::string& folder) { return out + "/" + skin + "/" + folder; }, err);
        if (n < 0) { std::fprintf(stderr, "mnm-skin-gen: %s\n", err.c_str()); return 1; }
        std::printf("%d images from %zu art bits\n", n, bits.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mnm-skin-gen: %s\n", e.what());
        return 1;
    }
    return 0;
}

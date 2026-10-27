#!/usr/bin/env bash
# Makes the skin recipe (skin-recipe/art.bin) the Gen2 download carries instead of a skin: needs the OS file only to find which skin
# pixel follows which art bit (art.bin itself holds no Elektron art), and checks that the recipe filled in gives mk_skin.py's output.
#   build_recipe.sh <os.syx> <out dir> [mpc-vst-plugins checkout]      -> <out dir>/recipe/art.bin
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd); ROOT=$(cd "$HERE/.." && pwd)
OS=$(realpath "$1"); OUT=$(realpath -m "$2"); MV=$(realpath "${3:-$HOME/mpc-vst}")
U=libs/monomodule/src
mkdir -p "$OUT"
docker image inspect mnm-x86-builder >/dev/null 2>&1 || docker build -q -t mnm-x86-builder -f "$ROOT/tools/Dockerfile.x86-builder" "$ROOT" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r:ro -v "$OUT":/out -v "$(dirname "$OS")":/os:ro -w /r mnm-x86-builder sh -c "
  g++ -std=c++17 -O1 -I$U/plugin/one -I$U/core -o /out/mnm-artdump vst/skin/mnm_artdump.cpp \
    $U/plugin/one/RomArt.cpp $U/plugin/one/SpecData.cpp $U/core/firmware/Firmware.cpp &&
  /out/mnm-artdump /os/$(basename "$OS") /out/art.json"
# the generators write into the repo's vst/ tree only through their out dir; run them from a copy so the source tree stays clean
rm -rf "$OUT/src" && mkdir -p "$OUT/src" && cp -r "$HERE" "$OUT/src/vst" && rm -rf "$OUT/src/vst/build" "$OUT/src/vst/fx/build"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MPC_VST_TOOLS=/mv/tools -v "$OUT":/w -v "$MV":/mv:ro -w /w/src python:3.11-slim sh -c \
  "pip install -q --no-warn-script-location --target /tmp/p pillow numpy >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 vst/skin/skin_recipe.py make /w/art.json /w"

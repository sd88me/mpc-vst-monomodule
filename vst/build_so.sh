#!/usr/bin/env bash
# Builds monomodule_one.so for the Force (armhf) inside the mnm-armhf-builder image.
#   build_so.sh <schwung-monomodule glue tree> <dsp56300 arm32 tree> <dir holding dsp56k_recomp.inl> <mpc-vst checkout>
# The recompiled DSP embeds firmware words: it is generated from the user's own OS file and never shipped.
set -euo pipefail
GLUE=$(realpath "$1"); DSP=$(realpath "$2"); REC=$(realpath "$3"); MV=$(realpath "$4")
HERE=$(cd "$(dirname "$0")" && pwd)
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h; python3 "$MV/tools/gen_vst.py" "$HERE/fx/vst.json" --params-h
mkdir -p "$HERE/build/obj"
docker run --rm -u "$(id -u):$(id -g)" -v "$GLUE":/g -v "$DSP":/dsp -v "$REC":/rec:ro -v "$HERE":/v -v "$MV":/mv:ro \
  -v "$(realpath "$GLUE/../xbuild")":/xb:ro mnm-armhf-builder sh -c "
  cmake -S /g -B /v/build/obj -G Ninja -DCMAKE_TOOLCHAIN_FILE=/xb/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
    -DMNM_DSP56300_DIR=/dsp -DMNM_VST_DIR=/v -DMPC_VST_DIR=/mv '-DCMAKE_CXX_FLAGS=-DDSP56K_RECOMP -I/rec' >/dev/null &&
  ninja -C /v/build/obj monomodule_one monomodule_fx mnm-vst-smoke mnm-vst-smoke-fx 2>&1 | tail -5 &&
  arm-linux-gnueabihf-strip -o /v/build/monomodule_one.so /v/build/obj/monomodule_one.so && arm-linux-gnueabihf-strip -o /v/build/monomodule_fx.so /v/build/obj/monomodule_fx.so"
md5sum "$HERE/build/monomodule_one.so" "$HERE/build/monomodule_fx.so"

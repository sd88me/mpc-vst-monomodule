#!/usr/bin/env bash
# Builds monomodule_one.so and monomodule_fx.so for the Force (armhf), self-contained (just this repo's
# submodules + the CMakeLists.txt at its root), inside the mnm-armhf-builder image (built automatically
# from tools/Dockerfile.armhf-builder on first use).
#   build_so.sh <dir holding dsp56k_recomp.inl> <mpc-vst-plugins checkout>
# The recompiled DSP embeds firmware words: it is generated from the user's own OS file (libs/dsp56300/
# tools/arm32jit_prototype/recomp/) and never shipped -- REC below must point at your own build of it.
set -euo pipefail
REC=$(realpath "$1"); MV=$(realpath "$2")
ROOT=$(cd "$(dirname "$0")/.." && pwd)   # repo root (this script lives in vst/)
if [ ! -f "$ROOT/libs/monomodule/src/core/firmware/Firmware.h" ] || [ ! -f "$ROOT/libs/dsp56300/source/asmjit/CMakeLists.txt" ]; then
  echo "submodules missing -- run: git -C '$ROOT' submodule update --init --recursive" >&2
  exit 1
fi
docker image inspect mnm-armhf-builder >/dev/null 2>&1 || docker build -q -t mnm-armhf-builder -f "$ROOT/tools/Dockerfile.armhf-builder" "$ROOT" >/dev/null
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/vst.json" --params-h
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/fx/vst.json" --params-h
mkdir -p "$ROOT/vst/build/obj"
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$REC":/rec:ro -v "$MV":/mv:ro mnm-armhf-builder sh -c "
  cmake -S /r -B /r/vst/build/obj -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/tools/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
    -DMPC_VST_DIR=/mv '-DCMAKE_CXX_FLAGS=-DDSP56K_RECOMP -I/rec' >/dev/null &&
  ninja -C /r/vst/build/obj monomodule_one monomodule_fx mnm-vst-smoke mnm-vst-smoke-fx 2>&1 | tail -5 &&
  arm-linux-gnueabihf-strip -o /r/vst/build/monomodule_one.so /r/vst/build/obj/monomodule_one.so &&
  arm-linux-gnueabihf-strip -o /r/vst/build/monomodule_fx.so /r/vst/build/obj/monomodule_fx.so"
md5sum "$ROOT/vst/build/monomodule_one.so" "$ROOT/vst/build/monomodule_fx.so"

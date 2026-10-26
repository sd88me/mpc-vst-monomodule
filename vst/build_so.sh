#!/usr/bin/env bash
# Builds monomodule_one.so and monomodule_fx.so for the Force (armhf), self-contained (just this repo's
# submodules + the CMakeLists.txt at its root), inside the mnm-armhf-builder-glibc231 image (built automatically
# from tools/Dockerfile.armhf-builder on first use).
#   build_so.sh <dir holding dsp56k_recomp.inl> <mpc-vst-plugins checkout> [armhf|aarch64]
# armhf (default): Gen1 (Force, MPC Live II, X, One...), outputs vst/build/<so>. aarch64: Gen2 (MPC Live III...), outputs
# vst/build[/fx/build]/aarch64/<so> via the mnm-aarch64-builder image; there the DSP runs on the dsp56300 JIT (arm64), not
# recompiled blocks, so the .so holds no firmware words and the <recomp dir> argument is not used (pass -).
# The recompiled DSP embeds firmware words: it is generated from the user's own OS file (libs/dsp56300/
# tools/arm32jit_prototype/recomp/) and never shipped -- REC below must point at your own build of it.
set -euo pipefail
MV=$(realpath "$2"); ARCH=${3:-armhf}
ROOT=$(cd "$(dirname "$0")/.." && pwd)   # repo root (this script lives in vst/)
case $ARCH in
  armhf)   IMG=mnm-armhf-builder-glibc231; DOCKERFILE=Dockerfile.armhf-builder;  TC=armhf.cmake;   STRIP=arm-linux-gnueabihf-strip; SUB=""; OBJ=obj
           REC=$(realpath "$1"); FLAGS="-DDSP56K_RECOMP -I/rec -mcpu=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard";;
  aarch64) IMG=mnm-aarch64-builder;       DOCKERFILE=Dockerfile.aarch64-builder; TC=aarch64.cmake; STRIP=aarch64-linux-gnu-strip;   SUB="aarch64/"; OBJ=obj-aarch64
           REC=$ROOT; FLAGS="";;
  *) echo "usage: build_so.sh <recomp dir> <mpc-vst-plugins> [armhf|aarch64]" >&2; exit 2;;
esac
if [ ! -f "$ROOT/libs/monomodule/src/core/firmware/Firmware.h" ] || [ ! -f "$ROOT/libs/dsp56300/source/asmjit/CMakeLists.txt" ]; then
  echo "submodules missing -- run: git -C '$ROOT' submodule update --init --recursive" >&2
  exit 1
fi
docker image inspect $IMG >/dev/null 2>&1 || docker build -q -t $IMG -f "$ROOT/tools/$DOCKERFILE" "$ROOT" >/dev/null
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/vst.json" --params-h
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/fx/vst.json" --params-h
# objects compiled by another builder image (an older glibc, say) must not be reused: ninja cannot tell the sysroot changed
ID=$(docker image inspect -f '{{.Id}}' $IMG)
[ "$(cat "$ROOT/vst/build/$OBJ/.builder" 2>/dev/null)" = "$ID" ] || rm -rf "$ROOT/vst/build/$OBJ"
mkdir -p "$ROOT/vst/build/$OBJ" "$ROOT/vst/build/$SUB" "$ROOT/vst/fx/build/$SUB"; echo "$ID" > "$ROOT/vst/build/$OBJ/.builder"
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$REC":/rec:ro -v "$MV":/mv:ro $IMG sh -c "
  cmake -S /r -B /r/vst/build/$OBJ -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/tools/$TC -DCMAKE_BUILD_TYPE=Release \
    -DMPC_VST_DIR=/mv '-DCMAKE_CXX_FLAGS=$FLAGS' '-DCMAKE_SHARED_LINKER_FLAGS=${FLAGS#*-I/rec}' >/dev/null &&
  { ninja -C /r/vst/build/$OBJ monomodule_one monomodule_fx mnm-vst-smoke mnm-vst-smoke-fx mnm-skin-gen >/tmp/ninja.log 2>&1 || { tail -n 25 /tmp/ninja.log; exit 1; }; } &&
  $STRIP -o /r/vst/build/${SUB}monomodule_one.so /r/vst/build/$OBJ/monomodule_one.so &&
  $STRIP -o /r/vst/fx/build/${SUB}monomodule_fx.so /r/vst/build/$OBJ/monomodule_fx.so"
md5sum "$ROOT/vst/build/${SUB}monomodule_one.so" "$ROOT/vst/fx/build/${SUB}monomodule_fx.so"

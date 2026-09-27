#!/usr/bin/env bash
# Builds Monomodule One and FX entirely from YOUR OWN Monomachine OS file. This is the only supported way
# to get the plugins: the DSP is static-recompiled ahead of time (there is no 32-bit ARM runtime JIT), and
# that recompiled code embeds your OS file's actual DSP program, so it must never be pre-built and shared —
# see libs/dsp56300/tools/arm32jit_prototype/recomp/README.md and vst/skin/.gitignore.
#
#   release/build_from_os.sh <your-os.syx> <mpc-vst-plugins checkout> [layout]
#
# Takes a few minutes (three separate Docker builds: x86 discovery, the bit-exactness gate, and the armhf
# .so + skin). Needs Docker and this repo's submodules (`git submodule update --init --recursive`).
# Output: vst/build/{monomodule_one.so, skin/}, vst/fx/build/{monomodule_fx.so, skin/}.
set -euo pipefail
OS=$(realpath "$1"); MV=$(realpath "$2"); LAYOUT="${3:-2x2}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ ! -f "$ROOT/libs/monomodule/src/core/firmware/Firmware.h" ] || [ ! -f "$ROOT/libs/dsp56300/source/asmjit/CMakeLists.txt" ]; then
  echo "submodules missing -- run: git -C '$ROOT' submodule update --init --recursive" >&2
  exit 1
fi
docker image inspect mnm-armhf-builder >/dev/null 2>&1 || docker build -q -t mnm-armhf-builder -f "$ROOT/tools/Dockerfile.armhf-builder" "$ROOT" >/dev/null
docker image inspect mnm-x86-builder >/dev/null 2>&1 || docker build -q -t mnm-x86-builder -f "$ROOT/tools/Dockerfile.x86-builder" "$ROOT" >/dev/null

echo "== 1/4: discovery build (x86) =="
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$WORK":/w mnm-x86-builder sh -c "
  cmake -S /r -B /w/disc -G Ninja -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-DDSP56K_RECOMP_DISCOVERY' >/dev/null &&
  ninja -C /w/disc mnm-recomp-discover mnm-golden 2>&1 | tail -5"

echo "== 2/4: tracing your OS file's DSP program =="
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r:ro -v "$WORK":/w -v "$(dirname "$OS")":/os:ro mnm-x86-builder sh -c "
  cd /w && MNM_DSP_INTERP=1 ./disc/mnm-recomp-discover /os/$(basename "$OS") disc.txt &&
  nm -C ./disc/mnm-recomp-discover > nm.txt &&
  python3 /r/libs/dsp56300/tools/arm32jit_prototype/recomp/recomp_gen2.py disc.txt nm.txt > dsp56k_recomp.inl"
echo "  $(wc -l < "$WORK/dsp56k_recomp.inl") lines generated"

echo "== 3/4: bit-exactness gate (x86 interpreter vs. the recompiled build, all 22 machines) =="
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$WORK":/w -v "$(dirname "$OS")":/os:ro mnm-x86-builder sh -c "
  cmake -S /r -B /w/gate -G Ninja -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-DDSP56K_RECOMP -I/w' >/dev/null &&
  ninja -C /w/gate mnm-golden 2>&1 | tail -5 &&
  cd /w &&
  MNM_DSP_INTERP=1 ./gate/mnm-golden /os/$(basename "$OS") > /w/recomp.hashes &&
  ./disc/mnm-golden /os/$(basename "$OS") > /w/reference.hashes &&
  diff /w/reference.hashes /w/recomp.hashes && echo GATE-PASSED"

echo "== 4/4: armhf .so + skins =="
"$ROOT/vst/build_so.sh" "$WORK" "$MV"
"$ROOT/vst/skin/build_skin.sh" "$OS" "$MV" "layout=$LAYOUT"
"$ROOT/vst/skin/build_skin.sh" "$OS" "$MV" "layout=$LAYOUT" fx=1

echo
echo "Built: vst/build/monomodule_one.so, vst/build/skin/, vst/fx/build/monomodule_fx.so, vst/fx/build/skin/"
echo "Deploy with release/install.sh <device-ip> (see release/README.md)."

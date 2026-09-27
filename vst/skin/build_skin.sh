#!/usr/bin/env bash
# Builds the skin folder from the user's own OS file. Needs Docker. usage: build_skin.sh <os.syx> [mpc-vst checkout]
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd); ROOT=$(cd "$HERE/.." && pwd)
OS=$(realpath "$1"); MV=$(realpath "${2:-$HOME/mpc-vst}")
U=libs/monomodule/src
mkdir -p "$HERE/build"
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$(dirname "$OS")":/os:ro -w /r mnm-x86-builder sh -c "
  g++ -std=c++17 -O1 -I$U/plugin/one -I$U/core -o vst/build/mnm-artdump vst/skin/mnm_artdump.cpp \
    $U/plugin/one/RomArt.cpp $U/plugin/one/SpecData.cpp $U/core/firmware/Firmware.cpp &&
  vst/build/mnm-artdump /os/$(basename "$OS") vst/build/art.json"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" >/dev/null
EXTRA="${@:3}"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MPC_VST_TOOLS=/mv/tools -v "$ROOT":/r -v "$MV":/mv:ro -w /r python:3.11-slim sh -c \
  "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 vst/skin/mk_skin.py vst/build/art.json vst/params.json vst/build/skin $EXTRA"

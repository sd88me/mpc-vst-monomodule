#!/usr/bin/env bash
# Extracts the factory kit bank from the user's own OS file as a normal kit-dump .syx.
#   extract_factory.sh <os.syx> <out.syx>
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd); ROOT=$(cd "$HERE/.." && pwd)
OS=$(realpath "$1"); OUT=$(realpath -m "$2")
U=libs/monomodule/src
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$(dirname "$OS")":/os:ro -v "$(dirname "$OUT")":/out -w /r mnm-x86-builder sh -c "
  g++ -std=c++17 -O1 -I$U/core -I$U/core/firmware -I$U/core/library -o /tmp/mnm-factorybank vst/skin/mnm_factorybank.cpp $U/core/library/MnmDump.cpp $U/core/library/MnmEncode.cpp $U/core/firmware/Firmware.cpp &&
  /tmp/mnm-factorybank /os/$(basename "$OS") /out/$(basename "$OUT")"

#!/usr/bin/env bash
# Builds the Gen2 (aarch64: MPC Live III and kin) download of Monomodule: two ordinary plugin zips, Monomodule One and Monomodule FX,
# with NOTHING of Elektron's in them. The DSP runs on the dsp56300 JIT, so (unlike the Gen1 build) the .so holds no code derived from the
# OS file, and the skin is a recipe the plugin fills in on the device from the user's own OS file. The user only drops
# Elektron_SFX6-60_OS1.32B.syx into the plugin's monomodule/ folder (one copy serves both plugins), loads the plugin once, and restarts MPC.
#
#   release/build_gen2.sh <Monomachine OS .syx> [-v version] [-m mpc-vst-plugins checkout] [-s]
#
#   The OS file is used here only to build and to check: the bit-exactness gate (the JIT must give the interpreter's audio on every
#   machine), and the skin recipe (which skin pixel follows which bit of the LCD art; the recipe holds no art, and is checked to give
#   mk_skin.py's skin exactly). -s skips the gate (it needs arm64 emulation, `docker run --platform linux/arm64`, about 15 minutes).
# Needs Docker. Output: dist/Monomodule-One-<version>-mpc-aarch64.zip and dist/Monomodule-FX-<version>-mpc-aarch64.zip.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
rp() { if [ -d "$1" ]; then (cd "$1" && pwd); elif [ -e "$1" ]; then echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; else echo "$1"; fi; }
usage() { sed -n '2,13p' "$0"; exit 2; }
[ $# -ge 1 ] || usage
OS=$(rp "$1"); shift
VERSION=""; MV="${MPC_VST_DIR:-}"; GATE=1
while getopts "v:m:sh" o; do case $o in v) VERSION=$OPTARG;; m) MV=$OPTARG;; s) GATE=0;; *) usage;; esac; done
if [ -z "$VERSION" ]; then VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed -E 's/^v//; s/^([0-9]+\.[0-9]+\.[0-9]+)-.*/\1/'); fi
case "$VERSION" in [0-9]*) ;; *) VERSION="0.0.0-dev.$VERSION";; esac
if [ -z "$MV" ]; then
  for c in "$ROOT/../mpc-vst" "$HOME/mpc-vst" "$HOME/.cache/mpc-vst-monomodule/mpc-vst-plugins"; do [ -f "$c/tools/release.py" ] && MV="$c" && break; done
  if [ -z "$MV" ]; then
    MV="$HOME/.cache/mpc-vst-monomodule/mpc-vst-plugins"; mkdir -p "$(dirname "$MV")"
    git clone -q https://github.com/sd88me/mpc-vst-plugins.git "$MV"
  fi
fi
MV=$(rp "$MV")
for f in "$OS" "$MV/tools/release.py" "$MV/tools/gen_vst.py" "$MV/tools/catalog_check.py"; do [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }; done
command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1 || { echo "Docker isn't available: start it (Windows: Docker Desktop with WSL integration on) and run this again." >&2; exit 1; }
cd "$ROOT"
[ -f libs/dsp56300/source/dsp56kEmu/dsp.h ] || git submodule update --init --recursive
WORK=$ROOT/build-gen2; mkdir -p "$WORK" dist
docker image inspect mnm-x86-builder >/dev/null 2>&1 || docker build -q -t mnm-x86-builder -f tools/Dockerfile.x86-builder . >/dev/null
echo "Monomodule $VERSION for Gen2  (plugins checkout: $MV)"

echo "== 1/4 plugin (aarch64, the dsp56300 JIT)"
vst/build_so.sh - "$MV" aarch64 2>&1 | grep -E 'error|^[0-9a-f]{32} '

if [ $GATE = 1 ]; then
  echo "== 2/4 bit-exactness gate: the JIT on arm64 (emulated) must give the x86 interpreter's audio on all 22 machines"
  rm -rf "$WORK/x86" && mkdir -p "$WORK/x86"
  docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$WORK/x86":/w -v "$(dirname "$OS")":/os:ro mnm-x86-builder sh -c "
    cmake -S /r -B /w/b -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 &&
    ninja -C /w/b mnm-golden >/dev/null 2>&1 && MNM_DSP_INTERP=1 /w/b/mnm-golden /os/$(basename "$OS") > /w/ref.hashes 2>/dev/null"
  docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$MV":/mv:ro mnm-aarch64-builder sh -c "ninja -C /r/vst/build/obj-aarch64 mnm-golden >/dev/null 2>&1"
  # MNM_PREWARM=1: with the prewarm the plugin does at load (it must not change a hash)
  docker run --rm --platform linux/arm64 -e MNM_PREWARM=1 -v "$ROOT":/r:ro -v "$(dirname "$OS")":/os:ro arm64v8/debian:bookworm-slim \
    /r/vst/build/obj-aarch64/mnm-golden "/os/$(basename "$OS")" > "$WORK/a64.hashes" 2>/dev/null
  diff "$WORK/x86/ref.hashes" "$WORK/a64.hashes" >/dev/null && echo "   GATE PASSED ($(tail -1 "$WORK/a64.hashes"))" \
    || { echo "GATE FAILED: the aarch64 JIT does not match the interpreter - not packaging" >&2; exit 1; }
else
  echo "== 2/4 gate skipped (-s)"
fi

echo "== 3/4 skin recipe (about 15 minutes: ~75 renders of the skins, then the check)"
rm -rf "$WORK/skin"
vst/skin/build_recipe.sh "$OS" "$WORK/skin" "$MV" | grep -E "^check:" | sed 's/^/   /'
rm -f "$WORK/skin/art.json" "$WORK/skin/images.pkl"; rm -rf "$WORK/skin/probe" "$WORK/skin/src"   # Elektron's art: not kept next to the release files
[ -f "$WORK/skin/recipe/art.bin" ] || { echo "no recipe was made" >&2; exit 1; }

echo "== 4/4 packages"
ONE="sd88me - VST - Monomodule One"; FX="sd88me - VST - Monomodule FX"
PAY="$WORK/payload"; rm -rf "$PAY" && mkdir -p "$PAY/monomodule/dumps" "$PAY/skin-recipe"
cp "$WORK/skin/recipe/art.bin" "$PAY/skin-recipe/art.bin"
touch "$PAY/monomodule/dumps/.keep"
cat > "$PAY/monomodule/PUT-OS-FILE-HERE.txt" <<'TXT'
Put your Monomachine OS file here, named Elektron_SFX6-60_OS1.32B.syx (OS 1.32B, a free download from Elektron).
One copy serves Monomodule One and Monomodule FX if both are installed (put it in either plugin's monomodule folder).
Then load the plugin once in MPC (it makes its skin from that file) and restart MPC to see the skin.
Your own kit dumps (.syx) go in dumps/.
TXT
python3 "$MV/tools/gen_vst.py" vst/vst.json >/dev/null; python3 "$MV/tools/gen_vst.py" vst/fx/vst.json >/dev/null   # their pluginlist-entry.xml
REQ="Your own Monomachine OS 1.32B file (a free download from Elektron), put in the plugin's monomodule/ folder; MPC restarted once after the first load (the plugin makes its skin from that file). Gen2 MPCs (aarch64) only"
python3 "$MV/tools/release.py" --so vst/build/aarch64/monomodule_one.so --skin "$WORK/skin/recipe/files/one/$ONE" --entry vst/build/pluginlist-entry.xml \
  --version "$VERSION" --extra "$PAY/monomodule:monomodule" --extra "$PAY/skin-recipe:skin-recipe" --user-data monomodule --user-files release/userfiles.json \
  --folder-name "Monomodule One" --id monomodule-one-gen2 --repo sd88me/mpc-vst-monomodule --license AGPL-3.0-or-later --requires "$REQ" \
  --about "Monomodule One: the Monomachine's synth engine as an MPC OS instrument (Gen2; runs your own OS file)" -o dist
python3 "$MV/tools/release.py" --so vst/fx/build/aarch64/monomodule_fx.so --skin "$WORK/skin/recipe/files/fx/$FX" --entry vst/fx/build/pluginlist-entry.xml \
  --version "$VERSION" --extra "$PAY/monomodule:monomodule" --extra "$PAY/skin-recipe:skin-recipe" --user-data monomodule \
  --folder-name "Monomodule FX" --id monomodule-fx-gen2 --repo sd88me/mpc-vst-monomodule --license AGPL-3.0-or-later --requires "$REQ" \
  --about "Monomodule FX: the Monomachine's effect machines as an MPC OS audio effect (Gen2; runs your own OS file)" -o dist
for z in dist/Monomodule-One-"$VERSION"-mpc-aarch64.zip dist/Monomodule-FX-"$VERSION"-mpc-aarch64.zip; do
  ls -l "$z"
  python3 "$MV/tools/catalog_check.py" "$z" --catalog --expect-repo sd88me/mpc-vst-monomodule
done
echo
echo "Built dist/Monomodule-{One,FX}-$VERSION-mpc-aarch64.zip: nothing of Elektron's inside."

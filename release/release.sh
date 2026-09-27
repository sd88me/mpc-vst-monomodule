#!/usr/bin/env bash
# One-command build + package (+ optional deploy), from just your OS file. Wraps build_from_os.sh,
# package.sh, and (with -d) install.sh into a single call, and auto-clones the mpc-vst-plugins sibling
# checkout so you don't need to find/clone it yourself first.
#
#   release/release.sh <your-os.syx> [-d <device-ip>] [-v <version>] [-m <mpc-vst-plugins checkout>] [-l layout]
#
# Examples:
#   release/release.sh mono.syx                        # -> dist/Monomodule-<version>-mpc-armv7.zip
#   release/release.sh mono.syx -d 192.168.1.44         # build, package, AND install straight onto the device
#
# Needs Docker, and ssh/scp on PATH if you pass -d. Everything else (mpc-vst-plugins checkout, version
# string) is figured out or cloned automatically. See README.md's "Install" section for what each step does.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MPC_VST_REPO="https://github.com/sd88me/mpc-vst-plugins.git"
MPC_VST_CACHE="$HOME/.cache/mpc-vst-monomodule/mpc-vst-plugins"

usage() { echo "usage: release/release.sh <your-os.syx> [-d device-ip] [-v version] [-m mpc-vst-plugins-checkout] [-l layout]" >&2; exit 1; }

[ $# -ge 1 ] || usage
OS="$1"; shift
DEVICE=""; VERSION=""; MV=""; LAYOUT="2x2"
while [ $# -gt 0 ]; do
  case "$1" in
    -d) DEVICE="$2"; shift 2 ;;
    -v) VERSION="$2"; shift 2 ;;
    -m) MV="$2"; shift 2 ;;
    -l) LAYOUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -f "$OS" ] || { echo "error: OS file not found: $OS" >&2; exit 1; }

# Auto-version: latest tag, else date-based dev version.
if [ -z "$VERSION" ]; then
  VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed 's/^v//')
  [ -n "$VERSION" ] || VERSION="dev-$(date +%Y%m%d)"
fi

# Auto-clone the mpc-vst-plugins sibling checkout if one wasn't given and none of the usual spots have it.
if [ -z "$MV" ]; then
  for candidate in "$HOME/mpc-vst" "$MPC_VST_CACHE"; do
    [ -f "$candidate/tools/gen_vst.py" ] && MV="$candidate" && break
  done
fi
if [ -z "$MV" ]; then
  echo "== 0/5: cloning mpc-vst-plugins (one-time; cached at $MPC_VST_CACHE) =="
  mkdir -p "$(dirname "$MPC_VST_CACHE")"
  git clone -q "$MPC_VST_REPO" "$MPC_VST_CACHE"
  MV="$MPC_VST_CACHE"
elif [ -d "$MV/.git" ]; then
  echo "== 0/5: updating mpc-vst-plugins checkout at $MV =="
  git -C "$MV" pull -q --ff-only || echo "  (couldn't fast-forward -- using it as-is)"
fi

echo "== building Monomodule $VERSION from $(basename "$OS") =="
"$ROOT/release/build_from_os.sh" "$OS" "$MV" "$LAYOUT"

echo "== packaging =="
"$ROOT/release/package.sh" "$VERSION"
ZIP="$ROOT/dist/Monomodule-$VERSION-mpc-armv7.zip"
echo "Built: $ZIP"

if [ -n "$DEVICE" ]; then
  echo "== deploying to root@$DEVICE =="
  TOP="Monomodule-$VERSION-mpc-armv7"
  STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
  python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$ZIP" "$STAGE"
  scp -r "$STAGE/$TOP" "root@$DEVICE:/tmp/"
  ssh "root@$DEVICE" "sh /tmp/$TOP/install.sh"
  echo "Deployed and installed on $DEVICE."
else
  echo "Deploy with:"
  echo "  scp -r dist/Monomodule-$VERSION-mpc-armv7 root@<device-ip>:/tmp/"
  echo "  ssh root@<device-ip> sh /tmp/Monomodule-$VERSION-mpc-armv7/install.sh"
  echo "or re-run with -d <device-ip> to do that automatically."
fi

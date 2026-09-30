#!/usr/bin/env bash
# Packages an already-built vst/build + vst/fx/build (see release/build_from_os.sh) into one shareable zip that holds
# TWO portable plugin packages (One and FX), each made by mpc-vst-plugins' tools/release.py (docs/RELEASING.md), plus a
# top-level install.sh/uninstall.sh that run both. Each plugin is one self-contained folder in /sdcard/Synths, and BOTH
# folders carry your OS file (the engine reads it at run time from <plugin folder>/monomodule/). Kit dumps go in One's
# monomodule/dumps (user data, kept across upgrades). This zip is per-user: it was built from YOUR OS file, so don't
# share it around.
#   release/package.sh <version> <your-os.syx> [-o dist] [-m <mpc-vst-plugins checkout>]
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: release/package.sh <version> <your-os.syx> [-o dist] [-m checkout]" >&2; exit 1; }
VERSION="$1"; OS=$(realpath "$2"); shift 2
OUT="dist"; MV="${MPC_VST:-}"
while [ $# -gt 0 ]; do
  case "$1" in -o) OUT="$2"; shift 2 ;; -m) MV="$2"; shift 2 ;; *) echo "unknown option $1" >&2; exit 1 ;; esac
done
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "$MV" ]; then
  for c in "$HOME/mpc-vst" "$HOME/.cache/mpc-vst-monomodule/mpc-vst-plugins" "$ROOT/../mpc-vst-plugins"; do
    [ -f "$c/tools/release.py" ] && MV="$c" && break
  done
fi
[ -f "$MV/tools/release.py" ] || { echo "need an mpc-vst-plugins checkout (-m or MPC_VST)" >&2; exit 1; }
[ -f "$OS" ] || { echo "OS file not found: $OS" >&2; exit 1; }
ONE="shnolk - VST - Monomodule One"; FX="shnolk - VST - Monomodule FX"
for f in "$ROOT/vst/build/monomodule_one.so" "$ROOT/vst/build/monomodule_fx.so" \
         "$ROOT/vst/build/skin/$ONE" "$ROOT/vst/fx/build/skin/$FX" \
         "$ROOT/vst/build/pluginlist-entry.xml" "$ROOT/vst/fx/build/pluginlist-entry.xml"; do
  [ -e "$f" ] || { echo "missing: $f -- run release/build_from_os.sh first" >&2; exit 1; }
done

for so in monomodule_one monomodule_fx; do   # a .so built before the portable layout still looks in /sdcard/vst and finds no OS file
  strings "$ROOT/vst/build/$so.so" | grep -q "/proc/self/maps" || { echo "$so.so is stale (no plugin-dir lookup): re-run release/build_from_os.sh" >&2; exit 1; }
done

TOP="Monomodule-$VERSION-mpc-armv7"
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
D="$STAGE/$TOP"; mkdir -p "$D"
OSDIR="$STAGE/osdata"; mkdir -p "$OSDIR/dumps"
cp "$OS" "$OSDIR/Elektron_SFX6-60_OS1.32B.syx"
touch "$OSDIR/dumps/.keep"

common=(--version "$VERSION" --repo sd88me/mpc-vst-monomodule --license "AGPL-3.0-or-later" -o "$STAGE/pkg"
        --requires "Built from your own Monomachine OS file; only for the device you built it for")
python3 "$MV/tools/release.py" --so "$ROOT/vst/build/monomodule_one.so" --skin "$ROOT/vst/build/skin/$ONE" \
  --entry "$ROOT/vst/build/pluginlist-entry.xml" --id monomodule-one --extra "$OSDIR:monomodule" \
  --user-data monomodule/dumps --about "Monomachine engine as an instrument. " "${common[@]}"
python3 "$MV/tools/release.py" --so "$ROOT/vst/build/monomodule_fx.so" --skin "$ROOT/vst/fx/build/skin/$FX" \
  --entry "$ROOT/vst/fx/build/pluginlist-entry.xml" --id monomodule-fx --extra "$OSDIR:monomodule" \
  --about "Monomachine engine as an audio effect. " "${common[@]}"
for z in "$STAGE"/pkg/*.zip; do python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$z" "$STAGE/x"; done
mv "$STAGE"/x/Monomodule-One-* "$D/one"
mv "$STAGE"/x/Monomodule-FX-* "$D/fx"

cat > "$D/install.sh" <<'EOF'
#!/bin/sh
# Installs Monomodule One and Monomodule FX (each as its own folder in /sdcard/Synths). Run on the device as root:
#   sh install.sh [-y] [-t <synths-dir>]     (the arguments are passed on to both installers)
set -e
cd "$(dirname "$0")"
YES=0; for a in "$@"; do [ "$a" = "-y" ] && YES=1; done
if [ $YES = 0 ]; then
    printf "Install Monomodule One and FX? MPC is stopped and restarted (once per plugin). Save your project first. [y/N] "
    read -r ok; case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi
sh one/install.sh -y "$@"
sh fx/install.sh -y "$@"
EOF
sed 's/Installs/Removes/; s/install\.sh/uninstall.sh/g; s/Install Mono/Remove Mono/' "$D/install.sh" > "$D/uninstall.sh"
chmod +x "$D/install.sh" "$D/uninstall.sh"
cat > "$D/INSTALL.md" <<EOF
# Monomodule for MPC OS $VERSION

Built from your own Monomachine OS file. **Only for the device you built it for**: the plugin embeds your OS file's DSP
program, so don't pass this zip to anyone else. Needs a first-generation MPC OS standalone device (32-bit ARM: Force,
MPC Live/Live II, One, X, Key 61) and root SSH access.

\`\`\`
scp -r $TOP root@<device-ip>:/tmp/
ssh root@<device-ip> sh /tmp/$TOP/install.sh
\`\`\`

Installs **Monomodule One** (instrument) and **Monomodule FX** (effect), each as one folder in \`/sdcard/Synths\`
(save your project first: MPC restarts). Your OS file is inside both folders. Kit \`.syx\` dumps go in
\`/sdcard/Synths/$ONE/monomodule/dumps/\` (kept on upgrade) or MPC's \`/sdcard/Force Documents/Monomachine Dumps/\`.
An older install in \`/sdcard/vst\` is replaced and its dumps moved. \`uninstall.sh\` removes both. Each plugin's own
instructions are in \`one/INSTALL.md\` and \`fx/INSTALL.md\`.
EOF

mkdir -p "$OUT"
python3 - "$STAGE" "$TOP" "$OUT/$TOP.zip" <<'PYEOF'
import os, stat, sys, zipfile
stage, top, out = sys.argv[1:4]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(os.path.join(stage, top)):
        dirs.sort(); files.sort()
        for f in files:
            p = os.path.join(root, f)
            zi = zipfile.ZipInfo.from_file(p, os.path.relpath(p, stage))
            zi.external_attr = (stat.S_IMODE(os.lstat(p).st_mode) << 16) | (zi.external_attr & 0xFFFF)
            with open(p, "rb") as fh:
                z.writestr(zi, fh.read(), zipfile.ZIP_DEFLATED)
PYEOF
echo "$OUT/$TOP.zip"

#!/usr/bin/env bash
# Packages an already-built vst/build + vst/fx/build (see release/build_from_os.sh) into one shareable zip
# with install.sh/uninstall.sh, following mpc-vst-plugins' release convention (docs/RELEASING.md) but for
# two plugins at once. This zip is per-user: it was built from YOUR OS file, so don't share it around.
#   release/package.sh <version> [-o dist]
set -euo pipefail
VERSION="$1"; OUT="dist"
[ "${2:-}" = "-o" ] && OUT="$3"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REL="$ROOT/release"
for f in "$ROOT/vst/build/monomodule_one.so" "$ROOT/vst/build/monomodule_fx.so" \
         "$ROOT/vst/build/skin/shnolk - VST - Monomodule One" "$ROOT/vst/fx/build/skin/shnolk - VST - Monomodule FX" \
         "$ROOT/vst/build/pluginlist-entry.xml" "$ROOT/vst/fx/build/pluginlist-entry.xml"; do
  [ -e "$f" ] || { echo "missing: $f -- run release/build_from_os.sh first" >&2; exit 1; }
done

TOP="Monomodule-$VERSION-mpc-armv7"
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
D="$STAGE/$TOP"
mkdir -p "$D/payload/vst" "$D/payload/Synths"
cp "$ROOT/vst/build/monomodule_one.so" "$D/payload/vst/"
cp "$ROOT/vst/build/monomodule_fx.so" "$D/payload/vst/"
cp -a "$ROOT/vst/build/skin/shnolk - VST - Monomodule One" "$D/payload/Synths/"
cp -a "$ROOT/vst/fx/build/skin/shnolk - VST - Monomodule FX" "$D/payload/Synths/"
cp "$ROOT/vst/build/pluginlist-entry.xml" "$D/plugin_one.xml"
cp "$ROOT/vst/fx/build/pluginlist-entry.xml" "$D/plugin_fx.xml"
sed "s/@VERSION@/$VERSION/g" "$REL/install.sh" > "$D/install.sh"
sed "s/@VERSION@/$VERSION/g" "$REL/uninstall.sh" > "$D/uninstall.sh"
cp "$REL/plugin_list.awk" "$D/"
chmod +x "$D/install.sh" "$D/uninstall.sh"

( cd "$D" && find payload plugin_one.xml plugin_fx.xml -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )

cat > "$D/INSTALL.md" <<EOF
# Monomodule for MPC OS $VERSION

Built from your own Monomachine OS file (release/build_from_os.sh). **Only for the device you built it
for** -- the plugin embeds your OS file's DSP program (see the repo README's "Why this exists"); don't
pass this zip to anyone else. Requirements: a first-generation MPC OS standalone device (32-bit ARM:
Force, MPC Live/Live II, One, X, Key 61), root SSH access.

## Install

\`\`\`
scp -r $TOP root@<device-ip>:/tmp/
ssh root@<device-ip> sh /tmp/$TOP/install.sh
\`\`\`

Stops MPC (save your project first), installs both **Monomodule One** (instrument) and **Monomodule FX**
(audio effect), backs up \`MPC.settings\`, and restarts MPC. Running it again upgrades in place.

Add presets: copy Monomachine kit \`.syx\` dumps (including the factory bank, if you extracted one with
\`release/extract_factory.sh\`) into \`/sdcard/vst/monomodule/dumps/\` or MPC's own
\`/sdcard/Force Documents/Monomachine Dumps/\` -- both are watched live, no reinsert needed.

## Uninstall

\`\`\`
ssh root@<device-ip> sh /tmp/$TOP/uninstall.sh
\`\`\`

## Manual steps (if you'd rather not run the installer)

1. Copy \`payload/vst/monomodule_one.so\` and \`payload/vst/monomodule_fx.so\` to \`/sdcard/vst/\`.
2. Copy \`payload/Synths/shnolk - VST - Monomodule One\` and \`... FX\` to \`/sdcard/Synths/\`.
3. Stop MPC (\`systemctl stop acvs\`), back up \`MPC.settings\`, insert the \`<PLUGIN .../>\` line from
   \`plugin_one.xml\` and \`plugin_fx.xml\` into its \`pluginList-arm\` \`<KNOWNPLUGINS>\`, restart MPC
   (\`systemctl start acvs\`).

## Known issue

Q-Link nudges on a 0-127 knob can climb a few steps then reset near 0 in MPC's **Track** Q-Link mode;
**Screen** mode is unaffected. See the repo README's Status section.
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
            arc = os.path.relpath(p, stage)
            zi = zipfile.ZipInfo.from_file(p, arc)
            zi.external_attr = (stat.S_IMODE(os.lstat(p).st_mode) << 16) | (zi.external_attr & 0xFFFF)
            with open(p, "rb") as fh:
                z.writestr(zi, fh.read(), zipfile.ZIP_DEFLATED)
PYEOF
echo "$OUT/$TOP.zip"

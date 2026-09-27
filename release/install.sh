#!/bin/sh
# Monomodule for MPC OS @VERSION@ installer. Installs BOTH plugins (Monomodule One and Monomodule FX; comment out
# the FX block below if you only want One). Run on the device as root:
#   sh install.sh [-y]
# Stops MPC, copies both plugins and their skins, backs up MPC.settings, adds both plugin-list entries and
# starts MPC again. Safe to run again (upgrades in place).
set -e
cd "$(dirname "$0")"
YES=0; [ "$1" = "-y" ] && YES=1
die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
case "$(uname -m)" in armv7*) ;; *) die "this build is for 32-bit ARM MPC OS devices (Gen1); this one is $(uname -m)" ;; esac
SETTINGS=$(ls /media/az01-internal/Settings/*/MPC.settings 2>/dev/null | head -n 1)
[ -n "$SETTINGS" ] || die "MPC.settings not found (not an MPC OS device?)"
command -v systemctl >/dev/null || die "systemctl not found"
sha256sum -c SHA256SUMS >/dev/null 2>&1 || die "files damaged (SHA256SUMS mismatch): copy the folder again"
grep -q '/sdcard/Synths' "$SETTINGS" || echo "warning: /sdcard/Synths isn't in MPC's SynthContentLocations; the skin may not show"

echo "Installing Monomodule for MPC OS @VERSION@ (Monomodule One + Monomodule FX):"
echo "  /sdcard/vst/monomodule_one.so, /sdcard/vst/monomodule_fx.so, both skins, and two entries in $SETTINGS"
if [ $YES = 0 ]; then
    printf "MPC will be stopped and restarted. Save your project first. Continue? [y/N] "
    read -r ok; case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi

systemctl stop acvs
trap 'systemctl start acvs' EXIT
i=0; while pidof MPC >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
pidof MPC >/dev/null && die "MPC did not stop"

mkdir -p /sdcard/vst /sdcard/Synths
cp payload/vst/monomodule_one.so /sdcard/vst/monomodule_one.so.new && mv /sdcard/vst/monomodule_one.so.new /sdcard/vst/monomodule_one.so
cp payload/vst/monomodule_fx.so /sdcard/vst/monomodule_fx.so.new && mv /sdcard/vst/monomodule_fx.so.new /sdcard/vst/monomodule_fx.so
mkdir -p /sdcard/vst/monomodule/dumps
rm -rf "/sdcard/Synths/shnolk - VST - Monomodule One"; cp -a "payload/Synths/shnolk - VST - Monomodule One" /sdcard/Synths/
rm -rf "/sdcard/Synths/shnolk - VST - Monomodule FX"; cp -a "payload/Synths/shnolk - VST - Monomodule FX" /sdcard/Synths/

BAK="$SETTINGS.bak-monomodule-$(date +%Y%m%d-%H%M%S)"
cp "$SETTINGS" "$BAK"
awk -v mode=add -v file=/sdcard/vst/monomodule_one.so -v entryfile=plugin_one.xml -f plugin_list.awk "$SETTINGS" > "$SETTINGS.new1"
awk -v mode=add -v file=/sdcard/vst/monomodule_fx.so -v entryfile=plugin_fx.xml -f plugin_list.awk "$SETTINGS.new1" > "$SETTINGS.new2"
rm -f "$SETTINGS.new1"
n1=$(grep -c 'file="/sdcard/vst/monomodule_one.so"' "$SETTINGS.new2" || true)
n2=$(grep -c 'file="/sdcard/vst/monomodule_fx.so"' "$SETTINGS.new2" || true)
[ "$n1" = 1 ] && [ "$n2" = 1 ] || { rm -f "$SETTINGS.new2"; die "settings edit failed (entry counts $n1/$n2); MPC.settings unchanged"; }
if command -v python3 >/dev/null; then
    python3 -c 'import sys, xml.etree.ElementTree as E; E.parse(sys.argv[1])' "$SETTINGS.new2" 2>/dev/null ||
        { rm -f "$SETTINGS.new2"; die "edited settings aren't valid XML; MPC.settings unchanged"; }
fi
mv "$SETTINGS.new2" "$SETTINGS"
sync

echo "Done. Settings backup: $BAK"
echo "Starting MPC. Add 'Monomodule One' and/or 'Monomodule FX' to a track from the plugin browser."
echo "Presets: copy Monomachine kit .syx dumps into /sdcard/vst/monomodule/dumps/ or /sdcard/Force Documents/Monomachine Dumps/"

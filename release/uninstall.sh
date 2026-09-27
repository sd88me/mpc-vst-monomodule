#!/bin/sh
# Monomodule for MPC OS uninstaller. Removes BOTH plugins, their skins and their MPC.settings entries. Run on
# the device as root:
#   sh uninstall.sh [-y]
set -e
cd "$(dirname "$0")"
YES=0; [ "$1" = "-y" ] && YES=1
die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
SETTINGS=$(ls /media/az01-internal/Settings/*/MPC.settings 2>/dev/null | head -n 1)
[ -n "$SETTINGS" ] || die "MPC.settings not found"
if [ $YES = 0 ]; then
    printf "Remove Monomodule One and Monomodule FX? MPC will be stopped and restarted. Save your project first. [y/N] "
    read -r ok; case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi

systemctl stop acvs
trap 'systemctl start acvs' EXIT
i=0; while pidof MPC >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
pidof MPC >/dev/null && die "MPC did not stop"

BAK="$SETTINGS.bak-monomodule-$(date +%Y%m%d-%H%M%S)"
cp "$SETTINGS" "$BAK"
awk -v mode=remove -v file=/sdcard/vst/monomodule_one.so -f plugin_list.awk "$SETTINGS" > "$SETTINGS.new1"
awk -v mode=remove -v file=/sdcard/vst/monomodule_fx.so -f plugin_list.awk "$SETTINGS.new1" > "$SETTINGS.new2"
rm -f "$SETTINGS.new1"
n1=$(grep -c 'file="/sdcard/vst/monomodule_one.so"' "$SETTINGS.new2" || true)
n2=$(grep -c 'file="/sdcard/vst/monomodule_fx.so"' "$SETTINGS.new2" || true)
[ "$n1" = 0 ] && [ "$n2" = 0 ] || { rm -f "$SETTINGS.new2"; die "settings edit failed; MPC.settings unchanged"; }
if command -v python3 >/dev/null; then
    python3 -c 'import sys, xml.etree.ElementTree as E; E.parse(sys.argv[1])' "$SETTINGS.new2" 2>/dev/null ||
        { rm -f "$SETTINGS.new2"; die "edited settings aren't valid XML; MPC.settings unchanged"; }
fi
mv "$SETTINGS.new2" "$SETTINGS"

rm -f /sdcard/vst/monomodule_one.so /sdcard/vst/monomodule_fx.so
rm -rf "/sdcard/Synths/shnolk - VST - Monomodule One" "/sdcard/Synths/shnolk - VST - Monomodule FX"
sync
echo "Removed. Settings backup: $BAK"
echo "Left in place: /sdcard/vst/monomodule/ (your OS file and dumps) -- remove by hand if you want it gone too."

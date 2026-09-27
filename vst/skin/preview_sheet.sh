#!/usr/bin/env bash
# Colour-preset contact sheet (offline): vst/skin/preview_sheet.sh [layout] -> vst/build/colour_presets.png
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); MV=${MPC_VST:-$HOME/mpc-vst}; L=${1:-tabs}
LIST="default inverted lowcontrast red red-inverted blue blue-inverted green green-inverted orange orange-inverted"
for v in $LIST; do
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MPC_VST_TOOLS=/mv/tools -v "$ROOT":/r -v "$MV":/mv:ro -w /r python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 vst/skin/mk_skin.py vst/build/art.json vst/params.json vst/build/skin_$v skin=$v layout=$L" >/dev/null
done
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/r -w /r python:3.11-slim sh -c \
  "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 -c \"
from PIL import Image
names='$LIST'.split()
ims=[Image.open('vst/build/skin_%s/preview_tab0.png'%n).crop((0,20,1280,600)).resize((640,290)) for n in names]
cols=2; rows=(len(ims)+1)//2
sheet=Image.new('RGB',(1280,290*rows))
for i,im in enumerate(ims): sheet.paste(im,((i%cols)*640,(i//cols)*290))
sheet.save('vst/build/colour_presets.png')
\""

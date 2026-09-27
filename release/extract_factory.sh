#!/usr/bin/env bash
# Convenience wrapper: extracts the factory kit bank from your OS file (see vst/skin/extract_factory.sh
# for what this actually does, and README.md's "The factory bank"). Drop the result straight into
# /sdcard/vst/monomodule/dumps/ or MPC's own Documents area.
#   release/extract_factory.sh <your-os.syx> <out.syx>
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec "$ROOT/vst/skin/extract_factory.sh" "$@"

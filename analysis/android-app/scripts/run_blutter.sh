#!/usr/bin/env bash
# Decompile the Flutter libapp.so with blutter (https://github.com/worawit/blutter).
# Needs: capstone-devel libicu-devel cmake ninja gcc>=13; python venv with pyelftools requests.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; TP="$ROOT/third_party"
[ -d "$TP/blutter" ] || git clone --depth 1 https://github.com/worawit/blutter.git "$TP/blutter"
[ -d "$TP/blutter-venv" ] || { python3 -m venv "$TP/blutter-venv"; "$TP/blutter-venv/bin/pip" install pyelftools requests; }
cd "$TP/blutter"
"$TP/blutter-venv/bin/python3" blutter.py "$TP/apk/ex/arm64/lib/arm64-v8a" "$TP/apk/blutter_out"

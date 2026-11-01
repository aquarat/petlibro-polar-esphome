#!/usr/bin/env bash
# Decompile the Java/Kotlin part (base split) with jadx 1.5.6 into third_party/apk/jadx_out.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; TP="$ROOT/third_party"
if [ ! -x "$TP/jadx/bin/jadx" ]; then
  curl -sSL -o "$TP/jadx.zip" https://github.com/skylot/jadx/releases/download/v1.5.6/jadx-1.5.6.zip
  mkdir -p "$TP/jadx" && unzip -oq "$TP/jadx.zip" -d "$TP/jadx"
fi
"$TP/jadx/bin/jadx" -j 8 --show-bad-code -d "$TP/apk/jadx_out" "$TP/apk/xapk/com.designlibro.petlibro.apk" || true

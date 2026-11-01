#!/usr/bin/env bash
# Reproduce the string searches used for analysis/android-app/README.md.
# Needs: third_party/apk/ex (unzipped splits) and third_party/apk/blutter_out (run_blutter.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; TP="$ROOT/third_party"; B="$TP/apk/blutter_out"
LIB="$TP/apk/ex/arm64/lib/arm64-v8a/libapp.so"
[ -f "$LIB" ] || { mkdir -p "$TP/apk/ex/base" "$TP/apk/ex/arm64";
  unzip -oq "$TP/apk/xapk/com.designlibro.petlibro.apk" -d "$TP/apk/ex/base";
  unzip -oq "$TP/apk/xapk/config.arm64_v8a.apk" -d "$TP/apk/ex/arm64"; }
strings -n 6 "$LIB" > "$TP/libapp.strings"
# English translation table = the Map<String,String> in pp.txt that contains "view_feed109_upcoming": "Upcoming"
awk '/Map<String, String>\(/{buf=""; inmap=1} inmap{buf=buf $0 "\n"} /^}/{ if (inmap && buf ~ /"view_feed109_upcoming": "Upcoming"/) printf "%s", buf; inmap=0}' \
    "$B/pp.txt" > "$TP/en_strings.txt"
echo "== REST endpoints (wet feeder / plate / motor / ota)"
grep -E '^/(device|data)/' "$TP/libapp.strings" | sort -u | grep -iE 'wet|plate|motor|rotor|resume|ota|hardVersion|Lighting|Sound|realInfo'
echo "== AF109 English strings"
grep -E '"(view_feed109|view_product_feeder_109)[a-z0-9_]*"' "$TP/en_strings.txt"
echo "== AF109 Dart files"
find "$B/asm" -ipath '*109*'

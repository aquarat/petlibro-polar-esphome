#!/usr/bin/env bash
# Download the Petlibro Android app (com.designlibro.petlibro) from APKPure's public mirror.
# Output: third_party/apk/<filename> plus SHA256SUMS. Usage: download_apk.sh [version|latest]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/third_party/apk"; mkdir -p "$OUT"; cd "$OUT"
PKG=com.designlibro.petlibro
VER="${1:-latest}"
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/130 Safari/537.36"
URL="https://d.apkpure.com/b/XAPK/$PKG?version=$VER"
echo "source: $URL"
curl -sSL -A "$UA" -o petlibro.xapk.part -D headers.txt "$URL"
FN=$(grep -i content-disposition headers.txt | tail -1 | sed -E 's/.*filename="?([^";]+)"?.*/\1/' | tr -d '\r')
mv petlibro.xapk.part "$FN"
sha256sum "$FN" | tee -a SHA256SUMS

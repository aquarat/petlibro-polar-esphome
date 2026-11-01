#!/usr/bin/env bash
# Two full reads + hash compare. Read-only. SIZE from flash-id (default 4 MB).
set -euo pipefail
cd "$(dirname "$0")/.."
P=${PORT:-/dev/ttyUSB0}; S=${SIZE:-0x400000}; B=${BAUD:-460800}; T=$(date +%Y%m%dT%H%M%S)
C="--chip esp32c3 -p $P -b $B --before no-reset --after no-reset"
for i in 1 2; do tools/ro_esptool.sh esptool $C read-flash 0 $S firmware/stock/${T}_polar_backup_$i.bin; done
sha256sum firmware/stock/${T}_polar_backup_*.bin | tee firmware/stock/${T}_sha256.txt
[ "$(cut -d' ' -f1 firmware/stock/${T}_sha256.txt | sort -u | wc -l)" = 1 ] && echo "MATCH" || { echo "MISMATCH - do not trust either dump"; exit 1; }

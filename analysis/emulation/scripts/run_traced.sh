#!/usr/bin/env bash
# Boot a patched copy under QEMU with MMIO tracing AND the gdb hook driver (gdb/hooks.py).
# Usage: run_traced.sh IMAGE SECONDS [SCENARIO]   (SCENARIO = idle | inputs)
# Outputs: work/<name>.uart0.log, work/<name>.mmio.txt, work/<name>.hooks.log
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
IMG=$(realpath "$1"); SECS=${2:-120}; SCEN=${3:-inputs}
NAME=$(basename "$IMG" .bin)
cd "$HERE"
GDB=1 TRACE=work/$NAME.mmio.txt scripts/run_qemu.sh "$IMG" "$SECS" &
QPID=$!
sleep 1
HOOK_LOG=work/$NAME.hooks.log SCENARIO=$SCEN timeout "$SECS" gdb -batch -x gdb/connect.gdb -x gdb/hooks.py > work/$NAME.gdb.out 2>&1 || true
wait $QPID || true
echo "hooks: work/$NAME.hooks.log  mmio: work/$NAME.mmio.txt"

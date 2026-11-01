#!/usr/bin/env bash
# Run a (patched copy of the) PLAF109 flash image in Espressif QEMU, offline.
# Usage: run_qemu.sh IMAGE [SECONDS] [extra qemu args...]
#   env: GDB=1 adds -s -S (gdb stub on :1234, wait for gdb)
#        TRACE=file  writes memory_region_ops_{read,write} trace through mmio_filter.py to file
# Outputs work/<name>.uart0.log (raw UART0 bytes).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
Q=$HERE/../../third_party/qemu-esp/qemu/bin/qemu-system-riscv32
IMG=$(realpath "$1"); SECS=${2:-60}; shift || true; shift || true
case "$IMG" in */firmware/*) echo "refusing to run on firmware/ original; copy to work/ first" >&2; exit 1;; esac
NAME=$(basename "$IMG" .bin)
OUT=$HERE/work/$NAME.uart0.log
ARGS=(-M esp32c3 -nographic -nic none -monitor none
      -drive file="$IMG",if=mtd,format=raw -serial file:"$OUT")
[ "${GDB:-0}" = 1 ] && ARGS+=(-s -S)
if [ -n "${TRACE:-}" ]; then
  ARGS+=(-trace memory_region_ops_write -trace memory_region_ops_read -D /dev/stderr)
  timeout "$SECS" "$Q" "${ARGS[@]}" "$@" 2> >(python3 "$HERE/scripts/mmio_filter.py" > "$TRACE") >/dev/null || true
  sleep 2
else
  timeout "$SECS" "$Q" "${ARGS[@]}" "$@" >/dev/null 2>&1 || true
fi
echo "UART0 log: $OUT"

#!/usr/bin/env bash
# Read-only wrapper: refuses any esptool/espefuse subcommand that can modify the chip.
set -euo pipefail
cd "$(dirname "$0")/.."
tool=$1; shift
allow_esptool='^(chip-id|chip_id|flash-id|flash_id|get-security-info|get_security_info|read-mac|read_mac|read-flash|read_flash|read-flash-status|read_flash_status|image-info|image_info|verify-flash|verify_flash|read-mem|read_mem)$'
allow_espefuse='^(summary|dump|get-custom-mac|get_custom_mac|check-error|check_error|adc-info|adc_info)$'
deny='(write|erase|burn|merge|load[-_]ram|run|protect|execute|set|flash[-_]encrypt|encrypt|reset|elf2image|sign|key)'
case $tool in esptool) allow=$allow_esptool;; espefuse) allow=$allow_espefuse;; *) echo "usage: ro_esptool.sh esptool|espefuse ..." >&2; exit 2;; esac
ok=0; prev=
for a in "$@"; do
  case $a in -*) prev=$a; continue;; esac
  case $prev in --chip|-c|--port|-p|--baud|-b|--before|--after|--connect-attempts) prev=; continue;; esac
  prev=
  [[ $a =~ ^$deny ]] && { echo "REFUSED: '$a' looks like a write" >&2; exit 2; }
  [[ $a =~ $allow ]] && ok=1
done
[[ $ok == 1 ]] || [[ " $* " == *" version "* ]] || { echo "REFUSED: no allowed read-only subcommand in: $*" >&2; exit 2; }
exec .venv/bin/$tool "$@"

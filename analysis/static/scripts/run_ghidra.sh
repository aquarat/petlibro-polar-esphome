#!/bin/bash
# Re-run the full static analysis. Needs third_party/ghidra_12.2_PUBLIC (with linux_arm_64 natives
# built from Ghidra/Features/Decompiler/src/decompile/cpp: `make ghidra_opt sleigh_opt ARCH_TYPE=`)
# and third_party/esp-idf-4.4/components/esp_rom/esp32c3/ld.
set -euo pipefail
R=$(cd "$(dirname "$0")/../../.." && pwd)
S=$R/analysis/static
W=$S/work
mkdir -p $W/ghidra_proj $W/out
python3 -I $S/scripts/img2elf.py $R/firmware/stock/partitions/ota_0.bin $W/ota_0.elf
$R/third_party/ghidra_12.2_PUBLIC/support/analyzeHeadless $W/ghidra_proj af109 \
  -import $W/ota_0.elf -overwrite -processor RISCV:LE:32:default -cspec gcc \
  -scriptPath $S/ghidra_scripts \
  -preScript Esp32c3Setup.java $R/third_party/esp-idf-4.4/components/esp_rom/esp32c3/ld \
  -postScript NameAndDump.java $W/out \
  -analysisTimeoutPerFile 3600 2>&1 | tee $W/ghidra.log | grep -E "Esp32c3Setup|named|decompiled|ERROR" || true

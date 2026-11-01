#!/usr/bin/env bash
# Run ONLY with the chip already in download mode (log shows "waiting for download").
# Read-only. Stop the passive logger first: only one process can own the port.
set -euo pipefail
cd "$(dirname "$0")/.."
P=${PORT:-/dev/ttyUSB0}; T=$(date +%Y%m%dT%H%M%S); O=captures/${T}_security
R="tools/ro_esptool.sh"; C="--chip esp32c3 -p $P --before no-reset --after no-reset"
$R esptool $C flash-id          2>&1 | tee $O.flash_id.txt
$R esptool $C get-security-info 2>&1 | tee $O.security_info.txt
$R espefuse $C summary          2>&1 | tee $O.efuse_summary.txt
grep -E 'SECURE_BOOT|SPI_BOOT_CRYPT_CNT|DIS_DOWNLOAD|ENABLE_SECURITY_DOWNLOAD|DIS_USB|DIS_PAD_JTAG|KEY_PURPOSE|MAC' $O.efuse_summary.txt | tee $O.key_fields.txt

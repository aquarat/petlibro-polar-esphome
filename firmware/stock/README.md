# Stock firmware

`20261030T203938_polar_backup_1.bin`: full 4 MB read of the device on 2026-10-30, taken twice,
byte-identical (`*_sha256.txt`). `_2` is kept locally, not committed.

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | 0x0 | 0x8000 | ESP-IDF v4.4.6-dirty 2nd stage, checksum+SHA valid |
| partition table | 0x8000 | 0x1000 | MD5 valid |
| nvs | 0x9000 | 0x20000 | Device settings; may contain Wi-Fi and cloud credentials |
| otadata | 0x29000 | 0x2000 | copy 1: seq 1, VALID (ota_0); copy 2 blank in the dump |
| phy_init | 0x2b000 | 0x1000 | RF calibration |
| ota_0 | 0x30000 | 0x1af000 | Stock app "AF109" 2.0.38, built Sep 25 2025, ELF SHA256 509a1e2d623af84c... |
| ota_1 | 0x1e0000 | 0x1af000 | Blank (never updated) |
| — | 0x38f000 | 0x71000 | Unpartitioned, blank |

`partitions/` holds those regions cut out of the dump.

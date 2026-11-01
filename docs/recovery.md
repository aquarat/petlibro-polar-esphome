# Recovery and flashing

## Enter download mode
1. Unplug mains and the backup battery.
2. Hold GPIO9 (module pin 8, next to GND pin 9) to GND, plug in mains, release after ~1 s.
3. UART shows `boot:0x4 (DOWNLOAD(USB/UART0/1))` / `waiting for download`.

Restart without touching the device: esptool `--after watchdog-reset` (RTC watchdog) works on the C3.
After a 460800-baud command with `--after no-reset`, the stub stays at 460800; reattach with
`tools/read2_fastbaud.py`-style library code, not the CLI (which syncs at 115200).

## Back to the stock firmware
- If `ota_0` still holds the stock app: erase otadata copy 2 (0x2a000, 4 KB). The bootloader then
  uses copy 1 (seq 1 -> ota_0).
- Otherwise write the full backup at 0x0:
  `esptool --chip esp32c3 -p /dev/ttyUSB0 --before no-reset write-flash 0 firmware/stock/20261030T203938_polar_backup_1.bin`

## ESPHome
- Normal path: OTA from the ESPHome container on <esphome-host> (`esphome run /config/petlibro-polar.yaml`).
- The stock IDF 4.4.6 bootloader has no rollback support; safe mode (10 failed boots) is the net.

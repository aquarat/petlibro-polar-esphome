# Petlibro Polar PLAF109 – findings log

Repository: <repo>
- captures/   raw (.bin) and timestamped (.log) UART captures, esptool/espefuse output
- firmware/stock/  flash backups + sha256 (sensitive: Wi-Fi creds, per-device cloud keys)
- tools/  passive_log.py (receive-only), ro_esptool.sh (refuses writes/burns),
          security_check.sh, backup.sh

## 2026-10-30 setup
- Host: <workstation> (Fedora Asahi, aarch64). Adapter: FTDI FT232R, serial <redacted>, /dev/ttyUSB0.
- Port access granted with a device ACL (`setfacl -m u:$USER:rw`). Not persistent:
  re-run after replugging the adapter, or add the user to `dialout`.
- esptool 5.6.0 + pyserial in .venv.
- Passive logger opens 115200 8N1 with DTR/RTS deasserted, never transmits.
- Port silent with feeder unpowered (expected).

## 2026-10-30 20:20 first boot capture (captures/20261030T185619_uart_115200.*)
Power-on boot, normal SPI boot. Wiring: adapter RX <- board TX confirmed (clean text, 0 non-printable bytes).
Adapter TX -> board RX NOT yet verified (nothing has been transmitted).

- ROM: ESP-ROM esp32c3-api1-20210207, rst:0x1 POWERON, boot:0xc SPI_FAST_FLASH_BOOT
- Chip revision v0.4. Flash 4 MB, DIO, 80 MHz, "generic" flash chip.
- 2nd-stage bootloader: ESP-IDF v4.4.6-dirty, Info-level logging.
- Partition table (offset 0x8000):
  | # | label    | type/sub   | offset   | length   |
  |---|----------|------------|----------|----------|
  | 0 | nvs      | data/nvs   | 0x009000 | 0x020000 |
  | 1 | otadata  | data/ota   | 0x029000 | 0x002000 |
  | 2 | phy_init | data/phy   | 0x02b000 | 0x001000 |
  | 3 | ota_0    | app/ota_0  | 0x030000 | 0x1af000 |
  | 4 | ota_1    | app/ota_1  | 0x1e0000 | 0x1af000 |
  0x38f000..0x400000 (~452 KB) not in the table.
- Booted app from ota_0 (0x30000). Project "AF109", version 2.0.38,
  built Sep 25 2025 18:36:42, ELF SHA256 509a1e2d623af84c..., unicore, 160 MHz.
- App log goes silent at 468 ms after "system_info: SYSYTEM_INFO_init".
  No garbage afterwards, so most likely the app lowers log level (or repurposes UART0),
  not a baud change.

### Security reading from the boot log (indicative, NOT confirmed)
- No secure_boot / flash_encrypt / signature lines anywhere, although the bootloader
  logs at Info level. An IDF v4.4 bootloader with Secure Boot V2 prints
  "secure_boot_v2: Verifying with RSA-PSS..." for the app, and one with flash
  encryption prints "flash_encrypt: flash encryption is enabled ...".
- Partition table and app headers were read and logged in plaintext by the bootloader;
  that is true either way (decryption is transparent), so it proves nothing alone.
- Conclusion: secure boot and flash encryption are PROBABLY off.
  Must be confirmed with espefuse summary / get-security-info in download mode
  (SECURE_BOOT_EN, SPI_BOOT_CRYPT_CNT, DIS_DOWNLOAD_MODE, ENABLE_SECURITY_DOWNLOAD).

## 2026-10-30 20:27 second boot, user holding SW1 during power-up
- rst:0x1 POWERON again, so the power cycle was a real cold boot (battery not holding it up).
- boot:0xc (SPI_FAST_FLASH_BOOT) again: GPIO9 read HIGH. Download mode would show boot:0x4.
- Same app, same output, silent after 459 ms.
- Conclusion: SW1 does not pull GPIO9 low at reset (or not with enough strength).
  GPIO9 has to be reached another way (module pin, test pad, or trace).

## ESP32-C3-WROOM-02U pinout (Espressif datasheet v1.7, docs/datasheets/esp32-c3-wroom-02_datasheet_en.pdf)
Top view, U.FL/antenna end at the top. Left edge top-to-bottom: 1 3V3, 2 EN, 3 IO4, 4 IO5,
5 IO6, 6 IO7, 7 IO8, 8 IO9, 9 GND. Right edge bottom-to-top: 10 IO10, 11 RXD (GPIO20),
12 TXD (GPIO21), 13 IO18 (USB D-), 14 IO19 (USB D+), 15 IO3, 16 IO2, 17 IO1, 18 IO0.
Pin 19 = centre GND pad.
- GPIO9: pin 8, directly above GND pin 9. Weak internal pull-up. Download = GPIO9 low.
- GPIO8: pin 7, directly above GPIO9. Floating by default; must be HIGH for download mode
  (GPIO8=0 + GPIO9=0 is not a valid mode). It also gates ROM UART printing.
- Header sanity check: ISP RX/TX should beep through to pins 11/12.
- USB D-/D+ on pins 13/14: alternative route (native USB-Serial-JTAG) if a USB lead
  can be attached; still needs GPIO9 low to enter the ROM download mode.

## 2026-10-30 20:36 third boot, user attempting GPIO9 -> GND
- rst:0x1 POWERON, boot:0x8 (SPI_FAST_FLASH_BOOT). NOT download mode; stock app booted normally.
- Strap byte decode (ESP32-C3: bit3 = GPIO9, bit2 = GPIO8, bit0 = GPIO2):
  0xc = GPIO9 1, GPIO8 1 (normal, seen on boots 1 and 2)
  0x8 = GPIO9 1, GPIO8 0  <- this boot
  0x4 = GPIO9 0, GPIO8 1  <- what download mode needs
- Interpretation: GPIO8 (module pin 7) was pulled low instead of GPIO9 (pin 8). Likely off by one pin.
  No harm done. esptool not run because the chip was not in the ROM loader.

## 2026-10-30 ~20:37 fourth attempt
- 20:37:48 (epoch 1793392668.692): ROM banner "ESP-ROM:esp32c3-api1-20210207" only, then ~11 s of silence.
  The next ROM lines (Build/rst/boot) normally follow within milliseconds, so the chip
  most likely lost power or was reset right after the banner (supply dip, contact bounce,
  or the jumper touching something it shouldn't). Cause unknown; asked user.
- 20:38:00 (epoch 1793392680.108): full cold boot, rst:0x1 POWERON, boot:0xc. GPIO9 and GPIO8 both high,
  normal SPI boot, stock app running. Not download mode.

## 2026-10-30 20:38 download mode reached (GPIO9 -> GND at power-on), boot:0x4
### Security (captures/20261030T203920_security.*) -- CONFIRMED wide open
- SECURE_BOOT_EN False, SPI_BOOT_CRYPT_CNT 0b000 (flash encryption off), all KEY_PURPOSE USER, all key blocks zero.
- DIS_DOWNLOAD_MODE False, ENABLE_SECURITY_DOWNLOAD False, DIS_PAD_JTAG False, DIS_USB_JTAG False,
  USB-Serial-JTAG enabled, WR_DIS 0, RD_DIS 0. Nothing is locked or write-protected.
- Chip ESP32-C3 (QFN32) rev v0.4, 40 MHz xtal, MAC f8:5b:1b:xx:xx:xx. Flash JEDEC mfr 0x46 dev 0x4016, 4 MB.
- esptool's unsigned stub ran from RAM, and (below) an unsigned ESPHome app booted from flash:
  unsigned code runs both ways.

### Backups (firmware/stock/, chmod 400)
- 20261030T203938_polar_backup_1.bin and _2.bin, 4194304 bytes each, byte-identical,
  sha256 e5a3062e561f4fdf6539590ca77e941dc5c65cc89ce2a6dce18fded1d390d32a
- Contents check: partition table MD5 valid; bootloader and ota_0 image checksum + SHA-256 valid
  (ota_0 = AF109 2.0.38); otadata sector 0 seq 1 state VALID (CRC = zlib.crc32(seq, 0xffffffff));
  otadata sector 1 blank; ota_1 completely blank (never OTA-updated); 0x38f000-0x400000 blank.
- Extracted to firmware/stock/partitions/: bootloader.bin, ota_0.bin, ota_1.bin, nvs.bin.
- Still only on this machine. TODO: second copy elsewhere.
- Gotcha: after a read at 460800 with --after no-reset, the stub stays at 460800; the next esptool
  CLI call syncs at 115200 and fails. Reattach at 460800 via the library (tools/read2_fastbaud.py).

## 2026-10-30 20:49 ESPHome installed into stock ota_1 (user asked for an ESPHome reflashable image)
- Config: <esphome-host> <esphome-config>/petlibro-polar.yaml (copy in esphome/), API key
  petlibro_polar_api_key appended to secrets.yaml (backup secrets.yaml.bak.20261030-pre-petlibro).
  Drives NO GPIOs. Logger on UART0 (ISP header) at 115200.
- Built in the esphome container (ESPHome 2026.10.0, ESP-IDF 5.5.6), app 903920 bytes.
- tools/install_ota1.py: pre-checked target regions blank, wrote app @0x1e0000 and
  otadata sector 1 @0x2a000 (seq 2, state VALID -> ota_1), verified both, confirmed bootloader,
  partition table, otadata sector 0 and ota_0 head unchanged, then RTC-watchdog reset (no power cycle).
- Result: stock IDF v4.4.6 bootloader boots the IDF 5.5.6 ESPHome app fine.
  Wi-Fi '<ssid>', IP <feeder-ip>, mDNS petlibro-polar. API (6053, Noise) and OTA (3232) answer;
  `esphome logs` over API works from <esphome-host>. Boot log captures/20261030T204921_first_esphome_boot.bin.
- Caveat from ESPHome: "Bootloader too old for OTA rollback". A bad OTA will not auto-revert;
  safe_mode (10 failed boots -> 300 s OTA-only) still applies. If an image dies before that,
  recovery is serial + GPIO9 jumper.
- Next ESPHome OTA writes ota_0, overwriting the stock app on the chip (backups hold it).
- Stock firmware no longer runs: Peltier, fan, motor are not controlled. Food will not be chilled.

### Restore to stock (serial, download mode)
  erase otadata sector 1 (0x2a000, 4 KB) -> boots ota_0 again, if ota_0 is still intact;
  otherwise write the backup in full at 0x0.

## 2026-10-30 21:15 ESPHome build 2 (expander bring-up), first OTA over Wi-Fi
- OTA from the esphome container worked (974800 bytes) -> wrote ota_0, overwriting the stock app on the chip.
  Stock app now exists only in firmware/stock/. Bootloader boots ota_0 ("Loaded app from partition at offset 0x30000").
- I2C scan on GPIO7/GPIO8: one device at 0x58. ID reg 0x10 = 0x23 -> **AW9523B confirmed** (static analysis
  identified it; U6 is almost certainly this chip).
- Init mirrored from stock with both motors braked: outputs 0xC060 (P0_5,P0_6,P1_6,P1_7 = 1).
- AW9523B inputs: P0=0x63 P1=0xC0 -> P0[1:0]=0b11 -> **hardware revision 5.2.0** (homing by widest tray mark).
- NTC on GPIO1 reads 22.6 C (room). GPIO0 = mains. Battery ADC 0.00 V (battery disconnected).
- Fan observed NOT running with nothing driven (expander at idle). So the fan is not simply always powered.
- 21:23 attempt to switch P0_4, P0_7 and Peltier P1_4 on via web_server failed: requests lacked a
  Content-Length header and were rejected (device log "Content length is required for post").
  Nothing was switched; user then reported the fan not spinning, as expected. User stepped away; all outputs
  confirmed off. Fan wiring still untested. Next: P0_4 -> P0_7 -> P1_4 one at a time with the user watching.

## 2026-10-30 23:00 serial adapter removed; device is Wi-Fi-only
- User unplugged the FTDI adapter; on replug the laptop (Asahi, USB-C) showed no USB bus at all
  (xhci removed on unplug, not re-created). The adapter has since been removed for good.
- From now on the device is reachable only over Wi-Fi: OTA on 3232, API on 6053, web on 80, IP <feeder-ip>.
- Recovery from a build that dies before Wi-Fi: ESPHome safe_mode (10 failed boots -> 300 s OTA-only mode).
  Below that: reattach GND/TX/RX to the ISP header and use the GPIO9 jumper (docs/recovery.md).
- Consequence for the first v1 flash: no UART view of early-boot crashes; logs only via the API.

## 2026-10-30 23:02-23:07 fan wiring test (bring-up build 2, user watching the fan)
Device had rebooted ~3 min earlier (all outputs off, mains, food 22.5 C).
| Time | P0_4 | P0_7 | P1_4 Peltier | Fan (user) |
|---|---|---|---|---|
| 23:02:44 | on | off | off | not spinning |
| 23:03:12 | off | on | off | not spinning |
| 23:03:47 | on | on | on | **spinning** |
| 23:04:07 | off | off | on | **still spinning** |
| 23:06:57 | off | off | off (3 min watchdog fired) | - |
- **Verified: the fan is switched by the Peltier output P1_4 in hardware, independent of P0_4/P0_7.**
  A Peltier-on state always has the fan running (unless the fan itself fails; nothing senses it).
- P0_4/P0_7 do not power the fan. What they feed is still unknown (sensors/IR/motor driver).
- Food NTC unchanged (22.5-22.6 C) over 3 min of Peltier with an empty tray: inconclusive for cooling
  performance; the NTC is in the food compartment, not on the plate.

## 2026-10-30 23:08-23:17 v1 firmware stage 1 on hardware (OTA, user at the feeder)
- OTA of petlibro-polar-v1 (1069488 B) ok. AW9523B ID 0x23, outputs 0xC260 (motors braked + status LED),
  readback verify every 1 s with no mismatches; safe_mode "Boot seems successful" at 60 s. HW rev 5.2.0.
  Nothing moved at boot; cooling compiled out; all meals disabled.
- **Lid sensor polarity (GPIO19): high at BOTH end stops, low in between** -> `sensor_end_level: true` correct.
  Jogs of 0.3 s: ~10 jogs (~3 s motor time) end to end each way. Initial "open" position was short of the
  open end stop (sensor low); after jogging it was fully open.
- Full lid moves (manual profile): close 3.9 s (sensor left 0.6 s, end 3.2 s, stopped on >0.4 V current
  during run-on = end stop, by design); open 4.1 s (end 3.05 s + 1 s run-on, current only ~0.11 V at the
  open stop). Travel current ~0.03-0.065 V. User: no straining or grinding, looks good both ways.
- Feeder close (pet-checked): IR emitter on -> "Pet present" false (clear); 10 s clear wait, then close 3.9 s.
  With the emitter off "Pet present" reads true: break-beam receiver, only meaningful while GPIO2 PWM runs.
- Tray (lid closed): jog 0.5 s ok. **Home: mark widths 167, 157, 505 ms; mark interval ~3.26-3.29 s
  (~9.8 s per turn); homed to position 1 at the wide mark.** Matches the emulation (160/510 ms).
- Tray ADC2 current works with Wi-Fi on: 124-171 raw while turning (threshold 400), 0 invalid samples.
- Steps: 1->2->3->1, 3.30-3.41 s each, no faults.
- Not yet checked: whether each stop aligns a compartment with the lid opening (needs the user's eyes).
- Bug to fix: "Filled compartments" defaults to 12 while compartments = 3.
- 23:20-23:25 alignment check (lid opened at each stop, user looking): **positions 1, 2 and 3 all present
  their compartment perfectly centred** in the lid opening. Stopping on the gate edge (as stock) needs no
  offset for 3 compartments. Close lid (pet-checked) -> step -> open repeated cleanly at each position.
- 23:24 **anti-pinch part 1 (object blocking the beam before the close): pass.** Close request -> "waiting
  for pet to leave" for as long as the beam was blocked; beam cleared 23:24:34, close started 23:24:44
  (10 s clear), closed 23:24:48. captures/20261030T2327_antipinch_part1.log
- 23:28 **anti-pinch part 2 (object pushed in during the close): pass.** Close started +10.13 s; beam broken
  +11.16 s -> reversed in the same state update, open at +13.28 s ("Pet in the way while closing: reopened
  (1)"); waited 10 s clear, closed +27.29 s. User: worked as expected. captures/20261030T2328_antipinch_part2.log
- 23:29 bug found on hardware: after a reboot (lid state UNKNOWN, lid physically closed) "Close lid" drove into
  the closed end stop; the over-current before leaving the end level was taken as a pinch -> reopened, then the
  30 s retry closed it properly. Because the sensor is high at both ends, UNKNOWN + sensor high is ambiguous.
  Fix (polar_lid.cpp): a close stall before the sensor ever left the end level, starting from UNKNOWN or CLOSED,
  now finishes as "closed (end stop)"; from a known OPEN start a close stall still reopens. Compiled, not yet
  flashed (cooldown test running).
- Build with cooling_allowed true flashed 23:29; "Filled compartments" now follows the compartment count (3).
- 23:30 cooldown test started: setpoint 4.0 C, hysteresis 0.5, max continuous on 60 min, lid closed, empty tray.
  Log: captures/20261030T2330_cooldown_4C.csv (every 30 s, 60 min).
- 00:29 cooldown 1 finished: 23.4 -> 17.7 °C in 60 min of continuous Peltier; 60 min limit fired and latched
  cooling off as designed. Summary and graph: docs/cooling.md.
- 00:31 lid fix flashed. After the reboot (lid state unknown, physically closed) "Close lid" -> pushed 0.35 s
  into the closed stop -> "closed (end stop)", no reopen. **Fix verified on hardware.**

## 2026-10-31 00:35-04:50 cooldown 2 (4 h continuous, empty tray)
- Compartment air 17.3 -> plateau about 14.5 °C from about 03:00 (about 8.5 K below the 23 °C room); limit fired at 04:35.
  Details, graph and hypotheses: docs/cooling.md.

## 2026-10-31 23:36-23:49 sound tests (OTA of 7efef43, user listening)
- OTA 23:36 of the polar_sound build (1077616 B). S1-S10 pass; S11 skipped. Bell clip at 0x00 and 0x01 only,
  volume 0..15 (0 mute), 0xEF = volume 15 (stock's feed-start byte), 0xFE stop, 0xF2 loop.
  Details: docs/esphome-firmware.md "Sound test results".
- Found 2026-11-01 10:24: tray state `fault: home mark not found (widths not distinct)`, "Tray homed" false,
  last event "Feed finished: ok", Sound volume 0, lid open minutes 240. Cause not known (no HA history then).
- Cause found 2026-11-01: the interrupted S11 command had run (switch on, Feed now); the feed homed the tray from
  compartment 3 and hit a homing bug (wide mark seen twice). Reproduced from compartment 3, fixed in polar_tray.

## 2026-11-01 10:38 serve_and_return build (OTA)
- Homing fix verified (from compartment 3: 498, 162, 153, 495 ms -> homed on the 4th edge).
- No-show serve test, no-show 1 min, bell off: lid state unknown after boot -> closed (end stop), tray 1->2,
  open 3.9 s, IR clear throughout, closed at 1 min 0 s, tray 2->1, "Serve done (no show)". Pass.
- Settings for use: no-show 30, pet gone 2, max open 45, interval 120 from 08:00, alternate; bell volume 15,
  14 repeats every 2 min; cooling on, max-on cap 0 (no limit). Schedule left OFF until the pet-path test.

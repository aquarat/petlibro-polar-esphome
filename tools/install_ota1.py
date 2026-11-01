#!/usr/bin/env python3
"""WRITE: install an app into stock ota_1 + select it via otadata sector 1.
Touches only 0x1e0000.. (app) and 0x2a000..0x2afff (otadata copy 2).
Pre-checks both regions are blank, verifies after, watchdog-resets, captures boot log."""
import sys, time, datetime, serial
from esptool.cmds import detect_chip, write_flash, verify_flash
PORT, BAUD = "/dev/ttyUSB0", 460800
APP, OTA = "esphome/installed/petlibro-polar-app_esphome-2026.10.0.bin", "esphome/installed/otadata_sector1_select_ota1.bin"
esp = detect_chip(PORT, BAUD, "no-reset"); esp = esp.run_stub()
for addr, n in ((0x1e0000, 0x1000), (0x2a000, 0x1000)):
    blk = esp.read_flash(addr, n)
    assert blk == b"\xff" * n, f"region {addr:#x} not blank on chip - abort"
print("pre-check: target regions blank")
write_flash(esp, [(0x1e0000, APP), (0x2a000, OTA)])
verify_flash(esp, [(0x1e0000, APP), (0x2a000, OTA)])
# stock regions must be unchanged
import hashlib
bk = open("firmware/stock/20261030T203938_polar_backup_1.bin", "rb").read()
for name, a, n in (("bootloader+ptable", 0, 0x9000), ("otadata0", 0x29000, 0x1000), ("ota_0 head", 0x30000, 0x10000)):
    assert esp.read_flash(a, n) == bk[a:a+n], name
    print("unchanged:", name)
esp.watchdog_reset()
esp._port.close()
stamp = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
s = serial.Serial(); s.port, s.baudrate, s.timeout = PORT, 115200, 0.2; s.dtr = s.rts = False; s.open()
end = time.time() + 45; data = b""
while time.time() < end: data += s.read(4096)
open(f"captures/{stamp}_first_esphome_boot.bin", "wb").write(data)
print(f"captured {len(data)} bytes -> captures/{stamp}_first_esphome_boot.bin")

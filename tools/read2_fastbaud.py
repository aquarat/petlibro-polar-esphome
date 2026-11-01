#!/usr/bin/env python3
"""Read-only: reattach to an esptool stub left running at a non-default baud and read flash."""
import sys
from esptool.cmds import detect_chip, read_flash
port, baud, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
esp = detect_chip(port, baud, "no-reset")
print("stub already running:", esp.sync_stub_detected)
esp = esp.run_stub()
read_flash(esp, 0, 0x400000, out, no_progress=True)
print("done", out)

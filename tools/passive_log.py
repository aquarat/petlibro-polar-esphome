#!/usr/bin/env python3
"""Receive-only UART logger. Never writes to the port. DTR/RTS held deasserted.
Writes raw bytes (.bin) and timestamped text (.log) side by side."""
import sys, time, datetime, serial
port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
stamp = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
base = f"captures/{stamp}_uart_{baud}"
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, baud, 0.2
s.dtr = False; s.rts = False
s.open()
print(f"logging {port}@{baud} -> {base}.log/.bin", flush=True)
buf = b""
with open(base + ".bin", "ab") as fb, open(base + ".log", "a", buffering=1) as ft:
    ft.write(f"# start {datetime.datetime.now().isoformat()} {port} {baud} 8N1 receive-only\n")
    while True:
        d = s.read(4096)
        if not d:
            if buf:  # flush partial line after idle
                ft.write(f"{time.time():.3f} | {buf.decode('utf-8','backslashreplace')}\n"); buf = b""
            continue
        fb.write(d); fb.flush()
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            ft.write(f"{time.time():.3f} | {line.rstrip(b'\r').decode('utf-8','backslashreplace')}\n")

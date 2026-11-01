#!/usr/bin/env python3
"""Filter QEMU memory_region_ops_{read,write} trace (stdin) into a compact, time-ordered
peripheral trace (stdout).

- UART0 TX FIFO writes are reassembled into 'LOG' lines (the firmware's own log).
- Time column: ms timestamp of the most recent firmware log line (FreeRTOS tick based);
  events between two log lines happened between those two times.
- Keeps accesses to GPIO, IO_MUX, I2C0, LEDC, RMT, SARADC, SPI2, UART1, RTC_IO/SENS,
  GPIO_SD, APB_CTRL, SYSTEM clock/reset enables; drops flash/cache/intmatrix/timer noise.
- Collapses consecutive identical lines into one with 'xN'.
"""
import re, sys

BLOCKS = [  # (base, size, name)
    (0x60004000, 0x1000, 'GPIO'),
    (0x60009000, 0x1000, 'IO_MUX'),
    (0x60010000, 0x1000, 'UART1'),
    (0x60013000, 0x1000, 'I2C0'),
    (0x60016000, 0x1000, 'RMT'),
    (0x60019000, 0x1000, 'LEDC'),
    (0x60024000, 0x1000, 'SPI2'),
    (0x60040000, 0x1000, 'SARADC'),
    (0x60008400, 0x0400, 'RTC_IO'),
    (0x60008800, 0x0000, 'EFUSE'),
    (0x6001f000, 0x0000, 'TIMG0'),
    (0x600c0000, 0x0100, 'SYSTEM'),
    (0x60026000, 0x1000, 'APB_CTRL'),
    (0x6000e000, 0x0000, 'RTC_I2C'),
]
SYST_LO = {0x60023044: 0, 0x6002304c: 1}  # UNIT0_VALUE_LO, UNIT1_VALUE_LO (C3: 0x40/0x44, 0x48/0x4c)
SYST_LO = {0x60023044, 0x60023048, 0x6002304c, 0x60023040}
pat = re.compile(r"memory_region_ops_(read|write) cpu \d+ mr 0x[0-9a-f]+ addr (0x[0-9a-f]+) value (0x[0-9a-f]+) size (\d+) name '([^']*)'")

def blk(a):
    for b, s, n in BLOCKS:
        if b <= a < b + s:
            return n, a - b
    return None, None

t_ms = 0.0
line = bytearray()
last = None; cnt = 0
def emit(s):
    global last, cnt
    if s == last:
        cnt += 1; return
    flush()
    last = s; cnt = 1
def flush():
    global last, cnt
    if last is not None:
        sys.stdout.write(last + (' x%d' % cnt if cnt > 1 else '') + '\n')
    last = None; cnt = 0

for raw in sys.stdin:
    m = pat.search(raw)
    if not m:
        continue
    rw, a, v, sz, name = m.groups()
    a = int(a, 16); v = int(v, 16)
    if a == 0x60023044 or a == 0x6002304c or a == 0x60023040 or a == 0x60023048:
        pass
    if rw == 'read' and 0x60023040 <= a <= 0x6002304c and a in (0x60023044, 0x6002304c):
        # VALUE_LO: low 32 bits of 52-bit counter at 16 MHz
        continue
    if rw == 'write' and a == 0x60000000:
        if v == 0x0a:
            txt = re.sub(r'\x1b\[[0-9;]*m', '', line.decode('latin1')).strip()
            if txt:
                mt = re.match(r'[IWEDV] \((\d+)\)', txt)
                if mt:
                    t_ms = float(mt.group(1))
                emit('%10.0f LOG  %s' % (t_ms, txt))
            line.clear()
        elif v != 0x0d:
            line.append(v & 0xff)
        continue
    n, off = blk(a)
    if n is None:
        continue
    emit('%10.0f %s %-8s +0x%03x %s 0x%08x' % (t_ms, 'W' if rw == 'write' else 'R', n, off,
                                            '<-' if rw == 'write' else '->', v))
flush()

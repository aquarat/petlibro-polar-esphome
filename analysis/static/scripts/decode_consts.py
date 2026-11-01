#!/usr/bin/env python3
"""Decode the stock-app constants cited in analysis/static/README.md straight from ota_0.bin
(DROM segment: vaddr 0x3c0f0020 <-> file offset 0x20). Read-only.
Usage: decode_consts.py [path/to/ota_0.bin]"""
import struct, sys, os
p = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '../../../firmware/stock/partitions/ota_0.bin')
d = open(p, 'rb').read()
def rd(a, n): o = a - 0x3c0f0020 + 0x20; return d[o:o + n]
def f64(a): return struct.unpack('<d', rd(a, 8))[0]
def cstr(a): o = a - 0x3c0f0020 + 0x20; return d[o:d.index(b'\0', o)].decode()
print('NTC (Read_Ntc_Temp @0x4200d488):')
for a, what in [(0x3c0fa9f8, 'mV -> V divisor'), (0x3c0faa00, 'supply V'), (0x3c0faa08, 'pull-up ohm'),
                (0x3c0faa10, 'beta'), (0x3c0faa18, '1/T0 (1/298.15)'), (0x3c0faa20, 'K offset'),
                (0x3c0faa28, 'added offset degC'), (0x3c0faa30, 'rounding x100'), (0x3c0faa38, 'invalid if below')]:
    print('  %08x %-22s %r' % (a, what, f64(a)))
print('  %08x %-22s %r' % (0x3c0fa9f0, 'R25 (float)', struct.unpack('<f', rd(0x3c0fa9f0, 4))[0]))
print('Cooling (CTRL_THREAD_coolHanlde @0x4200e6f6):')
print('  %08x thaw threshold degC %r' % (0x3c0fae60, f64(0x3c0fae60)))
print('  %08x on/off threshold degC %r' % (0x3c0fae68, f64(0x3c0fae68)))
print('Battery table (@0x3c0fa6c0, used by 0x4200c81c): mV -> %')
print('  ', [struct.unpack_from('<HB', rd(0x3c0fa6c0 + 4 * i, 3)) for i in range(11)], 'above 4499 mV ->', rd(0x3c0fa6ea, 1)[0])
t = struct.unpack('<5I', rd(0x3c0fa6f4, 20))
print('LEDC timer cfg (@0x3c0fa6f4, AllDet_init @0x4200ce24): speed_mode=%d duty_res=%d bits timer=%d freq=%d Hz clk_cfg=%d' % t)
print('HW version strings:', cstr(0x3c0facc0), cstr(0x3c0facb0), cstr(0x3c0facb8))
print('log tag passed to esp_log_level_set in SYSYTEM_INFO_init:', repr(cstr(0x3c0f40d8)))
print('MQTT topic format:', cstr(0x3c0fcdac), 'model', cstr(0x3c0fcda4),
      'subtopics', [cstr(a) for a in (0x3c0fcd9c, 0x3c0fcdc4, 0x3c0fcdcc, 0x3c0fcdd0, 0x3c0fcddc, 0x3c10aeec, 0x3c0fcde4, 0x3c0fcdec, 0x3c0fcdf4)],
      'dirs', [cstr(0x3c0fcd94), cstr(0x3c0fcdc8)])

#!/usr/bin/env python3
"""Decode the compact trace from mmio_filter.py into ESP32-C3 peripheral semantics.

Usage: mmio_decode.py trace.txt [--all]   (default hides raw I2C0/SARADC/SYSTEM/APB register churn;
                                            I2C transactions are decoded at API level by gdb/hooks.py)
Caveat: QEMU's C3 machine has no IO_MUX, LEDC, I2C or SARADC model. Those addresses are a
catch-all 'iomem' region whose reads return 0, so driver read-modify-write sequences write only
the bits set in that call (cumulative register state is not visible). Values below are what the
firmware WROTE, not what hardware would hold.
"""
import re, sys

SIG = {6: 'U0TXD/U0RXD', 7: 'U0RTS/CTS', 9: 'U1TXD/U1RXD', 10: 'U1RTS/CTS',
       45: 'LEDC_LS_OUT0', 46: 'LEDC_LS_OUT1', 47: 'LEDC_LS_OUT2', 48: 'LEDC_LS_OUT3',
       49: 'LEDC_LS_OUT4', 50: 'LEDC_LS_OUT5', 53: 'I2CEXT0_SCL', 54: 'I2CEXT0_SDA',
       55: 'GPIO_SD0', 56: 'GPIO_SD1', 57: 'GPIO_SD2', 58: 'GPIO_SD3',
       63: 'FSPICLK', 64: 'FSPIQ', 65: 'FSPID', 128: 'GPIO(simple)'}

def pins(mask):
    return ','.join(str(i) for i in range(22) if mask >> i & 1)

def iomux(v):
    f = []
    f.append('MCU_SEL=%d' % ((v >> 12) & 7))
    if v >> 9 & 1: f.append('IE')
    if v >> 8 & 1: f.append('WPU')
    if v >> 7 & 1: f.append('WPD')
    f.append('DRV=%d' % ((v >> 10) & 3))
    if v >> 15 & 1: f.append('FILTER')
    if v & 0x1f: f.append('slp:%s%s%s%s%s' % ('OE ' if v & 1 else '', 'SLPSEL ' if v & 2 else '',
                                               'WPD ' if v & 4 else '', 'WPU ' if v & 8 else '', 'IE' if v & 16 else ''))
    return ' '.join(f)

def decode(blk, off, val, rw):
    if blk == 'GPIO':
        if rw != 'W' and off != 0x03c: return None
        if off == 0x004: return 'GPIO_OUT = %#x (high: %s)' % (val, pins(val))
        if off == 0x008: return 'GPIO_OUT set   pins [%s]' % pins(val)
        if off == 0x00c: return 'GPIO_OUT clear pins [%s]' % pins(val)
        if off == 0x020: return 'GPIO_ENABLE = %#x' % val
        if off == 0x024: return 'GPIO output-enable  pins [%s]' % pins(val)
        if off == 0x028: return 'GPIO output-disable pins [%s]' % pins(val)
        if off == 0x03c: return 'GPIO_IN read -> %#x (QEMU model value)' % val
        if off in (0x044, 0x048, 0x04c): return 'GPIO_STATUS%s %#x' % ({0x44: '', 0x48: '_W1TS', 0x4c: '_W1TC'}[off], val)
        if 0x074 <= off <= 0x0c8:
            n = (off - 0x74) // 4
            it = (val >> 7) & 7
            return 'GPIO_PIN%d: int_type=%d%s int_ena=%#x%s' % (n, it, ' open-drain' if val & 4 else '',
                                                              (val >> 13) & 0x1f, ' wakeup' if val >> 10 & 1 else '')
        if 0x154 <= off < 0x554:
            sig = (off - 0x154) // 4
            return 'IN_SEL  signal %d (%s) <- %s%s' % (sig, SIG.get(sig, '?'),
                   ('GPIO%d' % (val & 0x1f)) if val & 0x40 else 'direct/IO_MUX (sel=0)', ' inv' if val & 0x20 else '')
        if 0x554 <= off <= 0x5a8:
            n = (off - 0x554) // 4
            s = val & 0xff
            return 'OUT_SEL GPIO%d <- signal %d (%s)%s%s' % (n, s, SIG.get(s, '?'), ' inv' if val >> 8 & 1 else '',
                                                           ' oen_by_sel' if val >> 9 & 1 else '')
        return 'GPIO +%#x <- %#x' % (off, val)
    if blk == 'IO_MUX':
        if rw != 'W': return None
        if 0x04 <= off <= 0x58:
            return 'IO_MUX GPIO%d <- %#06x  %s' % ((off - 4) // 4, val, iomux(val))
        return 'IO_MUX +%#x <- %#x' % (off, val)
    if blk == 'LEDC':
        if rw != 'W': return None
        if off < 0xa0:
            ch, r = off // 0x14, off % 0x14
            if r == 0: return 'LEDC ch%d CONF0 <- %#x timer=%d sig_out_en=%d idle_lv=%d para_up=%d' % (ch, val, val & 3, val >> 2 & 1, val >> 3 & 1, val >> 4 & 1)
            if r == 4: return 'LEDC ch%d HPOINT <- %d' % (ch, val)
            if r == 8: return 'LEDC ch%d DUTY <- %d (raw %#x)' % (ch, val >> 4, val)
            if r == 0xc: return 'LEDC ch%d CONF1 <- %#x duty_start=%d' % (ch, val, val >> 31)
        if 0xa0 <= off < 0xc0 and (off - 0xa0) % 8 == 0:
            t = (off - 0xa0) // 8
            res = val & 0xf; div = (val >> 4) & 0x3ffff
            s = 'LEDC timer%d CONF <- %#x res=%d bits div=%d/256 rst=%d pause=%d tick_sel=%d para_up=%d' % (
                t, val, res, div, val >> 23 & 1, val >> 22 & 1, val >> 24 & 1, val >> 25 & 1)
            if div:
                s += '  => f = 80 MHz/(div/256)/2^res = %.1f Hz (if APB clk)' % (80e6 / (div / 256.0) / (1 << res))
            return s
        if off == 0xd0: return 'LEDC CONF <- %#x apb_clk_sel=%d clk_en=%d' % (val, val & 3, val >> 31)
        return 'LEDC +%#x <- %#x' % (off, val)
    if blk == 'SARADC':
        if off == 0x20 and rw == 'W' and (val >> 29 & 1):
            unit = 'ADC1' if val >> 31 & 1 else ('ADC2' if val >> 30 & 1 else '?')
            return 'SARADC onetime start %s ch%d atten=%d' % (unit, (val >> 25) & 0xf, (val >> 23) & 3)
        return None
    if blk == 'UART1':
        return 'UART1 %s +%#x %#x' % (rw, off, val)
    return None

def main():
    show_all = '--all' in sys.argv
    pat = re.compile(r'^\s*(\d+) (R|W) (\S+)\s+\+0x([0-9a-f]+) (?:<-|->) 0x([0-9a-f]+)(?: x(\d+))?')
    for line in open(sys.argv[1], errors='replace'):
        if ' LOG ' in line:
            print(line.rstrip()); continue
        m = pat.match(line)
        if not m: continue
        t, rw, blk, off, val, cnt = m.groups()
        off = int(off, 16); val = int(val, 16)
        d = decode(blk, off, val, rw)
        if d is None:
            if show_all: print(line.rstrip())
            continue
        print('%10s      %s%s' % (t, d, (' (x%s)' % cnt) if cnt else ''))

if __name__ == '__main__':
    main()

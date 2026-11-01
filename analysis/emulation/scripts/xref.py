#!/usr/bin/env python3
"""Find code references (lui/auipc + addi/load/store) to addresses in the AF109 app ELF.
Usage: xref.py ota_0.elf ADDR[-END] [...]      (hex)
       xref.py ota_0.elf --dis START END       (disassemble range)
       xref.py ota_0.elf --calls TARGET [...]  (jal / auipc+jalr call sites)
Linear sweep with capstone, RV32IMC. Tracks per-register constants built by lui/auipc
and reports the instruction that completes an address in range. Read-only."""
import sys, struct
from capstone import Cs, CS_ARCH_RISCV, CS_MODE_RISCV32, CS_MODE_RISCVC

def sections(path):
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shnum, = struct.unpack_from('<H', d, 0x30)
    out = []
    for i in range(shnum):
        nm, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', d, shoff + i*40)
        if typ == 1 and (flags & 4):  # PROGBITS, EXEC
            out.append((addr, d[off:off+size]))
    return out

def md():
    m = Cs(CS_ARCH_RISCV, CS_MODE_RISCV32 | CS_MODE_RISCVC)
    m.skipdata = True
    return m

def func_start(code, base, addr):
    return None

def main():
    elf = sys.argv[1]
    secs = sections(elf)
    m = md()
    if sys.argv[2] == '--dis':
        s, e = int(sys.argv[3], 16), int(sys.argv[4], 16)
        for a, c in secs:
            if a <= s < a + len(c):
                for i in m.disasm(c[s-a:e-a], s):
                    print('%08x: %-8s %s' % (i.address, i.mnemonic, i.op_str))
        return
    if sys.argv[2] == '--calls':
        tg = set(int(x, 16) for x in sys.argv[3:])
        for a, c in secs:
            prev = None
            for i in m.disasm(c, a):
                ops = i.op_str.replace(' ', '').split(',')
                if i.mnemonic in ('jal', 'c.jal', 'j', 'c.j'):
                    t = i.address + int(ops[-1], 0)
                    if t in tg: print('%08x: %s -> %08x' % (i.address, i.mnemonic, t))
                if i.mnemonic == 'jalr' and prev is not None and prev.mnemonic == 'auipc':
                    po = prev.op_str.replace(' ', '').split(',')
                    t = (prev.address + (int(po[1], 0) << 12) + int(ops[-1], 0)) & 0xffffffff
                    if t in tg: print('%08x: auipc+jalr -> %08x' % (i.address, t))
                prev = i
        return
    ranges = []
    for t in sys.argv[2:]:
        if '-' in t:
            a, b = t.split('-'); ranges.append((int(a, 16), int(b, 16)))
        else:
            v = int(t, 16); ranges.append((v, v + 1))
    for a, c in secs:
        regs = {}
        for i in m.disasm(c, a):
            mn, ops = i.mnemonic, i.op_str.replace(' ', '').split(',')
            if mn in ('lui', 'c.lui') and len(ops) == 2:
                regs[ops[0]] = (int(ops[1], 0) << 12) & 0xffffffff; continue
            if mn == 'auipc':
                regs[ops[0]] = (i.address + (int(ops[1], 0) << 12)) & 0xffffffff; continue
            val = None
            if mn in ('addi', 'c.addi') and len(ops) == 3 and ops[1] in regs:
                val = (regs[ops[1]] + int(ops[2], 0)) & 0xffffffff
            elif len(ops) == 2 and '(' in ops[1]:
                off, r = ops[1].rstrip(')').split('(')
                if r in regs:
                    val = (regs[r] + int(off or '0', 0)) & 0xffffffff
            if val is not None and any(lo <= val < hi for lo, hi in ranges):
                print('%08x: %s %s -> %08x' % (i.address, mn, i.op_str, val))
            if ops and ops[0] in regs and not mn.startswith('s') and not mn.startswith('c.s') and not mn.startswith('b'):
                if not (mn in ('addi','c.addi') and ops[0] == ops[1]):
                    regs.pop(ops[0], None)
                elif val is not None:
                    regs[ops[0]] = val
            if mn in ('ret', 'c.jr', 'jr', 'c.j', 'j', 'jal', 'c.jal', 'jalr', 'c.jalr'):
                regs = {}

if __name__ == '__main__':
    main()

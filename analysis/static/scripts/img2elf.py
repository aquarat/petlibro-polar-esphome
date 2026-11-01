#!/usr/bin/env python3
"""Convert an ESP32-C3 app image (ESP-IDF v4.4) into a minimal ELF32 RISC-V file
with one PT_LOAD program header + section per segment at its load address.
Usage: img2elf.py ota_0.bin out.elf
Read-only on the input."""
import struct, sys

def parse(path):
    d = open(path, 'rb').read()
    assert d[0] == 0xE9
    nseg = d[1]
    entry = struct.unpack_from('<I', d, 4)[0]
    off = 24  # 8 header + 16 extended header
    segs = []
    for _ in range(nseg):
        addr, ln = struct.unpack_from('<II', d, off)
        segs.append((addr, d[off + 8: off + 8 + ln]))
        off += 8 + ln
    return entry, segs

def name_for(a):
    if 0x3c000000 <= a < 0x3c800000: return '.flash.rodata'
    if 0x42000000 <= a < 0x42800000: return '.flash.text'
    if 0x40380000 <= a < 0x403e0000: return '.iram'
    if 0x3fc80000 <= a < 0x3fce0000: return '.dram'
    return '.seg'

def write_elf(entry, segs, out):
    names = [name_for(a) + '_%08x' % a for a, _ in segs]
    shstr = b'\0' + b''.join(n.encode() + b'\0' for n in names) + b'.shstrtab\0'
    ehsz, phsz, shsz = 52, 32, 40
    n = len(segs)
    data_off = ehsz + phsz * n
    offs = []; blob = b''
    for a, s in segs:
        offs.append(data_off + len(blob)); blob += s
        blob += b'\0' * (-len(blob) % 4)
    shstr_off = data_off + len(blob)
    sh_off = shstr_off + len(shstr); sh_off += -sh_off % 4
    hdr = b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8
    hdr += struct.pack('<HHIIIIIHHHHHH', 2, 243, 1, entry, ehsz, sh_off, 0x1,  # EF_RISCV_RVC
                       ehsz, phsz, n, shsz, n + 2, n + 1)
    ph = b''
    for (a, s), o in zip(segs, offs):
        x = name_for(a) in ('.flash.text', '.iram')
        ph += struct.pack('<IIIIIIII', 1, o, a, a, len(s), len(s), 5 if x else 6, 4)
    sh = b'\0' * shsz
    ni = 1
    for (a, s), o, nm in zip(segs, offs, names):
        x = name_for(a) in ('.flash.text', '.iram')
        sh += struct.pack('<IIIIIIIIII', ni, 1, 2 | (4 if x else 1), a, o, len(s), 0, 0, 4, 0)
        ni += len(nm) + 1
    sh += struct.pack('<IIIIIIIIII', ni, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    out_b = hdr + ph + blob + shstr
    out_b += b'\0' * (sh_off - len(out_b)) + sh
    open(out, 'wb').write(out_b)

if __name__ == '__main__':
    e, s = parse(sys.argv[1])
    for a, d in s: print('%08x %06x %s' % (a, len(d), name_for(a)))
    write_elf(e, s, sys.argv[2])

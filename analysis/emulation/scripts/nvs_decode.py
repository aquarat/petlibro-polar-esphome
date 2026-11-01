#!/usr/bin/env python3
"""Decode an ESP-IDF NVS partition (format v2, IDF v4.x) into a key list. Read-only.

Usage: nvs_decode.py nvs.bin [--values] [--markdown]
  default : namespace, key, type, length, page/entry, state. NO values printed.
  --values: also print numeric values, and string/blob values ONLY for keys on the
            ALLOW list below (non-secret, verified by hand). Everything else stays
            '<redacted>'. Never use this output in committed docs without review.
"""
import struct, sys, zlib

TYPES = {0x01: 'u8', 0x11: 'i8', 0x02: 'u16', 0x12: 'i16', 0x04: 'u32', 0x14: 'i32',
         0x08: 'u64', 0x18: 'i64', 0x21: 'str', 0x41: 'blob(v1)', 0x42: 'blob_data', 0x48: 'blob_index'}
PAGE_STATES = {0xffffffff: 'EMPTY', 0xfffffffe: 'ACTIVE', 0xfffffffc: 'FULL',
               0xfffffff8: 'FREEING', 0xfffffff0: 'CORRUPT'}
ALLOW = set()   # (namespace, key) pairs whose str/blob value may be shown with --values

def crc32(b):
    return zlib.crc32(b, 0xffffffff)  # == ESP-IDF esp_rom_crc32_le(0xffffffff, ...)

def parse(path):
    d = open(path, 'rb').read()
    items = []
    for p in range(len(d) // 4096):
        pg = d[p * 4096:(p + 1) * 4096]
        state, seq, ver = struct.unpack_from('<IIB', pg, 0)
        if state == 0xffffffff:
            continue
        bitmap = pg[32:64]
        i = 0
        while i < 126:
            st = (bitmap[i // 4] >> ((i % 4) * 2)) & 3
            e = pg[64 + i * 32: 64 + (i + 1) * 32]
            if st != 2:  # 2 = written, 3 = empty, 0 = erased
                i += 1; continue
            ns, typ, span, chunk = e[0], e[1], e[2], e[3]
            ecrc, = struct.unpack_from('<I', e, 4)
            key = e[8:24].split(b'\0')[0].decode('latin1')
            data = e[24:32]
            ok = crc32(e[0:4] + e[8:32]) == ecrc
            it = dict(page=p, seq=seq, idx=i, ns=ns, type=TYPES.get(typ, hex(typ)), span=span,
                      chunk=chunk, key=key, crc_ok=ok, raw=data, payload=b'')
            if typ in (0x21, 0x41, 0x42):
                size, = struct.unpack_from('<H', data, 0)
                payload = pg[64 + (i + 1) * 32: 64 + (i + span) * 32][:size]
                it['size'] = size; it['payload'] = payload
            elif typ == 0x48:
                it['size'], it['chunks'], it['chunk_start'] = struct.unpack_from('<IBB', data, 0)
            else:
                n = {'u8': 1, 'i8': 1, 'u16': 2, 'i16': 2, 'u32': 4, 'i32': 4, 'u64': 8, 'i64': 8}[it['type']]
                fmt = {'u8': '<B', 'i8': '<b', 'u16': '<H', 'i16': '<h', 'u32': '<I', 'i32': '<i', 'u64': '<Q', 'i64': '<q'}[it['type']]
                it['size'] = n; it['value'] = struct.unpack_from(fmt, data, 0)[0]
            items.append(it)
            i += max(span, 1)
    nsmap = {0: '(ns-table)'}
    for it in items:
        if it['ns'] == 0 and 'value' in it:
            nsmap[it['value']] = it['key']
    for it in items:
        it['nsname'] = nsmap.get(it['ns'], 'ns%d' % it['ns'])
    return items

def main():
    items = parse(sys.argv[1])
    show = '--values' in sys.argv
    md = '--markdown' in sys.argv
    # merge blob_data chunks under their blob_index
    rows = []
    for it in items:
        if it['ns'] == 0:
            continue
        if it['type'] == 'blob_data':
            continue
        if it['type'] == 'blob_index':
            total = sum(len(x['payload']) for x in items if x['ns'] == it['ns'] and x['key'] == it['key'] and x['type'] == 'blob_data')
            it['payload_len'] = total
        rows.append(it)
    if md:
        print('| namespace | key | type | length (bytes) | value |')
        print('|---|---|---|---|---|')
    for it in rows:
        ln = it.get('size')
        if show and 'value' in it:
            val = str(it['value'])
        elif show and (it['nsname'], it['key']) in ALLOW:
            val = repr(it['payload'])
        else:
            val = '<redacted>' if it['type'] not in ('u8','i8','u16','i16','u32','i32','u64','i64') else '<not shown>'
        flags = '' if it['crc_ok'] else ' CRC-BAD'
        if md:
            print('| %s | %s | %s | %s | %s |' % (it['nsname'], it['key'], it['type'], ln, val))
        else:
            print('%-16s %-16s %-11s len=%-6s p%02d/e%03d%s  %s' % (it['nsname'], it['key'], it['type'], ln, it['page'], it['idx'], flags, val))

if __name__ == '__main__':
    main()

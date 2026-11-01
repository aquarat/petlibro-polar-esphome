#!/usr/bin/env python3
"""List namespaces/keys/types/sizes in an ESP-IDF NVS partition (v2 format).
Prints values only for small integer types; blobs/strings shown as length (plus hex with --values,
which may reveal credentials: keep such output local)."""
import struct, sys
TYPES = {0x01:'u8',0x11:'i8',0x02:'u16',0x12:'i16',0x04:'u32',0x14:'i32',0x08:'u64',0x18:'i64',
         0x21:'str',0x41:'blob',0x42:'blob_data',0x48:'blob_idx'}
def main(path, show=False):
    d = open(path,'rb').read()
    ns = {0:'(ns-table)'}
    rows = []
    for p in range(0, len(d), 4096):
        page = d[p:p+4096]
        state = struct.unpack_from('<I', page, 0)[0]
        if state == 0xffffffff: continue
        bitmap = page[32:64]
        i = 0
        while i < 126:
            st = (bitmap[i//4] >> ((i%4)*2)) & 3
            e = page[64+32*i:64+32*(i+1)]
            if st != 2:  # 2 = written, 3 = empty, 0 = erased
                i += 1; continue
            nsi, typ, span, chunk = e[0], e[1], e[2], e[3]
            key = e[8:24].split(b'\0')[0].decode('latin1')
            data = e[24:32]
            t = TYPES.get(typ, hex(typ))
            if nsi == 0 and t == 'u8':
                ns[data[0]] = key
            val = ''
            if t in ('u8','i8','u16','i16','u32','i32','u64','i64'):
                fmt = {'u8':'<B','i8':'<b','u16':'<H','i16':'<h','u32':'<I','i32':'<i','u64':'<Q','i64':'<q'}[t]
                val = str(struct.unpack_from(fmt, data)[0])
            elif t in ('str','blob','blob_data'):
                size = struct.unpack_from('<H', data)[0]
                payload = page[64+32*(i+1): 64+32*(i+span)][:size]
                val = 'len=%d' % size + (' ' + payload.hex() if show else '')
            elif t == 'blob_idx':
                size, nch, start = struct.unpack_from('<IBB', data)
                val = 'total=%d chunks=%d' % (size, nch)
            rows.append((ns.get(nsi, nsi), key, t, chunk, val))
            i += max(span, 1)
    for r in rows: print('%-14s %-16s %-9s chunk=%-3s %s' % r)
if __name__ == '__main__':
    main(sys.argv[1], '--values' in sys.argv)

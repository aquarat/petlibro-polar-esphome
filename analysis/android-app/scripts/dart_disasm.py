#!/usr/bin/env python3
"""Disassemble a Dart AOT function in libapp.so that blutter did not emit into a .dart file
(e.g. mixin-deduplicated methods), resolving object-pool loads (PP = x27) against blutter's
pp.txt and call targets against blutter's ida_script/addNames.py.

Usage: dart_disasm.py BLUTTER_OUT LIBAPP_SO ADDR|NAME_SUBSTRING
"""
import re, subprocess, sys

out_dir, lib, target = sys.argv[1], sys.argv[2], sys.argv[3]
pp = {}
for l in open(f"{out_dir}/pp.txt", errors="replace"):
    m = re.match(r"\[pp\+0x([0-9a-f]+)\] (.*)", l)
    if m:
        pp[int(m.group(1), 16)] = m.group(2)[:140]
funcs, names = {}, {}
for l in open(f"{out_dir}/ida_script/addNames.py", errors="replace"):
    m = re.match(r"ida_funcs.add_func\(0x([0-9a-f]+), 0x([0-9a-f]+)\)", l)
    if m:
        funcs[int(m.group(1), 16)] = int(m.group(2), 16)
    m = re.match(r'idaapi.set_name\(0x([0-9a-f]+), "(.*)"\)', l)
    if m:
        names[int(m.group(1), 16)] = m.group(2)

if re.fullmatch(r"(0x)?[0-9a-fA-F]+", target):
    starts = [int(target, 16)]
else:
    starts = [a for a, n in names.items() if target in n]
for a in starts:
    end = funcs.get(a, a + 0x200)
    print(f"=== {a:#x} {names.get(a, '?')}")
    dis = subprocess.run(["objdump", "-D", "--no-show-raw-insn", f"--start-address={a}",
                          f"--stop-address={end}", lib], capture_output=True, text=True).stdout
    hi = {}
    for l in dis.splitlines():
        if not re.match(r"\s+[0-9a-f]+:\s", l):
            continue
        note = ""
        m = re.search(r"add\s+(x\d+), x27, #0x([0-9a-f]+), lsl #12", l)
        if m:
            hi[m.group(1)] = int(m.group(2), 16) << 12
        m = re.search(r"ldr\s+\w+, \[(x\d+), #(\d+)\]", l)
        if m and m.group(1) in hi:
            off = hi[m.group(1)] + int(m.group(2))
            note = f"   ; pp+{off:#x} {pp.get(off, '?')}"
        m = re.search(r"ldr\s+\w+, \[x27, #(\d+)\]", l)
        if m:
            off = int(m.group(1)); note = f"   ; pp+{off:#x} {pp.get(off, '?')}"
        m = re.search(r"\bbl\s+([0-9a-f]+)", l)
        if m:
            note = "   ; " + names.get(int(m.group(1), 16), "?")
        print(l.split("<")[0].rstrip() + note)

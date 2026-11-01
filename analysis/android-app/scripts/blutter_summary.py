#!/usr/bin/env python3
"""Summarise a blutter .dart asm file: classes, functions, string literals and small int
constants, in order. Usage: blutter_summary.py FILE [FILE...]"""
import re, sys
for path in sys.argv[1:]:
    print(f"=== {path}")
    last = None
    for line in open(path, errors="replace"):
        m = re.match(r"^(class .*|\s{2}\S.*\(.*\) \{|\s{2}(static |get |set |\[closure\] ).*)$", line.rstrip())
        if m and not line.strip().startswith("//"):
            print(line.rstrip()); continue
        for s in re.findall(r'^\s*// 0x[0-9a-f]+: r\d+ = (".*")$', line):
            if s != last: print("    str", s); last = s
        for s in re.findall(r'^\s*// 0x[0-9a-f]+: r\d+ = (-?\d+)$', line):
            print("    int", s)

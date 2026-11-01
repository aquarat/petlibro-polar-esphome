#!/usr/bin/env python3
"""Condense a gdb/hooks.py log into a time-ordered pin trace (stdout).
Usage: pin_trace.py work/<name>.hooks.log
Drops periodic app-call noise and gdb bookkeeping, collapses GPIO6 one-wire bit-banging into
the AUDIO byte lines, and collapses the P1_1 LED blink into a single note per run of toggles."""
import re, sys
DROP = re.compile(r'STATS|ISR handler returned|CALL (Operate_Door_Protect|FEED_PLAN_plateHandle|BSP_setExistState|CTRL_THREAD_coolHanlde)|'
                  r'gpio_set_level GPIO6 |gpio_set_intr_type GPIO10 type=[03]|gpio_isr_handler_add GPIO10|INJECT ISR|hooks armed')
blink = 0
for line in open(sys.argv[1], errors='replace'):
    line = line.rstrip()
    if DROP.search(line):
        continue
    if 'P1_1) ->' in line:
        blink += 1
        if blink == 1:
            print(line + '   [P1_1 toggles ~every 1.2 s from here: LED blink; further toggles omitted]')
        continue
    print(line)

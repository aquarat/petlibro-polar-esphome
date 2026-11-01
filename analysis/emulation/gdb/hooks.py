# gdb Python driver for the AF109 2.0.38 app under Espressif QEMU (esp32c3).
# Load with:  gdb -batch -x gdb/connect.gdb -x gdb/hooks.py
# Env: HOOK_LOG=path (default work/hooks.log), SCENARIO=idle|inputs (default inputs), RUN_MS=limit
#
# What it does (all via breakpoints at function entry/exit; QEMU gdbstub breakpoints do not patch
# memory, so they work in flash-mapped code):
#  - logs ESP-IDF driver calls: gpio_config/set_level/get_level/set_intr_type/isr_handler_add,
#    ledc_timer_config/channel_config/set_duty/update_duty, i2c_param_config, adc configs
#  - EMULATES the AW9523B I/O expander (I2C addr 0x58) by short-circuiting
#    i2c_master_write_to_device / i2c_master_write_read_device (QEMU has no I2C model; without this
#    every transfer times out after 1000 ticks). ID reg 0x10 returns 0x23. Input regs from SIM.
#  - INJECTS ADC raw values at the end of adc_hal_convert (needs the 'adcnowait' image patch)
#  - OVERRIDES gpio_get_level() with simulated input levels and, on a simulated edge, runs the
#    firmware's registered GPIO ISR handler by redirecting the CPU into it from a task context.
# Addresses: AF109 2.0.38 ota_0 (ELF SHA256 509a1e2d...). Names from analysis/static (Ghidra)
# and this directory's own disassembly (scripts/xref.py).
import gdb, os, struct, time

LOG = open(os.environ.get('HOOK_LOG', 'work/hooks.log'), 'w', buffering=1)
SCEN = os.environ.get('SCENARIO', 'inputs')
RUN_MS = int(os.environ.get('RUN_MS', '0'))
TICK = 0x3fca0f38          # FreeRTOS xTickCount (xTaskGetTickCount @0x4038d5c8), 1 tick = 1 ms
DEV_CTRL = 0x3fc9a2a0      # FUN_4200f088() -> dev ctrl struct: +3 plate-move flag, +4 set_plate_pos, +5 curr_plate_pos

inf = gdb.selected_inferior()
def rd32(a): return struct.unpack('<I', bytes(inf.read_memory(a, 4)))[0]
def rd8(a):  return bytes(inf.read_memory(a, 1))[0]
def wr8(a, v): inf.write_memory(a, bytes([v & 0xff]))
def wr32(a, v): inf.write_memory(a, struct.pack('<I', v & 0xffffffff))
def rdbuf(a, n): return bytes(inf.read_memory(a, n))
def reg(r): return int(gdb.parse_and_eval('$' + r)) & 0xffffffff
def setreg(r, v): gdb.execute('set $%s = %d' % (r, v & 0xffffffff))
def now(): 
    try: return rd32(TICK)
    except Exception: return -1
def log(s): LOG.write('%8d  %s\n' % (now(), s))
def ret(val=None):
    if val is not None: setreg('a0', val)
    setreg('pc', reg('ra'))

# ---------------- simulated world ----------------
# GPIO input levels returned by gpio_get_level(). None = let QEMU's GPIO_IN (0) through.
SIM_GPIO = {0: 0, 10: 0, 18: 1, 19: 0, 20: 0}
if os.environ.get('SIM_GPIO'):   # e.g. SIM_GPIO="0=0,10=1,18=1,19=1,20=0"
    SIM_GPIO = {int(k): int(v) for k, v in (x.split('=') for x in os.environ['SIM_GPIO'].split(','))}
# ADC raw (12-bit) per (unit, channel). unit 0 = ADC1, 1 = ADC2.
SIM_ADC = {(0, 1): 1900,   # GPIO1  NTC (Read_Ntc_Temp)
           (0, 3): 50,     # GPIO3  door/lid motor current (Read_DoorMotorAdc)
           (0, 4): 2000,   # GPIO4  supply/battery (x2 after cal)
           (1, 0): 50}     # GPIO5  plate motor current (Read_PlateMotorAdc, adc2)
# AW9523B registers
# P0_0/P0_1 = hardware-version straps. 0 -> "5.0.0", 2 -> "5.1.0", 1|3 -> "5.2.0". The real device's NVS
# log ring holds 'Microtime=' records, which only the 5.2.0 code path writes -> default 0x01.
AW = {0x00: int(os.environ.get('AW_P0', '1'), 0), 0x01: 0x00, 0x10: 0x23}
AW_OUT = [None, None]
ISR = {}                    # gpio -> (handler, arg) from gpio_isr_handler_add
PENDING_ISR = []            # gpios whose ISR should be run at the next safe point
EVENTS = []                 # (t_ms, fn, label)
T0 = None                   # tick when Control_ThreadHandle started (scenario anchor)
LAST_GET = {}

AW_NAMES = {0: 'P0_0 hwver0(in)', 1: 'P0_1 hwver1(in)', 4: 'P0_4', 5: 'P0_5 plate-motor A', 6: 'P0_6 plate-motor B',
            7: 'P0_7', 8: 'P1_0', 9: 'P1_1', 12: 'P1_4 PTC/Peltier', 14: 'P1_6 door-motor B', 15: 'P1_7 door-motor A'}

def aw_out_changed(port, val):
    old = AW_OUT[port]
    AW_OUT[port] = val
    if old is None:
        log('AW9523B OUT P%d = %#04x (initial)' % (port, val)); return
    ch = old ^ val
    for b in range(8):
        if ch >> b & 1:
            n = port * 8 + b
            log('AW9523B pin %d (%s) -> %d' % (n, AW_NAMES.get(n, 'P%d_%d' % (port, b)), val >> b & 1))
    plant_motors()

def awpin(n):
    v = AW_OUT[n // 8] or 0
    return v >> (n % 8) & 1

# ---- plant model (INFERRED from the NVS 'io:10 / Microtime' ring and the firmware logic) ----
# Tray: while plate motor pins P0_5 != P0_6 the tray turns. GPIO10 (gate) rests at 0 and goes 1
# while a slot passes: SLOT_MS[i] ms per slot, 3 slots per turn, one wide 'home' slot, GAP_MS apart.
# Lid: door motor P1_7/P1_6. DoorMotor_Reset drives P1_7=1,P1_6=0 to CLOSE and waits for GPIO19=1.
SLOT_MS = [160, 160, 510]
GAP_MS = 3100
PL = dict(run=False, gate=0, slot=0, next=None, remain=GAP_MS)
DOOR = dict(dir=0, t=None)
def plant_motors():
    t = now()
    run = awpin(5) != awpin(6)
    if run and not PL['run']:
        PL['run'] = True; PL['next'] = t + PL['remain']
        log('PLANT tray motor ON (P0_5=%d P0_6=%d), next gate edge in %d ms' % (awpin(5), awpin(6), PL['remain']))
    elif not run and PL['run']:
        PL['run'] = False; PL['remain'] = max(1, PL['next'] - t)
        log('PLANT tray motor OFF (P0_5=%d P0_6=%d)' % (awpin(5), awpin(6)))
    d = 0
    if awpin(15) == 1 and awpin(14) == 0: d = 1      # closing (as DoorMotor_Reset drives it)
    elif awpin(15) == 0 and awpin(14) == 1: d = -1   # opening
    if d != DOOR['dir']:
        DOOR['dir'] = d; DOOR['t'] = t
        log('PLANT lid motor %s (P1_7=%d P1_6=%d)' % ({1: 'CLOSING', -1: 'OPENING', 0: 'STOP'}[d], awpin(15), awpin(14)))

def plant_tick():
    if SCEN == 'idle': return
    t = now()
    if PL['run'] and t >= PL['next']:
        if PL['gate'] == 0:
            PL['gate'] = 1; PL['next'] = t + SLOT_MS[PL['slot']]
            log('PLANT gate GPIO10 -> 1 (slot %d%s)' % (PL['slot'], ' HOME/wide' if PL['slot'] == 2 else ''))
        else:
            PL['gate'] = 0; PL['slot'] = (PL['slot'] + 1) % 3; PL['next'] = t + GAP_MS
            log('PLANT gate GPIO10 -> 0')
        set_input(10, PL['gate'])
    if DOOR['dir'] == 1 and SIM_GPIO.get(19) != 1 and t - DOOR['t'] > 1500:
        log('PLANT lid closed -> GPIO19 = 1'); set_input(19, 1)
    if DOOR['dir'] == -1 and SIM_GPIO.get(19) != 0 and t - DOOR['t'] > 400:
        log('PLANT lid left closed position -> GPIO19 = 0'); set_input(19, 0)

def set_input(pin, lvl):
    SIM_GPIO[pin] = lvl
    if pin in ISR:
        PENDING_ISR.append(pin)

# ---------------- hook handlers ----------------
H = {}
def hook(addr):
    def deco(f):
        H[addr] = f; return f
    return deco

@hook(0x42045776)   # gpio_config(const gpio_config_t *)
def h_gpio_config():
    p = reg('a0'); lo, hi, mode, pu, pd, it = struct.unpack('<6I', rdbuf(p, 24))
    m = lo | hi << 32
    log('gpio_config pins=[%s] mode=%d pullup=%d pulldown=%d intr=%d  (ra=%#x)' % (
        ','.join(str(i) for i in range(22) if m >> i & 1), mode, pu, pd, it, reg('ra')))

@hook(0x420454c8)   # gpio_set_level(pin, level)
def h_set_level():
    pin, lvl = reg('a0'), reg('a1')
    log('gpio_set_level GPIO%d = %d  (ra=%#x)' % (pin, lvl, reg('ra')))

@hook(0x4200d7e8)   # app: one-wire audio byte on GPIO6 (LSB first, 1:3 / 3:1 ms pulses)
def h_audio():
    log('AUDIO cmd byte %#04x on GPIO6 (ra=%#x)' % (reg('a0') & 0xff, reg('ra')))

@hook(0x42045570)   # gpio_get_level(pin) -> override
def h_get_level():
    pin = reg('a0')
    if pin in SIM_GPIO and SIM_GPIO[pin] is not None:
        v = SIM_GPIO[pin]
        if LAST_GET.get(pin) != v:
            log('gpio_get_level GPIO%d -> %d (simulated) (ra=%#x)' % (pin, v, reg('ra')))
            LAST_GET[pin] = v
        ret(v)

@hook(0x420452d2)
def h_intr_type():
    log('gpio_set_intr_type GPIO%d type=%d' % (reg('a0'), reg('a1')))

@hook(0x4204595c)   # gpio_isr_handler_add(pin, fn, arg)
def h_isr_add():
    ISR[reg('a0')] = (reg('a1'), reg('a2'))
    log('gpio_isr_handler_add GPIO%d handler=%#x arg=%#x' % (reg('a0'), reg('a1'), reg('a2')))

@hook(0x4204802a)   # ledc_timer_config(const ledc_timer_config_t *)
def h_ledc_timer():
    sm, res, tn, f, clk = struct.unpack('<5I', rdbuf(reg('a0'), 20))
    log('ledc_timer_config speed_mode=%d duty_res=%d bits timer=%d freq=%d Hz clk_cfg=%d' % (sm, res, tn, f, clk))

@hook(0x42048466)   # ledc_channel_config(const ledc_channel_config_t *)
def h_ledc_ch():
    g, sm, ch, it, ts, duty, hp, fl = struct.unpack('<8I', rdbuf(reg('a0'), 32))
    log('ledc_channel_config GPIO%d speed_mode=%d channel=%d timer=%d duty=%d hpoint=%d' % (g, sm, ch, ts, duty, hp))

@hook(0x42048722)
def h_ledc_duty():
    log('ledc_set_duty mode=%d ch=%d duty=%d (ra=%#x)' % (reg('a0'), reg('a1'), reg('a2'), reg('ra')))

@hook(0x4204830e)
def h_ledc_duty_hp():
    log('ledc_set_duty_with_hpoint mode=%d ch=%d duty=%d hpoint=%d' % (reg('a0'), reg('a1'), reg('a2'), reg('a3')))

@hook(0x42046cd2)   # i2c_param_config(port, const i2c_config_t *)
def h_i2c_param():
    mode, sda, scl, sdapu, sclpu, clk = struct.unpack('<IiiBBxxI', rdbuf(reg('a1'), 20))
    log('i2c_param_config port=%d mode=%d SDA=GPIO%d SCL=GPIO%d pullups=%d/%d clk=%d Hz' % (reg('a0'), mode, sda, scl, sdapu, sclpu, clk))

@hook(0x4204764e)   # i2c_master_write_to_device(port, addr, buf, len, ticks)
def h_i2c_w():
    port, addr, buf, n = reg('a0'), reg('a1'), reg('a2'), reg('a3')
    data = rdbuf(buf, n)
    if addr == 0x58 and n == 2:
        r, v = data
        AW[r] = v
        if r in (2, 3): aw_out_changed(r - 2, v)
        else: log('AW9523B write reg %#04x = %#04x' % (r, v))
    else:
        log('i2c write port=%d addr=%#04x data=%s' % (port, addr, data.hex()))
    ret(0)

AW_LASTREAD = {}
@hook(0x420476f4)   # i2c_master_write_read_device(port, addr, wbuf, wlen, rbuf, rlen, ticks)
def h_i2c_wr():
    port, addr, wb, wl, rb, rl = [reg(r) for r in ('a0', 'a1', 'a2', 'a3', 'a4', 'a5')]
    w = rdbuf(wb, wl)
    if addr == 0x58:
        r = w[0]
        out = bytes(AW.get((r + i) & 0xff, 0) for i in range(rl))
        inf.write_memory(rb, out)
        if AW_LASTREAD.get(r) != out:
            log('AW9523B read reg %#04x -> %s' % (r, out.hex())); AW_LASTREAD[r] = out
    else:
        log('i2c write_read port=%d addr=%#04x w=%s rlen=%d (no device emulated)' % (port, addr, w.hex(), rl))
        ret(0x107); return   # ESP_ERR_TIMEOUT
    ret(0)

@hook(0x4204937c)
def h_adc1_atten(): log('adc1_config_channel_atten ch%d atten=%d (GPIO%d)' % (reg('a0'), reg('a1'), reg('a0')))
@hook(0x42049434)
def h_adc2_atten(): log('adc2_config_channel_atten ch%d atten=%d (GPIO%d)' % (reg('a0'), reg('a1'), reg('a0') + 5))

ADC_SEEN = {}
@hook(0x4205180e)   # adc_hal_convert after get_raw: s1=adc_n, s3=channel, s2=int* out
def h_adc_raw():
    unit, ch, out = reg('s1'), reg('s3'), reg('s2')
    v = SIM_ADC.get((unit, ch))
    if v is not None:
        wr32(out, v)
    k = (unit, ch)
    ADC_SEEN[k] = ADC_SEEN.get(k, 0) + 1
    if ADC_SEEN[k] in (1, 10, 100, 1000) or LAST_ADC.get(k) != v:
        log('ADC%d ch%d read #%d -> raw %s (simulated)' % (unit + 1, ch, ADC_SEEN[k], v))
        LAST_ADC[k] = v
LAST_ADC = {}

# app-level functions: log entry only
APP = {0x420189e2: 'Set_Plate_Position', 0x42018e18: 'DL_MQTT_REC_feedNow', 0x42018b1a: 'Control_Door',
       0x4200b2c8: 'FEED_PLAN_plateHandle', 0x4200e3b0: 'Plate_Micro_Handle', 0x4200945e: 'FEED_PLAN_openDoorHandle',
       0x4200997e: 'FEED_PLAN_closeDoorHandle', 0x4200e8a8: 'Operate_Door_Protect', 0x4200cfae: 'BSP_setExistState',
       0x4200e6f6: 'CTRL_THREAD_coolHanlde', 0x4201dbd4: 'FUN_4201dbd4 (plate-move kick)'}
for a, n in APP.items():
    def mk(n=n):
        def f(): log('CALL %s(a0=%#x, a1=%#x) ra=%#x' % (n, reg('a0'), reg('a1'), reg('ra')))
        return f
    H[a] = mk()

# ---------------- scenario ----------------
def arm_scenario():
    log('SCENARIO %s' % SCEN)
    if SCEN != 'inputs':
        return
    def rotate(lbl):
        def f():
            cur = rd8(DEV_CTRL + 5); tgt = cur % 3 + 1
            wr8(DEV_CTRL + 4, tgt); wr8(DEV_CTRL + 3, 1)
            log('SIM rotate request (%s): dev_ctrl+4 set_plate_pos=%d curr=%d, +3=1 (what Set_Plate_Position does)' % (lbl, tgt, cur))
        return f
    def door(v, lbl):
        def f():
            wr8(DEV_CTRL + 1, v); wr8(DEV_CTRL + 8, 1)
            log('SIM lid request (%s): dev_ctrl+1=%d +8=1 (what Control_Door/barnDoorState does)' % (lbl, v))
        return f
    E = EVENTS.append
    E((35000, lambda: SIM_ADC.__setitem__((0, 1), 1200), 'ADC1 ch1 (GPIO1 NTC) raw 1900 -> 1200'))
    E((40000, lambda: set_input(18, 0), 'key GPIO18 -> 0 (press)'))
    E((41000, lambda: set_input(18, 1), 'key GPIO18 -> 1 (release)'))
    E((50000, rotate('A'), 'plate rotate request A'))
    E((65000, door(4, 'open'), 'lid open request (barnDoorState=1)'))
    E((80000, door(1, 'close'), 'lid close request (barnDoorState=0)'))
    E((95000, lambda: set_input(20, 1), 'IR GPIO20 -> 1'))
    E((97000, lambda: set_input(20, 0), 'IR GPIO20 -> 0'))
    E((105000, lambda: SIM_ADC.__setitem__((1, 0), 3000), 'ADC2 ch0 (GPIO5 plate current) raw -> 3000'))
    E((106000, rotate('B, high current'), 'plate rotate request B'))
    E((135000, lambda: SIM_ADC.__setitem__((1, 0), 50), 'ADC2 ch0 raw -> 50'))
    E((150000, lambda: set_input(0, 1), 'power-detect GPIO0 -> 1 (DC removed)'))
    E((158000, lambda: set_input(0, 0), 'power-detect GPIO0 -> 0 (DC back)'))
    EVENTS.sort(key=lambda e: e[0])

CTRL_START = 0x4200ec58   # Control_ThreadHandle (thread entry); arm scenario when it is first entered
LASTNOW = [0]
def run_events():
    t = now()
    if t + 1000 < LASTNOW[0]:
        log('REBOOT detected (tick went %d -> %d): resetting plant state; scenario stops' % (LASTNOW[0], t))
        AW_OUT[0] = AW_OUT[1] = None; PL.update(run=False, gate=0, slot=0, remain=GAP_MS); DOOR.update(dir=0, t=None)
        ISR.clear(); PENDING_ISR.clear(); INJ.clear(); EVENTS.clear()
        SIM_GPIO.update({0: 0, 10: 0, 18: 1, 19: 0, 20: 0})
    LASTNOW[0] = t
    if EVENTS and EVENTS[0][0] <= t and not PENDING_ISR:
        _, fn, lbl = EVENTS.pop(0)
        log('EVENT %s' % lbl)
        fn()

# ISR injection: at a safe stop (gpio_get_level entry in task context), redirect the CPU into the
# ISR handler with ra = original pc, so the handler returns to the hooked function entry.
def try_inject_isr():
    if not PENDING_ISR: return False
    pin = PENDING_ISR.pop(0)
    fn, arg = ISR[pin]
    log('INJECT ISR for GPIO%d: handler %#x(arg=%#x) from task context' % (pin, fn, arg))
    INJ.append((reg('a0'), reg('ra'), reg('sp')))
    setreg('ra', reg('pc')); setreg('a0', arg); setreg('pc', fn)
    return True
INJ = []

VTASKDELAY = 0x4038e004          # task-context safe point for ISR injection (frequent)
INJ_PCS = (0x42045570, VTASKDELAY)
STOPS = {}
LASTREP = [0]
def main():
    global T0
    gdb.execute('set pagination off'); gdb.execute('set confirm off')
    gdb.execute('set breakpoint always-inserted on')   # avoid Z0/z0 churn on every resume
    for a in list(H) + [CTRL_START, VTASKDELAY]:
        gdb.execute('break *%#x' % a, to_string=True)
    log('hooks armed: %d breakpoints, scenario=%s' % (len(H) + 2, SCEN))
    while True:
        try:
            gdb.execute('continue', to_string=True)
        except gdb.error as e:
            log('gdb error: %s' % e); break
        pc = reg('pc')
        if T0 is None:
            T0 = 0; arm_scenario()
        if pc in INJ_PCS and INJ and INJ[-1][2] == reg('sp') and reg('ra') == pc:
            a0, ra, _ = INJ.pop(); setreg('a0', a0); setreg('ra', ra)   # back from injected ISR
            log('ISR handler returned')
        run_events(); plant_tick()
        if pc in INJ_PCS and try_inject_isr():
            continue
        STOPS[pc] = STOPS.get(pc, 0) + 1
        if now() // 10000 != LASTREP[0]:
            LASTREP[0] = now() // 10000
            log('STATS stops so far: ' + ', '.join('%#x:%d' % kv for kv in sorted(STOPS.items(), key=lambda kv: -kv[1])[:8]))
        f = H.get(pc)
        if f:
            try: f()
            except Exception as e: log('hook error at %#x: %r' % (pc, e))
        if RUN_MS and now() > RUN_MS:
            log('RUN_MS reached'); break

main()

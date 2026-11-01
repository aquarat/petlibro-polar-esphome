# Static analysis of the stock app (AF109 2.0.38)

Input: `firmware/stock/partitions/ota_0.bin` (ESP-IDF v4.4.6, ESP32-C3). Ghidra 12.2 headless, ROM symbols from
IDF v4.4 `esp32c3.rom*.ld`; functions named from ESP-IDF `__func__` strings (heuristic, see `func_names.txt`).
Addresses are virtual (IROM `0x42xxxxxx`, DROM `0x3c0fxxxx`, DRAM `0x3fcxxxxx`). `FUN_ram_x` = unnamed function at x.

Every claim below is tagged:
- **[V]** verified: read directly from code/constants (address given), and where noted cross-checked
  against the emulation log `analysis/emulation/work/uart0_verbose.log` (gpio driver's own config dump).
- **[I]** inference: plausible reading of names/behaviour, not proven.

Timing: FreeRTOS tick = 1 ms **[I, strong]** (AW9523B reset `vTaskDelay(200)` + 1000-tick I2C timeout matches
the 1.21 s gap in the emulation log). All `vTaskDelay`/timer values below are given in ms on that basis.
The main control loop (`Control_ThreadHandle` @0x4200f6d0) runs every 500 ms **[V]** (`vTaskDelay(500)`), so
"N loops" = N x 0.5 s.

## 1. Hardware map

### External chips
| Chip | Bus | Evidence | Conf. |
|---|---|---|---|
| **AW9523B** 16-bit I/O expander, 7-bit addr **0x58** | I2C0, SDA=GPIO7, SCL=GPIO8, 100 kHz, internal pull-ups | `AW9523B_i2cInit` @0x4200d9c8: `i2c_param_config(0,{mode=1,sda=7,scl=8,pu=1,pu=1,100000})`, reads ID reg 0x10, tag "aw9523b" | V |
| U6 (QFN) = the AW9523B | | only I2C device in the firmware; AW9523B is QFN-24 | I |
| One-wire sound/voice chip on GPIO6 | bit-banged | `FUN_ram_4200d7e8` @0x4200d7e8: start = low 5 ms, 8 bits LSB first, "1" = high 3 ms/low 1 ms, "0" = high 1/low 3, idle high. This is the WTN6-family one-wire protocol | V (protocol) / I (chip family) |
| No UART1, RMT, SPI, PCNT or second I2C device | | no calls to `uart_driver_install/uart_param_config/rmt_config/spi_bus_initialize/pcnt_*` from app code | V |

AW9523B init (@0x4200d9c8) **[V]**: GPIO9 low 200 ms then high (RSTN); reg 0x12=0xFF, 0x13=0xFF (all pins GPIO
mode, no LED/dimming); 0x04=0x03 (P0_0,P0_1 inputs, rest outputs); 0x05=0x00 (P1 all outputs); 0x11=0x10 (P0
push-pull); outputs 0x02/0x03 = 0. Pin write helper `FUN_ram_4200d8a6(pin, level)` keeps a 16-bit shadow
(0x3fca3314) and writes reg 0x02 (P0) and 0x03 (P1) every time; pin n = P0_n for 0..7, P1_(n-8) for 8..15.
Hardware revision = P0 input bits[1:0] (`GetHardwareVersion` @0x4200d930): 0 -> "5.0.0", 2 -> "5.1.0",
1 or 3 -> "5.2.0". The tray logic differs per revision (section 2). **Our unit's revision is unknown** (the
emulator has no AW9523B and reads 0).

### GPIO table
| Pin | Dir | Peripheral | Function | Evidence (address) | Conf. |
|---|---|---|---|---|---|
| GPIO0 | in, no pull | GPIO (polled) | Power source: **0 = DC/mains, 1 = battery**. power_type 1=DC, 2=battery, 3=DC+battery-level>0 | `AllDet_init` @0x4200ce24 gpio_config mask 0x1; `Power_Handle` @0x4200c948, `FUN_ram_4200c700`; logs "sitch to POWER_BAT_MODE/POWER_DC_MODE" | V |
| GPIO1 | analog | ADC1_CH1, 11 dB, 12 bit | NTC temperature (food compartment), 10-sample mean | `Adc_init` @0x4200d26e string "ADC1_CHAN1_TEMP"; `Read_Ntc_Temp` @0x4200d488 | V |
| GPIO2 | out | LEDC ch0, timer0, low-speed, 8 bit, **38 kHz, duty 127/256** | IR emitter for presence ("EXIST") detection; PWM on only while feeding / lid open | `AllDet_init` @0x4200ce24 (ledc cfg @0x3c0fa6f4), `BSP_setExistState` @0x4200cfae logs "GPIO_EXIST_ON/OFF" | V (signal) / I (IR LED) |
| GPIO3 | analog | ADC1_CH3 (mV, eFuse-calibrated) | Lid (door) motor current | "ADC1_CHAN3_MOTOR", `Read_DoorMotorAdc` @0x4200d402 | V |
| GPIO4 | analog | ADC1_CH4 (mV) | Battery voltage, x2 divider (`FUN_ram_4200d360` returns mV*2) | "ADC1_CHAN4_BAT", @0x4200d360 | V |
| GPIO5 | analog | ADC2_CH0 (raw 12-bit counts) | Tray (plate) motor current | "ADC2_CHAN0_MOTOR", `Read_PlateMotorAdc` @0x4200d37e (`adc2_get_raw`) | V |
| GPIO6 | out, idle high | GPIO | Sound chip one-wire data | `BSP_aduioInit` @0x4200d738, @0x4200d7e8 | V |
| GPIO7 | od | I2C0 SDA | AW9523B | @0x4200d9c8 | V |
| GPIO8 | od | I2C0 SCL | AW9523B (strap pin: must be high at reset) | @0x4200d9c8 | V |
| GPIO9 | out | GPIO | AW9523B RSTN (also the boot strap) | @0x4200d9c8 `gpio_set_level(9,0); delay 200; (9,1)`; emu log "GPIO[9] OutputEn:1" | V |
| GPIO10 | in, **pull-up** | GPIO any-edge IRQ | **Tray position sensor** (code calls it "Micro_Sw"/"Plate_Micro"); by elimination this is CON6 "food-bowl optical gate" | `AllDet_init` mask 0x400, pull-up, `gpio_set_intr_type(10,3)`; handler in `gpio_task_example` @0x4201d47c; emu log "GPIO[10] Pullup:1" | V (role) / I (=CON6) |
| GPIO18 | in, no pull | GPIO any-edge IRQ | **Button**, active low; hold 3 s = Wi-Fi reset | `Key_init` @0x4200d10c mask 0x40000; @0x4201d47c, `KeyRestore_TimerCallback` @0x4201d42e | V (role) / I (which physical button) |
| GPIO19 | in, no pull | GPIO any-edge IRQ | **Lid (door) position sensor**; 1 = stop condition for lid moves (see open question 8) | `AllDet_init` mask 0x80000; `Operate_Door_Protect` @0x4200e8a8, door handlers | V |
| GPIO20 | in, no pull | GPIO (polled; IRQ type set, no handler) | **IR receiver**: 1 = something/pet in front | `Infrared_Handle` @0x4200c604, `FEED_PLAN_getIrIdenState` @0x42008b2c, "EXIST_SOMETHING report" | V |
| GPIO21 | out | UART0 TX | console; app logs off unless NVS `storage/sysInfo[0] != 0` | `SYSYTEM_INFO_init` @0x420081c0: `esp_log_level_set("*", NONE)` | V |
| GPIO11-17 | | | flash / not used | | V |

GPIO18/19 are the USB D-/D+ pins: the stock app uses them as plain GPIO, so USB-Serial-JTAG is unusable while it runs **[V]**.

### AW9523B pin table
| Pin | Dir | Function | Levels | Evidence | Conf. |
|---|---|---|---|---|---|
| P0_0, P0_1 | in | hardware revision straps | see above | @0x4200d930 | V |
| P0_4, P0_7 | out | sensor/motor power enable (set 1 at boot; on battery only on while lid or tray moves) | 1 = on | `AllDet_init`, `CTRL_THREAD_batteryModeHandle` @0x4200e4fc | V (behaviour) / I (what they power) |
| **P0_5, P0_6** | out | **Tray motor (J4) H-bridge inputs** | fwd: P0_5=1,P0_6=0; reverse: 0,1; **brake: 1,1** (boot default) | `Motor_init` @0x4200d1a2, `Device_Module_Ctrl` @0x4200ea0e ("PLATE_READY_SET_POS" fwd, "PLATE_READY_BACK_POS" rev), "GPIO_MOTOR_BRAKE" in `FEED_PLAN_plateHandle` @0x4200b2c8 | V |
| P1_0 (8), P1_1 (9) | out | two status LEDs (CON3), 1 = on; colours unknown | | `AlarmLed_init` @0x4200d162, patterns @0x4200c3d4/@0x4200c73a | V (pins) / I (colours) |
| **P1_4 (12)** | out | **Peltier ("PTC") on/off**, not PWM (AW9523B left in GPIO mode, no dim registers written) | 1 = cooling on | "GPIO_PTC_ON" in `CTRL_THREAD_coolHanlde` @0x4200e6f6; MQTT `SWITCH_COOL_SERVICE` `coolState` "OPEN"->1/"CLOSE"->0 in `Contrl_Cool_State` @0x42018866 | V |
| **P1_6 (14), P1_7 (15)** | out | **Lid/door motor H-bridge** | close ("GPIO_DOOR_MOTOR_FORWARD"): P1_7=1,P1_6=0; open ("GPIO_DOOR_MOTOR_BACK"): 0,1; brake 1,1 | `Door_Motor_init` @0x4200d1e8, `Device_Module_Ctrl` ("DOOR_READY_CLOSE"/"DOOR_READY_OPEN"), `FEED_PLAN_openDoorHandle` @0x4200945e | V |
| P0_2, P0_3, P1_2, P1_3, P1_5 | out | never written after init (stay 0) | | io_callsites.txt | V |

**Fan: no separate control exists in the firmware** **[V]**: no ESP GPIO or AW9523B pin other than P1_4 is
touched by cooling code. So the fan is either switched together with the Peltier (P1_4) or always powered
**[I]**; measure before driving P1_4 from ESPHome.

### Temperature (NTC on GPIO1) **[V]** @0x4200d488, constants @0x3c0fa9f0..0x3c0faa38
```
mV  = esp_adc_cal_raw_to_voltage(mean of 10 adc1_get_raw(CH1)) - tempCoeff   # tempCoeff: NVS "temp_info" (4 B), set by MQTT cmd TEMP_CAL
V   = mV / 1000
R   = V / (3.3 - V) * 10000            # NTC to GND, 10k pull-up to 3.3 V
T   = 1 / (ln(R / 10000) / 3950 + 1/298.15) - 273.15 + 0.5      # B=3950, R25=10k, +0.5 degC offset
T   = trunc(T * 100) / 100;  if T < -200: keep previous value
```

### Battery (GPIO4) **[V]** table @0x3c0fa6c0 (`FUN_ram_4200c81c`)
mV(after x2) -> %: 3300 0, 3420 10, 3540 20, ... +120 mV per 10 % ..., 4500 100; >4499 = 100.
On battery, < 3300 mV for 5 consecutive loops (2.5 s) -> `FUN_ram_4200c88c`: brake both motors, LEDs off,
Wi-Fi stop, suspend control task, then poll every 1 s until GPIO0 = 0 (DC back) and restart.

## 2. Tray (plate) rotation **[V unless marked]**

State lives in `dev_ctrl` (0x3fc9a2a0, NVS `storage/dev_ctrl`, 64 B):
`+0` lid state, `+1` lid command, `+2` plate state (0 idle, 2 moving fwd, 3 arrived, 5 moving back),
`+3` plate command (1 = go fwd, 4 = go back), `+4` target plate, `+5` current plate, `+6` "calibrated".
Stock NVS: `00 00 03 00 01 01 ...` = plate arrived, target 1, current 1.

**There are 3 positions (plates 1..3).** Position arithmetic everywhere is `p = (p+1) & 3; if p == 0: p = 1`
(@0x4200e39c, @0x4200b2c8); `FEED_PLAN_getEmptyPlate` @0x4200a836 iterates `{1,2,3}`; at most 3 plans.
Each counted edge on GPIO10 = one plate step (one third of a turn). Rotation is **forward only** in normal use;
reverse is only used for stall recovery.

```
start_move(target):                        # Device_Module_Ctrl @0x4200ea0e when +3 == 1
    dev.target = target; state = MOVING
    AW P0_5=1, P0_6=0                      # forward
    disable GPIO10 IRQ; re-enable after 500 ms   # Close_SW_Interrupt @0x4201da2c + MicroSw_Debounce timer
    (HW 5.2.0) start plateErr timer (plateErrTimeout s, default 60)    # FUN_ram_4201dbd4(1)

on GPIO10 edge (queue task @0x4201d47c):
    HW 5.0.0: act on rising edge (level 1): calibrated = 1; step()
    HW 5.1.0: act on falling edge (level 0): calibrated = 1; step()
    HW 5.2.0: act on falling edge: step(); then home detection:
        dt = time since last rising edge (= width of the high pulse)
        if dt < 1000 ms and pulses < 4: stop plateErr timer; max_dt = max(max_dt, dt); pulses += 1
        if not calibrated and pulses > 2 and dt >= max_dt - 150 ms:
            current = 1; calibrated = 1            # the widest mark is plate 1 (home)
    on rising edge: remember timestamp

step():                                    # FUN_ram_4200e39c / Plate_Micro_Handle @0x4200e3b0
    if state != MOVING: return
    stop plateErr timer
    current = next(current)                # 1->2->3->1
    if current == target and calibrated:
        if HW 5.0.0: delay 50 ms           # overrun to centre the bowl
        AW P0_5=1, P0_6=1                  # brake
        state = ARRIVED; report platePosition
    disable GPIO10 IRQ for 500 ms          # debounce: ignore the rest of this mark
    save dev_ctrl to NVS
```
Boot (`DoorMotor_Reset_ThreadHandle` @0x4200ec58): close the lid first (see 2b); on HW 5.2.0 also set target=1
and start a move, and only start the control thread once state==ARRIVED and current==1 (homing). On 5.0.0/5.1.0
no homing: `current` comes from NVS and `calibrated` becomes 1 on the first edge.
On HW 5.2.0, if no gate edge arrives within `plateErrTimeout` (s) of a move start: `Report_plateErrEvent`,
`new_dev_attr[10] = 2`.

**Stall / jam** (`Feeding_Monitor` @0x4200b8dc, recovery in `FEED_PLAN_plateHandle` @0x4200b2c8 and
`FEED_PLAN_thawHandle` @0x42008e9e):
```
every 500 ms: if adc2_raw(GPIO5) > plateStuckCurrent (default 400 counts) for 6 loops (3 s) while MOVING:
    plateMotorState = STUCK
if STUCK:
    if retries < 3:  retries++; brake; delay 1000 ms; reverse (P0_5=0,P0_6=1) for 4 loops (2 s);
                     then forward again, GPIO10 IRQ re-enabled after 2000 ms   ("plate motor restore")
    else:            brake; state = idle; report error ("MOTOR_STUCK stop plate motor"); during a feed the
                     record is closed with error code 0x7fc
```
Lid motor uses the same test with GPIO3 mV > doorStuckCurrent (400 mV), 3 retries, error codes 0x7f6 (open) / 0x7f0 (close).

**Where the tray rests** (`FEED_PLAN_plateHandle`, every 500 ms, DC power only, no feed within 30 min):
`target = first plate (1,2,3) not assigned to a pending plan` (`FEED_PLAN_getEmptyPlate`). If all 3 plates hold
pending meals, advance one plate every 30 min (1,800,000 ms) so each meal takes turns ("switch plate make cool").
So "current plate" is the plate under the lid opening, and the empty one is parked there **[I, strong]**.

### 2b. Feed sequence (state @0x3fca13e8, `Feed_Handle` @0x4200ae46)
```
T-30 min (One_PlanCheck @0x4200ab0a: now < plan_time < now+1800 s, not executed):
    play sound 0xEF; state = THAW(4); rotate tray to plan.plate (start_move)
T-1 h edge (checkOnePlanBeforeOneHour @0x42008bca, plan_time in (now+3597, now+3600]):
    if T <= 13.0 C: Peltier off and auto-cooling suspended until the feed (thaw)
At plan time (yyyy-mm-dd hh:mm and second 0..1), or at once for WET_FOOD_FEED_NOW:
    OPEN (5): lid motor "back"; done when GPIO19 == 1 within doorCheckSignalTime (5 s), or after
              doorNotcheckSignalTime (10 s) regardless; play sound 1; run on 1000 ms; brake; IR PWM on
    ONGOING (10): wait plan.feedingDuration minutes; ringer (ringerMode SMART): sound 1 every ringerInterval min
              for ringerDuration min until GPIO20 sees a pet
    then close when no pet on GPIO20 for 20 loops (10 s) or irSensorIdenTimeout expired
    CLOSE (7): lid motor "forward"; same 5 s / 10 s sensor windows; if GPIO20 sees a pet while closing:
              stop and reopen ("Identify the pet DOOR_READY_OPEN"); run on 1000 ms; brake
    record GRAIN_THAW / GRAIN_START / OPEN_DOOR / CLOSE_DOOR / GRAIN_END, report WET_GRAIN_OUTPUT_EVENT
```
Note the open/close windows accept GPIO19 == 1 only during the first doorCheckSignalTime and otherwise run the
full doorNotcheckSignalTime, so whether GPIO19 is high at rest (closed) is not clear from code alone (open
question 8).
Manual lid (`SWITCH_DOOR_SERVICE`) and boot close: ignore GPIO19 for the first 2000 ms (`Timer_DoorSignalHandle`
@0x4200de5a sets the enable flag), then stop at GPIO19 == 1 or after 40 loops (20 s), run on 1000 ms, brake.
Boot close: forward 2000 ms, then poll GPIO19 every 100 ms up to 5 s, brake.

## 3. Cooling **[V]** `CTRL_THREAD_coolHanlde` @0x4200e6f6 (every 500 ms)
```
if power_type == BATTERY:            Peltier off (Power_Handle also forces P1_4=0 on battery, 1 on DC entry)
elif manual override (dev_ctrl+7, set by SWITCH_COOL_SERVICE): do nothing (cloud set P1_4 directly)
elif not-bound flag 0x3fca364c != 0: Peltier off
else:
    if a plan is exactly 1 h away and T <= 13.0 C: Peltier off, thaw-latch until the feed starts
    elif decision allowed (flag gp-0x79c):
        T <= 12.0 C -> Peltier off;  T > 12.0 C -> Peltier on;  flag = 0
    if flag == 0: after 600 loops (300 s) flag = 1          # sample-and-hold bang-bang, 5 min period
```
Flag 0x3fca364c (`FUN_ram_420122a0`/`FUN_ram_420122ae`) is set to 1 at init (@0x42012d70) and cleared when
stored MQTT info exists (@0x4201b3e6) or from the binding reply `code` (@0x420161fc), i.e. it means "not bound
to the cloud" **[I, strong]**: an unbound stock unit never cools, and pet events are not logged.
Thresholds @0x3c0fae60 (13.0) and @0x3c0fae68 (12.0). No temperature hysteresis band, no PWM, no fan logic,
no over-temperature or Peltier-current protection in firmware (only the battery cut-off). The cloud also has
`coolSwitch`, `coolAgingType`, `coolStartTimeUtc/EndTimeUtc/coolTimes` attributes stored in `dev_attr`; I did not
find them gating the loop above **[I]**.

## 4. Scheduling, state, UI

Time: UTC ms from MQTT `NTP_SYNC` (`Service_Ntp_Sync`) plus `timezone`; saved to NVS `rtc_info` (4 B) and
after each feed (`DL_MQTT_INIT_saveLocalTimeAfterFeed`). No RTC chip. Plans (max 3, 53-byte records @0x3fca1498,
NVS `fd_plan`): planId (u32), year u16, month, day, min, hour, plate, executed, ..., feedingDuration (u32 at +0x2b).

NVS namespace `storage` (stock dump listed with `scripts/nvs_keys.py`):
| Key | Size | Meaning | Evidence |
|---|---|---|---|
| sysInfo | 200 | [0] = log enable (0 -> all logs off). Absent in our dump, hence the silent console | @0x420081c0 |
| dev_attr | 72 | +4 ringerMode, +5 ringerInterval min, +6 ringerDuration min, +0xb doorCheckSignalTime s, +0xc doorNotcheckSignalTime s, +0xe powerMode, +0x15 magic '2', +0x16 closeDoorTime, +0x1d.. light switch/aging/start/end, +0x2b.. cool, +0x39.. sound | `Dev_Attr_Init` @0x4200bd2c, parser @0x42016658 |
| new_dev_attr | 200 | +0 doorStuckCurrent (400), +2 plateStuckCurrent (400), +4 irSensorIdenTimeout (10), +6 idenFirstPlateTime (450), +8 plateErrTimeout s (60), +10 plate error flag | `DEV_ATTR_newAttrInit` @0x4200bf94 |
| dev_ctrl | 64 | lid/plate state, target/current plate (section 2) | @0x4200ef34 |
| temp_info | 4 | NTC mV offset (tempCoeff) | @0x4200d488 |
| fd_plan, fd_plan_index, fd_rear_index, fd_fron_index, feed_info, feed_err_info | | plans and feed log ring | feed_plan module |
| pet_info, pet_rear_index, pet_fron_index | | pet near/leave log ring | @0x4200e0fe |
| rtc_info, bindIdInfo, mqtt_conn_info, log_rec_key | | time, cloud binding, broker creds (sensitive), debug log ring | |

Button GPIO18 (`gpio_task_example` @0x4201d47c): press starts a 3000 ms one-shot timer, release cancels it.
On expiry: queue `RESET` report to cloud, wait 1 s, `CLear_WIfiStaConfig` (back to AP/BLE provisioning, AP SSID
`PL-AF109-xxxx`). No short-press action found.

LEDs (`FUN_ram_4200c73a`, every 500 ms; LED schedule `lightSwitch/lightAgingType/start/end` can turn both off):
error (plate/lid stuck) or battery -> P1_0 solid; Wi-Fi state other than 0/3 -> P1_0 blinks 1 s/1 s;
otherwise by network state (`FUN_ram_4200c3d4`): P1_1 slow blink 1 s/1 s, P1_1 fast 0.5/0.5, P1_1 solid
(cloud connected), or P1_0/P1_1 alternating. Mapping of those states to "pairing/connecting" is **[I]**.

Sound: only two codes are sent: 0xEF at feed start (on WTN6 chips 0xE0-0xEF = volume, 0xEF max **[I]**) and
0x01 (meal call) at lid open and for the ringer. Gated by `soundSwitch` schedule and volume byte != 0
(`FUN_ram_4200c276`).
Details (2026-10-31, full write-up in `docs/esphome-firmware.md` "Sound") **[V]**: exactly four call sites of
`FUN_ram_4200d7e8`: `Feed_Handle` (0xEF, when a feed job is accepted: plan pick-up 30 min ahead or
WET_FOOD_FEED_NOW), `FEED_PLAN_openDoorHandle` (0x01), ringer `FUN_ram_4200a23e` (0x01; ringerMode "NORMAL" 0 /
"SMART" 1, SMART repeats every ringerInterval min for ringerDuration min + 10 s until GPIO20 = 1; skipped for
plan id 666666666) and `DL_MQTT_REC_funcTest` `FUN_ram_42018c3a` (0x01, action "AUDIO"). No sound at boot,
button, pairing or errors. dev_attr +0x15 is both the layout magic '2' and the reported `volume` (never parsed
from the cloud, never sent to the chip). No BUSY input: GPIO6 is output-only and no GPIO read relates to sound.
Per-plan `enableAudio`/`audioTimes` (plan +0x13/+0x14) are stored and never read.

## 5. Cloud (summary)
MQTT topics `dl/PLAF109/<device_sn>/device/<t>/<post|sub>` with t in heart, ota, ntp, broadcast, config, event,
service, system (`FUN_ram_42011dea`), JSON with `cmd`, `msgId`, `ts`. Commands received (`Receive_handle`):
NTP_SYNC, OTA_UPGRADE, GET_CONFIG, BINDING, UNBIND, WIFI_RECONNECT/CHANGE_SERVICE, ATTR_SET/GET_SERVICE,
SERVER_CONFIG_PUSH, RESTORE, SWITCH_COOL_SERVICE {coolState OPEN|CLOSE}, **SET_PLATE_POS_SERVICE
{platePosition 1..3}**, SWITCH_DOOR_SERVICE, DEVICE_FUNCTION_TEST_SERVICE, WET_FOOD_FEED_NOW_SERVICE {planId,
feedingDuration}, WET_FOOD_FEED_STOP_SERVICE, WET_GRAIN_FEEDING_PLAN_SERVICE {plans[]: planId, plate,
executionDay "Y-MM-DD", executionTime "HH:MM", feedingDuration}, DEVICE_PRINT_LOG_SWITCH_SERVICE,
TRIGGER_DEVICE_LOG_REPORT_SERVICE, GET_SOME_ATTR_SERVICE, TEMP_CAL {tempCoeff}, AUDIO. Reported: HEARTBEAT
{count,rssi,wifiType,temperature}, DEVICE_PROPERTIES {powerType, electricQuantity, platePosition, doorState,
coolState, ...}, WET_GRAIN_OUTPUT_EVENT, PET_DETECT_EVENT {NEAR/LEAVE}, ERROR_EVENT {errorCode}, ATTR_PUSH_EVENT.
A product key/secret pair is hard-coded at 0x3c0fcbd0 (not reproduced here). Strings: `work/strings_drom.txt`.

## 6. Open questions
1. Hardware revision of our board (AW9523B reg 0x00 bits 1:0). Decides edge polarity and whether a wide home mark exists. Read it once over I2C from ESPHome.
2. Fan wiring: on P1_4 with the Peltier, or always on? Measure the fan with P1_4 = 0 and = 1.
3. What P0_4 and P0_7 power (gate emitter, IR, motor driver?). Keep both high when anything moves.
4. Which physical button is GPIO18 (SW1 or top button), and whether the other one is wired to anything.
5. Physical form of the tray marks (3 slots, one wider = home on 5.2.0). Needed for the 6/12 compartment plan:
   the firmware only knows 3 edges per turn, so finer steps need timing between edges or extra marks.
6. Lid connector (second motor) and its sensor connector are not in `docs/hardware.md` yet.
7. Units of the raw plate-current threshold (400 ADC2 counts at 11 dB, about 0.4 V) vs actual motor current.
8. Lid sensor semantics: is GPIO19 a pulse/edge at each end, or level high at both end stops? The boot close
   (run 2 s, then stop at GPIO19 == 1) suggests "1 = at an end stop"; the feed-open window (stop if 1 within
   the first 5 s) would then stop almost immediately unless the sensor is low at the closed position.

## 7. Candidate ESPHome sketch (not a full config)
```yaml
i2c: {sda: GPIO7, scl: GPIO8, frequency: 100kHz}
output:
  - {platform: gpio, id: aw_rst, pin: GPIO9}            # hold high after a 200 ms low pulse at boot
  - {platform: ledc, id: ir_tx, pin: GPIO2, frequency: 38000Hz}   # set level 0.5 while sensing
# AW9523B @0x58: no stock ESPHome component; use a small external component or lambdas:
#   regs 0x12/0x13=0xFF, 0x04=0x03, 0x05=0x00, 0x11=0x10; outputs reg 0x02 (P0) / 0x03 (P1)
#   tray motor P0_5/P0_6 (fwd 1/0, brake 1/1), lid P1_7/P1_6 (close 1/0, open 0/1, brake 1/1),
#   Peltier P1_4, LEDs P1_0/P1_1, power enables P0_4/P0_7 = 1
binary_sensor:
  - {platform: gpio, id: tray_gate, pin: {number: GPIO10, mode: INPUT_PULLUP}}   # count one edge per plate
  - {platform: gpio, id: lid_end,   pin: GPIO19}
  - {platform: gpio, id: ir_rx,     pin: GPIO20}
  - {platform: gpio, id: on_battery, pin: GPIO0}       # 1 = battery
  - {platform: gpio, id: button,    pin: {number: GPIO18, inverted: true}}
sensor:
  - {platform: ntc, sensor: ntc_r, calibration: {b_constant: 3950, reference_temperature: 25C, reference_resistance: 10kOhm}}
  - {platform: resistance, id: ntc_r, sensor: ntc_v, configuration: DOWNSTREAM, resistor: 10kOhm}
  - {platform: adc, id: ntc_v, pin: GPIO1, attenuation: 12db}
  - {platform: adc, id: lid_current, pin: GPIO3, attenuation: 12db}
  - {platform: adc, id: battery, pin: GPIO4, attenuation: 12db, filters: [multiply: 2]}
  - {platform: adc, id: tray_current, pin: GPIO5, attenuation: 12db}  # ADC2: unreliable while Wi-Fi is on
logger: {hardware_uart: UART0}   # GPIO18/19 are I/O, so no USB-Serial-JTAG
```
Rules carried over from the stock code: brake (both inputs high) on every stop; ignore the gate for 500 ms after
each counted edge; 3 s over-current = stall, then brake 1 s, reverse 2 s, retry up to 3 times; Peltier only on
DC power, with fan coverage confirmed first.

## Files
- `scripts/img2elf.py` - app image to ELF (one section per segment).
- `scripts/run_ghidra.sh` - full re-run: ELF, Ghidra import with `ghidra_scripts/Esp32c3Setup.java` (ROM/peripheral
  blocks and ROM symbols) and `ghidra_scripts/NameAndDump.java` (names from `__func__` strings, string xrefs,
  decompile of all functions) into `work/out/`.
- `scripts/fn.sh <regex>` - print decompiled functions by name/address. `scripts/io_callsites.sh` - all
  GPIO/AW9523B/ADC/LEDC/sound call sites (output in `work/io_callsites.txt`).
- `scripts/decode_consts.py` - prints every numeric constant cited above from the image.
- `scripts/nvs_keys.py` - lists NVS keys (values only with `--values`; keep that output local).
- `work/` (git-ignored, regenerable): ELF, Ghidra project, `out/decomp_all.c`, `out/string_xrefs.txt`,
  `out/func_names.txt`, `strings_drom.txt`.

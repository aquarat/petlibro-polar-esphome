# Emulating the stock AF109 2.0.38 firmware (QEMU, ESP32-C3)

Dynamic analysis of the stock app in `ota_0`, run in Espressif's QEMU fork, to learn how it drives
the hardware. Everything runs offline, on copies under `work/` (gitignored). `firmware/` is never written.

Markers: **[O]** observed in the emulator; **[S]** read from code (disassembly, or the decompile in
`../static/work/out/`); **[I]** inferred. Emulator behaviour that the firmware did not cause is called out.

## 1. Result in one paragraph

The unmodified image boots in QEMU exactly as on the board and goes silent at the same point.
The silence is a log-level switch, not a UART change. `SYSTEM_INFO_init` (0x420081d0) loads the
NVS blob `sysInfo`. That blob is absent on this device ("systemInfoParam NULL"), so its debug flag is 0
and the function calls `esp_log_level_set("*", ESP_LOG_NONE)`. A non-zero flag gives VERBOSE instead,
and 0x4200826c sets the flag (a cloud debug switch [I]). Logging kept working with one patched instruction.
Four more patches (Wi-Fi, BLE, SAR-ADC wait) and a gdb driver got the app to its steady-state
idle loop (`Control_ThreadHandle start...`, `Feeding_Monitor`, `Power_Handle`). The driver also emulates
the AW9523B I/O expander, injects ADC values and GPIO edges, and models the tray gate and the lid switch.
In that state the app homed the tray and the lid at boot, rotated the tray one compartment on request,
opened and closed the lid, detected a motor stall, and reacted to the key and to DC power loss.

## 2. Reproduce

Versions: Espressif QEMU `esp-develop-9.2.2-20260417`, asset `qemu-riscv32-softmmu-esp_develop_9.2.2_20260417-aarch64-linux-gnu.tar.xz`
(sha256 `a9f7b98636008edcf7a11c96f10b3a3ec83c2a890fc54c3e3ceb3ec9edace427`), in `third_party/qemu-esp/qemu/`.
GNU gdb 17.2 (Fedora; multi-arch, `set architecture riscv:rv32`), Python 3.14, capstone 5.0.7.
The app ELF view is `../static/work/ota_0.elf` (built by `../static/scripts/img2elf.py`). Host: Fedora Asahi aarch64.

```sh
cd analysis/emulation
# 0. QEMU (prebuilt, aarch64 linux)
mkdir -p ../../third_party/qemu-esp && cd ../../third_party/qemu-esp
curl -LO https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/qemu-riscv32-softmmu-esp_develop_9.2.2_20260417-aarch64-linux-gnu.tar.xz
tar xf qemu-riscv32-softmmu-*.tar.xz && cd -
# 1. working copy (never run on firmware/ directly; run_qemu.sh refuses)
cp ../../firmware/stock/20261030T203938_polar_backup_1.bin work/flash.bin && chmod 644 work/flash.bin
# 2. stock behaviour: boots, goes silent after SYSYTEM_INFO_init
scripts/run_qemu.sh work/flash.bin 30          # -> work/flash.uart0.log
# 3. patched copy (checksum + SHA-256 refixed, verified with esptool image-info)
python3 scripts/patch_image.py work/flash.bin work/flash_v7.bin loglevel,nowifi,noble,adcnowait
# 4a. plain run with full MMIO trace
TRACE=work/trace_v7.txt scripts/run_qemu.sh work/flash_v7.bin 240
python3 scripts/mmio_decode.py work/trace_v7.txt > work/trace_v7.decoded.txt
# 4b. run under the gdb driver (AW9523B emulation, ADC/GPIO injection, tray/lid model, scenario)
cp work/flash_v7.bin work/run_d.bin
scripts/run_traced.sh work/run_d.bin 300 inputs     # or 'idle' (no input events)
python3 scripts/pin_trace.py work/run_d.hooks.log > traces/run_d.pin_trace.txt
# RAM-only alternative for the log level (no image patch):
GDB=1 scripts/run_qemu.sh work/flash.bin 60 & gdb -batch -x gdb/connect.gdb -x gdb/loglevel.gdb
# NVS key list (values redacted by default)
python3 scripts/nvs_decode.py ../../firmware/stock/partitions/nvs.bin --values --markdown
```

QEMU runs with `-nic none -monitor none`, with no network device and no route out. The `esp32c3` machine has no Wi-Fi
model anyway. A run under gdb executes at about host real time (`set breakpoint always-inserted on` matters: without it every stop
re-sends 30 Z0/z0 packets and the guest runs about 10x slower).

| File | Purpose |
|---|---|
| `scripts/run_qemu.sh` | Boot an image copy offline. `GDB=1` adds the stub on :1234 and waits; `TRACE=f` adds the MMIO trace |
| `scripts/run_traced.sh` | QEMU + MMIO trace + `gdb/hooks.py` in one go |
| `scripts/patch_image.py` | Named patches on a copy, verifies the original checksum/SHA first, re-fixes both |
| `scripts/mmio_filter.py` | Filters QEMU `memory_region_ops_read/write` trace events: UART0 TX becomes LOG lines, keeps GPIO/IO_MUX/LEDC/I2C/SARADC/UART1/RTC_IO, collapses polling |
| `scripts/mmio_decode.py` | Register-level decode (GPIO matrix, IO_MUX, LEDC, SARADC) |
| `scripts/pin_trace.py` | Condenses the gdb hook log into a pin trace |
| `scripts/nvs_decode.py` | Standalone NVS v2 parser (CRC-checked); prints values only for numeric keys |
| `scripts/xref.py` | capstone helper: string/address xrefs, `--calls`, `--dis` |
| `gdb/connect.gdb`, `gdb/hooks.py`, `gdb/loglevel.gdb` | gdb connection, Python hook driver, RAM log-level override |
| `traces/` | Redacted outputs of the final run `run_d` (UART, decoded MMIO, pin trace) and the unpatched boot log |

## 3. Patches (emulated copy only)

| Name | Address (flash off) | Change | Why |
|---|---|---|---|
| `loglevel` | 0x42008202 (0x068202) | `c.li a1,0` → `c.li a1,5` | `esp_log_level_set("*", NONE)` becomes VERBOSE in `SYSTEM_INFO_init` |
| `nowifi` | 0x4209c4d4 (0x0fc4d4) | `esp_wifi_start` → `li a0,0; ret` | No RF/PHY model. Without it `register_chipv7_phy` spins forever; with only PHY skipped (`nophy`, kept for reference) the MAC code faults at 0x420cfffc |
| `noble` | 0x4201b508, 0x4201b50c, 0x4201cdbc | NOP `NimBle_Cfg_Init` and the BLE-start call; BLE start → `li a0,0; ret` | No BT controller: `esp_bt_controller_init` asserts (`select_src_ret && set_div_ret`); later NimBLE mutex asserts |
| `adcnowait` | 0x42051804 (0x0b1804) | `c.beqz a0,-6` → `c.nop` in `adc_hal_convert` | No SAR ADC model: the done bit is never set, so the interrupt watchdog fires. Raw reads then return 0 unless gdb injects a value |

Consequence: the app believes Wi-Fi/BLE started and never connects. It stays in "AP + BLE provisioning,
WIFI_DISCONNECT / MQTT_DISCONNECT" mode, which is also what this device's NVS implies (no STA credentials, §7).

## 4. How far it runs (boot log, `traces/run_d.uart0_redacted.log`)

Unpatched (`traces/stock_unpatched_boot.log`) [O]: ROM → IDF v4.4.6 bootloader → app header → heap/flash →
`Main_function: [APP] Startup..` → `system_info: SYSYTEM_INFO_init systemInfoParam NULL`, then silence. This is
identical to the real UART capture, except chip rev v0.3 (QEMU efuse), flash mode SLOW READ and the flash chip shown as "gd".

Patched, condensed [O] (times in ms; identifiers redacted):
```
I (1555) system_info: SYSYTEM_INFO_init systemInfoParam NULL
I (...)  gpio: GPIO[9]| OutputEn: 1                    <- AW9523B reset line
I (...)  aw9523b: AW9523B_i2cInit id=0x23 ... DL_PRODUCT_HARDWARE_VERSION:5.2.0   (with AW emulation)
I (...)  BSP_init: Ptc_init / Motor_init / Door_Motor_init: OK, Key_init: ok!, AlarmLed_init: ok!
I (...)  gpio: GPIO[10] pullup Intr / GPIO[19] / GPIO[20] / GPIO[0] / GPIO[4,1,3,5] ADC / GPIO[6] out
I (...)  BSP_init: BSP_aduioInit: OK, Function:Bsp_init
I (...)  MQTT_INIT: bindId:3 ... Mqtt_DataInit exist mqtt info, dev_sn:<redacted>, product_key:<redacted>, product_secret:<redacted>
I (...)  dev_attr: led_enable=1 led_mode=1 ... doorCheckSignalTime=5 doorNotcheckSignalTime=10 ... plateStuckCurrent=400 doorStuckCurrent=400
I (...)  feed_plan: feed_record_s empty ...   dl_rep_log: recLogInit cnt=30
I (...)  wifi: ... ble_net: initialise_wifi enter ap mode / Bt_init / Ap_Config_Udp_Thread Socket bound
I (...)  dl_queue: gpio_task_example: start up, Monitor_InterruptTask Timer_PlateErrHandle start
I (...)  control_thread: CTRL_THREAD_devModuleInit curr_plate_pos=1 set_plate_pos=1
I (...)  control_thread: DoorMotor_Reset_ThreadHandle DOOR_MOTOR_ZERO -> CLOSING -> CLOSE_OVER
I (...)  control_thread: Device_Module_Ctrl PLATE_READY_SET_POS ... (tray homing, see 5.3)
W (26199) control_thread: Control_ThreadHandle start...
I (...)  BSP_init: BSP_setExistState GPIO_EXIST_OFF
I (26923) dl_led: sitch to POWER_DC_MODE AP,open ap and ble
I (...)  feed_plan: Feeding_Monitor lastFeedState=0      (every ~1.2 s, then periodic Power_Handle, Read_*MotorAdc)
```
It ran stably for several guest minutes (v7: 240 s). It reboots only when the scenario pulls DC power and restores it (5.5).

## 5. Peripheral trace

Two sources:

- **MMIO trace** ([O]): every guest read and write to the peripheral blocks, from QEMU's generic `memory_region_ops_*`
  trace events. This works for unmodelled blocks too, because QEMU maps them as a catch-all "esp32c3.iomem"
  region. Reads from those blocks return 0, so the driver's read-modify-write splits each field into
  its own write: the decode shows what was written, not cumulative register state.
- **API hooks** (`gdb/hooks.py`, [O]): arguments of the ESP-IDF driver calls and of the app functions. They are more
  meaningful than raw registers. Function addresses come from `../static` names plus `scripts/xref.py`.

### 5.1 Boot (time-ordered, from `traces/run_d.pin_trace.txt` and the MMIO decode)
1. IDF startup: every IO_MUX pad gets its sleep config (SLP_SEL, isolate). This is generic IDF, not app logic.
2. `i2c_param_config(0)`: SDA=GPIO7, SCL=GPIO8, internal pull-ups, 100 kHz. MMIO: OUT_SEL GPIO7←I2CEXT0_SDA(54),
   GPIO8←I2CEXT0_SCL(53), open-drain, IN_SEL 53←GPIO8, 54←GPIO7.
3. GPIO9 output: low for 200 ms, then high (AW9523B RSTN).
4. AW9523B @0x58: read ID reg 0x10; reg 0x12/0x13=0xFF (all pins GPIO mode, not LED mode); reg 0x04=0x03 (P0_0,P0_1
   inputs, rest outputs); reg 0x05=0x00; GCR 0x11=0x10 (P0 push-pull); outputs 0. Read P0 input: low 2 bits = hardware version.
5. `Ptc_init` P1_4=1 → `Motor_init` P0_5=1, P0_6=1 → `Door_Motor_init` P1_7=1, P1_6=1 (both H-bridges in brake).
6. `Key_init`: GPIO18 input, any-edge. `AlarmLed_init`: P1_1=0, P1_0=0.
7. `AllDet_init`: GPIO10 input with pull-up, any-edge; GPIO19 any-edge; GPIO20 any-edge; P0_7=1, P0_4=1; GPIO0 any-edge;
   `ledc_timer_config` timer0 low-speed 8-bit **38000 Hz**; `ledc_channel_config` **GPIO2** ch0 (OUT_SEL GPIO2←LEDC_LS_OUT0);
   duty 127/256 (≈50 %).
8. `Adc_init`: ADC1 12-bit; ADC1 ch4 (GPIO4), ch1 (GPIO1), ch3 (GPIO3), ADC2 ch0 (GPIO5), all 11 dB; eFuse two-point calibration.
9. `BSP_aduioInit`: GPIO6 output, high.
10. `gpio_isr_handler_add` for GPIO18, 19, 10 (handler 0x403824ba = `xQueueSendFromISR(q@0x3fca1044, &gpio)`). GPIO0 and GPIO20 get no handler and are polled.
11. Lid homing: P1_6=0 (P1_7=1) drives "closing" until GPIO19=1, then brake. Tray homing: see 5.3.
12. ~25 s: `BSP_setExistState(OFF)` sets LEDC duty 0 (38 kHz carrier off); P1_4 goes 1→0 (cooling off; no plan, idle [I]).

### 5.2 Idle
P1_1 toggles every ~1.2 s (status LED blinking; unbound/AP mode [I]). ADC reads: GPIO4 ~2/s, GPIO5 and GPIO3
during the periodic `Feeding_Monitor`/`Power_Handle`, GPIO1 ×10 per `Read_Ntc_Temp`. GPIO0 is polled by `Power_Handle`.
AW9523B P0 input is polled every 20–500 ms (`GetHardwareVersion`). No UART1, SPI2 or RMT traffic, and no further
IO_MUX or matrix changes. GPIO21 (U0TXD) is untouched. GPIO20 (U0RXD pad) is reconfigured as a plain input, so the
console has no RX after init [O].

### 5.3 Tray (plate) with the simulated gate [O + model I]
Model, from the real device's NVS log ring (`io:10 state`, `Microtime=` records): GPIO10 rests at 0 and goes high
as a slot passes, ~160 ms per normal slot and ~510 ms for one wide "home" slot, with ~3.1 s between slots
(3 per turn). Polarity and timing are inferred from those records.
- Boot homing (`PLATE_READY_SET_POS`): P0_5=1, P0_6=0 (motor forward). Each falling edge calls `Plate_Micro_Handle`,
  which advances the position 1→2→3→1. `gpio_task_example` times every high pulse (`max_time_diff`, `check_cnt`).
  After ≥3 pulses, a pulse ≥ (max−150 ms) sets `curr_plate_is_cal=1` (home); at target, P0_6=1 brakes ("GPIO_MOTOR_BRAKE").
- Rotate request (what `Set_Plate_Position`/MQTT `platePosition` does: dev_ctrl 0x3fc9a2a0 +4 = target, +3 = 1):
  motor on, **stops at the first falling edge**, position 1→2. One request = one slot = 1/3 turn [O].
- Stall (ADC2 ch0 / GPIO5 raw 3000 ≈ high motor current): `MOTOR_STUCK` → brake → `PLATE_READY_BACK_POS`, reverse
  (P0_5=0, P0_6=1) ~1.8 s → brake → forward again → `plate motor restore` → stop at the next slot. P1_0 goes on during the fault.
- H-bridge on AW9523B: (P0_5,P0_6) = (1,0) forward, (0,1) reverse, (1,1) brake [O].

### 5.4 Lid ("door") [O + model I]
Model: GPIO19=1 when closed; it changes after 1.5 s of closing or 0.4 s of opening (timing invented).
`Control_Door` (MQTT `barnDoorState`: dev_ctrl +1 = 4 open / 1 close, +8 = 1): `DOOR_READY_OPEN` sets
P1_7=0, P1_6=1 (open). The motor kept running ~15 s until the close request, because nothing in the
model reports "fully open"; on the real device an open-end signal or timing (doorCheckSignalTime=5,
doorNotcheckSignalTime=10) presumably stops it [I]. `DOOR_READY_CLOSE` sets P1_7=1, P1_6=0 until GPIO19 rises → brake → `CLOSE_OVER`.

### 5.5 Other inputs [O]
- Key GPIO18: press (0) → ISR → `open Timer_KeyRestoreHandle`; release → `close Timer_KeyRestoreHandle` (long press = reset, per manual [I]).
- IR GPIO20 → 1 for 2 s in idle: no visible reaction. It is read only during a feed (`FEED_PLAN_getIrIdenState`, `MACHINE_INFRARED_EVENT irState`) [S].
- GPIO0 → 1: `POWER_BAT_MODE, close ap and ble`, P0_4=P0_7=0, `Suspend_AllTask`, then "wait USB insert" each second. Back to 0 → reboot.
  So GPIO0 low = DC adapter present [O].
- ADC1 ch1 (NTC) raw 1900 → 1200: accepted, and `Read_Ntc_Temp` logs only every 60th call. No cooling reaction seen in idle.
- Feed: `DL_MQTT_REC_feedNow` needs a plan (planId). Not exercised end-to-end; the tray and lid steps it uses are covered above.

### 5.6 Pin table

| Pin | Activity seen | Inferred function | Conf. |
|---|---|---|---|
| GPIO0 | input, no pull; polled; 0=DC mode, 1=battery mode (suspend, reboot on return) | DC-adapter detect, active low | high |
| GPIO1 | ADC1 ch1 11 dB, 10-sample average → `Read_Ntc_Temp` | food NTC thermistor | high |
| GPIO2 | LEDC ch0, timer0, 38 kHz, 8-bit, duty 127 then 0 (`BSP_setExistState`) | 38 kHz IR emitter carrier for presence ("exist") sensing | high (signal), medium (purpose) |
| GPIO3 | ADC1 ch3 → `Read_DoorMotorAdc` (mA), vs `doorStuckCurrent`=400 | lid motor current sense | high |
| GPIO4 | ADC1 ch4, ×2 after cal → `bat_adc` | battery/supply voltage divider | medium |
| GPIO5 | ADC2 ch0 → `Read_PlateMotorAdc`; high value → MOTOR_STUCK | tray motor current sense | high |
| GPIO6 | output, idle high; `FUN_4200d7e8` bit-bangs 8-bit LSB-first (0: 1 ms hi/3 ms lo, 1: 3/1) | one-wire sound IC command line (not played in runs) | high (code), not observed |
| GPIO7 / GPIO8 | I2C0 SDA / SCL, 100 kHz, internal pull-ups | AW9523B bus | high |
| GPIO9 | output; low 200 ms at boot then high | AW9523B RSTN (also boot strap) | high |
| GPIO10 | input pull-up, any-edge ISR; pulse timing | tray optical gate (CON6) | high |
| GPIO18 | input, any-edge ISR | button (SW1/reset key) | high |
| GPIO19 | input, any-edge ISR; 1 = lid closed | lid position switch/gate | medium |
| GPIO20 | input (U0RXD pad repurposed), polled at feed time | IR pet sensor output | medium |
| GPIO21 | untouched (U0TXD) | console TX | high |
| AW P0_0,P0_1 | inputs | hardware-version straps (0→5.0.0, 2→5.1.0, 1/3→5.2.0); this unit is 5.2.0 [I, from NVS Microtime records] | medium |
| AW P0_4, P0_7 | 1 after AllDet_init, 0 in battery mode | sensor power enables (IR / gate) | low |
| AW P0_5, P0_6 | H-bridge: 10 fwd, 01 rev, 11 brake | tray motor (J4) | high |
| AW P1_0 | on during stall/power fault | alarm/red LED | medium |
| AW P1_1 | toggles 1.2 s in idle | status LED | medium |
| AW P1_4 | 1 at init, cleared in idle | Peltier ("PTC") enable; fan possibly on the same line | medium (low for fan) |
| AW P1_6, P1_7 | 10 close, 01 open, 11 brake | lid motor | high |

No fan control line was seen separately; the fan may share P1_4 or be hard-wired [I]. Verify before driving the Peltier.

## 6. ESPHome notes [I]
The tray step is "run until the next falling edge on GPIO10". The firmware has no finer position; it times the slot
pulses only to find home. For 6 or 12 positions you need time-based stops between slots (≈3.1 s per third
in this model; measure the real value), or a finer encoder. Motor control is over I2C to the AW9523B (`aw9523` is not an
ESPHome built-in; use an external component or raw `i2c` writes to regs 0x02/0x03).

## 7. NVS (`firmware/stock/partitions/nvs.bin`, 32 pages, all CRCs valid)

| namespace | key | type | length (bytes) | value |
|---|---|---|---|---|
| nvs.net80211 | ap.sndchan | u8 | 1 | 1 |
| nvs.net80211 | ap.authmode | u8 | 1 | 0 (open AP) |
| nvs.net80211 | ap.chan | u8 | 1 | 1 |
| nvs.net80211 | ap.ssid | blob | 36 | redacted (provisioning AP, "PL-AF109-xxxx" pattern in firmware) |
| nvs.net80211 | ap.passwd | blob | 65 | redacted |
| nvs.net80211 | ap.hidden | u8 | 1 | 0 |
| nvs.net80211 | ap.max.conn | u8 | 1 | 1 |
| nvs.net80211 | bcn.interval | u16 | 2 | 100 |
| nvs.net80211 | ap.p_cipher | u8 | 1 | 0 |
| nvs.net80211 | ap.ftm_r | u8 | 1 | 0 |
| nvs.net80211 | ap.pmk_info | blob | 132 | redacted |
| phy | cal_data | blob | 1904 | RF calibration (not secret, not reproduced) |
| phy | cal_mac | blob | 6 | MAC (in docs/hardware.md) |
| phy | cal_version | u32 | 4 | 1110 |
| storage | mqtt_conn_info | blob | 150 | redacted: 3 strings: dev_sn (20 chars), product_key (16), **product_secret (32)** |
| storage | new_dev_attr | blob | 200 | u16 400, 400, 10, 450, 60, 1 then zeros (plate/door stuck current 400 mA, ...) |
| storage | dev_attr | blob | 72 | settings struct (LED/cool/sound schedules; app log decodes it, §4) |
| storage | rtc_info | blob | 4 | 0 |
| storage | dev_ctrl | blob | 64 | `00 00 03 00 01 01 01 ...`: state incl. plate position 1 |
| storage | bindIdInfo | blob | 4 | 3 |
| storage | log_rec_key | blob | 1504 | ring of 30 records `"sensor" / "io:10 state:N"` and `"Microtime=160..518"` + timestamp |

There are no `sta.*` keys: no Wi-Fi station credentials are stored, so the device has been reset or unprovisioned since binding.
There is no `sysInfo` key, which is why the log level is NONE. Secret-bearing values (AP password/PMK,
product_secret) are not written anywhere in this directory. The SN and product_key are also redacted in `traces/`.

## 8. Limits of the emulation
- Not modelled in QEMU 9.2.2-esp `esp32c3`: IO_MUX, LEDC, I2C, SAR ADC/APB_SARADC, RMT, SPI2, RTC IO, Wi-Fi/BT/PHY.
  Reads return 0 and writes are dropped (traced only). The GPIO block keeps no input state (GPIO_IN reads 0; writes are ignored).
  Modelled: UART0/1, SPI flash, systimer, TIMG0/1, intmatrix, eFuse (chip rev v0.3, not v0.4), crypto, GDMA, USB-Serial-JTAG.
- Everything an input "did" comes from my injections: AW9523B register values, ADC raw values, GPIO levels
  and ISR invocations (redirected into handler 0x403824ba from task context at `vTaskDelay`/`gpio_get_level` entry). The
  tray and lid plant models are guesses. The order and logic of the firmware's responses is real; the timings of the inputs are not.
- Interrupt-driven peripherals (I2C done ISR, ADC done) are short-circuited, so their timing is not real.
- `nowifi`/`noble` remove all network behaviour: MQTT, cloud commands and provisioning are not exercised.
- Time column in the hook log is FreeRTOS `xTickCount` (1 ms ticks). MMIO lines carry the timestamp of the preceding firmware log line.
- Reboots (watchdog, `esp_restart`) restart QEMU's guest. The driver resets its plant state when the tick counter jumps back.

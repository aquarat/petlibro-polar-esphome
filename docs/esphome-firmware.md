# ESPHome firmware v1 (re-implementation of stock AF109 2.0.38)

Status 2026-10-30: **compiled, never flashed, untested on hardware.** Everything below that says
"stock" comes from `analysis/static/README.md` (S), `analysis/emulation/README.md` (E) or
`analysis/android-app/README.md` (A). "OURS" marks a design choice with no stock equivalent.
The code cites the function address for every stock constant.

## Files

| Path | Contents |
|---|---|
| `esphome/petlibro-polar-v1.yaml` | Device config (`name: petlibro-polar`) |
| `esphome/components/aw9523b/` | AW9523B I/O expander hub + pin schema (like stock `pca9554`) |
| `esphome/components/polar_tray/` | Tray motor, gate, homing, stall recovery, sub-steps |
| `esphome/components/polar_lid/` | Lid motor, sensor windows, stall, pet reopen |
| `esphome/components/polar_feeder/` | Meal slots, run model, feed sequence, anti-pinch close |
| `esphome/components/polar_cooling/` | Peltier thermostat, duty cycle, safety limits |
| `esphome/components/polar_sound/` | GPIO6 sound chip: non-blocking one-wire transmitter, queue, volume, quiet hours, "call the pet" |

## Build (no flashing)

The repo YAML uses `external_components: path: components`. On <esphome-host> the components live in
`<esphome-config>/petlibro_components/` (the host's `components/` belongs to other projects), so the copy
rewrites that one line:

```sh
R=<repo>/esphome
ssh <esphome-host> 'mkdir -p <esphome-config>/petlibro_components'
tar -C $R/components --exclude=__pycache__ -cf - aw9523b polar_tray polar_lid polar_feeder polar_cooling polar_sound \
  | ssh <esphome-host> 'tar -C <esphome-config>/petlibro_components -xf -'
sed 's#^      path: components$#      path: petlibro_components#' $R/petlibro-polar-v1.yaml \
  | ssh <esphome-host> 'cat > <esphome-config>/petlibro-polar-v1.yaml'
ssh <esphome-host> 'docker exec esphome esphome compile /config/petlibro-polar-v1.yaml'
```

Result after the review fixes (ESPHome 2026.10.0, IDF 5.5.6): no compiler warnings; RAM 36.6 % (117,582 B);
image 1,069,110 B. The stock OTA slots are 0x1af000 (1,765,376 B), so it fits with ~40 % spare.
With `polar_sound` (2026-10-31, compiled only, not flashed): 0 compiler warnings, RAM 37.1 % (119,158 B),
image 1,077,246 B.
Remaining config warnings: GPIO18/19 are the USB-Serial-JTAG pins (stock uses them as GPIO too),
and the OTA password costs flash (kept, as in the bring-up build).

Both configs use `name: petlibro-polar`, so they share the build directory
`/config/.esphome/build/petlibro-polar`. Compiling one replaces the other's build output. The source
files are not touched.

## Design

### AW9523B hub (`aw9523b`)
- `reset_pin` GPIO9: low for `reset_duration` (200 ms), then high, then a 20 ms wait (stock
  AW9523B_i2cInit @0x4200d9c8). It runs without blocking (`set_timeout`), so pins written before the
  chip is ready go to a shadow register.
- Init: checks ID 0x10 == 0x23 (up to 5 tries, then `mark_failed`); writes 0x12/0x13 = 0xFF; writes the
  outputs (`initial_high_pins`, here P0_5, P0_6, P1_6, P1_7 = both motors braked); writes 0x11 = 0x10;
  then sets direction. **Outputs are written before direction**, so the brake pins never drive low.
  Stock writes the outputs (0) last.
- Direction: a pin is an input if it is listed in `input_pins` (P0_0, P0_1) or declared as an input
  through the pin schema. Every other pin is an output, as in stock (0x04 = 0x03, 0x05 = 0x00).
- Writes only touch the affected port register and are skipped when nothing changed. A failed write
  marks the port dirty; `loop()` retries a dirty port every iteration, and 5 consecutive failures trigger a
  recovery. The controllers also re-assert brake every 1 s while idle.
- **Readback (OURS; stock is write-only):** every `verify_interval` (1 s) the hub reads back 0x02/0x03
  (outputs), 0x04/0x05 (direction) and bit 4 of 0x11. A mismatch (for example the chip reset itself on a
  brownout) or 3 failed reads triggers a **recovery**:
  - The fail-safe pins are forced to their safe level (P0_5, P0_6, P1_6, P1_7 = 1, i.e. both motors
    braked; P1_4 = 0, Peltier off).
  - `on_recover` fires. The YAML faults the tray and the lid and runs stop all.
  - RSTN is pulsed and the chip is re-initialised.
  - More than 3 recoveries in 10 min: RSTN is held low and the hub is marked failed.
- **Shutdown:** `on_shutdown()` (reboot, OTA reboot) writes the fail-safe outputs. If that write fails,
  RSTN is held low (outputs high-Z; the H-bridge input pulls are unknown, so braked is preferred when the
  write works).
- **Limit:** a crash or watchdog reset skips `on_shutdown()`. GPIO9 then floats high (strap pull-up) and
  the expander keeps its last outputs until our setup pulses RSTN, about 0.3 to 0.5 s after reset.
  Stock behaves the same way.
- Pin schema `{aw9523b: aw, number: 0..15}` (8..15 = P1_0..P1_7) works with `switch`, `output` and
  `binary_sensor`. Inputs are polled (INTN wiring unknown). The v1 config declares no expander inputs;
  the HW revision is read once with `read_port(0)`.

### Tray (`polar_tray`)
- Motor P0_5/P0_6: 10 forward, 01 reverse, 11 brake. Direction changes always pass through brake.
  Before every move it switches on the `power_switches` (P0_4, P0_7): stock keeps them on at DC and, on
  battery, turns them on while a motor runs (CTRL_THREAD_batteryModeHandle @0x4200e4fc).
- Gate GPIO10 (pull-up, any-edge ISR into a 32-entry timestamp ring, processed in `loop()` with the
  high-frequency loop on while moving). A counted edge is a falling edge (HW 5.2.0). After each counted
  edge and after a move starts, the gate is ignored for 500 ms (Close_SW_Interrupt / MicroSw_Debounce).
  During that window rising edges are lost too, as in stock.
- Homing (HW 5.2.0 only; gpio_task_example @0x4201d47c): time each high pulse. Pulses under 1000 ms count
  while fewer than 4 have been seen. After 3 or more pulses, a pulse at least as wide as (widest - 150 ms)
  is the home mark = compartment 1. **Deviation:** stock reaches home only on a later edge (one more
  turn); we stop at that same edge straight away, so the tray ends in the same place one turn sooner.
  OURS: the widest mark must also be distinct, at least `home_width_ratio` (2.0) times the runner-up
  (the NVS log shows about 160 vs 510 ms). Otherwise homing faults "home mark not found" once 4 pulses
  are measured, or after 7 edges.
- OURS gate filter: a level change counts only after it has been stable for `gate_debounce` (10 ms).
  The 500 ms ignore window starts at the falling edge that ends a mark, so it does not overlap the 510 ms
  home mark itself (that review concern does not apply as stated). Chatter is filtered instead.
- OURS start window: if a sub-step stop left the next mark closer than 500 ms of running, the start
  ignore window shrinks to half the estimated time to that mark.
- Other HW revisions (5.0.0, 5.1.0) are not implemented: at boot the config reads P0[1:0] and inhibits
  both motors unless it reads 5.2.0.
- Stall (Feeding_Monitor @0x4200b8dc, FEED_PLAN_plateHandle @0x4200b2c8): every 500 ms while driving
  forward, ADC2 raw > 400 for 6 samples in a row means a stall. Recovery: brake 1 s, reverse 2 s, (OURS:
  brake 100 ms), forward again with the gate ignored for 2 s. After 3 retries: fault.
  OURS: the reverse can re-cross a mark, so after any recovery the position is no longer trusted. The move
  still finishes, then `homed` is cleared and the tray must be homed again. A homing move restarts its
  width measurement after a recovery.
- No-edge timeout 60 s (plateErrTimeout). OURS: restarted at every counted edge (stock stops the timer
  at the first one). Once the mark interval T is learned, the timeout is min(60 s, max(3T, 5 s)).
  OURS: hard cap of 90 s per move. Any fault brakes and clears `homed`. `on_shutdown()` brakes.
- Position: 1-based, `0`/unknown until homed. A jog, an interrupted homing or a fault clears it. It is
  not persisted across reboots.
- **Compartments 3 / 6 / 9 / 12** (`compartments`, also runtime select "Compartments"; a change
  clears homing). With k = N/3 (1 to 4) positions per mark segment, a target between marks is reached by
  running forward `j * T/k` ms after the preceding mark. T is the mark-to-mark time, learned (EMA) from
  consecutive edges in an uninterrupted forward run. **EXPERIMENTAL:** start-up acceleration, braking
  overrun and load all add error, and each mark re-syncs the position. If the timed phase runs into the
  next mark, the move stops at that mark with a warning. Sub-steps need homing and a learned T.

### ADC2 decision (tray motor current, GPIO5)
- Stock reads ADC2 ch0 raw (`adc2_get_raw`, legacy IDF 4.4 driver) and so has the same Wi-Fi arbiter
  issue described below.
- In IDF 5.5.6, `adc_oneshot_new_unit(ADC_UNIT_2)` on the ESP32-C3 returns `ESP_ERR_INVALID_ARG` unless
  `CONFIG_ADC_ONESHOT_FORCE_USE_ADC2_ON_C3=y` (`components/esp_adc/adc_oneshot.c:87`). The Kconfig help
  says "ADC2 Digital Controller is not stable" and refers to the chip errata. I did not check the errata
  text itself.
- On the C3 the ADC2 arbiter gives Wi-Fi priority. A sample preempted by Wi-Fi carries an invalid flag:
  `adc_oneshot_ll_raw_check_valid` gives -1 and `adc_oneshot_read` returns `ESP_ERR_TIMEOUT`
  (`hal/adc_oneshot_hal.c`).
  ESPHome's `adc` sensor would only log these as warnings, so it is not used for this pin.
- **Choice:** `polar_tray` sets that Kconfig option from its `__init__.py` and owns the ADC2 unit (12 dB,
  12-bit, raw counts, no calibration, as stock). A sample that is invalid 3 times in a row is skipped: it
  neither counts towards a stall nor resets the count.
- Invalid samples are reported, never silently dropped:
  - "Tray ADC2 invalid samples (last move)" (%) is published after each move.
  - After `stall_samples` consecutive invalid samples there is a warning ("stall detection is blind").
  - After `max_invalid_samples` (20, i.e. 10 s) consecutive invalid samples the move **faults**.
- Backstops that do not depend on ADC2: the 60 s no-edge timeout, the 90 s hard cap and the homing edge
  limit. Set `current_sensing: false` to drop ADC2 entirely if it misbehaves (tested in step 7).
- Unverified: how often samples are invalid with Wi-Fi active, and what 400 raw means in amps.

### Lid (`polar_lid`)
- Motor P1_7/P1_6: 10 close, 01 open, 11 brake. Sensor GPIO19 (binary sensor). Current GPIO3 (ESPHome
  `adc`, calibrated V, threshold 0.400 V = doorStuckCurrent 400 mV). `on_shutdown()` brakes.
- **Sensor polarity:** `sensor_end_level` (default `true` = high at an end stop, which the stock code
  implies; open question 8). Test step 4 determines it; set it before any feed test.
- **Stop rule (OURS; stock accepts the GPIO19 level from t = 0):** a move ends on the sensor only on a
  *transition* into the end level. The sensor must first have been seen away from the end level since
  the motion started, and only after `min_travel` (1 s) or the profile's ignore time. If the move started
  at a known end and the sensor never changed by the limit, the lid faults "lid sensor never changed".
  Otherwise it stops at the limit (stock "regardless").
- Profiles. Times are measured from the start of the motion; "run-on" means the motor keeps running for
  that long after the stop condition before braking.

  | Profile | Use | Ignore GPIO19 | Accept transition | Stop anyway at | Run-on | Source |
  |---|---|---|---|---|---|---|
  | feed | feed open/close | 1 s (stock 0) | < 5 s | 10 s | 1 s | Feed_Handle, dev_attr doorCheckSignalTime/doorNotcheckSignalTime |
  | reopen | safety reopen (pet or stall) | 2 s | < 5 s | 10 s | 1 s | OURS |
  | manual | test buttons | 2 s | < 20 s | 20 s | 1 s | Timer_DoorSignalHandle @0x4200de5a |
  | boot | boot close | 2 s | < 7 s | 7 s | 0 | DoorMotor_Reset_ThreadHandle @0x4200ec58 |
- **During the run-on** the pet check and the current check keep running. A pet while closing reopens
  the lid. Over-current during run-on is treated as the end stop (the sensor already confirmed it), so
  the lid brakes at once.
- **Anti-pinch while closing (OURS, departs from stock):**
  - Stock needs 3 s over 400 mV, then brakes 1 s, which would clamp for about 4 s.
  - We sample every `close_sample_interval` (100 ms) after `inrush_blanking` (300 ms). After
    `close_stall_samples` (2) samples over the threshold, the lid reverses **immediately** (no brake
    dwell) and reopens fully with the reopen profile. It reports a stall-reopen; the feeder decides when
    to retry.
- **Pet while closing** (stock "Identify the pet DOOR_READY_OPEN"): the lid reopens with the reopen
  profile.
- **Stall while opening** (stock rule): 6 x 500 ms over the threshold, brake 1 s, retry the open (up to
  3), then fault (stock code 0x7f6). The stock lid retry motion is unknown; not reversing is our choice,
  because reversing would close the lid towards a pet.
- Hard cap 60 s per move.

### Feeder (`polar_feeder`)
- **Meal slots:** 12 slots (time-of-day `datetime`, enable switch, per-meal lid-open override with
  -1 = global). In `rotate_and_open` only slots 1..N are active (N = compartments). In `lid_only` all
  12 are active.
- **Run model (OURS, replaces the stock dated plan list):**
  - "Refilled" makes compartment 1 the next one. In `rotate_and_open` it also homes the tray, which ends
    with compartment 1 under the opening.
  - Each meal or "Feed now" consumes the next compartment once the lid has opened.
  - Once `next` > "Filled compartments" (default 12, clamped to N), the run is **empty**: scheduled
    meals are skipped and reported, and nothing rotates until the next refill.
  - `next` is saved to flash.
- **The tray never moves unless the lid is known closed (OURS).** This applies to every tray move the
  feeder makes, including homing. If a feed needs a rotation and the lid is open or unknown, the feeder
  first runs an anti-pinch close, then rotates.
- **rotate_and_open:**
  - At meal time minus "Pre-position lead minutes" (default 30, as stock One_PlanCheck 1800 s), the
    tray moves the next compartment under the opening. This only happens when no feed is running, the
    lid is closed, the tray is idle, and the boot grace period is over. An unhomed tray is homed first.
  - At meal time the lid opens over that compartment. If the tray is not there, it closes the lid if
    needed and moves first.
- **Boot grace (OURS):** for 60 s after boot there is no pre-positioning, and meals due in that window
  are skipped and reported. The last started meal occurrence is saved to flash with `next`, so a reboot
  inside a meal minute does not start it twice. Together with the lid-closed rule, this means a reboot
  near a meal does not move the tray unless `home_on_boot` is set.
- **lid_only:** no rotation; the lid opens over whatever is there.
- **Lid-open time:** global "Lid open minutes" (default 240 = app closeDoorTime 4 h), or the per-meal
  override. 0 means the lid stays open until a close command.
- **Close and anti-pinch:**

  | Behaviour | Source |
  |---|---|
  | IR emitter on GPIO2, 38 kHz, duty 127/256, on from lid-open until the feed ends | stock BSP_setExistState @0x4200cfae |
  | Close only after GPIO20 has been clear for 10 s | stock (20 loops) |
  | A close never starts within 1 s (`ir_settle`) of the emitter switching on. A close from idle (Close lid, boot close) always goes through the 10 s clear wait | OURS |
  | P0_4/P0_7 stay on for the whole feed sequence (they may power the IR or gate sensors), also on battery | OURS |
  | A close stall reverses within ~0.5 s (see Lid) | OURS |
  | A pet seen while closing stops the lid and reopens it, then waits again | stock |
  | Never start a close while presence is detected; the stock irSensorIdenTimeout forced close is not reproduced (it would reopen anyway) | OURS |
  | Close stall: reopen, set "Lid anti-pinch tripped", retry after 30 s and 10 s clear; after 3 stalls the feed ends with the lid OPEN and an error | OURS |
  | `CAT_ANTI_PINCH` (app event, A section 6) | firmware trigger unknown; our anti-pinch binary sensor plays that role |
- **Seam for an external controller** (e.g. a load-cell scale): call the HA action `close_lid` or the
  YAML action `polar_feeder.close_now`. It ends the open period at once and still uses the anti-pinch
  rules. `open_lid` opens without rotating or consuming a compartment and stays open until close.
- **Boot close** (`polar_feeder.boot_close`, used when `home_on_boot` is true) goes through the same
  anti-pinch path with the boot sensor profile: IR and enables on, 10 s clear, pet check, stall reopen.
  The YAML homes the tray only if the lid ends up closed.
- **Battery:** the emulation (E 5.5) shows stock suspending all tasks when GPIO0 goes to 1. That run
  injected 0 V on the battery ADC, so it most likely took the low-battery path (< 3300 mV). The manual
  (A section 7) says feeding continues on battery and only refrigeration stops. OURS follows the manual:
  feeds run on battery, cooling is off, and the < 3.3 V lockout stops all motors.
- **Timers:** all `millis()` comparisons are unsigned elapsed-time differences. The close-retry hold uses
  a separate `hold_active_` flag, and the boot grace is latched, so the 49.7-day wrap is harmless.
- **Thaw latch:** if a meal is 1 h away and T ≤ 13.0 C, the latch is set until that feed starts
  (checkOnePlanBeforeOneHour @0x42008bca). Stock fires on the exact 1 h edge; we check once inside the
  window. Cooling obeys the latch only if "Cooling thaw rule (stock)" is on (default off).
- **Sounds:** see "Sound" below. The feeder only provides the hook: `is_serving()` is true while a meal or
  "Feed now" runs (not a plain "Open lid"), and the YAML starts the call from `on_lid_opened` and ends it
  from `on_feed_end`. The stock 0xEF at feed-job start is no longer sent.
- **Not reproduced:**
  - Stock parking at an empty plate, and the "advance every 30 min when all plates are full" rule. In
    the run model the compartment that was just eaten from stays under the opening until the next
    pre-position.
  - Missed meals after a reboot.
  - DST transitions (meal times use local wall-clock minutes).

### Cooling (`polar_cooling`) — UNTESTED, fan wiring UNKNOWN
- **Gates:** compile-time `cooling_allowed: "false"` (default) plus the runtime switch "Cooling enabled
  (UNTESTED)" (`ALWAYS_OFF` at boot).
- The P1_4 switch is `internal`: HA cannot pulse it and only sees a read-only "Peltier" binary sensor.
  The old YAML guard turned it off only after it was already on, so it is removed.
- Before turning the Peltier on, cooling switches P0_4/P0_7 on (stock holds them high on DC; the fan
  might depend on them). `assert_power_switches` / substitution `cooling_asserts_enables` can be set to
  false only for the fan-wiring test.
- `on_shutdown()` switches the Peltier off, and the hub's fail-safe pins also force P1_4 low.
- **OTA:** the ESPHome OTA receive loop blocks every other `loop()`, so no timeout runs during an upload.
  `ota: on_begin` runs stop all (both motors braked, feed cancelled, cooling disabled, Peltier off)
  before the upload starts.
- **Stock behaviour (CTRL_THREAD_coolHanlde @0x4200e6f6):** sample-and-hold decision every 300 s; on
  above 12.0 C; off at or below; never on battery; no hysteresis or fan logic. Stock leaves an unbound
  unit off. We have no cloud binding, so that rule does not apply.
- **Additions (user request):**
  - Setpoint number 2.0 to 15.0 C, step 0.5, default 12.0. Also clamped in code.
  - Hysteresis number, default 0.0 (= stock): on above setpoint + hysteresis, off at or below setpoint.
  - `interval` (default 5 min) and `min_off_time` (default 60 s; a turn-on waits for it).
  - "Cooling max continuous on" number, default 10 min, 0 = no limit. Reaching it switches cooling off
    and **disables** it until re-enabled.
  - Freeze guard: off below 1.0 C at any time (OURS).
  - Immediate off on battery, on a missing temperature, or when cooling is disabled.
  - "Peltier duty (1 h)" %, a rolling hour of one-minute buckets, published every 30 s.
  - "Peltier time since off" (min): the current on-run length while on, the off time while off.
- **Thaw rule:** optional, default off. It conflicts with low setpoints: at 2 to 13 C it would stop
  cooling an hour before every meal.
- NTC: ADC1 ch1, 10 samples, 10k pull-up to 3.3 V, B 3950, R25 10k, +0.5 C. The stock tempCoeff mV
  offset (NVS `temp_info`) is not applied.

### Other
- **LEDs (every 500 ms):** fault, battery, low-battery lockout or wrong HW revision gives P1_0 solid;
  Wi-Fi down gives P1_0 blinking 1 s/1 s; API not connected gives P1_1 blinking; API connected gives
  P1_1 solid. "LEDs enabled" turns both off. Stock maps network states to P1_1 patterns; the colours are
  unknown.
- **Button GPIO18** (active low, 50 ms debounce): a 3 s hold runs **stop all**. Stock uses it for a Wi-Fi
  reset, which would be unsafe and useless here. There is no short-press action (stock has none).
- **Power source and battery:** GPIO0 is the power source (1 = battery). GPIO4 x2 is the battery voltage;
  % uses the stock table (3300 mV = 0 %, 4500 mV = 100 %).
  - On battery with < 3.30 V for 3 s: stop all, and both motors are inhibited until DC returns. Stock
    uses 2.5 s, then suspends and reboots.
  - On battery and idle, P0_4/P0_7 are switched off.
- **Time:** Home Assistant time (feeder `time_id: ha_time`), plus SNTP so the clock keeps running
  without HA.

## Serve and return (`polar_feeder`, feed mode `serve_and_return`, 2026-11-01)

Strategy and reasons: docs/feeding-strategies.md. Storage compartments = every compartment except the rest
compartment (`serve: rest_compartment`, 1). The schedule ignores the 12 meal slots and pre-positioning; the
thaw rule never fires in this mode.

| Entity | Default | Meaning |
|---|---|---|
| Feed mode | | `serve_and_return` selects this mode |
| Serve schedule enabled | off (restored) | runs the interval schedule; "Feed now" serves at once regardless |
| Serve first meal | 08:00 | first serve of the day |
| Serve interval (min) | 120 | then every interval; the cycle restarts at the first-meal time each day |
| Serve compartment choice | alternate | alternate 2/3, or always the first / last storage compartment |
| Serve no-show close (min) | 30 | close if the IR beam never saw the pet |
| Serve close after pet gone (min) | 2 | close once the beam has been clear this long after the pet was seen |
| Serve max open (min) | 45 | close regardless (still waits for the beam to clear: anti-pinch) |

- IR presence is ignored for `serve: ir_settle` (3 s) after the emitter switches on (receiver reads "present"
  until it settles).
- The compartment to serve next is persisted (FeederPrefs field `serve_next`, the old `reserved`). "Refilled"
  resets it to the first storage compartment and moves the tray to the rest compartment.
- Events: `Serve N started: compartment C` / `Serve now`, `Serve: pet arrived after M min S s`,
  `Serve: closing (<why>) after ...` with why = pet gone / no show / max open time / close requested,
  `Serve done (<why>): compartment C back in the cold`. Stop all leaves the tray where it is (event says so).
- Bell: the existing on_lid_opened hook ("Call the pet at meals"); "Call stops when pet seen" ends it.
- Cooling switch: restore_mode RESTORE_DEFAULT_OFF (was ALWAYS_OFF) so cooling survives reboots.

**Homing fix (polar_tray, same day):** homing that started just before the home mark (tray at compartment 3)
saw the wide mark twice (502, 161, 154, 497 ms) and faulted "widths not distinct", because the second pass of
the same mark counted as the runner-up. The runner-up is now the widest mark more than 150 ms narrower than
the widest. Verified on hardware: from compartment 3 it homes on the 4th edge (498, 162, 153, 495 ms).

## Sound (`polar_sound`) — tested on hardware 2026-10-31 (S1-S10 pass, S11 skipped)

Sources: Ghidra decompile of the stock app (`analysis/static/work/out/decomp_all.c`, regenerate with
`analysis/static/scripts/run_ghidra.sh`), call-site list `analysis/static/scripts/io_callsites.sh`. **[V]** read
from code at the address given; **[I]** inference.

### What the stock firmware does

**Hardware path [V].** `BSP_aduioInit` @0x4200d738: `gpio_config` mask 0x40 (GPIO6), mode 2 (output only), no
pulls, then `gpio_set_level(6, 1)`. Nothing else touches GPIO6 except the send routine. There is no I2S, no
second LEDC channel (the only `ledc_channel_config` is the 38 kHz IR emitter in `AllDet_init`), no file system
and no audio buffer, so the only data the chip ever receives is one byte at a time.

**Send routine `FUN_ram_4200d7e8(code)` [V]:**
```
if FUN_ram_4200c276() == 0: return           # gate, below
GPIO6 = 0; vTaskDelay(5)                     # start: low 5 ms
for bit in 0..7:                             # LSB first
    if code & 1: GPIO6 = 1; vTaskDelay(3); GPIO6 = 0; vTaskDelay(1)   # "1": high 3, low 1
    else:        GPIO6 = 1; vTaskDelay(1); GPIO6 = 0; vTaskDelay(3)   # "0": high 1, low 3
    code >>= 1
GPIO6 = 1                                    # idle high; 37 ms per byte
```
At a 1 ms tick the first `vTaskDelay(5)` lasts 4 to 5 ms (it starts mid-tick); every later delay starts right
after a tick and lasts the full N ms **[I]**. This start pulse, the 3:1 / 1:3 high:low ratio and LSB-first
order are the WTN6-family one-wire format **[I]**: the chip is not identified on the board.

**Call sites: exactly four, no indirect calls [V]** (the address 0x4200d7e8 appears nowhere as data in
`ota_0.bin`):

| # | Caller | Byte | When |
|---|---|---|---|
| 1 | `Feed_Handle` @0x4200ae46 | 0xEF | a feed job is accepted (state 0/1/15 with a pending request): the scheduled plan pick-up 30 min ahead (`FUN_ram_4200ad9c`, One_PlanCheck window) or a cloud `WET_FOOD_FEED_NOW` (`FUN_ram_42008e2e` from `DL_MQTT_REC_feedNow`). Sent before thaw/rotation start |
| 2 | `FEED_PLAN_openDoorHandle` @0x4200945e | 0x01 | the lid reached open during a feed; then 1 s run-on, brake, state ONGOING (10). Also stamps the ringer clocks (feed struct +0x20 last ring, +0x28 open time, +0x30 pet seen = 0) |
| 3 | `FUN_ram_4200a23e` (ringer), called from `FEED_PLAN_feedOngoingHandle` @0x4200a356 | 0x01 | repeats during ONGOING (below) |
| 4 | `FUN_ram_42018c3a` (`DL_MQTT_REC_funcTest`) | 0x01 | MQTT `DEVICE_FUNCTION_TEST_SERVICE` with `action` = "AUDIO"; the app's "play meal call" button `/device/wetFeedingPlan/feedAudio` (`analysis/android-app/README.md`) |

**No sound** at boot, button press or 3 s hold, Wi-Fi reset / pairing, errors (stall, low battery,
plate error), lid close or feed end [V: no other call sites].

**Gate `FUN_ram_4200c276` [V]** (dev_attr @0x3fc9a144): sound only if `volume` (+0x15) != 0 and `soundSwitch`
(+0x39) == 1 and either `soundAgingType` (+0x3a) == 1 (always) or == 2 and the local time is inside
start (+0x3b h, +0x3c min) .. end (+0x3d, +0x3e), wrapping midnight. Stock's window is where sound is
**allowed**. Defaults from `Dev_Attr_Init` @0x4200bd2c: soundSwitch 1, soundAgingType 1, enableSound
(+0x38) 1. `FUN_ram_4200c360` mirrors the gate result into `enableSound` for the cloud.

**Volume [V/I].** Byte +0x15 is reported to the cloud as `volume` (`FUN_ram_4201413e`), but it is the same byte
`Dev_Attr_Init` uses as its layout magic ('2' = 0x32 = 50); the attribute parser @0x42016658 has no `volume`
key, and the value never reaches the chip. So stock has **no volume control** beyond mute-when-0. The 0xEF at
feed-job start is most likely WTN6 "volume 15 (max)" **[I]**: it is the only E-range byte and it always comes
well before the 0x01 calls.

**Ringer `FUN_ram_4200a23e` [V]** (skipped when the plan id is 666,666,666 = 0x27bc86aa, probably the app's
manual feed **[I]**):
```
ringerMode (+4): "NORMAL" = 0 (default), "SMART" = 1     # parser @0x42016658
if ringerMode == SMART and not pet_seen and now > open_time
   and (now - open_time) <= ringerDuration (+6, default 10 min) * 60 + 10 s
   and (now - last_ring) >= ringerInterval (+5, default 2 min) * 60:
       last_ring = now; send(0x01)
if ringerMode == SMART and GPIO20 == 1: pet_seen = 1     # pet in front: no more rings this feed
```
With the defaults SMART rings at 0, 2, 4, 6, 8 and 10 min (6 calls) unless a pet shows up; NORMAL gives the
single call at lid open. The ringer only runs inside `FEED_PLAN_feedOngoingHandle`, so it stops when the
close starts.

**BUSY pin: none [V].** GPIO6 is output-only, and every `gpio_get_level` (`FUN_ram_42045570`) call reads
GPIO0, GPIO19 (0x13), GPIO20 (0x14) or the GPIO10/GPIO18 pin from the interrupt queue
(`gpio_task_example` @0x4201d47c). Stock never waits for the chip and never knows if a clip is playing.

**Recorded voice: not on this device.** The firmware parses per-plan `enableAudio` and `audioTimes` into plan
bytes +0x13/+0x14 (`FUN_ram_42017188`, `FUN_ram_42017a90`) and never reads them again [V]; there is no audio
download, storage or playback path [V]. The app contains a generic recording feature (flutter_sound,
`/device/deviceAudio/add|all|use|remove`, `/device/device/playVoice`) used by other feeders; its
`RecordingController` has `isAF109` branches (0x1bde99c, 0x1bdfb14) whose effect I did not resolve [I]. Whatever
the app shows, the AF109 can only play what the chip already holds.

### Sound index and control byte table

| Byte | Stock meaning | Evidence |
|---|---|---|
| 0x01 | **Meal call**: lid open, SMART ringer, app "play" button | 4 call sites above [V] |
| 0x00 | plays the same bell as 0x01 (user: "sounds the same") | heard, S6 2026-10-31 |
| 0x02..0x10, 0x20, 0x40, 0x80 | silent: the chip holds no other clips at these indices | S6 2026-10-31 |
| 0xEF | sent once per feed job, before thaw/rotation | [V] call; **volume 15 confirmed** (S7) |
| 0xE0..0xEE | not used by stock | WTN6 volume levels 0..14 [I] |
| 0xF2 | not used by stock | WTN6 loop current voice [I] |
| 0xFE | not used by stock | WTN6 stop [I] |

### Implementation (OURS)

- **Transmitter:** a chain of `esp_timer` one-shots (dispatch in the esp_timer task, priority above the
  ESPHome loop task) clocks out the 17 phases of a byte with the stock timing (5 / 3 / 1 ms). `loop()` never
  blocks; it only starts the next queued byte once the line has been idle for `command_gap` (20 ms, OURS;
  stock never sends two bytes close together). Each phase is armed relative to the previous edge, so a late
  callback stretches only that phase (expected jitter well under 0.1 ms; the tolerance of the chip is
  unknown, so the stock timing is kept exactly). After each byte the log shows `Sent 0xNN in 37.0 ms`, and a
  warning if the frame took more than 2 ms longer or shorter than nominal.
- **Queue:** 16 bytes; full = dropped with a warning.
- **Commands:** `play(index 0..0xDF)` queues the volume byte (0xE0 + "Sound volume") and then the index when
  `volume_before_play` is true; `set_volume(level)` queues 0xE0 + level at once and updates the number;
  `stop()` clears the queue, ends a call, and queues 0xFE; `loop` queues 0xF2; `send(code)` queues any byte.
  0xE0+ bytes are not gated (silent on a WTN6 [I]); voice bytes are.
- **Gate:** "Sound enabled (UNTESTED)" (default off) blocks every voice byte, like stock `soundSwitch`. "Sound
  quiet hours" with start/end times (default 21:00 to 07:00, wraps midnight, off by default) blocks automatic
  sounds. This is the inverse of stock's allowed window. Manual plays from HA (Play sound select, the meal call
  button, `play_sound` with `force: true`) ignore quiet hours. With no valid clock, quiet hours do not apply.
- **Call the pet** (stock lid-open call + SMART ringer):
  - Started from `on_lid_opened` only when `feeder.is_serving()` (meal or Feed now) **and** "Call the pet at
    meals" is on (default **off**).
  - Plays "Call sound" (default 0x01), then "Call repeats" more times, "Call repeat interval" apart (defaults
    5 and 2 min = stock ringerDuration 10 / ringerInterval 2). 0 repeats = stock NORMAL. Interval floor 15 s.
  - "Call stops when pet seen" (default on, stock SMART): GPIO20 high ends the call, ignored for the first
    `pet_settle` 2 s (OURS: our IR receiver reads "present" until the emitter, switched on at lid open, settles).
    The pet check applies only to meal calls; outside a feed the emitter is off and the sensor reads true.
  - Ends at feed end (`on_feed_end`), on Stop all, Sound stop or `polar_sound.call_stop`. Quiet hours are
    checked at the start and before every repeat.
  - Manual: button "Sound: call the pet now" or HA `call_pet` (no meal switch needed, quiet hours apply).
- **Deviation:** the stock 0xEF at feed-job start is not sent (it would override the volume setting if it is
  the WTN6 volume-max byte). Test S7 checks what 0xEF does.
- `on_shutdown()` stops the timer and leaves GPIO6 high.

### Sound test results (2026-10-31 23:36-23:49 UTC, user listening at the feeder)

Build 7efef43 flashed by OTA at 23:36. Bytes sent with the API (`play_sound`, buttons, numbers); every frame
logged 37.1-38.0 ms (nominal 37). Clip: a short "meal bell", two dings ("ting ting"), well under 2 s.

| Test | Result |
|---|---|
| S1 boot | pass: status idle, no sound at boot or on the OTA reboot |
| S2 0xE2 alone | pass: silent |
| S3 0x01 at volume 2 | pass: bell heard, very faint (too quiet for the cat) |
| S4 volume 0/4/8/12/15 | pass: 0 silent, each step distinctly louder, 15 loudest. **0xE0+n = volume n confirmed** |
| S5 0xFE during clip | pass: clip cut off |
| S6 clip search | 0x00 and 0x01 = bell; 0x02-0x10, 0x20, 0x40, 0x80 silent |
| S7 0xEF | pass: 0xEF alone silent, then a raw 0x01 loud. Volume persists in the chip between commands; **stock plays every meal call at volume 15** |
| S8 0xF2 | pass: bell repeats until 0xFE |
| S9 quiet hours | pass in the log (`call skipped: quiet hours`, button still plays) |
| S10 manual call | pass in the log: 3 bells 30 s apart, `call finished (3 sounds)` |
| S11 meal call | skipped by the user (call engine already shown by S10; pet-seen stop to be exercised by the feeding programme) |

Conclusion: the chip is a WTN6-style one-wire player with one bell clip (indices 0 and 1). Volume and stop
work as the WTN6 datasheet describes. Default "Sound volume" 4 is quieter than stock (15); use 12-15 for the cat.

### Entities (sound)

| Entity | Type | Default | Notes |
|---|---|---|---|
| Sound enabled (UNTESTED) | switch | off | master gate |
| Sound volume | number 0..15 | 4 | sent before every play; 4 is OURS for the first test |
| Play sound | select 0x00..0x0F | shows 0x01 | choosing an option plays it (force) |
| Sound: play meal call (0x01) | button | | force |
| Sound stop | button | | 0xFE, clears queue and call |
| Sound status | text sensor | | last command / block reason / call progress |
| Call the pet at meals | switch | off | feeding hook |
| Call sound | select | 0x01 meal call (stock) | restored |
| Call repeats | number 0..30 | 5 | |
| Call repeat interval | number 0.5..60 min | 2 | |
| Call stops when pet seen | switch | on | |
| Sound quiet hours, start, end | switch, 2 times | off, 21:00, 07:00 | |
| Test: sound send volume now / send 0xEF / loop (0xF2) | buttons | | diagnostics |

HA actions: `play_sound(index, force)`, `sound_volume(level)`, `sound_stop`, `sound_send(code)`, `call_pet`.
To name more clips after test S6, add them under `sounds:` in the YAML (and raise `listed_indices` if needed).

### Sound test plan (user listening at the feeder; operator flashes, not before the cooling test ends)

Start with "Call the pet at meals" off, quiet hours off, nobody's ears near the speaker.

- **S1 Boot.** Log shows the `polar_sound` config and `Sound status: idle`; no sound at boot.
- **S2 Volume first, low.** "Sound enabled" on, "Sound volume" 2, press "Test: sound send volume now".
  Expect the log line `Sent 0xE2 in 37.0 ms` and **silence**. If a sound plays, the 0xE0 range is not volume
  on this chip: note it, and rebuild with `volume_before_play: false` before going on.
- **S3 First play.** Press "Sound: play meal call (0x01)": expect `Sent 0xE2`, `Sent 0x01`, and a quiet meal
  call. Note the clip and its length. If silent: volume 6, then 10; if still silent, scope GPIO6 (expect a
  5 ms low, then 3:1 / 1:3 pulses) before changing any timing.
- **S4 Volume steps.** Play 0x01 at 0, 4, 8, 12 and 15; note the loudness (is 0 mute?) and pick a level.
- **S5 Stop.** Play 0x01 and press "Sound stop" during the clip: does it cut off? (tests 0xFE).
- **S6 Find the other clips.** At low volume step "Play sound" through 0x00 and 0x02..0x0F, then a few
  higher indices with `play_sound` (e.g. 0x10, 0x20, 0x40); note sound or silence per index.
- **S7 What is 0xEF?** Volume 2, play 0x01 (quiet). Then HA `sound_send` code 1 (raw, no volume prefix):
  still quiet. Then "Test: sound send 0xEF" (expect silence) and `sound_send` code 1 again: **loud** means 0xEF
  is volume max. Warn the listener first.
- **S8 Loop.** Play 0x01, then "Test: sound loop current (0xF2)": does the clip repeat? "Sound stop" ends it.
- **S9 Quiet hours.** Turn quiet hours on with a window containing now: "Sound: call the pet now" gives
  `call skipped: quiet hours`; the meal call button still plays. Turn quiet hours off.
- **S10 Manual call.** Call repeats 2, interval 0.5: "Sound: call the pet now" plays 3 times 30 s apart; the
  status shows `call repeat 1/2`, `2/2`, `call finished (3 sounds)`.
- **S11 Meal call.** "Call the pet at meals" on, repeats 2, interval 0.5, lid-open 3 min. "Feed now": the call
  plays at lid open. Put a hand in front of the IR window after the first call: `call ended: pet seen`. Repeat
  without the hand: repeats run until the lid closes (`call ended: stopped`). "Open lid" must not call.

## Configuration options

Substitutions:

| Substitution | Default | Meaning |
|---|---|---|
| `home_on_boot` | `"false"` | Stock is true: close the lid (boot profile), then home the tray |
| `enables_on_boot` | `"false"` | Stock is true on DC: P0_4/P0_7 on at boot |
| `cooling_allowed` | `"false"` | Compile-time gate for the Peltier |
| `cooling_asserts_enables` | `"true"` | Cooling switches P0_4/P0_7 on before P1_4; set `"false"` only for the fan test |
| `compartments_default` | `"3"` | Initial compartment count |

Component options (defaults = stock where cited):

| Component | Options |
|---|---|
| `aw9523b` | `reset_pin`, `reset_duration` 200 ms, `initial_high_pins`, `input_pins`, `port0_push_pull`, `fail_safe_pins`, `verify_interval` 1 s, `on_recover` |
| `polar_tray` | `compartments`, `current_sensing`, `stall_threshold` 400, `stall_samples` 6, `stall_retries` 3, `edge_timeout` 60 s, `max_move_time` 90 s, `gate_ignore` 500 ms, `gate_debounce` 10 ms, `home_max_edges` 7, `home_width_ratio` 2.0, `max_invalid_samples` 20 |
| `polar_lid` | `sensor_end_level` true, `min_travel` 1 s, `stall_threshold` 0.4 V, `stall_samples` 6, `stall_retries` 3, `close_stall_samples` 2, `close_sample_interval` 100 ms, `inrush_blanking` 300 ms, `check_signal_time` 5 s, `nocheck_signal_time` 10 s, `run_on` 1 s, `max_move_time` 60 s |
| `polar_feeder` | `default_open_minutes` 240, `default_lead_minutes` 30, `thaw_threshold` 13.0, `thaw_lead` 60 min, `pet_leave_time` 10 s, `stall_retry_delay` 30 s, `max_close_stalls` 3, `ir_level` 127/256, `ir_settle` 1 s, `boot_grace` 60 s, `power_switches` |
| `polar_cooling` | `allowed`, `interval` 5 min, `min_off_time` 60 s, `freeze_guard` 1.0 C, `power_switches`, `assert_power_switches` |
| `polar_sound` | `pin`, `start_low` 5 ms, `bit_long` 3 ms, `bit_short` 1 ms, `command_gap` 20 ms, `volume_before_play` true, `time_id`, `enable`, `volume`, `quiet_enable`/`quiet_start`/`quiet_end`, `pet_sensor`, `pet_settle` 2 s, `call_enable`, `call_repeats`, `call_interval`, `call_stop_on_pet`, `call_default` 1, `sounds` (index: name), `listed_indices` 16, `play_select`, `call_sound`, `status` |

## Entities and services

**Controls:**
- Selects: Compartments, Feed mode, Play sound, Call sound.
- Numbers: Lid open minutes, Pre-position lead minutes, Filled compartments, Cooling setpoint, Cooling
  hysteresis, Cooling max continuous on, Meal 1..12 lid open minutes, Sound volume, Call repeats, Call
  repeat interval.
- Datetime (time): Meal 1..12 time, Sound quiet hours start/end.
- Switches: Meal 1..12 enabled, Cooling enabled (UNTESTED), Cooling thaw rule (stock), LEDs enabled,
  Sound enabled (UNTESTED), Sound quiet hours, Call the pet at meals, Call stops when pet seen, Power
  enable P0_4/P0_7.
- Buttons: Stop all, Feed now, Open lid, Close lid, Refilled, Tray home, Tray step, Clear faults,
  Restart, Sound: play meal call (0x01), Sound: call the pet now, Sound stop, and test buttons for tray
  jog, lid jog open/close, lid manual open/close without pet check, sound send volume now, sound send
  0xEF, sound loop (0xF2).

**Status:**
- Tray: position, target, mark interval, mark width, motor current (ADC2 raw), ADC2 invalid samples (last
  move), state, homing, fault, moving, homed.
- Lid: state, fault, closed, motor current.
- Feeder: state, last event, next meal, next compartment, run empty, feeding, thaw latch,
  lid anti-pinch tripped.
- Cooling: state, Peltier (read-only), Peltier duty (1 h), Peltier time since off.
- Inputs: food temperature, battery voltage/level, on battery, tray gate, lid sensor, pet present (IR),
  button.
- Sound: status (last command, block reason, call progress).
- Diagnostics: hardware revision, reset reason, Wi-Fi, uptime, ESP temperature.

**HA actions:** `stop_all`, `tray_home`, `tray_step`, `tray_go_to(compartment)`, `feed_now(minutes:
-1 global / 0 until close)`, `open_lid`, `close_lid`, `refilled`, `set_meal(slot, hour, minute, enabled)`,
`play_sound(index, force)`, `sound_volume(level)`, `sound_stop`, `sound_send(code)`, `call_pet`.
**YAML actions:** `polar_tray.home/step/go_to/jog/stop/clear_fault`,
`polar_lid.open/close/jog/stop/clear_fault`, `polar_feeder.feed_now/open_lid/close_now/refilled/boot_close/cancel`,
`polar_sound.play{index, force}/send{code, force}/set_volume{level}/stop/loop/call_start{meal}/call_stop`;
condition `polar_sound.is_calling`.
**Triggers:** tray `on_arrived`/`on_fault`, lid `on_opened`/`on_closed`/`on_pet_reopen`/`on_fault`,
feeder `on_prepare`/`on_feed_start`/`on_lid_opened`/`on_feed_end(compartment, result)`.

## Review fixes (2026-10-30, after a77202e)

| # | Finding | Resolution |
|---|---|---|
| 1 | No OTA/shutdown safety | `ota: on_begin` → stop all; `on_shutdown()` in hub (fail-safe outputs, RSTN low if the write fails), tray, lid, cooling. A crash reset skips it (documented limit) |
| 2 | Expander write-only | Readback verify every 1 s, write retries every loop, recovery with RSTN pulse + re-init, motors faulted via `on_recover` |
| 3 | Boot close without pet check; home regardless | Boot close goes through the feeder anti-pinch path; the tray homes only if the lid is closed |
| 4 | Run-on skipped pet and current checks | Both checks run during run-on |
| 5 | ~4 s clamp on a close stall | 100 ms sampling, 2 samples, immediate reversal (departs from stock) |
| 6 | Feed profile stopped on the sensor level at t = 0 | Transition-based stop, minimum travel, never-changed fault, reopen ignores the sensor 2 s, `sensor_end_level` option, test step 4 |
| 7 | `hold_until_ms_` wrap | `hold_active_` flag; boot grace latched; other comparisons audited (unsigned elapsed differences) |
| 8 | Tray moved with the lid not closed | Every feeder tray move requires `is_closed()`; close-first before a rotation |
| 9 | IDLE close, enables off on battery during a feed, IR settle | IDLE close goes through WAIT_CLEAR; 1 s IR settle; enables held during a feed; battery decision documented; GPIO20 no pull = stock |
| 10 | Cooling ignored P0_4/P0_7; Peltier pulse from HA | Cooling asserts the enables; P1_4 internal with a read-only binary sensor; fan test after a reboot |
| 11 | Prepare homed the tray after boot; refire after a reboot | 60 s boot grace plus the lid-closed rule; last-fed occurrence persisted |
| 12 | ADC2 silently blind | Invalid-% sensor, warning, fault after 20 consecutive invalid samples, adaptive edge timeout 3T |
| 13 | Home mark not required to be distinct | Widest ≥ 2.0 × runner-up, else fault |
| 14 | Position after a stall; double counts; start window | Position cleared after a recovery; 10 ms level-stable filter; start window from the position. Note: the 500 ms window starts at the falling edge after a mark, so it cannot split the 510 ms home mark |
| 15 | tray_gate interrupt | Already handled: ESPHome forces polling on shared pins (`allow_other_uses`; INFO in the config log). Made explicit with `use_interrupt: false` |

## Verified vs untested

Updated 2026-11-01 from docs/findings-log.md (HW rev 5.2.0, 3-compartment tray, mains power).

| Item | Status |
|---|---|
| AW9523B at 0x58, ID 0x23, stock init with brakes, HW rev 5.2.0; NTC formula; GPIO0 = DC | Verified (bring-up) |
| Fan switched with the Peltier by P1_4; Peltier cooling (4 h and 8 h runs) | Verified (fan wiring test, cooling tests; docs/cooling.md) |
| Tray: motor direction, gate mark widths (~160/155/505 ms), homing incl. from compartment 3 (fix 2026-11-01), stepping 1->2->3->1, compartment alignment with the lid opening, ADC2 current with Wi-Fi on | Verified |
| Lid: open/close by sensor, already-closed detection after a reboot, anti-pinch (beam blocked before close, object during close) | Verified |
| IR pet sensor (break-beam, valid only with the emitter on) | Verified |
| Sound chip: protocol, volume 0-15, stop, loop, clips 0x00/0x01; quiet hours and manual call (log) | Verified (docs: "Sound test results") |
| serve_and_return: no-show path end to end | Verified 2026-11-01; pet path and bell in a real serve: pending |
| Battery operation, LEDs, P0_4/P0_7 function, 6/12-compartment trays (timed sub-steps), thaw rule, meal call during a feed (S11) | Untested |
| Expander readback/recovery, OTA stop-all, shutdown hooks | Implemented, untested |

## First-flash test plan (user present; operator flashes, not this document's author)

Before starting:
- Feeder on the bench on DC, backup battery out, tray and lid visible, hands clear of the mechanism.
- Serial logger on the ISP header (115200) plus `esphome logs` over the API.
- Keep a finger near "Stop all" (or the 3 s button hold).
- Flash with the defaults: `home_on_boot` false, `cooling_allowed` false, compartments 3.

1. **Boot and expander.** Expect:
   - Log `aw9523b: Initialised: ID 0x23, outputs 0xC060`, then **no** `Readback mismatch` lines for a
     few minutes (the readback verify runs every 1 s).
   - "Hardware revision" reads `5.2.0 (ID 0x23, P0 0x63)`.
   - Nothing moves. LEDs follow the network state.

   If mismatches appear, note which register differs. Set `verify_interval` very large only after
   understanding why.

   Then check the inputs: turn the tray by hand and watch "Tray gate (GPIO10)" (expect high while a
   mark passes; note which mark is wide). Cover the IR window and watch "Pet present". Press the button.
2. **LEDs.** Disconnect Wi-Fi or stop HA and watch the blink patterns. Toggle "LEDs enabled".
3. **Fan wiring, right after a reboot** (before any motor or feed has run; P0_4/P0_7 are off at boot).
   Note the fan state:
   - (a) idle;
   - (b) "Power enable P0_4" on, then off;
   - (c) "Power enable P0_7" on, then off;
   - (d) both on.
   Also note anything else that starts (IR, gate LED), and check that the gate input still changes in
   each state. Do this before any motor move, because a move switches the enables on and would mask the
   result. P1_4 is tested in step 9.
4. **Lid sensor polarity (sets `sensor_end_level`).**
   - Use "Test: lid jog open 0.3 s" / "close 0.3 s" repeatedly, so the lid moves between its ends in
     small steps. Record GPIO19 at the closed end, in between, and at the open end.
   - High at both ends and low in between: keep `true`.
   - Low at both ends and high in between: set `false` and rebuild.
   - High only at the closed end (the emulation model): opens end on the 10 s limit, not on the sensor.
     Record the time a full open takes.
   - Then "Test: lid open (manual profile)" and "close": confirm it stops on the sensor transition plus
     1 s run-on, and read "Lid motor current" during the move and at the end stop.
5. **Tray jog.** "Test: tray jog forward 0.5 s" turns the tray a little in the stock direction. Check
   that the gate edges appear.
6. **Homing.** "Tray home" (the lid must be closed for feeder moves; the test button bypasses this).
   Expect:
   - About 1 to 2 turns, then `Home mark found: width ~510 ms (widest ~510, runner-up ~160)`.
   - "Tray homed" on, position 1, mark interval about 3 s.
   - Check that compartment 1 is under the lid opening.
   - If it faults "widths not distinct", record "Tray mark width" for each mark and adjust
     `home_width_ratio`.
7. **Stepping and ADC2.** Press "Tray step" 3 times: positions 2, 3, 1.
   - Read "Tray motor current (ADC2 raw)" and "Tray ADC2 invalid samples (last move)" after each step.
   - If invalid samples are frequent with Wi-Fi up, note the percentage. A move faults after 10 s
     without a valid sample.
   - Stall test: let the tray push gently against a soft obstacle (a sponge), never a finger. Expect,
     after 3 s over 400, brake, reverse and retry, and the tray reports "not homed" after the move. After
     3 retries: fault.
   - If the raw current never exceeds 400 when blocked, set `current_sensing: false` and rely on the
     timeouts.
8. **Expander recovery (log check only).** Do **not** short GPIO9: the ESP drives it high. There is no
   safe way to force an expander reset on the bench, so only confirm in the step 1 log that the verify
   runs without mismatches. The recovery path (fail-safe outputs, both motors faulted, cooling disabled,
   re-init) is untested.
9. **Cooling fan check** (needs a build with `cooling_allowed: "true"`; first with
   `cooling_asserts_enables: "false"`).
   - Reboot, so the enables are off and no motor has moved.
   - Set "Cooling max continuous on" to 2 and the setpoint below room temperature.
   - Turn on "Cooling enabled": P1_4 alone. Watch the Peltier and the **fan**.
   - Rebuild with `cooling_asserts_enables: "true"` and repeat: P1_4 plus P0_4/P0_7.
   - If the fan does not run within seconds, press Stop all and do not use cooling until the fan wiring
     is found.
   - Check that it switches off on battery (unplug DC with the battery fitted) and at the max-on limit.
10. **Cooldown test** (only after step 9 passes).
    - Set the setpoint (e.g. 4 C), max-on 0 or a high value, and hysteresis 0.5.
    - Starting from room temperature, the `cooldown` log line every 30 s gives T, setpoint, Peltier and
      1 h duty. The observer notes the fan state alongside.
    - Continue until the setpoint is reached or the duty stays at 100 % with T flat (the setpoint is
      not reachable).
    - Also note "Peltier time since off".
11. **Feeding, `rotate_and_open`.**
    - Set compartments 3, Feed mode `rotate_and_open`, lid open 1 min, lead 2 min.
    - With the lid closed, press "Refilled": the tray homes and next = 1.
    - Enable meal 1 three minutes ahead. Expect the tray to move at T-2 (lid closed), the lid to open at
      T, and the lid to close 1 min later after 10 s with GPIO20 clear. Next becomes 2.
    - Open the lid with "Open lid", then "Feed now": expect an anti-pinch close first, then the rotation
      to compartment 2, then the open.
    - Repeat until "Run empty"; the next meal is then skipped.
12. **Anti-pinch.**
    - During a close, put a hand (not a finger) in front of the IR sensor: the lid should reopen (also
      during the 1 s run-on) and wait for 10 s clear.
    - With a soft object blocking the lid: the lid should reverse within about 0.5 s (not 4 s), reopen
      fully, set "Lid anti-pinch tripped" and retry after 30 s. After 3 stalls the lid stays open with an
      error.
    - Check that "Close lid" defers while presence is detected, and that it always waits for 10 s clear
      when no feed is running.
13. **Feeding, `lid_only`.**
    - Several meal slots a few minutes apart, and "Open lid" / "Close lid".
    - Check that lid-open minutes 0 stays open until "Close lid".
14. **Sub-steps (experimental).** Set compartments 6 (then 9, 12), home, and step through all positions;
    measure how far each stop is from where it should be. Note the error before printing a tray.
15. **Battery.** With the battery fitted, unplug DC:
    - "On battery" on, cooling off.
    - P0_4/P0_7 off while idle but on during a feed; feeds still run.
    - Low battery (< 3.3 V), if it can be simulated: motors inhibited until DC returns.
16. **OTA safety.** Start a feed (lid open) or a tray move, then start an OTA from the ESPHome container.
    Expect "STOP ALL" in the log before the upload, motors braked and cooling off. Cancel the OTA if
    possible and check that the outputs stay safe.
17. **home_on_boot.** Rebuild with `home_on_boot: "true"` and reboot: anti-pinch boot close (10 s clear
    wait first), then the tray homes only if the lid closed.

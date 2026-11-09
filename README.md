# Petlibro Polar (PLAF109) – ESPHome firmware

Local, cloud-free replacement firmware for the **Petlibro Polar wet-food feeder (PLAF109)**, written as
ESPHome external components, plus the reverse-engineering notes it is built on.

Tested on one unit: board "PLAF109 MainBoard V1.0 2025.07.22", ESP32-C3-WROOM-02U, 4 MB flash, hardware
revision 5.2.0, stock app AF109 2.0.38, 3-compartment tray, mains power.

> Not affiliated with or endorsed by Petlibro. "Petlibro" and "Polar" are their trademarks. Flashing this
> firmware replaces the stock app, ends cloud/app control and may void your warranty. It moves motors near
> your pet and controls refrigeration of food: read the safety notes and test before relying on it.

## What it does
- **Home Assistant native** (ESPHome API): every setting is an entity; no cloud account.
- **Lid** with anti-pinch: IR break-beam check before and during every close, stall detection and reopen.
- **Tray** stepping and homing from the position gate (3 marks, home mark ~3x wider).
- **Cooling**: Peltier + fan with setpoint, hysteresis, freeze guard and an optional max-on limit.
- **Meal call**: the on-board sound chip (one bell clip, volume 0-15), "call the pet" with repeats that stop
  when the pet is seen, quiet hours.
- **Feed modes**
  - `rotate_and_open`: like stock, one compartment per meal on a 12-slot schedule.
  - `lid_only`: open over whatever is under the lid.
  - `serve_and_return`: keep food in the cooled rear compartments; on an interval bring one to the front,
    ring, wait for the pet, close a set time after the pet leaves (or on no-show / max time), and return
    the food to the cold. See `docs/feeding-strategies.md`.
  - `serve_on_demand`: the same cycle with no schedule, started when the pet breaks the IR beam. The lid stays
    open for at least a set time, then closes once the pet has gone. No bell, as the pet is already there.

What is verified on hardware and what is not: `docs/esphome-firmware.md`, "Verified vs untested".

## Layout
| Path | Contents |
|---|---|
| `esphome/` | `petlibro-polar-v1.yaml` (the firmware), `petlibro-polar.yaml` (first bring-up), `components/` |
| `docs/` | Hardware and pin map, recovery and flashing, firmware design and entities, cooling tests, feeding strategies, solderless-flashing research, the running findings log |
| `analysis/` | Stock firmware analysis: static (Ghidra), dynamic (QEMU), Android app; notes and the scripts to regenerate them |
| `captures/` | Test logs and temperature data referenced by the docs |
| `tools/` | Read-only esptool wrapper, flash backup, ota_1 installer, serial logger, plotting |

## Getting started
1. **Back up your own flash first** (`tools/backup.sh`, read-only, needs a 3.3 V serial adapter on the
   module; see `docs/recovery.md` for download mode). Keep the dump private: its NVS holds your Wi-Fi
   password and the device's cloud credentials.
2. Copy `esphome/components/` next to your ESPHome config, copy `esphome/petlibro-polar-v1.yaml`, and
   create `secrets.yaml` from `secrets.example.yaml`.
3. First install over serial (`docs/recovery.md`, `tools/install_ota1.py`); afterwards update over Wi-Fi.
   There is no proven no-open method yet: `docs/solderless-flashing.md`.
4. Go through the test plans in `docs/esphome-firmware.md` with the feeder in view.

## Not included
Stock firmware images, flash dumps, decompiled output, the Android app and datasheets are not
redistributed; the scripts in `analysis/` regenerate the analysis from your own dump and a downloaded APK.
Raw serial captures are omitted because they contain network details. Some docs still name these files.

## Safety
- Never burn eFuses; never enable secure boot or flash encryption on the chip.
- Firmware that drives the Peltier must also run the fan (on this board P1_4 switches both).
- Keep hands clear of the tray and lid while motors can move.
- The cooling is not a refrigerator: in tests the rear compartments reached about 10.5 °C, not 4 °C
  (`docs/cooling.md`). Plan food safety accordingly.

## Licence
GPL-3.0. See `LICENSE`.

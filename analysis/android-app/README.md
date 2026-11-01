# Petlibro Android app: Polar (PLAF109 / AF109) control path

All findings come from static analysis of the public APK and from reading public code. Nothing
was sent to Petlibro's cloud, no account was used, and the app was not run.

Evidence tags:
- **[V]**: verified, with a location. `asm/...` paths are relative to the blutter output
  `third_party/apk/blutter_out/asm/`. `0x...` addresses are offsets in `libapp.so`.
- **[I]**: inferred, with the reasoning given.

## 1. APK provenance

| Item | Value |
|---|---|
| Package | `com.designlibro.petlibro` ("Petlibro"). `com.dl.petlibro` is a different app, "PETLIBRO Lite" 1.0.6, made for v1.0 Granary feeders. The AF109 is not in it, and the app's own text says so (`view_add_device_notice_content`). |
| Version | 1.9.50 (versionCode 10950), minSdk 28, targetSdk 36 |
| Source | APKPure, `https://d.apkpure.com/b/XAPK/com.designlibro.petlibro?version=latest`, which redirects to `data.winudf.com` and gives `Petlibro_1.9.50_APKPure.xapk`. Downloaded 2026-10-30. |
| XAPK SHA-256 | `f9e22a269887f8beddd9e89d47cbc3b3ce571de6e3f1293c537e1729189fa9fa` |
| Splits (SHA-256) | base `com.designlibro.petlibro.apk` `bc2772e9…a176d5`; `config.arm64_v8a.apk` `2d3127fb…5ba71`; `config.xxxhdpi.apk` `15207dbb…44e2`. The full hashes are in `third_party/apk/SHA256SUMS` and from `sha256sum third_party/apk/xapk/*.apk`. |
| Manual | `https://us.petlibro.com/user-manual/polar-wet-food-feeder-af109/Polar-Wet-Food-Feeder-User-Manual.pdf`. This URL is a literal in libapp.so. Copy at `third_party/docs/`, SHA-256 `84d96baa…d40005`. |

The XAPK, its splits and all decompiled output stay under `third_party/`, which is gitignored.

## 2. Framework and tools

- The app is **Flutter, Dart AOT**. It has `lib/arm64-v8a/libapp.so` (38 MB), `libflutter.so` and `assets/flutter_assets/`. Dart 3.11.5, snapshot hash `78da37fed6bf1489361a312568249f3f` [V, blutter log].
  The Java/Kotlin dex holds only the Flutter embedding and plugins. jadx found no AF109, `ffee` or `wetFeeding` references there [V].
- Main package `package:petlibro/...`. Device layer `dl_device`, BLE layer `hi_ble` (uses `flutter_blue_plus`), app-side MQTT `hi_mqtt` (`mqtt_client`), UDP `udp`. Video uses the TUTK P2P libraries, which are for camera models only.
- Tools:
  - `blutter` (worawit/blutter, built locally against Dart 3.11.5 arm64) for Dart objects, the object pool and annotated assembly.
  - `jadx` 1.5.6 for the dex.
  - `scripts/dart_disasm.py` for methods that blutter only names. These are mixin-deduplicated, which covers most of `Feeder109Controller`. The script runs `objdump` and resolves object-pool (x27) loads and call targets.
- Translations are Dart `Map<String,String>` constants in the object pool. English is the map at `pp.txt` lines 72030–76389, extracted to `third_party/en_strings.txt` by `scripts/search_af109.sh`.

## 3. Architecture: how a command reaches the feeder

```
App --HTTPS REST--> api.<region>.petlibro.com --MQTT--> feeder (ESP32-C3)
App <--MQTT (hi_mqtt, cloud broker) <-- cloud push events (DEVICE_ATTR_CHANGE, FEED_PROCESS_CHANGE, ...)
```

- The app never talks to the feeder directly after provisioning. Every control is a cloud REST POST with JSON `{deviceSn, ...}` [V: `asm/petlibro/app/component/dl_device/feeder_dao.dart`].
- The cloud turns each REST call into an MQTT "service" call to the device.
  - The firmware's topic template is `dl/%s/%s/device/%s/%s` [V: `analysis/static/work/strings_drom.txt`]. It most likely expands to `dl/<product>/<sn>/device/<service|event|...>/<sub|post>` [I].
  - **The REST→MQTT mapping is not in the app.** The pairs in section 4 come from matching names and fields between the app and the firmware strings [I].
- The app subscribes to cloud MQTT for UI refresh. The event keys are `DEVICE_STATUS_CHANGE`, `DEVICE_WORK_RECORD_CHANGE`, `DEVICE_ATTR_CHANGE`, `DEVICE_GRAIN_STATUS_CHANGE`, `EXEC_CMD_REFRESH`, `DEVICE_SNACKBAR_REFRESH` and `FEED_PROCESS_CHANGE` [V: `asm/petlibro/app/component/dl_device/mqtt/strategy_refresh_event.dart`].

## 4. Polar commands (app → cloud), with the likely device-side MQTT service

Every request also carries `deviceSn`. The AF109 request classes are under `asm/petlibro/app/component/dl_device/feeder/*109*.dart`. Their callers are in `feeder_dao.dart` and in `Feeder109Controller`, a mixin of `DeviceDetailController`.

| REST endpoint | Params (beyond deviceSn) | Meaning | Probable firmware service [I] | Evidence |
|---|---|---|---|---|
| `/device/wetFeedingPlan/manualFeedNow` | `plate` (1–3) | "Feed Now / Open Now": open the lid over the given bowl | `WET_FOOD_FEED_NOW_SERVICE` | [V] `feeder_dao.dart` `manualFeedNow109` (0x1ca9260 `"deviceSn"`, 0x1ca9274 `"plate"`); HA `api.py set_manual_feed_now` |
| `/device/wetFeedingPlan/stopFeedNow` | `feedId` (= `manualFeedId` from wetListV3) | "Close Now": close the lid | `WET_FOOD_FEED_STOP_SERVICE` | [V] `stopFeedNow109` (0x1ca3034 `"feedId"`); HA `set_stop_feed_now` |
| `/device/wetFeedingPlan/platePositionChange` | `plate` | **Rotate the tray one compartment** ("Click the button to rotate food bowl") | `SET_PLATE_POS_SERVICE` | [V] `feeder_dao.dart` `platePositionChange109` (0x18bdac4/0x18bdaf4). See 4.1 |
| `/device/wetFeedingPlan/addFeedingPlanV3` / `editFeedingPlanV3` | plan list: `plate`, `label`, `executionStartTime`, `executionEndTime`, `id`, plus `templateName`, `autoRepeat`; edit adds `saveTemplate` | Meal schedule. At most 3 meals, one per bowl, each within 72 h | `WET_GRAIN_FEEDING_PLAN_SERVICE` / `GET_FEEDING_PLAN_EVENT` | [V] keys in `bean/device_feeder_bowl_status.dart`, `saveTemplate` in `edit109Plan`; body shape [I] |
| `/device/wetFeedingPlan/wetListV3` | – | Read plans. Response: `plan[]`, `templateName`, `manualFeedId`, `platePosition`, `autoRepeat`, `repeatState`, `allCancelState`, `baseTime`, `showManual`, `groupId`, `verifyTag` | – | [V] `DeviceFeederBowlStatusListVO` |
| `/device/wetFeedingPlan/reposition` | `plan[]` {id, plate, label, executionStartTime, executionEndTime}, `templateName` | "Reposition Schedule": the user put the bowl back in a different orientation, so the plan↔bowl mapping is rebuilt | (plan service) | [V] `wetFeedingPlanReposition` path; params from HA `set_reposition_schedule` |
| `/device/wetFeedingPlan/resetFeedingPlan` | `resetType` (e.g. `"cancel"`) | Cancel or reset the schedule ("Reset Meal Schedule" after a fault) | (plan service) | [V] `reset109Plan` |
| `/device/wetFeedingPlan/feedConfig` | – | Returns `thawTime` (default 60), `maxFeedingDuration` (480), `minFeedingDuration` (2), `maxTemplateNumber` (10), `max/minOpenNowDuration`, `recommendValue`, `feedingDurationOptions` (minutes) | – | [V] `bean/device_109_common_config.dart` (defaults used when a key is null) |
| `/device/wetFeedingPlan/hardVersionCap` | – | Capability probe. The log says "hardVersionCap109 support V2.0.20" or "… not support V2.0.20" | – | [V] `Feeder109Controller::hardVersionCap109` 0x10c0ec0 |
| `/device/device/hardVersionCompatible` | `scene: "platePositionRefresh"` | Asks whether the hardware supports targeted plate refresh | – | [V] `_checkPlateNumSupportable` 0x10c0bcc → `DeviceDao::deviceHardVersionCompatibleReq` |
| `/device/device/wetFoodFeedNow` | `planId`, `needThaw` (bool) | "Open after defrosting" or open immediately for a planned meal | `WET_FOOD_FEED_NOW_SERVICE` | [V] `feedNowWetFeederReq` |
| `/device/wetFeedingPlan/feedAgain` | `planId`, `executionTime` | "Feeding Again": reopen a finished meal | – | [V] `wetFeedAgain` |
| `/device/wetFeedingPlan/feedAudio` | – | Play the meal-call sound now | (`AUDIO`) | [V] `feedAudio109` |
| `/device/wetFeedingPlan/offlineList`, `verifyOffline`, `executablePlanList` | – | Plans that ran while the device was offline (the "Offline Updates" sheet) | – | [V] strings; `DeviceOfflineUpdates109VO` |
| `/device/device/resume` | `event` (the banner's eventKey) | Clear a latched fault after the user confirms. Used by `coolingAbnormalityRecovery`, `pinchCatRecovery109` and `resume109` | (`ATTR_SET_SERVICE`?) | [V] `Feeder109Controller::resume109` 0x18a3184 → `DeviceDao::deviceResumeReq` 0x18a2f34 |
| `/device/setting/updateCloseDoorTimeSetting` | `closeDoorTime` (min) | How long the lid stays open (default 4 h) | `ATTR_SET_SERVICE` `closeDoorTime` | [V] endpoint string, fw key `closeDoorTime` |
| `/device/setting/updateLightingSetting` | `lightSwitch`, `lightAgingType`, `lightingStartTime`, `lightingEndTime` | Status LED | `ATTR_SET_SERVICE` (`lightSwitch`, `lightingStartTimeUtc`…) | [V] HA `set_light_on/off`; fw keys |
| `/device/setting/updateSoundSetting` | `soundSwitch`, `soundAgingType`, `soundStartTime`, `soundEndTime` (+ ringer `ringerMode`/`ringerInterval`/`ringerDuration` for "Smart Sound") | Sound | `ATTR_SET_SERVICE` | [V] HA; fw keys; `view_feed109_device_sound_mode_smart` |
| `/device/device/reset` | – | Factory reset | – | [V] `device/reset_request.dart` |
| `/device/device/machineCleaningReset` | – | Reset the cleaning reminder | – | HA `set_cleaning_reset` |

The app has **no cooling on/off and no temperature setpoint control** for the AF109. No REST call or string sets a temperature [V: no such endpoint in the 372 `/device|/data` paths].

The firmware does have `SWITCH_COOL_SERVICE`, `coolSwitch`, `coolAgingType`, `coolStartTimeUtc`/`coolEndTimeUtc`, `freezeSwitch` and `TEMP_CAL`. Cooling may be cloud-controllable without being exposed in the app, or it may be driven by schedules only [I].

`SWITCH_DOOR_SERVICE`, `OPEN_DOOR` and `CLOSE_DOOR` in the firmware match the app's manual lid open/close (`view_feed109_opening_lid`, `…closing_lid`, "Manual feeding successful, the lid opened") [I].

### 4.1 Rotation: what the app tells us about "one step"

- **One app rotate = one compartment = 1/3 turn (120°).**
  - The bowl animation rotates by the constant `2.0943951023931953` = 2π/3 per step [V: `pp+0x17cf8`, used in `FeederBowlAnimation109::changeFeederAngle` 0x10bc644 and `Feeder109Controller::scheduleImageRotate`].
  - `rotateAndTogglePlate` cycles over 3 plates [V: 0x18bde5c, `mov x2,#3`].
  - The manual's spec table says "Number Of Meals: 3".
  - The app strings say "@count/@total bowls in use" and "Up to 3 meals per schedule".
  - The bowl artwork is `AF109_bowls_plate1..3.png`.
- **Two rotate code paths**, chosen by the hardware or firmware capability above:
  - *New hardware* (`_newHardwareRotatePlate` 0x18bdcbc) sends `platePositionChange` with `plate = next(current)` and wraps 3→1 (`cmp x0,#3`) [V call / I semantics].
  - *Old path* (`scheduleImageRotate` 0x18bd7dc) sends it with no explicit target and just animates one step [V].
  - The HA integration also notes that the device "will always rotate one bowl counter-clockwise regardless of what the plate ID is" [V: HA `api.py set_rotate_food_bowl`]. HA steps `(target-current) % 3` times to reach a target.
- Rotate requests are throttled (`EasyThrottle`, key `platePositionChange109`) and are refused while `zeroState == "PROCESSING"`. The toast is "Locating the food bowl. Please try again" [V: 0x18bd59c, `FeederCoreEx.isZeroStateProcessing` 0x18bd6f0].
- **How the firmware knows the bowl position.** This is from the app's side only:
  - The device reports `platePosition` (1–3) and `zeroState`. `zeroState = PROCESSING` drives the "Homing..." / "The meal trays are being repositioned" UI [V: `device_real_info_model.dart`, `feeder_109.dart`].
  - So there is a homing (zero) routine at start-up or after faults, and positions are counted from the home index.
  - The firmware strings (peer analysis) show more: `grating[0]`/`grating[1]` (two optical gate channels), `foodGratingSwitch`, `PLATE_READY_SET_POS`, `PLATE_READY_BACK_POS`, `plateStuckCurrent`/`plateMotorCurrent` (ADC current sensing) and `Read_PlateMotorAdc`.
  - That fits a DC gear motor that runs until an optical gate (connector CON6 "食盆光栅", food-bowl optical gate) sees the next slot, with stall detection by motor current [I].
  - The **app has no step, encoder or angle value**: it only ever asks for "plate N". The 120° step is set entirely by the firmware and the tray's gate slots. To get 6 or 12 positions you need the gate slot geometry (or timing) in our own firmware, not an app parameter.
- "Reposition Schedule" exists because a user can lift the bowl out and drop it back in another orientation. The schedule (which plate holds which meal) then has to be remapped. The device cannot sense which physical compartment is which, only the index positions [I from the strings "Please open the device and ensure that the food bowl is placed as per the scheduled time"].

## 5. Properties the device reports (realInfo and plan state)

The keys below appear in the app's `DeviceRealInfoModel` [V: `asm/dl_device/model/device_real_info_model.dart`] or in HA's Polar class, and also as firmware strings [V: `strings_drom.txt`]. Units and semantics are [I] unless stated.

| Key | Meaning |
|---|---|
| `platePosition` | Current bowl index 1–3 (in wetListV3, and realInfo per HA) |
| `zeroState` | `"PROCESSING"` while homing |
| `doorState`, `barnDoorState`, `barnDoorError` | Lid state; lid blocked (HA "door_blocked" = `barnDoorError`) |
| `coolingStatus`, `snowflake`, `temperatureProbeStatus`, `temperature` | Refrigeration status, cooling fault flag, probe fault, °C (HA converts to °F) |
| `powerType`, `powerMode`, `powerState`, `electricQuantity`, `batteryState`, `batterySupply8Hours` | Mains or battery, battery level. On battery the refrigeration stops ("powered only by batteries … refrigeration is disabled") |
| `lightSwitch`, `soundSwitch`, `enableLight`, `enableSound`, `volume`, `ringerMode/Interval/Duration` | LED and sound settings |
| `closeDoorTime`, `feedNowState`, `antiPinchState`, `errorState`, `exceptionMessage`, `runningState` | Lid timeout, manual-feed state, pinch sensor, generic error |
| `machineCleaningTime`, `machineNextCleaningTime`, `remainingCleaningDays` | Cleaning reminder |
| `wifiSsid`, `wifiRssi`, `softwareVersion`, `hardwareVersion`, `mac`, `timezone`, `online` | Housekeeping |

Plan item state (`DeviceFeederBowlStatusVO`): `state`, `editState`, `cancelState`, `lastState`, `failReason`, `feedAgainDuration`, `tempExecutionStartTime/EndTime`. The UI states are Upcoming, Preparing (thawing), Waiting to feed, Feeding, Complete and Status unknown [V].

## 6. Fault and event codes (Polar-relevant)

The banner dispatcher is `DeviceDetailController::onDeviceStatusBannerActionPressed` at 0x18a2b68. It branches on the `TriggerDeviceEvent` enum (`asm/petlibro/.../device_status_banner/controllers/action_helper/trigger_device_event.dart`) and on these event keys. The mapping of enum value to event key is [I] from branch order.

| Event key | Recovery action in app | Meaning |
|---|---|---|
| `DEVICE_CANNOT_ROTATE_EVENT` | `foodBowlMotorStallRecovery` | Tray (plate) motor stalled or cannot rotate. Firmware: `MOTOR_STUCK stop plate motor`, `plateStuckTimeout`, `Report_plateErrEvent` |
| `PLATE_ABNORMAL_EVENT` | – | Plate or bowl position abnormal (e.g. bowl missing, wrong position) [I] |
| `SNOWFLAKE_ERROR_EVENT` | `coolingAbnormalityRecovery` → `/device/device/resume`, then the "Reset Meal Schedule" dialog | Refrigeration fault. After recovery: "The device is back to normal. For your pet's safety, please check and replace the food, then reset the feeding schedule." |
| `CAT_ANTI_PINCH` | `pinchCatRecovery109` → resume | The lid anti-pinch sensor tripped |
| `BIN_DOOR_MOTOR_STALLED` | `binDoorMotorStalled109` | Lid (door) motor stalled. Firmware: `gateMotorState MOTOR_STUCK`, `doorStuckTimeout`, `doorStuckCurrent` |
| `LOW_BATTERY`, `POWER_CHANGE` | – | Battery low; mains/battery switch |
| `OTA_UPGRADE` | – | OTA progress push |

Plan failure reasons (strings): "Dispensing motor is busy / is locked / timed out, so this schedule was not executed". "Failed to open/close lid. Please try again." "Timeout exceeded, conditions for opening not met." "Your pet is eating. Please try later." The last one reflects the IR pet sensor: firmware `irState`, `PET_DETECT_EVENT`, and the manual's "safety sensor … ensure that the rotating lid continues to stay open".

Generic: "Device is error, function disabled". The status LED is solid red for "Feeder malfunction" (manual p.5).

## 7. User-facing description of the mechanism

From the manual (PDF in `third_party/docs/`) and the English strings:
- Parts: body, top lid, **rotating food section lid** (detachable from the top lid by a latch), temperature sensor (in the top lid), **food bowl** (removable, 3 compartments, MAX line), top-lid release button, status LED, cooling vents L/R, reset button (hold 3 s), battery compartment (3×AA, backup only).
- At meal time "the rotating food section lid will open and automatically close after the pet finishes eating". By default it stays open 4 h ("The lid has been open for four hours and automatically closed"), and this is configurable.
- 3 meals per schedule, each up to 72 h ahead. The food "needs to thaw for 30 minutes" before the lid opens ("Preparing"). The schedule is refused if the meal is less than 30 min away. The lid stays open for `closeDoorTime` minutes. A freshness warning appears if it is set longer than 1 h.
- Do not open the lid or change food after saving plans. That would invalidate the bowl↔meal mapping, hence "Reposition Schedule".
- Battery: refrigeration stops on batteries, but feeding within 12 h continues. Offline for more than 4 h or more than 8 h gives a food-safety warning.
- Ambient operating range 12–30 °C. Adapter 12 V 5 A. 2.4 GHz only. 3.4 kg.
- No cooling setpoint is shown anywhere in the app or the manual.

## 8. Provisioning (BLE first, SoftAP fallback)

[V] `asm/hi_ble/dl_ble.dart`, `asm/petlibro/app/component/dl_device/device_add/add_device_progress/controllers/add_device_progress.controller.dart`

- **GATT**: service `0xFFEE`. The app aborts if the service or `FFE1` is missing.
  - `FFE1`: write (app→device).
  - `FFE2`: read + notify (device results: wifiResult, mqttResult).
  - `FFE3`: read device info. Contains `fv` (firmware version), `pv` (protocol version) and the product identifier.
  - `FFE4`: notify, diagnostics (connection-error detail frames, at least 8 bytes).
  - The scan derives the MAC from the advertisement manufacturer data (`manufacturerDataToMac`).
  - The firmware has `ble_net/gatt_svr.c` on NimBLE, `BLE_WRITE_DEV_INFO_BIT`, and "ble connect wifi success" [V, static strings].
- **Frames**: they start with magic `"DL"` (0x44 0x4C) and include a 0xFF byte [V: `getBleWifiInfoData` 0x15ce108].
  - Newer firmware (`needEncode`) encrypts the payload with **AES-CBC** (`encrypt` package, `AESMode.cbc`).
  - Key bytes = UTF-8 `"DL"` + two device fields (likely MAC and product identifier) + six `0xA5` bytes, padded to 32 bytes. The IV is a 16-character substring [I from `encryptDeviceAES` 0x15c6aac / `decryptDeviceAES` 0x159bf28; exact field order not confirmed].
- **Sequence** (from the progress-node names):
  1. `app_ble_connect_device`.
  2. `app_ble_service`.
  3. `app_send_wifi_info` (`ssid`, `password`).
  4. `device_connect_wifi` (result on FFE2).
  5. `app_send_mqtt_info` (`cmd`, `mqttHost` as `host:1883`, `userId`, `httpUrl`, `useComplianceUrl`).
  6. `device_connect_mqtt`.
  7. `device_bind_user`: the cloud publishes `BINDING` state over MQTT, then the app checks bind (`/device/device/checkDeviceBind`).

  Error codes 2xxx (BLE connect), 3xxx (service), 4xxx (Wi-Fi send), 5xxx (Wi-Fi join), 6xxx (MQTT send), 7xxx (MQTT connect).
- **Hotspot fallback**: the phone joins the device AP. The app sends the same config over UDP to `10.235.76.1` or `192.168.1.1` and polls bind status [V strings]. The firmware switches the AP and BLE off on battery: "sitch to POWER_BAT_MODE, close ap and ble" [V static].
- The firmware's `mqttAddr`, `mqtts` and `MQTT_CONNECT_ERR` suggest the MQTT host comes from provisioning and is stored in NVS [I].

## 9. OTA

- App: `/device/ota/getUpgrade` (returns `jobItemId`, `targetVersion`, `upgradeDesc`, `jobName`, `progress`), `/device/ota/doUpgrade`, `/device/ota/getOtaItem`, `/device/setting/otaSetting` (auto-upgrade). Progress arrives over MQTT as `OTA_UPGRADE` with `state` and `progress`. Refused on battery ("The device is in battery mode…") [V: `device/get_upgrade_info_request.dart`, `do_upgrade_request.dart`, firmware_upgrade controller].
- Device: the cloud sends `OTA_INFORM` over MQTT with the URL (`upgradeType`, `targetSoftwareVersion`). The device reports `OTA_PROGRESS` and downloads with `esp_https_ota` into the other OTA slot [I from firmware strings `OTA_INFORM`, `OTA_PROGRESS`, `ESP_ERR_HTTPS_OTA_*`].
- **The app never handles the firmware image itself.**
- App version gates seen: firmware `2.0.16` (`compareFirmwareVersion` / `isMatchOnlyOpenThawingPlanFirmware`) and `2.0.20` (`hardVersionCap109`). Our unit runs 2.0.38, so it is on the newest code paths [V strings / I meaning].

## 10. Other models and public projects

- App product constants: `FEEDER_AF103` (Granary PLAF103), `AF105`, `AF107`, `AF108`, `AF109` (Polar), `AF203` (Granary camera), `AF205`, `AF301` [V strings].
- **The `wetFeedingPlan/*` endpoints and every `*109*` class are Polar-only.** No other product uses `manualFeedNow`/`platePositionChange` with `plate`.
  - Dry feeders (AF103/AF203/…) use `/device/device/manualFeeding`, `/device/feedingPlan/*` and `grainStatus` instead.
- Shared by all v2 products: `/device/device/realInfo`, `/device/setting/getAttributeSetting`, `updateLightingSetting`, `updateSoundSetting`, `/device/ota/*`, the `dl/...` MQTT topic family and generic services such as `ATTR_PUSH_EVENT`, `ATTR_SET_SERVICE`, `ATTR_GET_SERVICE`, `HEARTBEAT`, `NTP_SYNC`, `WIFI_CHANGE_SERVICE`, `WIFI_RECONNECT_SERVICE` and `DEVICE_PROPERTIES_SERVICE` [I: these also appear as app-side constants, e.g. `WIFI_CHANGE_SERVICE`, `DISCOVERY_RFID_SERVICE`].
- Cross-checks:
  - **jjjonesjr33/petlibro** (Home Assistant), commit `a28fe355` (2026-10-24), `custom_components/petlibro/devices/feeders/polar_wet_food_feeder.py` and `api.py`: https://github.com/jjjonesjr33/petlibro. It lists "Polar Wet Food Feeder (PLAF109)" and matches every Polar endpoint above: manualFeedNow{plate}, stopFeedNow{feedId}, platePositionChange{plate}, feedAudio, reposition{plan, templateName}, wetListV3, updateLightingSetting, updateSoundSetting. Its comment that rotation ignores `plate` agrees with the app's old-hardware path.
  - Older Tuya-based and PLAF103-only integrations: https://github.com/C4-Dimitri/ha_petlibro and the HA forum thread https://community.home-assistant.io/t/petlibro-cloud-integration-non-tuya-wip/759978. Both are cloud-only, with no Polar coverage.
  - Homebridge PetLibro and "Petlibro Local" were not found or not checked. No public local-MQTT documentation of the `dl/` topics was found.

## 11. Implications for the ESPHome rewrite

1. Stock behaviour is "advance one compartment", 120°, ended by the optical gate on CON6, with stall detection by motor current. The app sends only a target index and has no angle parameter. A 6- or 12-step tray therefore needs our own positioning: more gate slots, or timed or encoder-based moves. The app can teach us nothing more about this.
2. Homing (`zeroState`) runs before positions are trusted. Copy this: home on boot and after a stall.
3. The lid (door) is a separate motor with its own stall detection and an IR pet sensor that blocks closing. The Peltier stops on battery. Keep both behaviours.

## Scripts (`analysis/android-app/scripts/`)

| Script | Purpose |
|---|---|
| `download_apk.sh` | Fetches the XAPK from APKPure and appends its SHA-256 to `third_party/apk/SHA256SUMS` |
| `run_blutter.sh` | Clones and builds blutter, then decompiles libapp.so to `third_party/apk/blutter_out` |
| `run_jadx.sh` | Installs jadx 1.5.6 and decompiles the base split to `third_party/apk/jadx_out` |
| `blutter_summary.py` | Prints the classes, methods, string literals and integers in a blutter `.dart` file |
| `dart_disasm.py` | Disassembles a libapp.so function by address or name, with the object pool and call names resolved |
| `search_af109.sh` | Rebuilds `libapp.strings` and `en_strings.txt`, then lists the AF109 endpoints, strings and files |

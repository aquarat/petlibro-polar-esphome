# Solderless / no-open flashing of a stock PLAF109

Research note, 2026-10-31. Desk research + our own repo analysis only. No device was touched.
Tags: **[V]** verified (URL or repo file cited), **[I]** inference.

## Verdict

**No — not realistically, today, for an average non-technical user on a brand-new, unmodified
PLAF109, with no case opening and no soldering.** There is no exposed USB port and the chip's
native USB pins are used as GPIOs, so every *physical* route needs the case open. The only genuinely
no-open route is a **cloud/MQTT + OTA hijack** (avenues 2 and 3), and while it is the most promising
and is probably feasible on this wide-open hardware, it is **not proven** for *our* firmware: it
depends on unknowns in the stock OTA path (TLS trust and any image check) that we have **not yet
confirmed** in our decompile, and even if feasible it requires a broker + HTTPS server + a crafted
provisioning frame — far beyond an "average user". For a technical user it is a plausible project.
For the average user, the practical answer stays: a one-time assisted flash (serial, or a no-solder
pogo jig that still opens the case), or buy it pre-flashed.

Per-avenue ratings: **1 USB-Serial-JTAG — NO. 2 OTA from stock — MAYBE (best no-open path,
unverified). 3 BLE/SoftAP provisioning → OTA — MAYBE (enabler for 2). 4 Pogo/clip jig — WORKS
(no-solder but opens case). 5 Pre-flashed / module swap — WORKS (not "user on their own unit").**

---

## 1. Native USB-Serial-JTAG on the ESP32-C3 — **NO** (on a stock, unopened unit)

The C3 does have a built-in USB-Serial-JTAG peripheral on GPIO18 (D-) / GPIO19 (D+) that can flash
with no external adapter, and it can even drop itself into download mode over USB without the GPIO9
strap. On this board none of that is reachable:

- **The case has no USB port** and the module's USB pins are not routed to one.
- **GPIO18 and GPIO19 are used as ordinary GPIOs here:** GPIO18 = button (active low, 3 s hold =
  Wi-Fi reset), GPIO19 = lid position sensor **[V, docs/hardware.md pin map; analysis/static/README.md
  line 280 "GPIO18/19 are I/O, so no USB-Serial-JTAG"]**. They carry board signals, not a USB
  differential pair, so you cannot simply solder/clip a USB lead to them and expect enumeration
  without also freeing the pins in firmware — and the stock firmware drives them as I/O.
- The efuses confirm the *peripheral* is enabled (`DIS_USB_JTAG` False, USB-Serial-JTAG enabled)
  **[V, captures/20261030T203920_security.*; findings-log 2026-10-30 20:38]** — so the silicon path
  works; the blocker is purely that the pins go to the button and lid sensor, inside a sealed case.
- Entering ROM download at all still needs **GPIO9 low at reset** **[V, Espressif boot-mode doc;
  docs/hardware.md]**, which is also only reachable with the case open (SW1 does not reach GPIO9,
  findings-log 2026-10-30 20:27).

**Requires:** opening the case and board rework (free GPIO18/19, add a USB-C port wired to them, and
a way to strap GPIO9) — i.e. a hardware mod, not a stock unit. The user's framing is correct: the
*point* here is strictly no-open/no-solder on a stock unit, and a future USB-C mod does not satisfy
it. Rating **NO** for the stated question; a modded board is a different product.

## 2. OTA from the stock firmware — **MAYBE** (the most promising no-open path; unverified)

The stock app is ESP-IDF and does cloud-driven OTA: the cloud pushes `OTA_INFORM` over MQTT carrying
a URL, and the device downloads with `esp_https_ota` into the inactive OTA slot **[I from firmware
strings `OTA_INFORM`, `OTA_PROGRESS`, `ESP_ERR_HTTPS_OTA_*`, `esp_https_ota`; analysis/android-app
/README.md §9, analysis/static/README.md §5]**. Because **secure boot and flash encryption are
OFF** **[V, security capture]**, the *bootloader* imposes no signature barrier — an unsigned ESPHome
image already boots on this unit **[V, findings-log: ESPHome running from ota_0/ota_1 under the stock
IDF 4.4.6 bootloader]**. So the only questions are whether the stock *application's* OTA path can be
steered to our image and whether it checks the image itself. Concretely, to use this path you must:

1. Get the device to connect to an MQTT broker you control (see avenue 3), **or** get its existing
   cloud session to emit an `OTA_INFORM` pointing at your URL (needs Petlibro-side access — not
   realistic for a user).
2. Serve our ESPHome image at that URL over **HTTPS** in a way the device's `esp_https_ota` accepts.
3. Have the stock OTA handler accept the image (size fits the 0x1af000 OTA slots — ours does), write
   it, set the boot partition, and reboot — after which our image runs (already proven).

**The unknowns that decide feasibility (not assumed — must be read in our decompile, see end):**

- **TLS trust on the download.** Does `esp_https_ota`/`esp_http_client` use a pinned Petlibro CA, the
  ESP-IDF Mozilla cert bundle (`esp_crt_bundle_attach`), or skip verification
  (`skip_cert_common_name_check` / no `cert_pem`)? Pinned → you need that CA's cert for your host
  (hard). Mozilla bundle → a normal public cert (e.g. Let's Encrypt) on your own domain works. No
  check → trivial. **Unknown.**
- **Image authenticity check in the app.** Independent of secure boot, Petlibro's OTA handler may
  verify a signature/hash/magic or a version/product gate before `esp_ota_set_boot_partition`. If it
  does and we cannot satisfy it, the path is blocked even with a perfect download. **Unknown** — but
  note the app has version gates (2.0.16/2.0.20) and a product key/secret pair is baked in
  (0x3c0fcbd0), so some gating exists **[V strings]**.
- **URL source.** The OTA URL arrives over MQTT; whoever controls the broker controls the URL. The
  broker/host comes from provisioning and is stored in NVS (`mqttAddr`, `mqtts`, `httpUrl`) **[I]**.

If TLS is the Mozilla bundle (or off) and there is no app-level image signature, this is a complete
no-open path and would be the recommended one. **All three unknowns must be confirmed before
claiming it.** Prior art shows the v2 MQTT channel itself is plaintext on at least one sibling model
(see §3), which is encouraging for the control side but says nothing about the HTTPS OTA download.

## 3. Stock BLE / SoftAP provisioning → attacker broker → OTA — **MAYBE** (enabler for §2)

This is how you would satisfy step 1 of §2 without any Petlibro-side access. Provisioning (BLE
service `0xFFEE`, SoftAP UDP fallback) sends the device not just Wi-Fi creds but also
**`mqttHost` (as `host:1883`), `userId`, `httpUrl` and `useComplianceUrl`** **[V, analysis/android-app
/README.md §8, `app_send_mqtt_info`]**. That means the provisioning flow itself nominates the MQTT
broker and HTTP base — so a crafted provisioning frame can point a factory-fresh unit at
**your** broker, which can then issue `OTA_INFORM` with your URL. Supporting points and hurdles:

- **Plaintext MQTT on port 1883** is used by the v2 Petlibro stack: the tannerln7/Petlibro-Home-Assistant
  add-on "replaces the feeder's cloud MQTT connection with a local broker" and states the feeder
  "uses plaintext MQTT on tested firmware" with no UART and no DNS override required **[V,
  github.com/tannerln7/Petlibro-Home-Assistant]**. Our provisioning string `host:1883` matches
  plaintext MQTT **[V]**. So standing up a broker the device will talk to is realistic.
- **BLE frame crypto.** Newer firmware (`needEncode`) AES-CBC-encrypts the BLE provisioning payload,
  key = `"DL"` + two device fields (likely MAC + product id) + `0xA5` padding to 32 bytes, IV a
  16-char substring **[I, `encryptDeviceAES`/`decryptDeviceAES`]**. A no-open attacker must reproduce
  this (we have most of it). The **SoftAP/UDP** fallback (to `10.235.76.1` / `192.168.1.1`) may be an
  easier, possibly-unencrypted entry — **check whether it also applies `needEncode`**.
- **Does the device actually honour an arbitrary `mqttHost`/`httpUrl`,** or does it ignore them and
  use a compiled-in region host when `useComplianceUrl` is set? **Unknown — must be read in the
  firmware** (NVS write of `mqttAddr`/`httpUrl`, and whether OTA uses the provisioned `httpUrl` or a
  hardcoded one).
- Same two §2 unknowns still apply to the final download (TLS trust, image check).

**Requires:** a local broker, an HTTPS file server, a crafted (likely AES-encrypted) provisioning
frame, and the device un-bound/factory. This is a capable-hobbyist project, **not** an average-user
action. Rating **MAYBE**, and it is the practical front-end to §2.

## 4. One-time assisted no-solder flash (pogo/clip jig) — **WORKS** (no-solder, but opens the case)

A spring-contact/pogo-pin jig or a WROOM edge clip onto the existing **"ISP" pads (GND/TX/RX/VCC,
UART0)** plus a contact to pull **GPIO9** low gives the full serial download route with no soldering
**[V, docs/hardware.md ISP header + GPIO9 strap; docs/recovery.md]**. This is exactly how we flash,
minus the soldered header. Caveats:

- **It opens the case** — so it fails the "no-open" half of the question. List it as its own
  category: *no-solder but open*.
- GPIO9 is module pin 8 and has no dedicated test pad we have identified; the jig must reliably
  contact it (or a trace) while also pulling GPIO8 high (GPIO8=0+GPIO9=0 is invalid) **[V,
  findings-log strap decode]**. A bespoke fixture would be needed per board layout.
- After first flash, all further updates are ESPHome OTA over Wi-Fi (no jig again) **[V, repo
  workflow]**.

For a shop or a maker helping a friend, this is the robust, deterministic option. For the end user
alone it still means a screwdriver and a fixture.

## 5. Pre-flashed units / module swap — **WORKS** (but not "user on their own unit")

A seller can flash via the ISP pads before shipping, or swap in a pre-flashed ESP32-C3-WROOM-02U
module. Either delivers an ESPHome unit to a non-technical buyer with zero effort on their part. This
is out of scope for "an end user flashing their own stock unit", but it is the only option that is
truly turnkey for a non-technical person today. Mention and move on.

---

## What to verify in our own decompile to confirm the OTA path (§2/§3)

Do not run the decompiler for this note; these are the exact targets to read. Primary material:
`analysis/static/work/out/decomp_all.c`, `work/string_xrefs.txt`, `work/strings_drom.txt`,
`work/out/func_names.txt` (regenerable via `analysis/static/scripts/run_ghidra.sh`;
`scripts/fn.sh <regex>` prints a function; `scripts/nvs_keys.py` lists NVS keys). App side:
`analysis/android-app` (blutter output, `scripts/dart_disasm.py`).

**A. TLS trust on the OTA download (the decisive unknown):** in `analysis/static`, search the
decompile/strings for `esp_https_ota`, `esp_http_client_config` / `esp_http_client_init`, and the
`esp_http_client_config_t` fields set around the OTA handler:
- `cert_pem` / `cert_len` (a pinned PEM baked in → pinned trust),
- `crt_bundle_attach` / `esp_crt_bundle_attach` / `esp_crt_bundle_set` (Mozilla root bundle → a
  normal public cert on our host works),
- `skip_cert_common_name_check`, `use_global_ca_store`, `transport_type`,
- any literal `-----BEGIN CERTIFICATE-----` in `strings_drom.txt`, and which host it belongs to.
Also check whether the OTA URL scheme is `https://` or `http://` in the handler.

**B. Image authenticity / gating in the OTA handler:** find the `OTA_INFORM` / `OTA_UPGRADE` command
handler (reached from `Receive_handle` near `FUN_ram_42011dea`, the MQTT dispatcher in §5 of
analysis/static/README.md). Trace from there to `esp_ota_begin` / `esp_ota_write` / `esp_ota_end` /
`esp_ota_set_boot_partition`. Check for any of, *before* the boot-partition switch:
- a signature/hash check (`esp_image_verify`, `mbedtls_*`, SHA-256/RSA/Ed25519 over the image,
  compare against a field from `OTA_INFORM`),
- a product/model or version gate (`compareFirmwareVersion`, the product key/secret at `0x3c0fcbd0`,
  `targetSoftwareVersion` / `upgradeType` parsing),
- a magic/header check beyond the standard IDF image header.
Absence of all of these = an arbitrary image is accepted (best case for us).

**C. URL / broker source (does provisioning actually steer it):** confirm whether the OTA URL and
MQTT host are taken from the provisioned values or a compiled-in region host:
- NVS keys written at provisioning: run `scripts/nvs_keys.py` and look for `mqttAddr`, `mqtts`,
  `httpUrl`, `useComplianceUrl`, `userId` (keep values local);
- the code path from the BLE/SoftAP info handler (`ble_net/gatt_svr.c`, `BLE_WRITE_DEV_INFO_BIT`,
  "ble connect wifi success") to the NVS writes;
- whether `useComplianceUrl`/a compiled region host overrides the provisioned `httpUrl`/`mqttHost`.

**D. Provisioning frame crypto (to actually drive §3):** in `analysis/android-app`, re-read
`app_send_mqtt_info`, `getBleWifiInfoData` (0x15ce108), and `encryptDeviceAES` (0x15c6aac) /
`decryptDeviceAES` (0x159bf28) to pin the exact key field order and whether the **SoftAP/UDP**
fallback path (`10.235.76.1` / `192.168.1.1`) applies `needEncode` or sends cleartext. The firmware
counterpart is the `0xFFEE`/`FFE1`-`FFE4` GATT server and the UDP listener.

If A shows the Mozilla bundle or no cert check **and** B shows no app-level image signature **and**
C shows the provisioned `httpUrl`/`mqttHost` are honoured, then §2+§3 is a real end-to-end no-open
path and the verdict's "MAYBE" becomes "YES, for a technical user". Until all three are read, treat
it as unconfirmed.

---

## Sources

Verified facts carry a repo file or a URL; everything else is marked **[I]** above.

**Repo (primary, verified):** `docs/hardware.md`, `docs/recovery.md`, `docs/findings-log.md`,
`analysis/static/README.md` (esp. §5 cloud/MQTT, line 280 on GPIO18/19), `analysis/android-app
/README.md` (§8 provisioning, §9 OTA), `captures/20261030T203920_security.*` (efuse: secure boot /
flash encryption off, USB-JTAG enabled).

**Web:**
- ESP32-C3 boot mode / GPIO9 download strap — Espressif esptool docs:
  https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/boot-mode-selection.html
- ESP32-C3 serial connection incl. built-in USB-Serial-JTAG (GPIO18 D-/GPIO19 D+, flash without an
  external bridge) — ESP-IDF get-started:
  https://docs.espressif.com/projects/esp-idf/en/v5.1.6/esp32c3/get-started/establish-serial-connection.html
- ESP32-C3 USB-Serial-JTAG auto download-mode discussion (recent esptool enters download without
  RTS/DTR; manual BOOT+RESET fallback) — Espressif forum:
  https://esp32.com/viewtopic.php?p=111613
- ESPHome DIY sharing / adoption (project info required, API + mDNS, OTA needs already-running
  firmware, factory vs ota image) — ESPHome creators guide: https://esphome.io/guides/creators/ ;
  web installer generator: https://github.com/esphome/ewt-gen
- ESP Web Tools (browser Web-Serial flashing of ESP firmware; Chrome + HTTPS) — ESPHome org:
  https://github.com/esphome/esp-web-tools
- Prior Petlibro cloud/MQTT local-control work: tannerln7/Petlibro-Home-Assistant — local MQTT broker
  replaces the cloud connection, no UART/DNS override, plaintext MQTT on tested firmware (PLAF203):
  https://github.com/tannerln7/Petlibro-Home-Assistant and discussion
  https://github.com/tannerln7/Petlibro-Home-Assistant/discussions/1
- Petlibro HA cloud integration (v2 models use MQTT; endpoint cross-check for PLAF109):
  https://github.com/jjjonesjr33/petlibro
- Older Tuya-based / PLAF103 integrations and forum context:
  https://github.com/C4-Dimitri/ha_petlibro ,
  https://community.home-assistant.io/t/petlibro-cat-feeder/498637

Not found / not confirmed in desk research: any public "no-open" OTA-redirection flashing of a
PLAF109 or sibling to third-party firmware; any statement of the stock OTA's TLS trust or image
signature. These are the gaps our decompile (section above) must close.

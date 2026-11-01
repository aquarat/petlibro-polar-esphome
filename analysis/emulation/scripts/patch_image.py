#!/usr/bin/env python3
"""Patch a COPY of the full 4 MB flash image for emulation and refix the app image
checksum + appended SHA-256 of the ota_0 app (ESP-IDF v4.4 image format).

Usage: patch_image.py IN.bin OUT.bin PATCH[,PATCH...]   (names below; 'none' = just verify/copy)
Refuses to write into firmware/. Patches applied (app vaddr -> new halfword):
  0x42008202  c.li a1,0 (0x4581) -> c.li a1,5 (0x4595)
      SYSTEM_INFO_init: when the NVS 'sysInfo' debug flag is 0 the app calls
      esp_log_level_set("*", ESP_LOG_NONE); this makes it ESP_LOG_VERBOSE instead.
"""
import hashlib, struct, sys, os

APP_OFF = 0x30000
# name -> list of (vaddr, old bytes, new bytes, why)
PATCHES = {
  'loglevel': [(0x42008202, bytes.fromhex('8145'), bytes.fromhex('9545'),
                'SYSTEM_INFO_init: esp_log_level_set("*", NONE) -> VERBOSE')],
  # esp_phy_load_cal_and_init: `jal register_chipv7_phy` (0x420cee4c) -> `li a0,0`.
  # QEMU has no RF/PHY model; libphy calibration spins forever polling analog regs.
  'nophy': [(0x420eb9ea, bytes.fromhex('ef302ec6'), bytes.fromhex('13050000'),
             'esp_phy_load_cal_and_init: skip register_chipv7_phy (returns 0)')],
  # esp_wifi_start() entry -> `li a0,0; ret`. No Wi-Fi/RF model in QEMU; starting Wi-Fi either
  # spins in PHY calibration or (with nophy) faults in the MAC code. App sees ESP_OK.
  'nowifi': [(0x4209c4d4, bytes.fromhex('4111 06c6'.replace(' ', '')), bytes.fromhex('01458280'),
              'esp_wifi_start: return ESP_OK immediately')],
  # initialise_wifi, AP-mode branch: `jal NimBle_Cfg_Init(cb)` and `jal 0x4201cdbc` (BLE start,
  # takes NimBLE npl mutexes) -> nop. QEMU has no BT controller: esp_bt_controller_init asserts
  # (select_src_ret && set_div_ret), and skipping only the init makes later mutex_pend assert.
  'noble': [(0x4201b508, bytes.fromhex('ef105028'), bytes.fromhex('13000000'), 'initialise_wifi: skip NimBle_Cfg_Init'),
            (0x4201b50c, bytes.fromhex('ef10100b'), bytes.fromhex('13000000'), 'initialise_wifi: skip BLE start 0x4201cdbc'),
            # BLE advertising start (0x4201cdbc, 5 callers incl. dl_led "open ap and ble") -> li a0,0; ret
            (0x4201cdbc, bytes.fromhex('357106cf'), bytes.fromhex('01458280'), 'BLE adv start 0x4201cdbc: return 0')],
  # adc_hal_convert (0x42051782): `c.beqz a0,-6` polling APB_SARADC_INT_RAW done bit -> c.nop.
  # QEMU has no SAR ADC model (reads 0) so the loop never ends and the interrupt WDT fires.
  # With this patch every oneshot ADC read returns raw 0 unless gdb injects a value (gdb/adc_inject.gdb).
  'adcnowait': [(0x42051804, bytes.fromhex('6ddd'), bytes.fromhex('0100'),
                 'adc_hal_convert: do not wait for conversion done')],
}

def segments(img, base):
    nseg = img[base + 1]
    off = base + 24
    segs = []
    for _ in range(nseg):
        addr, ln = struct.unpack_from('<II', img, off)
        segs.append((addr, off + 8, ln))
        off += 8 + ln
    return segs, off

def vaddr_to_off(segs, va):
    for addr, data_off, ln in segs:
        if addr <= va < addr + ln:
            return data_off + va - addr
    raise ValueError(hex(va))

def fix(img, base):
    segs, end = segments(img, base)
    cks = 0xEF
    for _, data_off, ln in segs:
        for b in img[data_off:data_off + ln]:
            cks ^= b
    # pad so that (end + 1) is 16-aligned, checksum is the last byte of the padding
    pad = 15 - ((end - base) % 16)
    cpos = end + pad
    img[cpos] = cks
    hash_appended = img[base + 23]
    if hash_appended:
        h = hashlib.sha256(img[base:cpos + 1]).digest()
        img[cpos + 1:cpos + 33] = h
    return cks, cpos

def main():
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.abspath(dst).find('/firmware/') >= 0:
        sys.exit('refusing to write under firmware/')
    img = bytearray(open(src, 'rb').read())
    assert img[APP_OFF] == 0xE9
    # verify original checksum/hash first
    orig = bytes(img)
    c0, p0 = fix(img, APP_OFF)
    assert bytes(img) == orig, 'original image checksum/hash did not verify'
    segs, _ = segments(img, APP_OFF)
    names = [n for n in (sys.argv[3] if len(sys.argv) > 3 else 'loglevel').split(',') if n != 'none']
    for n in names:
        for va, old, new, why in PATCHES[n]:
            o = vaddr_to_off(segs, va)
            cur = bytes(img[o:o + len(new)])
            assert old is None or cur == old, 'unexpected %s at %08x' % (cur.hex(), va)
            img[o:o + len(new)] = new
            print('[%s] %08x (flash 0x%06x) %s -> %s  %s' % (n, va, o, cur.hex(), new.hex(), why))
    c, p = fix(img, APP_OFF)
    print('checksum 0x%02x at flash 0x%06x, sha256 refixed' % (c, p))
    open(dst, 'wb').write(img)

if __name__ == '__main__':
    main()

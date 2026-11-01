// Minimal AW9523B (I2C 0x58) helper for the Petlibro Polar PLAF109.
// Mirrors the stock init (@0x4200d9c8): regs 0x12/0x13=0xFF (GPIO mode), 0x04=0x03 (P0_0/P0_1 in),
// 0x05=0x00 (P1 out), 0x11=0x10 (P0 push-pull). Outputs kept in a shadow like the stock helper @0x4200d8a6.
#pragma once
#include "esphome/core/log.h"
#include "esphome/components/i2c/i2c_bus.h"

namespace aw9523b {
static const char *const TAG = "aw9523b";
static const uint8_t ADDR = 0x58;
// Idle: tray motor brake (P0_5,P0_6 = 1,1), lid motor brake (P1_6,P1_7 = 1,1), enables/Peltier/LEDs off.
static const uint16_t IDLE = (1u << 5) | (1u << 6) | (1u << 14) | (1u << 15);
static uint16_t shadow = IDLE;
static bool ready = false;

inline bool wr(esphome::i2c::I2CBus *bus, uint8_t reg, uint8_t val) {
  uint8_t d[2] = {reg, val};
  auto e = bus->write(ADDR, d, 2);
  if (e != esphome::i2c::ERROR_OK) ESP_LOGE(TAG, "write reg 0x%02X failed (%d)", reg, (int) e);
  return e == esphome::i2c::ERROR_OK;
}
inline int rd(esphome::i2c::I2CBus *bus, uint8_t reg) {
  uint8_t v = 0;
  auto e = bus->write_readv(ADDR, &reg, 1, &v, 1);
  if (e != esphome::i2c::ERROR_OK) { ESP_LOGE(TAG, "read reg 0x%02X failed (%d)", reg, (int) e); return -1; }
  return v;
}
inline bool flush(esphome::i2c::I2CBus *bus) {
  return wr(bus, 0x02, shadow & 0xFF) && wr(bus, 0x03, shadow >> 8);
}
// Outputs first (so no pin glitches high when switched to output), then mode/direction.
inline bool init(esphome::i2c::I2CBus *bus) {
  int id = rd(bus, 0x10);
  ESP_LOGI(TAG, "ID reg 0x10 = 0x%02X (AW9523B = 0x23)", id);
  if (id != 0x23) { ready = false; return false; }
  shadow = IDLE;
  ready = wr(bus, 0x12, 0xFF) && wr(bus, 0x13, 0xFF) && flush(bus) &&
          wr(bus, 0x11, 0x10) && wr(bus, 0x04, 0x03) && wr(bus, 0x05, 0x00);
  ESP_LOGI(TAG, "init %s, outputs 0x%04X", ready ? "ok" : "FAILED", shadow);
  return ready;
}
inline bool set(esphome::i2c::I2CBus *bus, uint8_t pin, bool on) {
  if (!ready) { ESP_LOGW(TAG, "not initialised, ignoring pin %u", pin); return false; }
  if (on) shadow |= (1u << pin); else shadow &= ~(1u << pin);
  bool ok = flush(bus);
  ESP_LOGI(TAG, "pin %u -> %d, outputs 0x%04X%s", pin, on, shadow, ok ? "" : " (write failed)");
  return ok;
}
inline bool get(uint8_t pin) { return shadow & (1u << pin); }
}  // namespace aw9523b

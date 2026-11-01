#include "aw9523b.h"
#include "esphome/core/log.h"

namespace esphome::aw9523b {

static const char *const TAG = "aw9523b";

static const uint8_t MAX_WRITE_FAILURES = 5;   // OURS: consecutive failed output writes before a reset
static const uint8_t MAX_READ_FAILURES = 3;    // OURS: consecutive failed verify reads before a reset
static const uint8_t MAX_RECOVERIES = 3;       // OURS: per 10 min; more than that -> mark_failed
static const uint32_t RECOVER_WINDOW_MS = 600000;

void AW9523BComponent::setup() {
  // Outputs/directions requested by pins before the chip is ready are shadowed and applied in init_chip_().
  // The reset pulse is non-blocking (set_timeout) so setup() does not stall the boot for 200 ms.
  // Stock AW9523B_i2cInit @0x4200d9c8: gpio_set_level(9, 0); vTaskDelay(200); gpio_set_level(9, 1).
  // Note: through an ESP reset GPIO9 floats high (strap pull-up), so the expander keeps its outputs until
  // this pulse; on_shutdown() therefore forces the fail-safe pins first.
  if (this->reset_pin_ != nullptr)
    this->reset_pin_->setup();
  this->recover_window_start_ms_ = millis();
  this->recovering_ = true;
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->digital_write(false);
    this->set_timeout("reset", this->reset_duration_, [this]() {
      this->reset_pin_->digital_write(true);
      // Registers accessible shortly after RSTN rises; 20 ms is generous (bring-up build used 20 ms).
      this->set_timeout("init", 20, [this]() { this->init_chip_(); });
    });
  } else {
    this->init_chip_();
  }
}

void AW9523BComponent::init_chip_() {
  uint8_t id = 0;
  if (this->read_register(REG_ID, &id, 1) != i2c::ERROR_OK || id != this->expected_id_) {
    this->chip_id_ = id;
    if (++this->init_attempts_ < 5) {
      ESP_LOGW(TAG, "ID read failed or wrong (0x%02X, want 0x%02X), retry %u", id, this->expected_id_,
               this->init_attempts_);
      this->set_timeout("init", 50, [this]() { this->init_chip_(); });
      return;
    }
    ESP_LOGE(TAG, "AW9523B not found at 0x%02X (ID 0x%02X); all expander outputs unavailable", this->address_, id);
    this->mark_failed();
    return;
  }
  this->chip_id_ = id;
  this->init_attempts_ = 0;

  // Stock order @0x4200d9c8 is 0x12,0x13,0x04,0x05,0x11, outputs=0. We write the outputs before direction
  // (and before push-pull) so pins that must start high (motor brake 1,1) are never driven low.
  bool ok = this->write_reg_(REG_MODE_P0, 0xFF) && this->write_reg_(REG_MODE_P1, 0xFF);
  ok = ok && this->flush_port_(0, true) && this->flush_port_(1, true);
  ok = ok && this->write_reg_(REG_GCR, this->p0_push_pull_ ? 0x10 : 0x00);
  ok = ok && this->write_direction_();
  if (!ok) {
    ESP_LOGE(TAG, "Register init failed");
    this->mark_failed();
    return;
  }
  this->ready_ = true;
  this->recovering_ = false;
  this->write_failures_ = 0;
  this->read_failures_ = 0;
  this->last_verify_ms_ = millis();
  ESP_LOGI(TAG, "Initialised: ID 0x%02X, outputs 0x%04X, inputs mask 0x%04X", id, this->output_, this->input_dir_);
}

void AW9523BComponent::apply_fail_safe_() {
  this->output_ = (this->output_ & ~this->fail_safe_mask_) | (this->initial_ & this->fail_safe_mask_);
}

void AW9523BComponent::on_shutdown() {
  // OURS: reboot / OTA reboot: brake both motors and switch the Peltier off before the ESP resets.
  this->apply_fail_safe_();
  bool ok = this->ready_ && this->flush_port_(0, true) && this->flush_port_(1, true);
  if (!ok && this->reset_pin_ != nullptr) {
    // Last resort: hold the chip in reset (outputs high-Z; the H-bridge input pulls are unknown, so
    // the braked state above is preferred whenever the write works).
    this->reset_pin_->digital_write(false);
  }
  ESP_LOGW(TAG, "Shutdown: fail-safe outputs %s", ok ? "written" : "NOT written, RSTN held low");
}

void AW9523BComponent::recover_(const char *reason) {
  if (this->recovering_ || this->is_failed())
    return;
  const uint32_t now = millis();
  if (now - this->recover_window_start_ms_ > RECOVER_WINDOW_MS) {
    this->recover_window_start_ms_ = now;
    this->recover_in_window_ = 0;
  }
  this->recover_count_++;
  ESP_LOGE(TAG, "Expander fault (%s): forcing fail-safe outputs and re-initialising (#%" PRIu32 ")", reason,
           this->recover_count_);
  this->apply_fail_safe_();
  this->recover_trigger_.trigger(std::string(reason));
  if (++this->recover_in_window_ > MAX_RECOVERIES) {
    ESP_LOGE(TAG, "Too many expander recoveries: marking FAILED");
    if (this->reset_pin_ != nullptr)
      this->reset_pin_->digital_write(false);  // hold in reset: nothing driven
    this->ready_ = false;
    this->mark_failed();
    return;
  }
  this->ready_ = false;
  this->recovering_ = true;
  this->port_dirty_[0] = this->port_dirty_[1] = true;
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->digital_write(false);
    this->set_timeout("reset", this->reset_duration_, [this]() {
      this->reset_pin_->digital_write(true);
      this->set_timeout("init", 20, [this]() { this->init_chip_(); });
    });
  } else {
    this->init_chip_();
  }
}

void AW9523BComponent::verify_() {
  // OURS: the stock driver is write-only. Read back the output, direction and GCR registers so a chip reset
  // (brownout) or a lost write is detected instead of silently masked by the shadow register.
  uint8_t out[2], dir[2], gcr;
  if (this->read_register(REG_OUTPUT_P0, &out[0], 1) != i2c::ERROR_OK ||
      this->read_register(REG_OUTPUT_P1, &out[1], 1) != i2c::ERROR_OK ||
      this->read_register(REG_CONFIG_P0, &dir[0], 1) != i2c::ERROR_OK ||
      this->read_register(REG_CONFIG_P1, &dir[1], 1) != i2c::ERROR_OK ||
      this->read_register(REG_GCR, &gcr, 1) != i2c::ERROR_OK) {
    this->status_set_warning();
    if (++this->read_failures_ >= MAX_READ_FAILURES)
      this->recover_("verify read failures");
    return;
  }
  this->read_failures_ = 0;
  const uint16_t out16 = out[0] | (out[1] << 8);
  const uint16_t dir16 = dir[0] | (dir[1] << 8);
  const bool gcr_ok = ((gcr & 0x10) != 0) == this->p0_push_pull_;
  if (out16 != this->written_ || dir16 != this->input_dir_ || !gcr_ok) {
    ESP_LOGE(TAG, "Readback mismatch: out 0x%04X (want 0x%04X) dir 0x%04X (want 0x%04X) gcr 0x%02X", out16,
             this->written_, dir16, this->input_dir_, gcr);
    this->recover_("readback mismatch");
  }
}

void AW9523BComponent::loop() {
  if (!this->ready_)
    return;
  // Polling mode (INTN is not known to be wired on the PLAF109): invalidate the input cache once per loop.
  if (this->has_input_pins_)
    this->reset_pin_cache_();
  // A failed output write is retried every loop, not only at the next change (OURS).
  if (this->port_dirty_[0] || this->port_dirty_[1]) {
    bool ok = true;
    if (this->port_dirty_[0])
      ok &= this->flush_port_(0, true);
    if (this->port_dirty_[1])
      ok &= this->flush_port_(1, true);
    if (ok) {
      this->write_failures_ = 0;
    } else if (++this->write_failures_ >= MAX_WRITE_FAILURES) {
      this->recover_("output write failures");
      return;
    }
  }
  const uint32_t now = millis();
  if (this->verify_interval_ms_ > 0 && now - this->last_verify_ms_ >= this->verify_interval_ms_ &&
      !this->port_dirty_[0] && !this->port_dirty_[1]) {
    this->last_verify_ms_ = now;
    this->verify_();
  }
}

void AW9523BComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "AW9523B:");
  LOG_I2C_DEVICE(this)
  LOG_PIN("  Reset pin: ", this->reset_pin_);
  ESP_LOGCONFIG(TAG,
                "  Chip ID: 0x%02X (expected 0x%02X)\n"
                "  Outputs: 0x%04X (fail-safe mask 0x%04X)\n"
                "  Input mask: 0x%04X\n"
                "  P0 push-pull: %s\n"
                "  Readback verify every %" PRIu32 " ms",
                this->chip_id_, this->expected_id_, this->output_, this->fail_safe_mask_, this->input_dir_,
                YESNO(this->p0_push_pull_), this->verify_interval_ms_);
  if (this->is_failed())
    ESP_LOGE(TAG, ESP_LOG_MSG_COMM_FAIL);
}

void AW9523BComponent::pin_mode(uint8_t pin, gpio::Flags flags) {
  if (flags & gpio::FLAG_INPUT) {
    this->input_dir_ |= (1u << pin);
    this->has_input_pins_ = true;
  } else {
    this->input_dir_ &= ~(1u << pin);
  }
  if (this->ready_)
    this->write_direction_();
}

bool AW9523BComponent::write_direction_() {
  return this->write_reg_(REG_CONFIG_P0, this->input_dir_ & 0xFF) &&
         this->write_reg_(REG_CONFIG_P1, (this->input_dir_ >> 8) & 0xFF);
}

int AW9523BComponent::read_port(uint8_t port) {
  if (!this->ready_ || port > 1)
    return -1;
  uint8_t v = 0;
  if (this->read_register(port == 0 ? REG_INPUT_P0 : REG_INPUT_P1, &v, 1) != i2c::ERROR_OK) {
    this->status_set_warning();
    return -1;
  }
  this->status_clear_warning();
  return v;
}

bool AW9523BComponent::digital_read_hw(uint8_t pin) {
  const uint8_t port = pin / 8;
  int v = this->read_port(port);
  if (v < 0)
    return false;
  if (port == 0) {
    this->input_state_ = (this->input_state_ & 0xFF00) | v;
  } else {
    this->input_state_ = (this->input_state_ & 0x00FF) | (v << 8);
  }
  return true;
}

bool AW9523BComponent::digital_read_cache(uint8_t pin) { return this->input_state_ & (1u << pin); }

void AW9523BComponent::digital_write_hw(uint8_t pin, bool value) {
  if (value) {
    this->output_ |= (1u << pin);
  } else {
    this->output_ &= ~(1u << pin);
  }
  if (!this->ready_)
    return;  // applied by init_chip_()
  // Like the stock helper @0x4200d8a6 we keep a shadow, but only the affected port register is written,
  // and only if it changed (or a previous write failed; loop() then retries every iteration).
  this->flush_port_(pin / 8, false);
}

bool AW9523BComponent::flush_port_(uint8_t port, bool force) {
  const uint8_t shift = port * 8;
  const uint8_t want = (this->output_ >> shift) & 0xFF;
  const uint8_t have = (this->written_ >> shift) & 0xFF;
  if (!force && !this->port_dirty_[port] && want == have)
    return true;
  if (!this->write_reg_(port == 0 ? REG_OUTPUT_P0 : REG_OUTPUT_P1, want)) {
    this->port_dirty_[port] = true;
    return false;
  }
  this->written_ = (this->written_ & ~(0xFFu << shift)) | (uint16_t(want) << shift);
  this->port_dirty_[port] = false;
  return true;
}

bool AW9523BComponent::write_reg_(uint8_t reg, uint8_t value) {
  auto err = this->write_register(reg, &value, 1);
  if (err != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Write reg 0x%02X = 0x%02X failed (%d)", reg, value, (int) err);
    this->status_set_warning();
    return false;
  }
  this->status_clear_warning();
  return true;
}

size_t AW9523BGPIOPin::dump_summary(char *buffer, size_t len) const {
  return buf_append_printf(buffer, len, 0, "P%u_%u via AW9523B", this->pin_ / 8, this->pin_ % 8);
}

}  // namespace esphome::aw9523b

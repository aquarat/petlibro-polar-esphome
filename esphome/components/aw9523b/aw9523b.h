#pragma once
// AW9523B 16-bit I2C I/O expander. See __init__.py for the stock (AF109 2.0.38) init sequence it mirrors.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/gpio_expander/cached_gpio.h"

namespace esphome::aw9523b {

// Register map (Awinic AW9523B datasheet; same addresses used by the stock helper @0x4200d8a6 / init @0x4200d9c8)
static const uint8_t REG_INPUT_P0 = 0x00;
static const uint8_t REG_INPUT_P1 = 0x01;
static const uint8_t REG_OUTPUT_P0 = 0x02;
static const uint8_t REG_OUTPUT_P1 = 0x03;
static const uint8_t REG_CONFIG_P0 = 0x04;  // 1 = input, 0 = output
static const uint8_t REG_CONFIG_P1 = 0x05;
static const uint8_t REG_ID = 0x10;
static const uint8_t REG_GCR = 0x11;  // bit 4: P0 push-pull
static const uint8_t REG_MODE_P0 = 0x12;  // 1 = GPIO, 0 = LED current mode
static const uint8_t REG_MODE_P1 = 0x13;

class AW9523BComponent final : public Component,
                               public i2c::I2CDevice,
                               public gpio_expander::CachedGpioExpander<uint8_t, 16> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_reset_duration(uint32_t ms) { this->reset_duration_ = ms; }
  void set_initial_outputs(uint16_t v) {
    this->output_ = v;
    this->initial_ = v;
  }
  /// Pins forced back to their initial level on shutdown and on recovery (motor brakes, Peltier off).
  void set_fail_safe_mask(uint16_t m) { this->fail_safe_mask_ = m; }
  void set_verify_interval(uint32_t ms) { this->verify_interval_ms_ = ms; }
  void set_input_mask(uint16_t m) { this->input_dir_ = m; }
  void set_port0_push_pull(bool pp) { this->p0_push_pull_ = pp; }
  void set_expected_id(uint8_t id) { this->expected_id_ = id; }

  /// Called by AW9523BGPIOPin::setup(). Before the chip is initialised this only records the direction.
  void pin_mode(uint8_t pin, gpio::Flags flags);

  /// True once the reset pulse, ID check and register init have completed.
  bool is_ready() const { return this->ready_; }
  /// Read one input port (0 or 1) directly. Returns -1 on I2C error or if not ready.
  int read_port(uint8_t port);
  /// Current output shadow (bit n = pin n), as last requested.
  uint16_t get_output_shadow() const { return this->output_; }
  uint8_t get_chip_id() const { return this->chip_id_; }
  uint32_t get_recover_count() const { return this->recover_count_; }
  /// Fires (with a reason) when the expander was found reset/unresponsive and is being re-initialised.
  /// Fail-safe pins are already forced to their safe level; motor controllers should fault.
  Trigger<std::string> *get_recover_trigger() { return &this->recover_trigger_; }

 protected:
  void init_chip_();
  void recover_(const char *reason);
  void verify_();
  void apply_fail_safe_();
  bool write_reg_(uint8_t reg, uint8_t value);
  bool flush_port_(uint8_t port, bool force);
  bool write_direction_();

  bool digital_read_hw(uint8_t pin) override;
  bool digital_read_cache(uint8_t pin) override;
  void digital_write_hw(uint8_t pin, bool value) override;

  GPIOPin *reset_pin_{nullptr};
  uint32_t reset_duration_{200};
  uint16_t output_{0};       // requested output levels
  uint16_t initial_{0};
  uint16_t fail_safe_mask_{0};
  uint32_t verify_interval_ms_{1000};
  uint32_t last_verify_ms_{0};
  uint8_t write_failures_{0};
  uint8_t read_failures_{0};
  uint32_t recover_count_{0};
  uint32_t recover_window_start_ms_{0};
  uint8_t recover_in_window_{0};
  bool recovering_{false};
  Trigger<std::string> recover_trigger_;
  uint16_t written_{0};      // levels last written successfully
  bool port_dirty_[2]{true, true};
  uint16_t input_dir_{0};    // 1 = input
  uint16_t input_state_{0};
  bool p0_push_pull_{true};
  bool has_input_pins_{false};
  bool ready_{false};
  uint8_t expected_id_{0x23};
  uint8_t chip_id_{0};
  uint8_t init_attempts_{0};
};

/// One expander pin usable by output/switch/binary_sensor platforms.
class AW9523BGPIOPin final : public GPIOPin, public Parented<AW9523BComponent> {
 public:
  AW9523BGPIOPin() {}
  void setup() override { this->pin_mode(this->flags_); }
  void pin_mode(gpio::Flags flags) override { this->parent_->pin_mode(this->pin_, flags); }
  bool digital_read() override { return this->parent_->digital_read(this->pin_) != this->inverted_; }
  void digital_write(bool value) override { this->parent_->digital_write(this->pin_, value != this->inverted_); }
  size_t dump_summary(char *buffer, size_t len) const override;

  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  void set_flags(gpio::Flags flags) { this->flags_ = flags; }
  gpio::Flags get_flags() const override { return this->flags_; }

 protected:
  uint8_t pin_{0};
  bool inverted_{false};
  gpio::Flags flags_{};
};

}  // namespace esphome::aw9523b

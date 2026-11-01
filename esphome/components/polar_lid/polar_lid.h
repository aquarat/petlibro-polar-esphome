#pragma once
// Petlibro Polar PLAF109 lid ("door") controller, re-implemented from the stock app AF109 2.0.38.
// References: analysis/static/README.md sections 2 and 2b, analysis/emulation/README.md section 5.4.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"

#include <vector>

namespace esphome::polar_lid {

enum class LidState : uint8_t { UNKNOWN = 0, CLOSED, OPEN, OPENING, CLOSING, STALL_BRAKE, JOG, FAULT };

/// Sensor window profile of one lid move. Times are from the start of the motion.
enum class LidProfile : uint8_t {
  FEED = 0,    // feed open/close (Feed_Handle @0x4200ae46, states OPEN 5 / CLOSE 7)
  MANUAL = 1,  // SWITCH_DOOR_SERVICE (Timer_DoorSignalHandle @0x4200de5a)
  BOOT = 2,    // boot close (DoorMotor_Reset_ThreadHandle @0x4200ec58)
  REOPEN = 3,  // OURS: safety reopen after a pet or a close stall (sensor ignored for >= 2 s)
};

class PolarLid : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override { this->motor_(Motor::BRAKE); }
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_motor_pins(GPIOPin *close_pin, GPIOPin *open_pin) {
    this->pin_close_ = close_pin;
    this->pin_open_ = open_pin;
  }
  void set_lid_sensor(binary_sensor::BinarySensor *s) { this->lid_sensor_ = s; }
  void set_sensor_end_level(bool level) { this->end_level_ = level; }
  void set_pet_sensor(binary_sensor::BinarySensor *s) { this->pet_sensor_ = s; }
  void set_current_sampler(voltage_sampler::VoltageSampler *s) { this->current_ = s; }
  void set_stall_threshold(float v) { this->stall_threshold_ = v; }
  void set_stall_samples(uint8_t n) { this->stall_samples_ = n; }
  void set_stall_retries(uint8_t n) { this->stall_retries_max_ = n; }
  void set_close_stall_samples(uint8_t n) { this->close_stall_samples_ = n; }
  void set_close_sample_interval(uint32_t ms) { this->close_sample_ms_ = ms; }
  void set_inrush_blanking(uint32_t ms) { this->inrush_ms_ = ms; }
  void set_min_travel(uint32_t ms) { this->min_travel_ms_ = ms; }
  void set_max_move_time(uint32_t ms) { this->max_move_ms_ = ms; }
  void set_feed_windows(uint32_t check_ms, uint32_t nocheck_ms) {
    this->feed_check_ms_ = check_ms;
    this->feed_nocheck_ms_ = nocheck_ms;
  }
  void set_run_on(uint32_t ms) { this->run_on_ms_ = ms; }
  void add_power_switch(switch_::Switch *s) { this->power_switches_.push_back(s); }
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_ = s; }
  void set_fault_binary_sensor(binary_sensor::BinarySensor *s) { this->fault_bs_ = s; }
  void set_closed_binary_sensor(binary_sensor::BinarySensor *s) { this->closed_bs_ = s; }
  void set_current_sensor(sensor::Sensor *s) { this->current_sensor_ = s; }

  bool open(LidProfile profile);
  bool close(LidProfile profile, bool check_pet);
  bool jog(bool open, uint32_t ms);
  void stop(const char *reason = "stop requested");
  void clear_fault();
  void set_inhibit(bool inhibit, const char *reason);
  /// External fault (e.g. expander reset): brake and require clear_fault().
  void force_fault(const char *reason) { this->fault_(reason); }

  LidState get_state() const { return this->state_; }
  bool is_busy() const {
    return this->state_ == LidState::OPENING || this->state_ == LidState::CLOSING ||
           this->state_ == LidState::STALL_BRAKE || this->state_ == LidState::JOG;
  }
  bool is_closed() const { return this->state_ == LidState::CLOSED; }
  bool is_open() const { return this->state_ == LidState::OPEN; }
  bool is_fault() const { return this->state_ == LidState::FAULT; }
  bool is_inhibited() const { return this->inhibited_; }
  /// True if the last close was aborted because a pet was seen (lid reopened). Cleared by the next close().
  bool was_pet_reopened() const { return this->pet_reopened_; }
  /// True if the last close stalled (over-current) and the lid was reopened. Cleared by the next close().
  bool was_stall_reopened() const { return this->stall_reopened_; }

  Trigger<> *get_opened_trigger() { return &this->opened_trigger_; }
  Trigger<> *get_closed_trigger() { return &this->closed_trigger_; }
  Trigger<> *get_pet_reopen_trigger() { return &this->pet_reopen_trigger_; }
  Trigger<std::string> *get_fault_trigger() { return &this->fault_trigger_; }

 protected:
  enum class Motor : uint8_t { BRAKE, CLOSE, OPEN };
  void motor_(Motor m);
  bool start_(bool opening, LidProfile profile, bool check_pet, bool is_retry);
  void end_(const char *how);
  void finish_(const char *how);
  void reopen_(bool pet);
  void fault_(const char *reason);
  void set_state_(LidState s, const char *text = nullptr);
  bool at_end_() const { return this->lid_sensor_ != nullptr && this->lid_sensor_->state == this->end_level_; }

  GPIOPin *pin_close_{nullptr};  // P1_7
  GPIOPin *pin_open_{nullptr};   // P1_6
  binary_sensor::BinarySensor *lid_sensor_{nullptr};  // GPIO19
  bool end_level_{true};
  binary_sensor::BinarySensor *pet_sensor_{nullptr};  // GPIO20
  voltage_sampler::VoltageSampler *current_{nullptr}; // GPIO3 ADC1 ch3
  float stall_threshold_{0.400f};
  uint8_t stall_samples_{6};
  uint8_t stall_retries_max_{3};
  uint8_t close_stall_samples_{2};
  uint32_t close_sample_ms_{100};
  uint32_t inrush_ms_{300};
  uint32_t min_travel_ms_{1000};
  uint32_t max_move_ms_{60000};
  uint32_t feed_check_ms_{5000};
  uint32_t feed_nocheck_ms_{10000};
  uint32_t run_on_ms_{1000};
  std::vector<switch_::Switch *> power_switches_;
  text_sensor::TextSensor *state_text_{nullptr};
  binary_sensor::BinarySensor *fault_bs_{nullptr};
  binary_sensor::BinarySensor *closed_bs_{nullptr};
  sensor::Sensor *current_sensor_{nullptr};
  Trigger<> opened_trigger_;
  Trigger<> closed_trigger_;
  Trigger<> pet_reopen_trigger_;
  Trigger<std::string> fault_trigger_;

  LidState state_{LidState::UNKNOWN};
  Motor motor_state_{Motor::BRAKE};
  bool inhibited_{false};
  bool opening_{false};
  LidProfile profile_{LidProfile::FEED};
  bool check_pet_{false};
  bool pet_reopened_{false};
  bool stall_reopened_{false};
  bool run_on_active_{false};
  bool started_at_end_{false};
  bool left_end_{false};
  LidState start_state_{LidState::UNKNOWN};  // state when the current move started
  bool sensor_changed_{false};
  bool initial_raw_{false};
  const char *end_how_{""};
  uint32_t ignore_ms_{0}, accept_until_ms_{0}, limit_ms_{0}, run_on_cfg_ms_{0};
  uint32_t move_start_ms_{0};
  uint32_t phase_start_ms_{0};
  uint32_t run_on_start_ms_{0};
  uint32_t last_sample_ms_{0};
  uint32_t jog_ms_{0};
  uint8_t over_count_{0};
  uint8_t retries_{0};
};

template<typename... Ts> class OpenAction final : public Action<Ts...>, public Parented<PolarLid> {
 public:
  void set_profile(LidProfile p) { this->profile_ = p; }
  void play(const Ts &...x) override { this->parent_->open(this->profile_); }

 protected:
  LidProfile profile_{LidProfile::MANUAL};
};
template<typename... Ts> class CloseAction final : public Action<Ts...>, public Parented<PolarLid> {
 public:
  void set_profile(LidProfile p) { this->profile_ = p; }
  void set_check_pet(bool c) { this->check_pet_ = c; }
  void play(const Ts &...x) override { this->parent_->close(this->profile_, this->check_pet_); }

 protected:
  LidProfile profile_{LidProfile::MANUAL};
  bool check_pet_{false};
};
template<typename... Ts> class StopAction final : public Action<Ts...>, public Parented<PolarLid> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};
template<typename... Ts> class ClearFaultAction final : public Action<Ts...>, public Parented<PolarLid> {
 public:
  void play(const Ts &...x) override { this->parent_->clear_fault(); }
};
template<typename... Ts> class JogAction final : public Action<Ts...>, public Parented<PolarLid> {
 public:
  TEMPLATABLE_VALUE(bool, open)
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->jog(this->open_.value(x...), this->duration_.value(x...)); }
};

}  // namespace esphome::polar_lid

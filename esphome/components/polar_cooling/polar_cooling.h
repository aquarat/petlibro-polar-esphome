#pragma once
// Petlibro Polar PLAF109 Peltier thermostat.
// Stock: CTRL_THREAD_coolHanlde @0x4200e6f6 (analysis/static/README.md section 3): every 500 ms loop, sample-and-hold
// decision every 600 loops (300 s): T <= 12.0 C -> P1_4 off, T > 12.0 C -> on; off on battery; no hysteresis,
// no PWM, no fan control. This component adds (OURS): adjustable setpoint (2..15 C) and hysteresis, a minimum
// off-time, a hard max-continuous-on limit, a freeze guard, duty-cycle reporting, and a master enable that is
// off at boot because the fan wiring is UNKNOWN and UNTESTED.

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/polar_feeder/polar_feeder.h"

#include <vector>

namespace esphome::polar_cooling {

class PolarCooling : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA - 2.0f; }

  void set_peltier(switch_::Switch *s) { this->peltier_ = s; }
  void set_temperature(sensor::Sensor *s) { this->temperature_ = s; }
  void set_on_battery(binary_sensor::BinarySensor *s) { this->on_battery_ = s; }
  void set_enable(switch_::Switch *s) { this->enable_ = s; }
  void set_thaw_rule(switch_::Switch *s) { this->thaw_rule_ = s; }
  void set_feeder(polar_feeder::PolarFeeder *f) { this->feeder_ = f; }
  void set_setpoint(number::Number *n) { this->setpoint_ = n; }
  void set_hysteresis(number::Number *n) { this->hysteresis_ = n; }
  void set_max_on(number::Number *n) { this->max_on_ = n; }
  void set_allowed(bool a) { this->allowed_ = a; }
  void set_interval(uint32_t ms) { this->interval_ms_ = ms; }
  void set_min_off(uint32_t ms) { this->min_off_ms_ = ms; }
  void set_freeze_guard(float c) { this->freeze_guard_ = c; }
  void set_duty_sensor(sensor::Sensor *s) { this->duty_sensor_ = s; }
  void set_since_off_sensor(sensor::Sensor *s) { this->since_off_sensor_ = s; }
  void set_state_text(text_sensor::TextSensor *s) { this->state_text_ = s; }
  /// P0_4/P0_7: stock holds them high whenever on DC (AllDet_init, Power_Handle). They might power the fan.
  void add_power_switch(switch_::Switch *s) { this->power_switches_.push_back(s); }
  void set_assert_power_switches(bool a) { this->assert_power_ = a; }

  /// Re-evaluate at the next loop (e.g. after enabling or a setpoint change).
  void evaluate_now() { this->force_eval_ = true; }
  bool is_on() const { return this->on_; }
  float get_duty() const { return this->duty_; }

 protected:
  void apply_(bool on, const char *why);
  void status_(const char *s);
  float setpoint_c_() const;
  float hysteresis_c_() const;
  uint32_t max_on_ms_() const;

  switch_::Switch *peltier_{nullptr};
  sensor::Sensor *temperature_{nullptr};
  binary_sensor::BinarySensor *on_battery_{nullptr};
  switch_::Switch *enable_{nullptr};
  switch_::Switch *thaw_rule_{nullptr};
  polar_feeder::PolarFeeder *feeder_{nullptr};
  number::Number *setpoint_{nullptr};
  number::Number *hysteresis_{nullptr};
  number::Number *max_on_{nullptr};
  bool allowed_{false};
  uint32_t interval_ms_{300000};
  uint32_t min_off_ms_{60000};
  float freeze_guard_{1.0f};
  sensor::Sensor *duty_sensor_{nullptr};
  sensor::Sensor *since_off_sensor_{nullptr};
  text_sensor::TextSensor *state_text_{nullptr};
  std::vector<switch_::Switch *> power_switches_;
  bool assert_power_{true};

  bool on_{false};
  bool want_on_{false};
  bool force_eval_{true};
  uint32_t last_eval_ms_{0};
  uint32_t on_since_ms_{0};
  uint32_t off_since_ms_{0};
  uint32_t last_tick_ms_{0};
  uint32_t last_publish_ms_{0};
  // rolling hour: 60 one-minute buckets of on-time (ms)
  uint32_t bucket_ms_[60]{};
  uint8_t bucket_idx_{0};
  uint32_t bucket_start_ms_{0};
  uint8_t buckets_filled_{0};
  float duty_{0.0f};
};

}  // namespace esphome::polar_cooling

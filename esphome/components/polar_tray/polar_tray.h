#pragma once
// Petlibro Polar PLAF109 tray (plate) controller.
// Behaviour re-implemented from the stock app AF109 2.0.38; addresses refer to analysis/static/README.md section 2
// and analysis/emulation/README.md section 5.3. Only the HW 5.2.0 homing algorithm (widest mark) is implemented.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/switch/switch.h"

#include <vector>

#include "esp_adc/adc_oneshot.h"

namespace esphome::polar_tray {

enum class TrayState : uint8_t {
  IDLE = 0,
  MOVING,        // motor forward, counting gate edges (or timing a sub-step)
  STALL_BRAKE,   // stall recovery: brake 1 s
  STALL_REVERSE, // stall recovery: reverse 2 s
  STALL_SETTLE,  // our addition: 100 ms brake between reverse and forward
  JOG,           // manual timed jog (test aid)
  FAULT,
};

/// Gate (GPIO10) edges captured in the ISR, processed in loop().
struct GateStore {
  static const uint8_t SIZE = 32;
  ISRInternalGPIOPin pin;
  volatile uint32_t ts_us[SIZE];
  volatile uint8_t level[SIZE];
  volatile uint8_t head{0};
  volatile uint32_t overflows{0};
  uint8_t tail{0};
  static void gpio_intr(GateStore *arg);
};

class PolarTray : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override { this->motor_(Motor::BRAKE); }
  float get_setup_priority() const override { return setup_priority::DATA; }

  // --- configuration ---
  void set_motor_pins(GPIOPin *a, GPIOPin *b) {
    this->pin_a_ = a;
    this->pin_b_ = b;
  }
  void set_gate_pin(InternalGPIOPin *pin) { this->gate_pin_ = pin; }
  void set_compartments(uint8_t n) { this->compartments_ = n; }
  void set_current_sensing(bool en, int channel) {
    this->current_sensing_ = en;
    this->adc_channel_ = channel;
  }
  void set_stall_threshold(int raw) { this->stall_threshold_ = raw; }
  void set_stall_samples(uint8_t n) { this->stall_samples_ = n; }
  void set_stall_retries(uint8_t n) { this->stall_retries_max_ = n; }
  void set_edge_timeout(uint32_t ms) { this->edge_timeout_ms_ = ms; }
  void set_max_move_time(uint32_t ms) { this->max_move_ms_ = ms; }
  void set_gate_ignore(uint32_t ms) { this->gate_ignore_ms_ = ms; }
  void set_home_max_edges(uint8_t n) { this->home_max_edges_ = n; }
  void set_home_width_ratio(float r) { this->home_ratio_ = r; }
  void set_gate_debounce(uint32_t ms) { this->gate_debounce_us_ = ms * 1000; }
  void set_max_invalid_samples(uint8_t n) { this->max_invalid_ = n; }
  void set_adc_invalid_sensor(sensor::Sensor *s) { this->adc_invalid_sensor_ = s; }
  void add_power_switch(switch_::Switch *s) { this->power_switches_.push_back(s); }

  void set_position_sensor(sensor::Sensor *s) { this->position_sensor_ = s; }
  void set_target_sensor(sensor::Sensor *s) { this->target_sensor_ = s; }
  void set_mark_interval_sensor(sensor::Sensor *s) { this->interval_sensor_ = s; }
  void set_pulse_width_sensor(sensor::Sensor *s) { this->width_sensor_ = s; }
  void set_current_sensor(sensor::Sensor *s) { this->current_sensor_ = s; }
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_ = s; }
  void set_homing_text_sensor(text_sensor::TextSensor *s) { this->homing_text_ = s; }
  void set_fault_binary_sensor(binary_sensor::BinarySensor *s) { this->fault_bs_ = s; }
  void set_moving_binary_sensor(binary_sensor::BinarySensor *s) { this->moving_bs_ = s; }
  void set_homed_binary_sensor(binary_sensor::BinarySensor *s) { this->homed_bs_ = s; }

  // --- commands (all return false and log if refused) ---
  bool home();
  bool step();
  /// compartment is 1-based (1..compartments)
  bool go_to(int compartment);
  bool jog(bool forward, uint32_t ms);
  /// Runtime change of the compartment count (3, 6, 9 or 12). Refused while busy; clears homing.
  bool change_compartments(uint8_t n);
  void stop(const char *reason = "stop requested");
  void clear_fault();
  /// Inhibit blocks all moves (low battery, wrong HW revision, ...). Inhibiting also stops a running move.
  void set_inhibit(bool inhibit, const char *reason);
  /// External fault (e.g. expander reset): brake, clear homing, require clear_fault().
  void force_fault(const char *reason) { this->fault_(reason); }

  // --- state for lambdas / other components ---
  bool is_busy() const { return this->state_ != TrayState::IDLE && this->state_ != TrayState::FAULT; }
  bool is_fault() const { return this->state_ == TrayState::FAULT; }
  bool is_homed() const { return this->homed_; }
  bool is_inhibited() const { return this->inhibited_; }
  /// 1-based compartment under the lid opening, or 0 if unknown.
  int get_position() const;
  int get_target() const { return this->target_ < 0 ? 0 : this->target_ + 1; }
  uint8_t get_compartments() const { return this->compartments_; }
  uint32_t get_mark_interval_ms() const { return this->interval_ms_; }
  Trigger<> *get_arrived_trigger() { return &this->arrived_trigger_; }
  Trigger<std::string> *get_fault_trigger() { return &this->fault_trigger_; }

 protected:
  enum class Motor : uint8_t { BRAKE, FORWARD, REVERSE };
  void motor_(Motor m);
  void power_on_();
  bool start_move_(int target_index, bool homing, bool raw_step);
  void handle_edge_(uint32_t ts_us, bool level);
  void counted_edge_(uint32_t ts_us, uint32_t width_ms);
  void arrive_();
  void fault_(const char *reason);
  void stall_check_(uint32_t now);
  bool read_current_(int *raw);
  void set_state_(TrayState s, const char *text = nullptr);
  void publish_position_();
  void publish_adc_stats_();
  uint8_t k_() const { return this->compartments_ / 3; }  // sub-steps per mark
  uint32_t sub_step_ms_() const { return this->interval_ms_ / this->k_(); }

  // config
  GPIOPin *pin_a_{nullptr};  // P0_5
  GPIOPin *pin_b_{nullptr};  // P0_6
  InternalGPIOPin *gate_pin_{nullptr};
  uint8_t compartments_{3};
  bool current_sensing_{true};
  int adc_channel_{0};
  int stall_threshold_{400};
  uint8_t stall_samples_{6};
  uint8_t stall_retries_max_{3};
  uint32_t edge_timeout_ms_{60000};
  uint32_t max_move_ms_{90000};
  uint32_t gate_ignore_ms_{500};
  uint8_t home_max_edges_{7};
  float home_ratio_{2.0f};
  uint32_t gate_debounce_us_{10000};
  uint8_t max_invalid_{20};
  sensor::Sensor *adc_invalid_sensor_{nullptr};
  std::vector<switch_::Switch *> power_switches_;

  // entities
  sensor::Sensor *position_sensor_{nullptr};
  sensor::Sensor *target_sensor_{nullptr};
  sensor::Sensor *interval_sensor_{nullptr};
  sensor::Sensor *width_sensor_{nullptr};
  sensor::Sensor *current_sensor_{nullptr};
  text_sensor::TextSensor *state_text_{nullptr};
  text_sensor::TextSensor *homing_text_{nullptr};
  binary_sensor::BinarySensor *fault_bs_{nullptr};
  binary_sensor::BinarySensor *moving_bs_{nullptr};
  binary_sensor::BinarySensor *homed_bs_{nullptr};
  Trigger<> arrived_trigger_;
  Trigger<std::string> fault_trigger_;

  // runtime
  GateStore store_{};
  adc_oneshot_unit_handle_t adc_{nullptr};
  HighFrequencyLoopRequester hf_;
  TrayState state_{TrayState::IDLE};
  Motor motor_state_{Motor::BRAKE};
  bool inhibited_{false};
  bool homed_{false};
  bool homing_{false};
  bool raw_step_{false};
  int mark_{-1};            // 0..2, last mark passed (0 = home mark = compartment 1); -1 unknown
  uint32_t seg_run_ms_{0};  // forward running time since the last mark (sub-step position estimate)
  int target_{-1};          // 0-based target compartment
  int marks_remaining_{0};
  bool timed_phase_{false};
  uint32_t timed_goal_ms_{0};
  uint32_t interval_ms_{0};      // learned mark-to-mark time (ms)
  uint32_t move_start_ms_{0};
  uint32_t last_progress_ms_{0};
  uint32_t ignore_until_us_{0};
  bool ignore_active_{false};
  uint32_t last_rise_us_{0};
  bool have_rise_{false};
  uint32_t prev_mark_us_{0};
  bool prev_mark_valid_{false};  // previous counted edge belongs to the same uninterrupted forward run
  uint32_t max_width_ms_{0};
  uint32_t widths_[4]{};
  uint8_t pulses_{0};
  bool position_suspect_{false};
  bool stable_level_{false};
  bool cand_valid_{false};
  bool cand_level_{false};
  uint32_t cand_ts_us_{0};
  uint8_t consecutive_invalid_{0};
  uint32_t move_adc_total_{0};
  uint32_t move_adc_invalid_{0};
  uint8_t edges_this_move_{0};
  uint8_t over_count_{0};
  uint8_t retries_{0};
  uint32_t phase_start_ms_{0};
  uint32_t last_tick_ms_{0};
  uint32_t last_sample_ms_{0};
  uint32_t jog_ms_{0};
  uint32_t adc_invalid_{0};
  uint32_t adc_total_{0};
};

// ---- actions ----
template<typename... Ts> class HomeAction final : public Action<Ts...>, public Parented<PolarTray> {
 public:
  void play(const Ts &...x) override { this->parent_->home(); }
};
template<typename... Ts> class StepAction final : public Action<Ts...>, public Parented<PolarTray> {
 public:
  void play(const Ts &...x) override { this->parent_->step(); }
};
template<typename... Ts> class StopAction final : public Action<Ts...>, public Parented<PolarTray> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};
template<typename... Ts> class ClearFaultAction final : public Action<Ts...>, public Parented<PolarTray> {
 public:
  void play(const Ts &...x) override { this->parent_->clear_fault(); }
};
template<typename... Ts> class GoToAction final : public Action<Ts...>, public Parented<PolarTray> {
 public:
  TEMPLATABLE_VALUE(int, compartment)
  void play(const Ts &...x) override { this->parent_->go_to(this->compartment_.value(x...)); }
};
template<typename... Ts> class JogAction final : public Action<Ts...>, public Parented<PolarTray> {
 public:
  TEMPLATABLE_VALUE(bool, forward)
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->jog(this->forward_.value(x...), this->duration_.value(x...)); }
};

}  // namespace esphome::polar_tray

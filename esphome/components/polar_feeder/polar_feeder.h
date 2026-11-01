#pragma once
// Petlibro Polar PLAF109 meal scheduler and feed sequence.
// Stock references (analysis/static/README.md 2, 2b, 3): Feed_Handle @0x4200ae46, One_PlanCheck @0x4200ab0a,
// checkOnePlanBeforeOneHour @0x42008bca, BSP_setExistState @0x4200cfae.
//
// Run model (OURS, replaces the stock dated plan list): a refill marks compartment 1 as next; every meal in
// rotate_and_open mode consumes the next compartment; when all filled compartments are used the run is "empty"
// and scheduled rotations stop until the next refill. lid_only mode never rotates.
//
// serve_and_return (OURS, docs/feeding-strategies.md): food is kept in the storage compartments at the
// cooled rear; the rest compartment (empty, default 1) sits under the lid. Every serve interval: bring the
// next storage compartment to the front, open, (call the pet), wait for the pet, close once the pet has been
// gone for the "pet gone" time (or nobody came within the no-show time, or the max-open time ran out), then
// return the tray to the rest compartment so leftovers go back to the cold.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/datetime/time_entity.h"
#include "esphome/components/number/number.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/polar_tray/polar_tray.h"
#include "esphome/components/polar_lid/polar_lid.h"

#include <string>
#include <vector>

namespace esphome::polar_feeder {

enum class FeedState : uint8_t { IDLE = 0, TRAY, OPENING, ONGOING, WAIT_CLEAR, CLOSING };
enum class FeedMode : uint8_t { ROTATE_AND_OPEN = 0, LID_ONLY = 1, SERVE_AND_RETURN = 2 };

struct Meal {
  datetime::TimeEntity *time{nullptr};
  switch_::Switch *enabled{nullptr};
  number::Number *open_minutes{nullptr};  // per-meal override: < 0 = use global, 0 = until close command
  int64_t fed_occ{-1};
  int64_t prepared_occ{-1};
  int64_t thaw_occ{-1};
};

class PolarFeeder : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  void set_tray(polar_tray::PolarTray *t) { this->tray_ = t; }
  void set_lid(polar_lid::PolarLid *l) { this->lid_ = l; }
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_pet_sensor(binary_sensor::BinarySensor *s) { this->pet_sensor_ = s; }
  void set_temperature(sensor::Sensor *s) { this->temperature_ = s; }
  void set_ir_emitter(output::FloatOutput *o) { this->ir_emitter_ = o; }
  void set_mode_select(select::Select *s) { this->mode_select_ = s; }
  void set_open_minutes_number(number::Number *n) { this->open_minutes_ = n; }
  void set_lead_minutes_number(number::Number *n) { this->lead_minutes_ = n; }
  void set_filled_number(number::Number *n) { this->filled_ = n; }
  void set_default_open_minutes(uint32_t m) { this->default_open_minutes_ = m; }
  void set_default_lead_minutes(uint32_t m) { this->default_lead_minutes_ = m; }
  void add_meal(datetime::TimeEntity *t, switch_::Switch *en, number::Number *open) {
    Meal m;
    m.time = t;
    m.enabled = en;
    m.open_minutes = open;
    this->meals_.push_back(m);
  }
  void set_thaw_threshold(float t) { this->thaw_threshold_ = t; }
  void set_thaw_lead(uint32_t s) { this->thaw_lead_s_ = s; }
  void set_pet_leave_time(uint32_t ms) { this->pet_leave_ms_ = ms; }
  void set_stall_retry_delay(uint32_t ms) { this->stall_retry_delay_ms_ = ms; }
  void set_max_close_stalls(uint8_t n) { this->max_close_stalls_ = n; }
  void set_ir_level(float l) { this->ir_level_ = l; }
  void set_ir_settle(uint32_t ms) { this->ir_settle_ms_ = ms; }
  void set_boot_grace(uint32_t ms) { this->boot_grace_ms_ = ms; }
  void add_power_switch(switch_::Switch *s) { this->power_switches_.push_back(s); }
  // serve_and_return settings (all optional; defaults used when an entity is missing or has no state)
  void set_serve_enabled_switch(switch_::Switch *s) { this->serve_enabled_ = s; }
  void set_serve_start_time(datetime::TimeEntity *t) { this->serve_start_ = t; }
  void set_serve_interval_number(number::Number *n) { this->serve_interval_ = n; }
  void set_serve_choice_select(select::Select *s) { this->serve_choice_ = s; }
  void set_serve_no_show_number(number::Number *n) { this->serve_no_show_ = n; }
  void set_serve_pet_gone_number(number::Number *n) { this->serve_pet_gone_ = n; }
  void set_serve_max_open_number(number::Number *n) { this->serve_max_open_ = n; }
  void set_serve_rest_compartment(int c) { this->serve_rest_ = c; }
  void set_serve_settle(uint32_t ms) { this->serve_settle_ms_ = ms; }
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_ = s; }
  void set_event_text_sensor(text_sensor::TextSensor *s) { this->event_text_ = s; }
  void set_next_meal_text_sensor(text_sensor::TextSensor *s) { this->next_text_ = s; }
  void set_next_compartment_sensor(sensor::Sensor *s) { this->next_comp_sensor_ = s; }
  void set_empty_binary_sensor(binary_sensor::BinarySensor *s) { this->empty_bs_ = s; }
  void set_feeding_binary_sensor(binary_sensor::BinarySensor *s) { this->feeding_bs_ = s; }
  void set_thaw_binary_sensor(binary_sensor::BinarySensor *s) { this->thaw_bs_ = s; }
  void set_anti_pinch_binary_sensor(binary_sensor::BinarySensor *s) { this->anti_pinch_bs_ = s; }

  /// Feed now: rotate_and_open -> next compartment (consumed); lid_only -> open over whatever is there;
  /// serve_and_return -> one serve cycle now with the next storage compartment.
  /// minutes < 0 = global lid-open time, 0 = stay open until close_now().
  bool feed_now(int minutes);
  /// Open the lid without rotating or consuming a compartment; stays open until close_now().
  bool open_lid();
  /// End the open period and close with anti-pinch (deferred while presence is detected). Seam for an
  /// external controller (e.g. a future load-cell scale) to end a meal early.
  void close_now();
  /// Refill: compartment 1 is next; in rotate_and_open mode the tray is homed (compartment 1 under the lid).
  /// serve_and_return: the first storage compartment is served next; the tray goes to the rest compartment.
  bool refilled();
  /// Boot close (stock DoorMotor_Reset_ThreadHandle): anti-pinch close with the boot sensor profile.
  bool boot_close();
  /// Abort the sequence without moving anything (stop-all; motors are braked by their own stop()).
  void cancel(const char *reason = "cancelled");

  bool is_feeding() const { return this->state_ != FeedState::IDLE; }
  /// True while a meal or "Feed now" is running (not a plain "Open lid"). Used by the "call the pet" sound
  /// hook in on_lid_opened (polar_sound); it changes no feeder behaviour.
  bool is_serving() const { return this->is_feeding() && this->feed_serves_; }
  bool is_empty() const { return this->next_ > this->filled_count_(); }
  int get_next_compartment() const { return this->is_empty() ? 0 : this->next_; }
  FeedMode get_mode() const;
  /// Thaw rule: cooling must stay off (checkOnePlanBeforeOneHour @0x42008bca).
  bool cooling_suspended() const { return this->thaw_latch_; }

  Trigger<int> *get_prepare_trigger() { return &this->prepare_trigger_; }
  Trigger<int> *get_feed_start_trigger() { return &this->feed_start_trigger_; }
  Trigger<int> *get_lid_opened_trigger() { return &this->lid_opened_trigger_; }
  Trigger<int, std::string> *get_feed_end_trigger() { return &this->feed_end_trigger_; }

 protected:
  bool start_feed_(int compartment, int32_t minutes, int meal_index, bool consume, bool serves = true);
  bool start_serve_(int occurrence_index);
  void serve_ongoing_(uint32_t now);
  void serve_close_(const char *why);
  int serve_compartment_() const;
  void serve_advance_();
  int serve_storage_count_() const;
  bool serve_storage_(int c) const;
  uint32_t serve_minutes_ms_(number::Number *n, float def) const;
  void serve_schedule_tick_(const ESPTime &t, bool in_grace);
  void end_feed_(const char *result);
  bool position_tray_(int compartment);
  void begin_close_();
  bool idle_close_(polar_lid::LidProfile profile, const char *why);
  void after_open_();
  void power_on_();
  void save_();
  void set_state_(FeedState s, const char *text);
  void event_(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void schedule_tick_();
  void meal_due_(size_t i);
  void set_thaw_(bool on);
  void ir_(bool on);
  void set_next_(int n);
  void publish_run_();
  int filled_count_() const;
  size_t active_slots_() const;
  int32_t meal_open_minutes_(const Meal &m) const;
  int32_t global_open_minutes_() const;
  uint32_t lead_s_() const;
  bool pet_present_() const { return this->pet_sensor_ != nullptr && this->pet_sensor_->state; }
  bool serve_mode_() const { return this->get_mode() == FeedMode::SERVE_AND_RETURN; }

  polar_tray::PolarTray *tray_{nullptr};
  polar_lid::PolarLid *lid_{nullptr};
  time::RealTimeClock *time_{nullptr};
  binary_sensor::BinarySensor *pet_sensor_{nullptr};
  sensor::Sensor *temperature_{nullptr};
  output::FloatOutput *ir_emitter_{nullptr};
  select::Select *mode_select_{nullptr};
  number::Number *open_minutes_{nullptr};
  number::Number *lead_minutes_{nullptr};
  number::Number *filled_{nullptr};
  uint32_t default_open_minutes_{240};
  uint32_t default_lead_minutes_{30};
  std::vector<Meal> meals_;
  float thaw_threshold_{13.0f};
  uint32_t thaw_lead_s_{3600};
  uint32_t pet_leave_ms_{10000};
  uint32_t stall_retry_delay_ms_{30000};
  uint8_t max_close_stalls_{3};
  float ir_level_{127.0f / 256.0f};
  uint32_t ir_settle_ms_{1000};
  uint32_t boot_grace_ms_{60000};
  std::vector<switch_::Switch *> power_switches_;
  text_sensor::TextSensor *state_text_{nullptr};
  text_sensor::TextSensor *event_text_{nullptr};
  text_sensor::TextSensor *next_text_{nullptr};
  sensor::Sensor *next_comp_sensor_{nullptr};
  binary_sensor::BinarySensor *empty_bs_{nullptr};
  binary_sensor::BinarySensor *feeding_bs_{nullptr};
  binary_sensor::BinarySensor *thaw_bs_{nullptr};
  binary_sensor::BinarySensor *anti_pinch_bs_{nullptr};
  Trigger<int> prepare_trigger_;
  Trigger<int> feed_start_trigger_;
  Trigger<int> lid_opened_trigger_;
  Trigger<int, std::string> feed_end_trigger_;

  ESPPreferenceObject pref_;
  int next_{1};
  FeedState state_{FeedState::IDLE};
  int feed_compartment_{0};
  int feed_meal_{-1};
  int32_t feed_minutes_{0};
  bool feed_consume_{false};
  bool feed_serves_{false};  // meal or feed-now (true), plain open_lid (false); see is_serving()
  bool tray_moved_{false};
  int pending_goto_{0};  // compartment to go to once homing finishes (0 = none)
  uint32_t phase_start_ms_{0};
  uint32_t last_pet_ms_{0};
  uint32_t hold_until_ms_{0};
  bool hold_active_{false};
  bool ir_on_{false};
  uint32_t ir_on_since_ms_{0};
  bool close_then_rotate_{false};
  polar_lid::LidProfile close_profile_{polar_lid::LidProfile::FEED};
  int64_t last_fed_ts_{0};
  uint32_t boot_ms_{0};
  bool grace_over_{false};
  uint8_t close_stalls_{0};
  uint16_t pet_reopens_{0};
  bool thaw_latch_{false};
  uint32_t last_schedule_ms_{0};
  uint32_t last_next_ms_{0};

  // serve_and_return
  switch_::Switch *serve_enabled_{nullptr};
  datetime::TimeEntity *serve_start_{nullptr};
  number::Number *serve_interval_{nullptr};
  select::Select *serve_choice_{nullptr};
  number::Number *serve_no_show_{nullptr};
  number::Number *serve_pet_gone_{nullptr};
  number::Number *serve_max_open_{nullptr};
  int serve_rest_{1};
  uint32_t serve_settle_ms_{3000};
  int serve_next_{0};          // storage compartment served next (persisted; 0 = first storage compartment)
  bool feed_serve_{false};     // the running feed is a serve cycle
  bool serve_returning_{false};
  bool serve_pet_seen_{false};
  uint32_t serve_first_pet_ms_{0};
  uint32_t serve_open_ms_{0};
  const char *serve_close_reason_{""};
};

template<typename... Ts> class FeedNowAction final : public Action<Ts...>, public Parented<PolarFeeder> {
 public:
  TEMPLATABLE_VALUE(int, minutes)
  void play(const Ts &...x) override { this->parent_->feed_now(this->minutes_.value(x...)); }
};
template<typename... Ts> class OpenLidAction final : public Action<Ts...>, public Parented<PolarFeeder> {
 public:
  void play(const Ts &...x) override { this->parent_->open_lid(); }
};
template<typename... Ts> class CloseNowAction final : public Action<Ts...>, public Parented<PolarFeeder> {
 public:
  void play(const Ts &...x) override { this->parent_->close_now(); }
};
template<typename... Ts> class RefilledAction final : public Action<Ts...>, public Parented<PolarFeeder> {
 public:
  void play(const Ts &...x) override { this->parent_->refilled(); }
};
template<typename... Ts> class BootCloseAction final : public Action<Ts...>, public Parented<PolarFeeder> {
 public:
  void play(const Ts &...x) override { this->parent_->boot_close(); }
};
template<typename... Ts> class CancelAction final : public Action<Ts...>, public Parented<PolarFeeder> {
 public:
  void play(const Ts &...x) override { this->parent_->cancel(); }
};

}  // namespace esphome::polar_feeder

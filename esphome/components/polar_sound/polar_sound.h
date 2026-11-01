#pragma once
// Petlibro Polar PLAF109 sound chip on GPIO6 (one-wire, WTN6-family protocol inferred).
//
// Stock references (analysis/static/README.md, docs/esphome-firmware.md "Sound"):
//   BSP_aduioInit   @0x4200d738  GPIO6 output (mode 2), no pulls, level 1 (idle high)
//   FUN_ram_4200d7e8            send one byte: low 5 ms, 8 bits LSB first, "1" = high 3 / low 1 ms,
//                               "0" = high 1 / low 3 ms, then high. Gated by FUN_ram_4200c276.
//   FUN_ram_4200c276            gate: dev_attr volume byte (+0x15) != 0, soundSwitch (+0x39) == 1,
//                               soundAgingType (+0x3a) 1 = always, 2 = only inside start..end (+0x3b..0x3e)
//   FUN_ram_4200a23e            ringer (ringerMode SMART): sound 1 every ringerInterval min for
//                               ringerDuration min (+10 s) after the lid opened, until GPIO20 sees a pet
// Stock sends only two bytes: 0xEF (Feed_Handle @0x4200ae46, feed job accepted) and 0x01 (lid open
// FEED_PLAN_openDoorHandle @0x4200945e, ringer FUN_ram_4200a23e, cloud "AUDIO" test FUN_ram_42018c3a).
// Volume 0xE0..0xEF, loop 0xF2 and stop 0xFE are WTN6 datasheet values, never used by stock [I].
//
// OURS: the byte is clocked out by an esp_timer one-shot chain (no blocking in loop()), with a queue, a
// minimum gap between frames, a volume prefix before each play, quiet hours and a "call the pet" repeater.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/datetime/time_entity.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"

#include <esp_timer.h>
#include <atomic>
#include <cstdint>

namespace esphome::polar_sound {

static const uint8_t MAX_VOICE_INDEX = 0xDF;  // WTN6: 0x00..0xDF voices [I]
static const uint8_t VOLUME_BASE = 0xE0;      // WTN6: 0xE0 + level, 16 levels; stock sends 0xEF only [I]
static const uint8_t VOLUME_MAX_LEVEL = 15;
static const uint8_t CMD_LOOP = 0xF2;         // WTN6: loop the current voice [I], never sent by stock
static const uint8_t CMD_STOP = 0xFE;         // WTN6: stop [I], never sent by stock
static const size_t QUEUE_LEN = 16;
static const size_t MAX_SEGMENTS = 17;        // start low + 8 x (high, low)

class PolarSound;

/// "Play sound" (plays the chosen index on every selection) and "Call sound" (remembered choice).
class SoundSelect : public select::Select, public Parented<PolarSound> {
 public:
  void set_play_on_select(bool p) { this->play_on_select_ = p; }
  void setup_state();

 protected:
  void control(size_t index) override;
  bool play_on_select_{false};
  ESPPreferenceObject pref_;
};

class PolarSound : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_pin(InternalGPIOPin *pin) { this->pin_ = pin; }
  void set_timing(uint32_t start_us, uint32_t long_us, uint32_t short_us) {
    this->start_us_ = start_us;
    this->long_us_ = long_us;
    this->short_us_ = short_us;
  }
  void set_command_gap(uint32_t ms) { this->gap_us_ = ms * 1000; }
  void set_volume_before_play(bool v) { this->volume_before_play_ = v; }
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_enable(switch_::Switch *s) { this->enable_ = s; }
  void set_volume_number(number::Number *n) { this->volume_ = n; }
  void set_quiet(switch_::Switch *en, datetime::TimeEntity *start, datetime::TimeEntity *end) {
    this->quiet_en_ = en;
    this->quiet_start_ = start;
    this->quiet_end_ = end;
  }
  void set_pet_sensor(binary_sensor::BinarySensor *s) { this->pet_ = s; }
  void set_pet_settle(uint32_t ms) { this->pet_settle_ms_ = ms; }
  void set_call_enable(switch_::Switch *s) { this->call_en_ = s; }
  void set_call_repeats(number::Number *n) { this->call_repeats_ = n; }
  void set_call_interval(number::Number *n) { this->call_interval_ = n; }
  void set_call_stop_on_pet(switch_::Switch *s) { this->call_stop_on_pet_ = s; }
  void set_default_call_index(uint8_t i) { this->default_call_index_ = i; }
  void set_play_select(SoundSelect *s) { this->play_select_ = s; }
  void set_call_select(SoundSelect *s) { this->call_select_ = s; }
  void set_status(text_sensor::TextSensor *t) { this->status_text_ = t; }

  /// Play a voice index (0..0xDF). Gated by the enable switch and, unless force, by quiet hours.
  /// With volume_before_play the volume byte from the volume number is queued first.
  bool play(int index, bool force = false);
  /// Queue the volume command 0xE0 + level (0..15) now. Not gated (a WTN6 volume byte is silent [I]).
  bool set_volume(int level);
  /// Queue any byte (test use). Gated like play() for 0x00..0xDF, not gated for control bytes.
  bool send(int code, bool force = false);
  /// Drop the queue, end a call, and queue the stop byte 0xFE [I].
  void stop();
  /// Loop the current voice (0xF2 [I]); stop() ends it.
  bool loop_current();

  /// Call the pet: play the call sound now and repeat per "Call repeats"/"Call repeat interval".
  /// meal = true: started by the feeding hook, needs the "Call the pet at meals" switch.
  void call_start(bool meal);
  /// End a running call and drop queued sounds (no stop byte: whatever plays finishes).
  void call_stop(const char *reason);
  bool is_call_active() const { return this->call_active_; }
  bool is_busy() const { return this->tx_busy_.load() || this->q_count_ > 0; }

  uint8_t call_index() const;

 protected:
  static void timer_cb_(void *arg);
  void on_timer_();
  void start_frame_(uint8_t code);
  bool enqueue_(uint8_t code);
  bool allowed_(bool force, const char **why) const;
  bool in_quiet_hours_() const;
  int volume_level_() const;
  void ring_();
  void status_(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

  InternalGPIOPin *pin_{nullptr};
  uint32_t start_us_{5000};
  uint32_t long_us_{3000};
  uint32_t short_us_{1000};
  uint32_t gap_us_{20000};
  bool volume_before_play_{true};

  time::RealTimeClock *time_{nullptr};
  switch_::Switch *enable_{nullptr};
  number::Number *volume_{nullptr};
  switch_::Switch *quiet_en_{nullptr};
  datetime::TimeEntity *quiet_start_{nullptr};
  datetime::TimeEntity *quiet_end_{nullptr};
  binary_sensor::BinarySensor *pet_{nullptr};
  uint32_t pet_settle_ms_{2000};
  switch_::Switch *call_en_{nullptr};
  number::Number *call_repeats_{nullptr};
  number::Number *call_interval_{nullptr};
  switch_::Switch *call_stop_on_pet_{nullptr};
  uint8_t default_call_index_{1};
  SoundSelect *play_select_{nullptr};
  SoundSelect *call_select_{nullptr};
  text_sensor::TextSensor *status_text_{nullptr};

  // transmitter (segments are written by loop() only while idle, read by the timer callback)
  esp_timer_handle_t timer_{nullptr};
  struct Segment {
    bool level;
    uint32_t us;
  };
  Segment seg_[MAX_SEGMENTS];
  volatile uint8_t seg_n_{0};
  volatile uint8_t seg_i_{0};
  std::atomic<bool> tx_busy_{false};
  volatile int64_t tx_start_us_{0};
  volatile int64_t tx_end_us_{0};
  uint8_t tx_code_{0};
  bool tx_logged_{true};

  uint8_t q_[QUEUE_LEN];
  size_t q_head_{0};
  size_t q_count_{0};

  // call
  bool call_active_{false};
  bool call_meal_{false};  // started by the feeding hook (IR emitter on, pet check valid)
  uint32_t call_start_ms_{0};
  uint32_t call_last_ms_{0};
  int call_done_{0};   // repeats played after the first call
  int call_total_{0};  // repeats planned
};

template<typename... Ts> class PlayAction final : public Action<Ts...>, public Parented<PolarSound> {
 public:
  TEMPLATABLE_VALUE(int, index)
  TEMPLATABLE_VALUE(bool, force)
  void play(const Ts &...x) override { this->parent_->play(this->index_.value(x...), this->force_.value(x...)); }
};
template<typename... Ts> class SendAction final : public Action<Ts...>, public Parented<PolarSound> {
 public:
  TEMPLATABLE_VALUE(int, code)
  TEMPLATABLE_VALUE(bool, force)
  void play(const Ts &...x) override { this->parent_->send(this->code_.value(x...), this->force_.value(x...)); }
};
template<typename... Ts> class SetVolumeAction final : public Action<Ts...>, public Parented<PolarSound> {
 public:
  TEMPLATABLE_VALUE(int, level)
  void play(const Ts &...x) override { this->parent_->set_volume(this->level_.value(x...)); }
};
template<typename... Ts> class StopAction final : public Action<Ts...>, public Parented<PolarSound> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};
template<typename... Ts> class LoopAction final : public Action<Ts...>, public Parented<PolarSound> {
 public:
  void play(const Ts &...x) override { this->parent_->loop_current(); }
};
template<typename... Ts> class CallStartAction final : public Action<Ts...>, public Parented<PolarSound> {
 public:
  void set_meal(bool m) { this->meal_ = m; }
  void play(const Ts &...x) override { this->parent_->call_start(this->meal_); }

 protected:
  bool meal_{false};
};
template<typename... Ts> class CallStopAction final : public Action<Ts...>, public Parented<PolarSound> {
 public:
  void play(const Ts &...x) override { this->parent_->call_stop("stopped"); }
};

template<typename... Ts> class CallActiveCondition final : public Condition<Ts...>, public Parented<PolarSound> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_call_active(); }
};

}  // namespace esphome::polar_sound

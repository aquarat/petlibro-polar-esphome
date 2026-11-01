#include "polar_cooling.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome::polar_cooling {

static const char *const TAG = "polar_cooling";
static const float SETPOINT_MIN = 2.0f;   // OURS: user requirement, avoid freezing the food / icing the NTC
static const float SETPOINT_MAX = 15.0f;
static const float STOCK_SETPOINT = 12.0f;  // @0x3c0fae68
static const uint32_t BUCKET_MS = 60000;
static const uint32_t PUBLISH_MS = 30000;

void PolarCooling::setup() {
  // Never on at boot. Stock Ptc_init sets P1_4=1 briefly at init (emulation 5.1 step 5); we do not.
  this->apply_(false, "boot");
  const uint32_t now = millis();
  this->off_since_ms_ = now - this->min_off_ms_;  // allow the first start without waiting
  this->last_tick_ms_ = now;
  this->bucket_start_ms_ = now;
  this->last_eval_ms_ = now;
}

void PolarCooling::on_shutdown() {
  if (this->peltier_ != nullptr)
    this->peltier_->turn_off();
  this->on_ = false;
}

void PolarCooling::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Polar cooling (UNTESTED: fan wiring unknown):\n"
                "  Allowed at compile time: %s\n"
                "  Evaluation interval: %" PRIu32 " ms, minimum off time: %" PRIu32 " ms\n"
                "  Setpoint %.1f C, hysteresis %.1f C, max continuous on %" PRIu32 " ms (0 = no limit)\n"
                "  Freeze guard: off below %.1f C",
                YESNO(this->allowed_), this->interval_ms_, this->min_off_ms_, this->setpoint_c_(),
                this->hysteresis_c_(), this->max_on_ms_(), this->freeze_guard_);
}

float PolarCooling::setpoint_c_() const {
  float v = STOCK_SETPOINT;
  if (this->setpoint_ != nullptr && !std::isnan(this->setpoint_->state))
    v = this->setpoint_->state;
  return clamp(v, SETPOINT_MIN, SETPOINT_MAX);
}

float PolarCooling::hysteresis_c_() const {
  if (this->hysteresis_ == nullptr || std::isnan(this->hysteresis_->state))
    return 0.0f;  // stock: none
  return clamp(this->hysteresis_->state, 0.0f, 5.0f);
}

uint32_t PolarCooling::max_on_ms_() const {
  if (this->max_on_ == nullptr || std::isnan(this->max_on_->state))
    return 10 * 60000;
  return (uint32_t) lroundf(std::max(this->max_on_->state, 0.0f)) * 60000;
}

void PolarCooling::status_(const char *s) {
  if (this->state_text_ != nullptr && this->state_text_->state != s)
    this->state_text_->publish_state(s);
}

void PolarCooling::apply_(bool on, const char *why) {
  const uint32_t now = millis();
  if (on != this->on_) {
    ESP_LOGI(TAG, "Peltier %s (%s)", on ? "ON" : "OFF", why);
    if (on) {
      this->on_since_ms_ = now;
    } else {
      this->off_since_ms_ = now;
    }
  }
  if (on && this->assert_power_) {
    // Assert the stock DC state of P0_4/P0_7 before the Peltier (fan wiring unknown, may depend on them).
    for (auto *s : this->power_switches_) {
      if (!s->state)
        s->turn_on();
    }
  }
  this->on_ = on;
  if (this->peltier_ != nullptr && this->peltier_->state != on) {
    if (on) {
      this->peltier_->turn_on();
    } else {
      this->peltier_->turn_off();
    }
  }
}

void PolarCooling::loop() {
  const uint32_t now = millis();

  // ---- duty-cycle bookkeeping (rolling hour of one-minute buckets)
  const uint32_t dt = now - this->last_tick_ms_;
  this->last_tick_ms_ = now;
  if (this->on_)
    this->bucket_ms_[this->bucket_idx_] += dt;
  if (now - this->bucket_start_ms_ >= BUCKET_MS) {
    this->bucket_start_ms_ += BUCKET_MS;
    if (this->buckets_filled_ < 60)
      this->buckets_filled_++;
    this->bucket_idx_ = (this->bucket_idx_ + 1) % 60;
    this->bucket_ms_[this->bucket_idx_] = 0;
  }
  if (now - this->last_publish_ms_ >= PUBLISH_MS) {
    this->last_publish_ms_ = now;
    uint64_t sum = 0;
    for (uint32_t b : this->bucket_ms_)
      sum += b;
    const uint64_t window = (uint64_t) this->buckets_filled_ * BUCKET_MS + (now - this->bucket_start_ms_);
    this->duty_ = window > 0 ? (float) (100.0 * (double) sum / (double) window) : 0.0f;
    if (this->duty_sensor_ != nullptr)
      this->duty_sensor_->publish_state(this->duty_);
    if (this->since_off_sensor_ != nullptr)
      this->since_off_sensor_->publish_state((now - this->off_since_ms_) / 60000.0f);
  }

  // ---- hard off conditions, checked continuously
  const float temp = this->temperature_ != nullptr ? this->temperature_->state : NAN;
  const char *block = nullptr;
  if (!this->allowed_) {
    block = "disabled at compile time";
  } else if (this->enable_ == nullptr || !this->enable_->state) {
    block = "disabled";
  } else if (this->on_battery_ != nullptr && this->on_battery_->state) {
    block = "off: on battery";  // Power_Handle / CTRL_THREAD_coolHanlde: never on battery
  } else if (std::isnan(temp)) {
    block = "off: no temperature";
  } else if (temp < this->freeze_guard_) {
    block = "off: freeze guard";
  } else if (this->thaw_rule_ != nullptr && this->thaw_rule_->state && this->feeder_ != nullptr &&
             this->feeder_->cooling_suspended()) {
    block = "off: thaw before meal";  // stock thaw rule, optional here
  }
  if (block != nullptr) {
    this->want_on_ = false;
    this->apply_(false, block);
    this->status_(block);
    this->force_eval_ = true;  // evaluate as soon as the block clears
    return;
  }

  const uint32_t max_on = this->max_on_ms_();
  if (this->on_ && max_on > 0 && now - this->on_since_ms_ >= max_on) {
    ESP_LOGE(TAG, "Peltier on for %" PRIu32 " min continuously: max-on limit, cooling DISABLED", max_on / 60000);
    this->apply_(false, "max-on limit");
    this->want_on_ = false;
    this->enable_->turn_off();  // latched: the user must re-enable
    this->status_("disabled: max-on limit reached");
    return;
  }

  // ---- sample-and-hold decision (stock: every 300 s)
  if (this->force_eval_ || now - this->last_eval_ms_ >= this->interval_ms_) {
    this->force_eval_ = false;
    this->last_eval_ms_ = now;
    const float sp = this->setpoint_c_();
    const float hy = this->hysteresis_c_();
    if (temp <= sp) {
      this->want_on_ = false;
    } else if (temp > sp + hy) {
      this->want_on_ = true;
    }  // inside the band: hold the previous decision
    ESP_LOGD(TAG, "Evaluate: %.2f C, setpoint %.1f, hysteresis %.1f -> %s", temp, sp, hy,
             this->want_on_ ? "on" : "off");
  }
  if (this->want_on_ && !this->on_) {
    if (now - this->off_since_ms_ < this->min_off_ms_) {
      this->status_("waiting: minimum off time");
      return;
    }
    this->apply_(true, "above setpoint");
  } else if (!this->want_on_ && this->on_) {
    this->apply_(false, "at or below setpoint");
  }
  this->status_(this->on_ ? "cooling" : "idle (at setpoint)");
}

}  // namespace esphome::polar_cooling

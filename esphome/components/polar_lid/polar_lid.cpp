#include "polar_lid.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome::polar_lid {

static const char *const TAG = "polar_lid";

static const uint32_t MONITOR_PERIOD_MS = 500;    // control loop period, vTaskDelay(500) @0x4200f6d0
static const uint32_t STALL_BRAKE_MS = 1000;      // same recovery timing as the tray (static README section 2)
static const uint32_t MANUAL_IGNORE_MS = 2000;    // Timer_DoorSignalHandle @0x4200de5a: sensor ignored first 2 s
static const uint32_t MANUAL_LIMIT_MS = 20000;    // ... then stop at GPIO19 == 1 or after 40 loops (20 s)
static const uint32_t BOOT_IGNORE_MS = 2000;      // DoorMotor_Reset_ThreadHandle @0x4200ec58: forward 2000 ms,
static const uint32_t BOOT_LIMIT_MS = 7000;       // ... then poll GPIO19 every 100 ms up to 5 s, brake (no run-on)
static const uint32_t REOPEN_IGNORE_MS = 2000;    // OURS: safety reopen ignores the sensor for >= 2 s
static const uint32_t JOG_MAX_MS = 2000;          // OURS: test aid
static const uint32_t BRAKE_REASSERT_MS = 1000;   // OURS

void PolarLid::setup() {
  this->pin_close_->setup();
  this->pin_open_->setup();
  this->motor_(Motor::BRAKE);  // Door_Motor_init @0x4200d1e8: P1_7=1, P1_6=1
  this->set_state_(LidState::UNKNOWN, "unknown");
}

void PolarLid::dump_config() {
  ESP_LOGCONFIG(TAG, "Polar lid:");
  LOG_PIN("  Close pin (P1_7): ", this->pin_close_);
  LOG_PIN("  Open pin (P1_6): ", this->pin_open_);
  ESP_LOGCONFIG(TAG,
                "  Sensor end-stop level: %s, minimum travel %" PRIu32 " ms\n"
                "  Open stall: > %.3f V x %u samples @500 ms, %u retries%s\n"
                "  Close stall: > %.3f V x %u samples @%" PRIu32 " ms after %" PRIu32 " ms inrush -> reopen\n"
                "  Feed windows: transition accepted < %" PRIu32 " ms, stop at %" PRIu32 " ms, run-on %" PRIu32
                " ms\n"
                "  Max move: %" PRIu32 " ms",
                this->end_level_ ? "high" : "low", this->min_travel_ms_, this->stall_threshold_,
                this->stall_samples_, this->stall_retries_max_,
                this->current_ == nullptr ? " (no current sampler: DISABLED)" : "", this->stall_threshold_,
                this->close_stall_samples_, this->close_sample_ms_, this->inrush_ms_, this->feed_check_ms_,
                this->feed_nocheck_ms_, this->run_on_ms_, this->max_move_ms_);
}

void PolarLid::motor_(Motor m) {
  // Lid H-bridge P1_7 / P1_6: close ("GPIO_DOOR_MOTOR_FORWARD") 1,0; open ("GPIO_DOOR_MOTOR_BACK") 0,1; brake 1,1
  // (Door_Motor_init @0x4200d1e8, Device_Module_Ctrl @0x4200ea0e, FEED_PLAN_openDoorHandle @0x4200945e).
  this->pin_close_->digital_write(true);
  this->pin_open_->digital_write(true);
  if (m == Motor::CLOSE) {
    this->pin_open_->digital_write(false);
  } else if (m == Motor::OPEN) {
    this->pin_close_->digital_write(false);
  }
  this->motor_state_ = m;
}

bool PolarLid::start_(bool opening, LidProfile profile, bool check_pet, bool is_retry) {
  const uint32_t now = millis();
  if (!is_retry) {
    if (this->inhibited_) {
      ESP_LOGW(TAG, "Lid move refused: inhibited");
      return false;
    }
    if (this->state_ == LidState::FAULT) {
      ESP_LOGW(TAG, "Lid move refused: in fault, clear it first");
      return false;
    }
    if (this->is_busy()) {
      ESP_LOGW(TAG, "Lid move refused: busy");
      return false;
    }
    this->retries_ = 0;
    this->move_start_ms_ = now;
    for (auto *s : this->power_switches_) {
      if (!s->state)
        s->turn_on();
    }
  }
  this->opening_ = opening;
  this->profile_ = profile;
  this->check_pet_ = check_pet && !opening;
  switch (profile) {
    case LidProfile::FEED:
      // doorCheckSignalTime 5 s (dev_attr +0xb) / doorNotcheckSignalTime 10 s (dev_attr +0xc), run on 1000 ms.
      // Stock accepts GPIO19 from t = 0; OURS: only a transition after min_travel (open question 8).
      this->ignore_ms_ = this->min_travel_ms_;
      this->accept_until_ms_ = this->feed_check_ms_;
      this->limit_ms_ = this->feed_nocheck_ms_;
      this->run_on_cfg_ms_ = this->run_on_ms_;
      break;
    case LidProfile::REOPEN:
      this->ignore_ms_ = std::max(REOPEN_IGNORE_MS, this->min_travel_ms_);
      this->accept_until_ms_ = std::max(this->feed_check_ms_, this->ignore_ms_ + 1000);
      this->limit_ms_ = std::max(this->feed_nocheck_ms_, this->accept_until_ms_);
      this->run_on_cfg_ms_ = this->run_on_ms_;
      break;
    case LidProfile::MANUAL:
      this->ignore_ms_ = std::max(MANUAL_IGNORE_MS, this->min_travel_ms_);
      this->accept_until_ms_ = MANUAL_LIMIT_MS;
      this->limit_ms_ = MANUAL_LIMIT_MS;
      this->run_on_cfg_ms_ = this->run_on_ms_;
      break;
    case LidProfile::BOOT:
      this->ignore_ms_ = std::max(BOOT_IGNORE_MS, this->min_travel_ms_);
      this->accept_until_ms_ = BOOT_LIMIT_MS;
      this->limit_ms_ = BOOT_LIMIT_MS;
      this->run_on_cfg_ms_ = 0;
      break;
  }
  this->run_on_active_ = false;
  this->over_count_ = 0;
  this->phase_start_ms_ = now;
  this->last_sample_ms_ = now;
  // Transition tracking (OURS): a move ends on the sensor only after it has left the end-stop level.
  this->initial_raw_ = this->lid_sensor_ != nullptr && this->lid_sensor_->state;
  this->started_at_end_ = this->at_end_() || this->state_ == LidState::OPEN || this->state_ == LidState::CLOSED;
  this->left_end_ = !this->at_end_();
  this->sensor_changed_ = false;
  this->start_state_ = this->state_;
  this->motor_(opening ? Motor::OPEN : Motor::CLOSE);
  ESP_LOGI(TAG, "Lid %s (profile %u%s%s), sensor %s", opening ? "OPENING" : "CLOSING", (unsigned) profile,
           this->check_pet_ ? ", pet check" : "", is_retry ? ", retry" : "", this->at_end_() ? "at end" : "between");
  this->set_state_(opening ? LidState::OPENING : LidState::CLOSING, opening ? "opening" : "closing");
  return true;
}

bool PolarLid::open(LidProfile profile) { return this->start_(true, profile, false, false); }

bool PolarLid::close(LidProfile profile, bool check_pet) {
  this->pet_reopened_ = false;
  this->stall_reopened_ = false;
  return this->start_(false, profile, check_pet, false);
}

bool PolarLid::jog(bool open, uint32_t ms) {
  if (this->inhibited_ || this->is_busy() || this->state_ == LidState::FAULT) {
    ESP_LOGW(TAG, "Lid jog refused (inhibited, busy or fault)");
    return false;
  }
  if (ms > JOG_MAX_MS)
    ms = JOG_MAX_MS;
  for (auto *s : this->power_switches_) {
    if (!s->state)
      s->turn_on();
  }
  this->jog_ms_ = ms;
  this->phase_start_ms_ = millis();
  this->move_start_ms_ = this->phase_start_ms_;
  this->motor_(open ? Motor::OPEN : Motor::CLOSE);
  ESP_LOGI(TAG, "Lid jog %s %" PRIu32 " ms", open ? "open" : "close", ms);
  this->set_state_(LidState::JOG, "jog");
  return true;
}

void PolarLid::stop(const char *reason) {
  this->motor_(Motor::BRAKE);
  if (this->is_busy()) {
    ESP_LOGW(TAG, "Lid stopped: %s", reason);
    this->set_state_(LidState::UNKNOWN, "stopped");
  }
}

void PolarLid::clear_fault() {
  if (this->state_ == LidState::FAULT) {
    ESP_LOGI(TAG, "Lid fault cleared");
    this->set_state_(LidState::UNKNOWN, "unknown");
  }
}

void PolarLid::set_inhibit(bool inhibit, const char *reason) {
  if (inhibit && !this->inhibited_) {
    ESP_LOGW(TAG, "Lid inhibited: %s", reason);
    this->stop(reason);
  } else if (!inhibit && this->inhibited_) {
    ESP_LOGI(TAG, "Lid inhibit released (%s)", reason);
  }
  this->inhibited_ = inhibit;
}

void PolarLid::end_(const char *how) {
  this->end_how_ = how;
  if (this->run_on_cfg_ms_ == 0) {
    this->finish_(how);
  } else {
    this->run_on_active_ = true;  // stock "run on 1000 ms; brake"
    this->run_on_start_ms_ = millis();
  }
}

void PolarLid::finish_(const char *how) {
  this->motor_(Motor::BRAKE);
  const bool opened = this->opening_;
  ESP_LOGI(TAG, "Lid %s (%s, %" PRIu32 " ms)", opened ? "OPEN" : "CLOSED", how, millis() - this->move_start_ms_);
  char buf[48];
  snprintf(buf, sizeof(buf), "%s (%s)", opened ? "open" : "closed", how);
  this->set_state_(opened ? LidState::OPEN : LidState::CLOSED, buf);
  if (opened) {
    this->opened_trigger_.trigger();
  } else {
    this->closed_trigger_.trigger();
  }
}

void PolarLid::reopen_(bool pet) {
  // Stock (pet): "Identify the pet DOOR_READY_OPEN". OURS (stall): reverse at once, no brake dwell.
  ESP_LOGW(TAG, "%s while closing: reopening", pet ? "Pet detected" : "Close stall (over-current)");
  if (pet) {
    this->pet_reopened_ = true;
  } else {
    this->stall_reopened_ = true;
  }
  this->start_(true, LidProfile::REOPEN, false, true);
  if (pet)
    this->pet_reopen_trigger_.trigger();
}

void PolarLid::fault_(const char *reason) {
  this->motor_(Motor::BRAKE);
  // Stock error codes: 0x7f6 lid open stuck, 0x7f0 lid close stuck (static README section 2).
  ESP_LOGE(TAG, "LID FAULT: %s while %s (stock code 0x%03X)", reason, this->opening_ ? "opening" : "closing",
           this->opening_ ? 0x7f6 : 0x7f0);
  char buf[64];
  snprintf(buf, sizeof(buf), "fault: %s", reason);
  this->set_state_(LidState::FAULT, buf);
  this->fault_trigger_.trigger(std::string(reason));
}

void PolarLid::loop() {
  const uint32_t now = millis();
  if (this->is_busy() && now - this->move_start_ms_ > this->max_move_ms_) {
    this->fault_("hard move timeout");
    return;
  }
  const uint32_t t = now - this->phase_start_ms_;
  switch (this->state_) {
    case LidState::OPENING:
    case LidState::CLOSING: {
      const bool closing = !this->opening_;
      // Pet seen while closing (also during run-on): stop and reopen (Feed_Handle @0x4200ae46).
      if (closing && this->check_pet_ && this->pet_sensor_ != nullptr && this->pet_sensor_->state) {
        this->reopen_(true);
        return;
      }
      // Current: stock samples every 500 ms and needs 6 samples (3 s) over doorStuckCurrent (Read_DoorMotorAdc
      // @0x4200d402, 400 mV). OURS for CLOSING: sample every close_sample_ms after an inrush blanking time and
      // reverse after close_stall_samples (anti-pinch, ~0.5 s instead of ~4 s clamp).
      const uint32_t period = closing ? this->close_sample_ms_ : MONITOR_PERIOD_MS;
      if (this->current_ != nullptr && t >= this->inrush_ms_ && now - this->last_sample_ms_ >= period) {
        this->last_sample_ms_ = now;
        const float v = this->current_->sample();
        if (this->current_sensor_ != nullptr)
          this->current_sensor_->publish_state(v);
        const bool over = !std::isnan(v) && v > this->stall_threshold_;
        this->over_count_ = over ? this->over_count_ + 1 : 0;
        if (over && this->run_on_active_) {
          // The sensor already confirmed the end stop; over-current now is the stop itself, not a pinch.
          this->finish_(this->end_how_);
          return;
        }
        if (closing && this->over_count_ >= this->close_stall_samples_) {
          // OURS (hardware 2026-10-30): the sensor is high at BOTH end stops, so after a reboot (state UNKNOWN)
          // a lid that is physically closed looks the same as an open one. Closing it then drives straight into
          // the closed end stop: over-current before the sensor ever left its end level. That is "already
          // closed", not a pinch. Only from UNKNOWN or CLOSED: from a known OPEN start a stall still reopens.
          if (!this->left_end_ && this->at_end_() &&
              (this->start_state_ == LidState::UNKNOWN || this->start_state_ == LidState::CLOSED)) {
            ESP_LOGI(TAG, "Close stalled before leaving the end stop (%.3f V) from %s: lid was already closed", v,
                     this->start_state_ == LidState::UNKNOWN ? "unknown" : "closed");
            this->finish_("end stop");
            return;
          }
          this->reopen_(false);
          return;
        }
        if (!closing && this->over_count_ >= this->stall_samples_) {
          this->over_count_ = 0;
          if (this->retries_ < this->stall_retries_max_) {
            this->retries_++;
            ESP_LOGW(TAG, "Lid MOTOR_STUCK opening (%.3f V): recovery %u/%u", v, this->retries_,
                     this->stall_retries_max_);
            this->motor_(Motor::BRAKE);
            this->phase_start_ms_ = now;
            this->set_state_(LidState::STALL_BRAKE, "stall recovery: brake");
          } else {
            this->fault_("motor stuck (current)");
          }
          return;
        }
      }
      if (this->run_on_active_) {
        if (now - this->run_on_start_ms_ >= this->run_on_cfg_ms_)
          this->finish_(this->end_how_);
        return;
      }
      if (this->lid_sensor_ != nullptr) {
        if (this->lid_sensor_->state != this->initial_raw_)
          this->sensor_changed_ = true;
        if (!this->at_end_())
          this->left_end_ = true;
      }
      if (t >= this->ignore_ms_ && t < this->accept_until_ms_ && this->left_end_ && this->at_end_()) {
        this->end_("sensor");
      } else if (t >= this->limit_ms_) {
        if (this->started_at_end_ && !this->sensor_changed_) {
          // OURS: started at an end stop and the sensor never changed: sensor or motor broken.
          this->fault_("lid sensor never changed");
        } else {
          this->end_("time");  // stock: stop after doorNotcheckSignalTime regardless
        }
      }
      break;
    }
    case LidState::STALL_BRAKE:
      if (t >= STALL_BRAKE_MS) {
        // Stock lid retry motion is unknown (static README 2: "same test ... 3 retries").
        // OURS: an OPEN stall is retried in the same direction (reversing would drive the lid towards a pet).
        this->start_(true, this->profile_, false, true);
      }
      break;
    case LidState::JOG:
      if (t >= this->jog_ms_) {
        this->motor_(Motor::BRAKE);
        this->set_state_(LidState::UNKNOWN, "unknown (jogged)");
      }
      break;
    default:
      if (t >= BRAKE_REASSERT_MS) {
        this->phase_start_ms_ = now;
        this->motor_(Motor::BRAKE);
      }
      break;
  }
}

void PolarLid::set_state_(LidState s, const char *text) {
  this->state_ = s;
  if (!this->is_busy())
    this->phase_start_ms_ = millis();
  if (this->state_text_ != nullptr && text != nullptr)
    this->state_text_->publish_state(text);
  if (this->fault_bs_ != nullptr)
    this->fault_bs_->publish_state(s == LidState::FAULT);
  if (this->closed_bs_ != nullptr)
    this->closed_bs_->publish_state(s == LidState::CLOSED);
}

}  // namespace esphome::polar_lid

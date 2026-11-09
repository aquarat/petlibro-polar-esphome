#include "polar_feeder.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstdarg>

namespace esphome::polar_feeder {

static const char *const TAG = "polar_feeder";

using polar_lid::LidProfile;

static const uint32_t SCHEDULE_PERIOD_MS = 1000;  // stock checks plans every 500 ms control loop; 1 s is enough
static const uint32_t PREF_HASH = 0x706f6c32;     // "pol2": persisted run state

struct FeederPrefs {
  int32_t next;
  int32_t serve_next;  // serve_and_return: storage compartment served next (was 'reserved', 0 before)
  int64_t last_fed_ts;  // epoch of the last meal occurrence started (prevents a refire after a reboot)
} __attribute__((packed));

void PolarFeeder::setup() {
  this->boot_ms_ = millis();
  this->pref_ = global_preferences->make_preference<FeederPrefs>(PREF_HASH);
  FeederPrefs p{};
  if (this->pref_.load(&p)) {
    if (p.next >= 1 && p.next <= 13)
      this->next_ = p.next;
    this->last_fed_ts_ = p.last_fed_ts;
    this->serve_next_ = p.serve_next;
  }
  this->ir_(false);
  this->set_state_(FeedState::IDLE, "idle");
  this->set_thaw_(false);
  if (this->anti_pinch_bs_ != nullptr)
    this->anti_pinch_bs_->publish_state(false);
  this->publish_run_();
}

void PolarFeeder::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Polar feeder:\n"
                "  Meal slots: %u\n"
                "  Next compartment: %d\n"
                "  Thaw check lead: %" PRIu32 " s, threshold %.1f C (only if the thaw switch is on)\n"
                "  Pet clear time: %" PRIu32 " ms, IR settle %" PRIu32 " ms, close-stall retry delay %" PRIu32
                " ms, max close stalls %u\n"
                "  Boot grace: %" PRIu32 " ms\n"
                "  Serve and return: rest compartment %d, IR settle before counting the pet %" PRIu32 " ms",
                (unsigned) this->meals_.size(), this->next_, this->thaw_lead_s_, this->thaw_threshold_,
                this->pet_leave_ms_, this->ir_settle_ms_, this->stall_retry_delay_ms_, this->max_close_stalls_,
                this->boot_grace_ms_, this->serve_rest_, this->serve_settle_ms_);
}

// ------------------------------------------------------------------------------------------------ helpers
void PolarFeeder::event_(const char *fmt, ...) {
  char buf[112];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  ESP_LOGI(TAG, "%s", buf);
  if (this->event_text_ != nullptr)
    this->event_text_->publish_state(buf);
}

void PolarFeeder::set_state_(FeedState s, const char *text) {
  this->state_ = s;
  this->phase_start_ms_ = millis();
  if (this->state_text_ != nullptr)
    this->state_text_->publish_state(text);
  if (this->feeding_bs_ != nullptr)
    this->feeding_bs_->publish_state(s != FeedState::IDLE);
}

void PolarFeeder::set_thaw_(bool on) {
  this->thaw_latch_ = on;
  if (this->thaw_bs_ != nullptr)
    this->thaw_bs_->publish_state(on);
}

void PolarFeeder::ir_(bool on) {
  // BSP_setExistState @0x4200cfae: LEDC ch0 on GPIO2, 38 kHz, duty 127/256 while the lid is open, 0 otherwise.
  if (on && !this->ir_on_)
    this->ir_on_since_ms_ = millis();
  this->ir_on_ = on;
  if (this->ir_emitter_ == nullptr)
    return;
  if (on) {
    this->ir_emitter_->set_level(this->ir_level_);
  } else {
    this->ir_emitter_->turn_off();
  }
}

void PolarFeeder::power_on_() {
  // P0_4/P0_7 may power the IR or gate sensors (unknown); keep them on for the whole sequence (OURS).
  for (auto *s : this->power_switches_) {
    if (!s->state)
      s->turn_on();
  }
}

void PolarFeeder::save_() {
  FeederPrefs p{};
  p.next = this->next_;
  p.last_fed_ts = this->last_fed_ts_;
  p.serve_next = this->serve_next_;
  this->pref_.save(&p);
}

FeedMode PolarFeeder::get_mode() const {
  if (this->mode_select_ != nullptr) {
    auto idx = this->mode_select_->active_index();
    if (idx.has_value() && *idx == 1)
      return FeedMode::LID_ONLY;
    if (idx.has_value() && *idx == 2)
      return FeedMode::SERVE_AND_RETURN;
    if (idx.has_value() && *idx == 3)
      return FeedMode::SERVE_ON_DEMAND;
  }
  return FeedMode::ROTATE_AND_OPEN;
}

int PolarFeeder::filled_count_() const {
  const int n = this->tray_->get_compartments();
  if (this->filled_ == nullptr || std::isnan(this->filled_->state))
    return n;
  const int f = (int) lroundf(this->filled_->state);
  return f < 1 ? 1 : (f > n ? n : f);
}

size_t PolarFeeder::active_slots_() const {
  // rotate_and_open: one slot per compartment (N = compartments); lid_only: every configured slot (up to 12).
  if (this->get_mode() == FeedMode::LID_ONLY)
    return this->meals_.size();
  if (this->serve_mode_())
    return 0;  // serve_and_return has its own interval schedule
  const size_t n = this->tray_->get_compartments();
  return n < this->meals_.size() ? n : this->meals_.size();
}

int32_t PolarFeeder::global_open_minutes_() const {
  if (this->open_minutes_ != nullptr && !std::isnan(this->open_minutes_->state) && this->open_minutes_->state >= 0)
    return (int32_t) lroundf(this->open_minutes_->state);
  return (int32_t) this->default_open_minutes_;
}

int32_t PolarFeeder::meal_open_minutes_(const Meal &m) const {
  if (m.open_minutes != nullptr && !std::isnan(m.open_minutes->state) && m.open_minutes->state >= 0)
    return (int32_t) lroundf(m.open_minutes->state);
  return this->global_open_minutes_();
}

uint32_t PolarFeeder::lead_s_() const {
  if (this->lead_minutes_ != nullptr && !std::isnan(this->lead_minutes_->state) && this->lead_minutes_->state >= 0)
    return (uint32_t) lroundf(this->lead_minutes_->state) * 60;
  return this->default_lead_minutes_ * 60;
}

void PolarFeeder::set_next_(int n) {
  this->next_ = n;
  this->save_();
  this->publish_run_();
}

void PolarFeeder::publish_run_() {
  if (this->next_comp_sensor_ != nullptr)
    this->next_comp_sensor_->publish_state(this->is_empty() ? NAN : (float) this->next_);
  if (this->empty_bs_ != nullptr)
    this->empty_bs_->publish_state(this->is_empty());
}

bool PolarFeeder::position_tray_(int compartment) {
  // OURS: the tray never moves unless the lid is known closed (nothing exposed while rotating).
  if (!this->lid_->is_closed()) {
    this->event_("Tray move to %d refused: lid not closed", compartment);
    return false;
  }
  if (this->tray_->get_position() == compartment)
    return true;
  if (this->tray_->is_homed())
    return this->tray_->go_to(compartment);
  // Position unknown (e.g. after a reboot without home_on_boot): home first, then go to the compartment.
  this->event_("Tray not homed: homing before moving to compartment %d", compartment);
  if (!this->tray_->home())
    return false;
  this->pending_goto_ = compartment;
  return true;
}

// ------------------------------------------------------------------------------------------------ commands
bool PolarFeeder::feed_now(int minutes) {
  // WET_FOOD_FEED_NOW_SERVICE / app manualFeedNow{plate}. OURS: the plate is the next one of the run.
  const int32_t m = minutes < 0 ? this->global_open_minutes_() : minutes;
  if (this->serve_mode_())
    return this->start_serve_(-1);
  if (this->get_mode() == FeedMode::LID_ONLY)
    return this->start_feed_(0, m, -1, false);
  if (this->is_empty()) {
    this->event_("Feed now refused: run empty, refill first");
    return false;
  }
  return this->start_feed_(this->next_, m, -1, true);
}

bool PolarFeeder::open_lid() { return this->start_feed_(0, 0, -1, false, false); }

bool PolarFeeder::refilled() {
  if (this->is_feeding()) {
    this->event_("Refill refused: a feed is running");
    return false;
  }
  this->set_next_(1);
  this->pending_goto_ = 0;
  if (this->serve_mode_()) {
    this->serve_next_ = 0;  // first storage compartment
    this->save_();
    this->event_("Refilled: compartment %d is served next", this->serve_compartment_());
    if (!this->tray_->is_busy() && this->tray_->get_position() != this->serve_rest_ &&
        !this->position_tray_(this->serve_rest_))
      this->event_("Refilled: tray move to rest compartment %d refused", this->serve_rest_);
    return true;
  }
  this->event_("Refilled: %d compartments, compartment 1 is next", this->filled_count_());
  if (this->get_mode() == FeedMode::ROTATE_AND_OPEN && !this->tray_->is_busy()) {
    if (!this->lid_->is_closed()) {
      this->event_("Refilled: lid not closed, tray homes at the next feed instead");
    } else if (!this->tray_->home()) {
      // Homing ends with compartment 1 under the lid opening (HW 5.2.0, widest mark = plate 1).
      this->event_("Refilled: tray homing refused (inhibited or fault)");
    }
  }
  return true;
}

bool PolarFeeder::start_feed_(int compartment, int32_t minutes, int meal_index, bool consume, bool serves) {
  if (this->is_feeding()) {
    this->event_("Feed refused: a feed is already running");
    return false;
  }
  if (this->lid_->is_fault() || this->lid_->is_busy() || this->lid_->is_inhibited()) {
    this->event_("Feed refused: lid busy, inhibited or in fault");
    return false;
  }
  this->feed_compartment_ = compartment;
  this->feed_meal_ = meal_index;
  this->feed_minutes_ = minutes;
  this->feed_consume_ = consume;
  this->feed_serves_ = serves;
  this->close_stalls_ = 0;
  this->pet_reopens_ = 0;
  this->hold_active_ = false;
  this->close_then_rotate_ = false;
  this->close_profile_ = LidProfile::FEED;
  this->tray_moved_ = false;
  if (this->anti_pinch_bs_ != nullptr)
    this->anti_pinch_bs_->publish_state(false);
  // Thaw latch lasts "until the feed starts" (CTRL_THREAD_coolHanlde @0x4200e6f6).
  this->set_thaw_(false);
  this->power_on_();
  const bool rotate = compartment != 0 && this->tray_->get_position() != compartment;
  if (rotate && !this->lid_->is_closed()) {
    // OURS: lid open or unknown: close it first (anti-pinch), then rotate.
    this->close_then_rotate_ = true;
    this->ir_(true);
    this->last_pet_ms_ = millis();
    this->set_state_(FeedState::WAIT_CLEAR, "closing lid before rotating");
  } else if (rotate) {
    if (this->tray_->is_busy()) {
      this->tray_moved_ = false;  // probably the prepare move: wait for it, then check / move once more
    } else if (!this->position_tray_(compartment)) {
      this->event_("Feed refused: cannot move tray to compartment %d (fault or inhibited)", compartment);
      return false;
    } else {
      this->tray_moved_ = true;
    }
    this->set_state_(FeedState::TRAY, "moving tray");
  } else if (this->lid_->is_open()) {
    this->after_open_();  // already open (lid_only): just start the open period
  } else {
    if (!this->lid_->open(LidProfile::FEED))
      return false;
    this->set_state_(FeedState::OPENING, "opening lid");
  }
  this->feed_start_trigger_.trigger(compartment);
  char dur[16];
  if (minutes == 0) {
    snprintf(dur, sizeof(dur), "until close");
  } else {
    snprintf(dur, sizeof(dur), "%" PRId32 " min", minutes);
  }
  if (this->feed_serve_) {
    // start_serve_() posts its own event
  } else if (meal_index >= 0) {
    this->event_("Meal %d started (compartment %d, open %s)", meal_index + 1, compartment, dur);
  } else {
    this->event_("%s (compartment %d, open %s)", consume ? "Feed now" : "Lid open", compartment, dur);
  }
  return true;
}

void PolarFeeder::after_open_() {
  if (this->feed_consume_ && this->feed_compartment_ == this->next_)
    this->set_next_(this->next_ + 1);  // compartment served: advance the run
  this->ir_(true);  // IR PWM on after the open (Feed_Handle @0x4200ae46)
  if (this->feed_serve_) {
    this->serve_advance_();
    this->serve_open_ms_ = millis();
    this->serve_pet_seen_ = this->serve_beam_;  // the beam break that started it counts as the cat
    if (this->serve_beam_)
      this->last_pet_ms_ = this->serve_open_ms_;
  }
  this->lid_opened_trigger_.trigger(this->feed_compartment_);
  this->set_state_(FeedState::ONGOING, this->serve_beam_     ? "serving: pet eating"
                                       : this->feed_serve_   ? "serving: waiting for pet"
                                       : this->feed_minutes_ == 0 ? "open until close"
                                                                  : "feeding");
}

void PolarFeeder::begin_close_() {
  // Anti-pinch (stock Feed_Handle CLOSE 7): close with the IR pet check active; a pet seen while closing
  // stops and reopens the lid. OURS: never START a close while presence is detected or before the IR
  // receiver has settled after the emitter was switched on.
  const uint32_t now = millis();
  if (this->pet_present_() || !this->ir_on_ || now - this->ir_on_since_ms_ < this->ir_settle_ms_) {
    if (!this->ir_on_)
      this->ir_(true);
    this->last_pet_ms_ = now;
    this->set_state_(FeedState::WAIT_CLEAR, "waiting for pet to leave");
    return;
  }
  this->power_on_();
  if (this->lid_->close(this->close_profile_, true)) {
    this->set_state_(FeedState::CLOSING, "closing lid");
  } else {
    this->end_feed_("error: lid close refused");
  }
}

bool PolarFeeder::idle_close_(LidProfile profile, const char *why) {
  if (this->is_feeding() || this->lid_->is_busy() || this->lid_->is_fault() || this->lid_->is_inhibited())
    return false;
  this->feed_compartment_ = 0;
  this->feed_meal_ = -1;
  this->feed_consume_ = false;
  this->close_stalls_ = 0;
  this->pet_reopens_ = 0;
  this->hold_active_ = false;
  this->close_then_rotate_ = false;
  this->close_profile_ = profile;
  this->idle_close_running_ = true;
  this->power_on_();
  this->ir_(true);
  this->event_("%s", why);
  // Through WAIT_CLEAR: IR settle and 10 s without presence before the motor starts.
  this->last_pet_ms_ = millis();
  this->set_state_(FeedState::WAIT_CLEAR, "waiting for pet to leave");
  return true;
}

bool PolarFeeder::boot_close() { return this->idle_close_(LidProfile::BOOT, "Boot close (anti-pinch)"); }

void PolarFeeder::close_now() {
  switch (this->state_) {
    case FeedState::ONGOING:
    case FeedState::WAIT_CLEAR:
      this->event_(this->pet_present_() ? "Close requested: deferred, presence detected" : "Close requested");
      this->hold_active_ = false;
      this->begin_close_();
      break;
    case FeedState::IDLE:
      if (!this->lid_->is_closed())
        this->idle_close_(LidProfile::FEED, "Close requested (no feed running)");
      break;
    default:
      this->event_("Close requested while %s: ignored", this->state_ == FeedState::CLOSING ? "closing" : "busy");
      break;
  }
}

void PolarFeeder::cancel(const char *reason) {
  if (!this->is_feeding())
    return;
  this->pending_goto_ = 0;
  this->close_then_rotate_ = false;
  if (this->feed_serve_ && !this->serve_returning_)
    this->event_("Serve cancelled: compartment %d may still be under the lid", this->feed_compartment_);
  this->end_feed_(reason);
}

void PolarFeeder::end_feed_(const char *result) {
  this->ir_(false);
  const int comp = this->feed_compartment_;
  this->event_("Feed finished: %s", result);
  this->feed_serve_ = false;
  this->serve_beam_ = false;
  this->serve_returning_ = false;
  // Any feed ending starts the on-demand cooldown (read only in serve_on_demand).
  if (!this->idle_close_running_) {
    this->od_cooldown_ = true;
    this->od_end_ms_ = millis();
  }
  this->idle_close_running_ = false;
  this->od_armed_ = false;
  this->od_edge_active_ = false;
  this->set_state_(FeedState::IDLE, "idle");
  this->feed_end_trigger_.trigger(comp, std::string(result));
}

// ------------------------------------------------------------------------------------------------ loop
void PolarFeeder::loop() {
  const uint32_t now = millis();

  if (this->pending_goto_ != 0 && !this->tray_->is_busy()) {
    const int c = this->pending_goto_;
    this->pending_goto_ = 0;
    if (!this->lid_->is_closed()) {
      this->event_("Move to compartment %d after homing refused: lid not closed", c);
    } else if (this->tray_->is_homed() && !this->tray_->go_to(c)) {
      this->event_("Move to compartment %d after homing refused", c);
    }
  }

  switch (this->state_) {
    case FeedState::IDLE:
      break;
    case FeedState::TRAY:
      if (this->tray_->is_busy() || this->pending_goto_ != 0)
        break;
      if (!this->lid_->is_closed()) {
        this->end_feed_("error: lid not closed while rotating");
        break;
      }
      if (this->serve_returning_) {
        if (this->tray_->get_position() == this->serve_rest_) {
          this->event_("Serve done (%s): compartment %d back in the cold", this->serve_close_reason_,
                       this->feed_compartment_);
          this->end_feed_("ok");
        } else {
          this->end_feed_("error: tray did not return to the rest compartment");
        }
        break;
      }
      if (this->tray_->get_position() != this->feed_compartment_) {
        if (!this->tray_moved_ && this->position_tray_(this->feed_compartment_)) {
          this->tray_moved_ = true;
          break;
        }
        // Stock closes the record with error 0x7fc on a plate motor fault.
        this->end_feed_("error: tray did not reach the compartment");
        break;
      }
      if (!this->lid_->open(LidProfile::FEED)) {
        this->end_feed_("error: lid open refused");
        break;
      }
      this->set_state_(FeedState::OPENING, "opening lid");
      break;
    case FeedState::OPENING:
      if (this->lid_->is_busy())
        break;
      if (!this->lid_->is_open()) {
        this->end_feed_("error: lid did not open");  // stock 0x7f6
        break;
      }
      this->after_open_();
      break;
    case FeedState::ONGOING:
      if (this->feed_serve_) {
        this->serve_ongoing_(now);
        break;
      }
      // plan.feedingDuration minutes; 0 = until close_now(). The ringer (ringerMode SMART, FUN_ram_4200a23e) lives
      // in polar_sound ("call the pet"), started from on_lid_opened when is_serving().
      if (this->feed_minutes_ > 0 && now - this->phase_start_ms_ >= (uint32_t) this->feed_minutes_ * 60000UL) {
        this->last_pet_ms_ = now;
        this->set_state_(FeedState::WAIT_CLEAR, "waiting for pet to leave");
      }
      break;
    case FeedState::WAIT_CLEAR:
      // Stock: close when no pet on GPIO20 for 20 loops (10 s). Stock also forces a close attempt after
      // irSensorIdenTimeout; OURS: no forced close while presence is detected (it would reopen anyway).
      if (this->pet_present_())
        this->last_pet_ms_ = now;
      if (this->hold_active_ && (int32_t) (now - this->hold_until_ms_) >= 0)
        this->hold_active_ = false;  // wrap-safe: compared only while a hold is active (<= minutes)
      if (!this->hold_active_ && now - this->last_pet_ms_ >= this->pet_leave_ms_)
        this->begin_close_();
      break;
    case FeedState::CLOSING:
      if (this->lid_->is_busy())
        break;
      if (this->lid_->is_closed()) {
        this->ir_(false);
        if (this->close_then_rotate_) {
          this->close_then_rotate_ = false;
          if (!this->position_tray_(this->feed_compartment_)) {
            this->end_feed_("error: cannot move tray after closing");
            break;
          }
          this->tray_moved_ = true;
          this->set_state_(FeedState::TRAY, "moving tray");
          break;
        }
        if (this->feed_serve_ && !this->serve_returning_) {
          // Lid closed over the served compartment: take it back to the cold (rest compartment to the front).
          this->serve_returning_ = true;
          if (!this->position_tray_(this->serve_rest_)) {
            this->end_feed_("error: cannot return the tray to the rest compartment");
            break;
          }
          this->set_state_(FeedState::TRAY, "returning tray");
          break;
        }
        this->end_feed_("ok");
      } else if (this->lid_->is_open() && this->lid_->was_stall_reopened()) {
        // Lid over-current while closing (possible pinch): the lid reopened. Retry later, give up after N.
        this->close_profile_ = LidProfile::FEED;
        this->close_stalls_++;
        if (this->anti_pinch_bs_ != nullptr)
          this->anti_pinch_bs_->publish_state(true);
        if (this->close_stalls_ >= this->max_close_stalls_) {
          this->close_then_rotate_ = false;
          this->end_feed_("error: lid stalled while closing, left OPEN (anti-pinch)");
          break;
        }
        this->event_("Lid stalled while closing: reopened, retry %u/%u in %" PRIu32 " s", this->close_stalls_,
                     this->max_close_stalls_, this->stall_retry_delay_ms_ / 1000);
        this->hold_until_ms_ = now + this->stall_retry_delay_ms_;
        this->hold_active_ = true;
        this->last_pet_ms_ = now;
        this->set_state_(FeedState::WAIT_CLEAR, "waiting to retry close");
      } else if (this->lid_->is_open() && this->lid_->was_pet_reopened()) {
        this->close_profile_ = LidProfile::FEED;
        this->pet_reopens_++;
        this->event_("Pet in the way while closing: reopened (%u)", this->pet_reopens_);
        this->last_pet_ms_ = now;
        this->set_state_(FeedState::WAIT_CLEAR, "waiting for pet to leave");
      } else {
        this->close_then_rotate_ = false;
        this->end_feed_("error: lid did not close");  // stock 0x7f0
      }
      break;
  }

  if (this->state_ == FeedState::IDLE)
    this->on_demand_idle_(millis());  // fresh time: end_feed_() above may have stamped a later millis() than 'now'

  if (now - this->last_schedule_ms_ >= SCHEDULE_PERIOD_MS) {
    this->last_schedule_ms_ = now;
    this->schedule_tick_();
  }
}

void PolarFeeder::meal_due_(size_t i) {
  const int32_t minutes = this->meal_open_minutes_(this->meals_[i]);
  if (this->is_feeding()) {
    this->event_("Meal %u missed: another feed is running", (unsigned) i + 1);
    return;
  }
  if (this->get_mode() == FeedMode::LID_ONLY) {
    this->start_feed_(0, minutes, (int) i, false);
    return;
  }
  if (this->is_empty()) {
    this->event_("Meal %u skipped: run empty, refill", (unsigned) i + 1);
    return;
  }
  this->start_feed_(this->next_, minutes, (int) i, true);
}

void PolarFeeder::schedule_tick_() {
  if (this->time_ == nullptr)
    return;
  ESPTime t = this->time_->now();
  if (!t.is_valid())
    return;
  const uint32_t now = millis();
  // Latched so the 32-bit millis() wrap (49.7 days) cannot re-enter the grace period.
  if (!this->grace_over_ && now - this->boot_ms_ >= this->boot_grace_ms_)
    this->grace_over_ = true;
  const bool in_grace = !this->grace_over_;
  if (this->on_demand_mode_()) {
    this->on_demand_status_(now);  // no clock schedule in this mode
    return;
  }
  if (this->serve_mode_()) {
    this->serve_schedule_tick_(t, in_grace);
    return;
  }
  const int now_min = t.hour * 60 + t.minute;
  const int64_t minute_start = (int64_t) t.timestamp - t.second;
  const bool rotate = this->get_mode() == FeedMode::ROTATE_AND_OPEN;
  const uint32_t lead = this->lead_s_();
  int best_secs = -1, best_meal = -1;
  const size_t slots = this->active_slots_();
  for (size_t i = 0; i < slots; i++) {
    Meal &m = this->meals_[i];
    if (m.enabled == nullptr || !m.enabled->state || m.time == nullptr || !m.time->has_state())
      continue;
    const int meal_min = m.time->hour * 60 + m.time->minute;
    const int delta_min = (meal_min - now_min + 1440) % 1440;
    // occurrence id = epoch of the meal minute (local wall-clock arithmetic; DST jumps not handled)
    const int64_t occ = minute_start + (int64_t) delta_min * 60;
    const int secs_to = delta_min * 60 - t.second;
    if (best_secs < 0 || secs_to < best_secs) {
      best_secs = secs_to;
      best_meal = (int) i;
    }
    if (delta_min == 0) {
      if (m.fed_occ != occ) {  // at plan time (Feed_Handle): open the lid
        m.fed_occ = occ;
        m.prepared_occ = occ;
        m.thaw_occ = occ;
        if (occ <= this->last_fed_ts_)
          continue;  // already started before a reboot (persisted)
        this->last_fed_ts_ = occ;
        this->save_();
        if (in_grace) {
          this->event_("Meal %u skipped: due within the boot grace period", (unsigned) i + 1);
        } else {
          this->meal_due_(i);
        }
      }
      continue;
    }
    // T-1 h (checkOnePlanBeforeOneHour @0x42008bca): if T <= 13.0 C, cooling off until the feed (thaw).
    // Only acted on by polar_cooling when its thaw-rule switch is on.
    if (secs_to <= (int) this->thaw_lead_s_ && m.thaw_occ != occ) {
      m.thaw_occ = occ;
      const float temp = this->temperature_ != nullptr ? this->temperature_->state : NAN;
      if (!std::isnan(temp) && temp <= this->thaw_threshold_) {
        this->set_thaw_(true);
        ESP_LOGI(TAG, "Meal %u within 1 h at %.1f C <= %.1f C: thaw latch set", (unsigned) i + 1, temp,
                 this->thaw_threshold_);
      }
    }
    // T-lead (One_PlanCheck @0x4200ab0a: now < plan < now+1800 s): move the meal's compartment under the lid.
    // OURS: never during the boot grace period, and only with the lid known closed (else retried each tick;
    // the feed itself closes the lid first at meal time).
    if (rotate && secs_to < (int) lead && m.prepared_occ != occ && !in_grace) {
      if (this->is_empty()) {
        m.prepared_occ = occ;
        this->event_("Meal %u: run empty, no rotation", (unsigned) i + 1);
      } else if (this->is_feeding() || !this->lid_->is_closed() || this->tray_->is_busy()) {
        // wait; tried again next tick
      } else {
        m.prepared_occ = occ;
        const int c = this->next_;
        if (this->tray_->get_position() != c) {
          if (this->position_tray_(c)) {
            this->event_("Meal %u: moving compartment %d under the lid", (unsigned) i + 1, c);
          } else {
            this->event_("Meal %u: prepare move refused (tray fault/inhibited)", (unsigned) i + 1);
          }
        }
        this->prepare_trigger_.trigger(c);
      }
    }
  }

  if (this->next_text_ != nullptr && (now - this->last_next_ms_ >= 30000 || this->next_text_->state.empty())) {
    this->last_next_ms_ = now;
    char buf[80];
    if (best_meal < 0) {
      snprintf(buf, sizeof(buf), "none");
    } else {
      const Meal &m = this->meals_[best_meal];
      if (!rotate) {
        snprintf(buf, sizeof(buf), "meal %d at %02u:%02u (in %d min), lid only", best_meal + 1, m.time->hour,
                 m.time->minute, best_secs / 60);
      } else if (this->is_empty()) {
        snprintf(buf, sizeof(buf), "meal %d at %02u:%02u: EMPTY, refill", best_meal + 1, m.time->hour, m.time->minute);
      } else {
        snprintf(buf, sizeof(buf), "meal %d at %02u:%02u (in %d min), compartment %d", best_meal + 1, m.time->hour,
                 m.time->minute, best_secs / 60, this->next_);
      }
    }
    if (this->next_text_->state != buf)
      this->next_text_->publish_state(buf);
  }
}

// ---------------------------------------------------------------------------------------- serve_and_return
bool PolarFeeder::serve_storage_(int c) const {
  return c >= 1 && c <= (int) this->tray_->get_compartments() && c != this->serve_rest_;
}

int PolarFeeder::serve_storage_count_() const {
  int n = 0;
  for (int c = 1; c <= (int) this->tray_->get_compartments(); c++)
    n += this->serve_storage_(c) ? 1 : 0;
  return n;
}

int PolarFeeder::serve_compartment_() const {
  // Choice: 0 = alternate through the storage compartments, 1 = first storage only, 2 = last storage only.
  const int n = (int) this->tray_->get_compartments();
  int first = 0, last = 0;
  for (int c = 1; c <= n; c++) {
    if (!this->serve_storage_(c))
      continue;
    if (first == 0)
      first = c;
    last = c;
  }
  size_t choice = 0;
  if (this->serve_choice_ != nullptr) {
    auto idx = this->serve_choice_->active_index();
    if (idx.has_value())
      choice = *idx;
  }
  if (choice == 1)
    return first;
  if (choice == 2)
    return last;
  return this->serve_storage_(this->serve_next_) ? this->serve_next_ : first;
}

void PolarFeeder::serve_advance_() {
  size_t choice = 0;
  if (this->serve_choice_ != nullptr) {
    auto idx = this->serve_choice_->active_index();
    if (idx.has_value())
      choice = *idx;
  }
  if (choice != 0)
    return;
  const int n = (int) this->tray_->get_compartments();
  int c = this->feed_compartment_;
  for (int i = 0; i < n; i++) {
    c = c % n + 1;
    if (this->serve_storage_(c))
      break;
  }
  this->serve_next_ = c;
  this->save_();
}

uint32_t PolarFeeder::serve_minutes_ms_(number::Number *num, float def) const {
  float m = def;
  if (num != nullptr && !std::isnan(num->state) && num->state >= 0)
    m = num->state;
  return (uint32_t) (m * 60000.0f);
}

bool PolarFeeder::start_serve_(int occurrence_index, bool beam) {
  if (this->serve_storage_count_() == 0) {
    this->event_("Serve refused: no storage compartment (rest compartment %d)", this->serve_rest_);
    return false;
  }
  const int c = this->serve_compartment_();
  this->feed_serve_ = true;
  this->serve_returning_ = false;
  this->serve_pet_seen_ = false;
  this->serve_beam_ = beam;
  this->serve_close_reason_ = "close requested";  // replaced by serve_close_() when it decides
  // minutes = 0: the generic ONGOING timer is not used; serve_ongoing_() decides when to close.
  if (!this->start_feed_(c, 0, -1, false, true)) {
    this->feed_serve_ = false;
    this->serve_beam_ = false;
    return false;
  }
  if (beam) {
    this->event_("Beam broken: serving compartment %d", c);
  } else if (occurrence_index >= 0) {
    this->event_("Serve %d started: compartment %d", occurrence_index + 1, c);
  } else {
    this->event_("Serve now: compartment %d", c);
  }
  return true;
}

void PolarFeeder::serve_close_(const char *why) {
  this->serve_close_reason_ = why;
  const uint32_t open_s = (millis() - this->serve_open_ms_) / 1000;
  this->event_("Serve: closing (%s) after %" PRIu32 " min %" PRIu32 " s", why, open_s / 60, open_s % 60);
  this->hold_active_ = false;
  this->begin_close_();  // anti-pinch: waits in WAIT_CLEAR while presence is detected
}

void PolarFeeder::serve_ongoing_(uint32_t now) {
  const uint32_t open_for = now - this->serve_open_ms_;
  // The IR receiver reads "present" until the emitter has settled: ignore it for serve_settle after switch-on.
  const bool settled = this->ir_on_ && now - this->ir_on_since_ms_ >= this->serve_settle_ms_;
  if (settled && this->pet_present_()) {
    if (!this->serve_pet_seen_) {
      this->serve_pet_seen_ = true;
      this->serve_first_pet_ms_ = now;
      this->event_("Serve: pet arrived after %" PRIu32 " min %" PRIu32 " s", open_for / 60000,
                   (open_for / 1000) % 60);
      this->set_state_(FeedState::ONGOING, "serving: pet eating");
    }
    this->last_pet_ms_ = now;
  }
  if (open_for >= this->serve_minutes_ms_(this->serve_max_open_, 45.0f)) {
    this->serve_close_("max open time");
  } else if (this->serve_beam_) {
    // Stay open for the minimum eating time, then close once the beam has been clear for the "pet gone" time.
    if (open_for >= this->serve_minutes_ms_(this->od_min_open_, 3.0f) &&
        now - this->last_pet_ms_ >= this->serve_minutes_ms_(this->serve_pet_gone_, 2.0f))
      this->serve_close_("pet gone");
  } else if (this->serve_pet_seen_) {
    if (now - this->last_pet_ms_ >= this->serve_minutes_ms_(this->serve_pet_gone_, 2.0f))
      this->serve_close_("pet gone");
  } else if (open_for >= this->serve_minutes_ms_(this->serve_no_show_, 30.0f)) {
    this->serve_close_("no show");
  }
}

void PolarFeeder::serve_schedule_tick_(const ESPTime &t, bool in_grace) {
  const uint32_t now = millis();
  const bool enabled = this->serve_enabled_ != nullptr && this->serve_enabled_->state;
  int interval = 120;
  if (this->serve_interval_ != nullptr && !std::isnan(this->serve_interval_->state))
    interval = (int) lroundf(this->serve_interval_->state);
  if (interval < 15)
    interval = 15;
  int start_min = 8 * 60;
  if (this->serve_start_ != nullptr && this->serve_start_->has_state())
    start_min = this->serve_start_->hour * 60 + this->serve_start_->minute;
  const int now_min = t.hour * 60 + t.minute;
  const int delta = (now_min - start_min + 1440) % 1440;  // minutes since today's (or yesterday's) start
  const int64_t minute_start = (int64_t) t.timestamp - t.second;

  if (enabled && delta % interval == 0 && minute_start > this->last_fed_ts_) {
    this->last_fed_ts_ = minute_start;  // persisted: no refire of this occurrence after a reboot
    this->save_();
    const int k = delta / interval;
    if (in_grace) {
      this->event_("Serve %d skipped: due within the boot grace period", k + 1);
    } else if (this->is_feeding()) {
      this->event_("Serve %d missed: another feed is running", k + 1);
    } else {
      this->start_serve_(k);
    }
  }

  if (this->next_text_ == nullptr || (now - this->last_next_ms_ < 30000 && !this->next_text_->state.empty()))
    return;
  this->last_next_ms_ = now;
  char buf[96];
  if (!enabled) {
    snprintf(buf, sizeof(buf), "serve schedule off");
  } else {
    int next_delta = (delta / interval + 1) * interval;
    if (next_delta >= 1440)
      next_delta = 1440;  // the cycle restarts at the start time
    const int in_min = next_delta - delta;
    const int at = (now_min + in_min) % 1440;
    snprintf(buf, sizeof(buf), "serve at %02d:%02d (in %d min), compartment %d", at / 60, at % 60, in_min,
             this->serve_compartment_());
  }
  if (this->next_text_->state != buf)
    this->next_text_->publish_state(buf);
}

// ----------------------------------------------------------------------------------------- serve_on_demand
void PolarFeeder::on_demand_idle_(uint32_t now) {
  if (!this->on_demand_active_()) {
    if (this->ir_on_)
      this->ir_(false);  // other modes: emitter only during feeds, as stock
    this->od_lid_checked_ = false;
    this->od_armed_ = false;
    this->od_edge_active_ = false;
    return;
  }
  // Works without a valid clock, so the grace latch cannot wait for schedule_tick_().
  if (!this->grace_over_ && now - this->boot_ms_ >= this->boot_grace_ms_)
    this->grace_over_ = true;
  if (!this->grace_over_)
    return;
  if (!this->ir_on_) {
    this->ir_(true);
    this->od_armed_ = false;
    this->od_edge_active_ = false;
    return;
  }
  // The receiver reads "present" until the emitter has settled.
  if (now - this->ir_on_since_ms_ < this->serve_settle_ms_)
    return;
  // A serve needs the lid known closed, but its state is unknown after a Stop all or a failed boot close (the sensor
  // is high at both end stops). One anti-pinch close settles it; if the lid is already shut it only nudges the end stop.
  if (!this->od_lid_checked_ && !this->lid_->is_closed() && !this->lid_->is_open() && !this->lid_->is_busy()) {
    if (this->idle_close_(LidProfile::FEED, "On demand: confirming the lid is closed"))
      this->od_lid_checked_ = true;
    return;  // refused (fault or inhibited): tried again once that clears
  }
  if (this->od_cooldown_) {
    if (now - this->od_end_ms_ < this->serve_minutes_ms_(this->od_cooldown_min_, 5.0f))
      return;
    this->od_cooldown_ = false;
  }
  const bool blocked = this->pet_present_();
  if (!this->od_armed_) {
    // Arm only after a clear beam: a cat still sitting there, or a beam that reads blocked permanently, never fires.
    if (blocked) {
      this->od_edge_active_ = false;
    } else if (!this->od_edge_active_) {
      this->od_edge_active_ = true;
      this->od_edge_ms_ = now;
    } else if (now - this->od_edge_ms_ >= this->od_rearm_ms_) {
      this->od_armed_ = true;
      this->od_edge_active_ = false;
    }
    return;
  }
  if (!blocked) {
    this->od_edge_active_ = false;
    return;
  }
  if (!this->od_edge_active_) {
    this->od_edge_active_ = true;
    this->od_edge_ms_ = now;
    return;
  }
  if (now - this->od_edge_ms_ < this->od_hold_ms_)
    return;
  this->od_armed_ = false;  // one attempt per beam break, whatever happens next
  this->od_edge_active_ = false;
  if (!this->lid_->is_closed() || this->lid_->is_busy() || this->tray_->is_busy() || this->pending_goto_ != 0) {
    this->event_("Beam broken: ignored (lid not closed, or a motor is busy)");
    return;
  }
  if (!this->start_serve_(-1, true)) {
    this->od_cooldown_ = true;  // refused (fault or inhibited): don't retry on every beam break
    this->od_end_ms_ = millis();
  }
}

void PolarFeeder::on_demand_status_(uint32_t now) {
  if (this->next_text_ == nullptr || (now - this->last_next_ms_ < 30000 && !this->next_text_->state.empty()))
    return;
  this->last_next_ms_ = now;
  char buf[64];
  if (!this->on_demand_active_()) {
    snprintf(buf, sizeof(buf), "on demand: off");
  } else if (this->od_cooldown_) {
    snprintf(buf, sizeof(buf), "on demand: cooling down, compartment %d next", this->serve_compartment_());
  } else {
    snprintf(buf, sizeof(buf), "on demand: compartment %d on beam break", this->serve_compartment_());
  }
  if (this->next_text_->state != buf)
    this->next_text_->publish_state(buf);
}

}  // namespace esphome::polar_feeder

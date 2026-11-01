#include "polar_sound.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace esphome::polar_sound {

static const char *const TAG = "polar_sound";

// ------------------------------------------------------------------------------------------------ selects
void SoundSelect::setup_state() {
  if (this->play_on_select_) {
    // Momentary: show the stock meal call; publishing does not play (control() is not called).
    const size_t def = this->parent_->call_index();
    this->publish_state(def < this->size() ? def : (size_t) 0);
    return;
  }
  this->pref_ = this->make_entity_preference<uint32_t>();
  uint32_t idx = 0;
  if (this->pref_.load(&idx) && idx < this->size()) {
    this->publish_state((size_t) idx);
  } else {
    const size_t def = this->parent_->call_index();
    this->publish_state(def < this->size() ? def : (size_t) 0);
  }
}

void SoundSelect::control(size_t index) {
  this->publish_state(index);
  if (this->play_on_select_) {
    // A person chose it in HA: quiet hours do not apply, the enable switch does.
    this->parent_->play((int) index, true);
  } else {
    uint32_t idx = index;
    this->pref_.save(&idx);
  }
}

// ------------------------------------------------------------------------------------------------ setup
void PolarSound::setup() {
  // BSP_aduioInit @0x4200d738: gpio_config(mask 0x40, mode 2 = output, no pulls), gpio_set_level(6, 1).
  this->pin_->digital_write(true);  // preset the output latch high so enabling the driver gives no low glitch
  this->pin_->setup();
  this->pin_->digital_write(true);

  esp_timer_create_args_t args{};
  args.callback = &PolarSound::timer_cb_;
  args.arg = this;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "polar_sound";
  args.skip_unhandled_events = false;
  esp_err_t err = esp_timer_create(&args, &this->timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  if (this->play_select_ != nullptr)
    this->play_select_->setup_state();
  if (this->call_select_ != nullptr)
    this->call_select_->setup_state();
  this->status_("idle");
}

void PolarSound::dump_config() {
  ESP_LOGCONFIG(TAG, "Polar sound (one-wire, WTN6-family protocol inferred, UNTESTED on hardware):");
  LOG_PIN("  Pin: ", this->pin_);
  ESP_LOGCONFIG(TAG,
                "  Start low %" PRIu32 " us, bit long %" PRIu32 " us, short %" PRIu32 " us, gap %" PRIu32 " us\n"
                "  Volume before play: %s, call sound default 0x%02X",
                this->start_us_, this->long_us_, this->short_us_, this->gap_us_, YESNO(this->volume_before_play_),
                this->default_call_index_);
}

void PolarSound::on_shutdown() {
  if (this->timer_ != nullptr)
    esp_timer_stop(this->timer_);
  this->tx_busy_.store(false);
  this->pin_->digital_write(true);  // idle high
}

// ------------------------------------------------------------------------------------------------ transmitter
void PolarSound::timer_cb_(void *arg) { static_cast<PolarSound *>(arg)->on_timer_(); }

// Runs in the esp_timer task (priority above loopTask), so Wi-Fi/API work in loop() cannot stretch a phase.
// Each phase is armed relative to the previous edge: a late callback lengthens only that one phase.
void PolarSound::on_timer_() {
  const uint8_t i = this->seg_i_ + 1;
  if (i >= this->seg_n_) {
    this->pin_->digital_write(true);  // FUN_ram_4200d7e8: gpio_set_level(6, 1) after the 8th bit
    this->tx_end_us_ = esp_timer_get_time();
    this->seg_i_ = i;
    this->tx_busy_.store(false);
    return;
  }
  this->seg_i_ = i;
  this->pin_->digital_write(this->seg_[i].level);
  esp_timer_start_once(this->timer_, this->seg_[i].us);
}

void PolarSound::start_frame_(uint8_t code) {
  // FUN_ram_4200d7e8: low 5 ms; bit b = (code >> b) & 1, LSB first; "1": high 3 ms, low 1 ms;
  // "0": high 1 ms, low 3 ms; then idle high. 37 ms per byte.
  uint8_t n = 0;
  this->seg_[n++] = {false, this->start_us_};
  for (uint8_t b = 0; b < 8; b++) {
    const bool one = (code >> b) & 1;
    this->seg_[n++] = {true, one ? this->long_us_ : this->short_us_};
    this->seg_[n++] = {false, one ? this->short_us_ : this->long_us_};
  }
  this->seg_n_ = n;
  this->seg_i_ = 0;
  this->tx_code_ = code;
  this->tx_logged_ = false;
  this->tx_busy_.store(true);
  this->tx_start_us_ = esp_timer_get_time();
  this->pin_->digital_write(false);
  esp_err_t err = esp_timer_start_once(this->timer_, this->seg_[0].us);
  if (err != ESP_OK) {
    this->pin_->digital_write(true);
    this->tx_busy_.store(false);
    this->tx_logged_ = true;
    ESP_LOGE(TAG, "esp_timer_start_once failed: %s (byte 0x%02X dropped)", esp_err_to_name(err), code);
  }
}

bool PolarSound::enqueue_(uint8_t code) {
  if (this->q_count_ >= QUEUE_LEN) {
    ESP_LOGW(TAG, "Queue full: byte 0x%02X dropped", code);
    return false;
  }
  this->q_[(this->q_head_ + this->q_count_) % QUEUE_LEN] = code;
  this->q_count_++;
  return true;
}

// ------------------------------------------------------------------------------------------------ loop
void PolarSound::loop() {
  if (!this->tx_busy_.load() && !this->tx_logged_) {
    this->tx_logged_ = true;
    const float ms = (this->tx_end_us_ - this->tx_start_us_) / 1000.0f;
    const float nominal = (this->start_us_ + 8 * (this->long_us_ + this->short_us_)) / 1000.0f;
    if (std::fabs(ms - nominal) > 2.0f) {
      ESP_LOGW(TAG, "Sent 0x%02X in %.1f ms (nominal %.1f ms): timing off", this->tx_code_, ms, nominal);
    } else {
      ESP_LOGD(TAG, "Sent 0x%02X in %.1f ms", this->tx_code_, ms);
    }
  }
  if (!this->tx_busy_.load() && this->q_count_ > 0 && esp_timer_get_time() - this->tx_end_us_ >= this->gap_us_) {
    const uint8_t code = this->q_[this->q_head_];
    this->q_head_ = (this->q_head_ + 1) % QUEUE_LEN;
    this->q_count_--;
    this->start_frame_(code);
  }

  if (!this->call_active_)
    return;
  // Ringer, after FUN_ram_4200a23e (ringerMode SMART): a pet on GPIO20 ends it; otherwise repeat the call
  // every ringerInterval. OURS: a repeat count instead of ringerDuration, and a settle time for the IR receiver
  // (our "Pet present" reads true while the 38 kHz emitter is off; the feeder turns it on at lid open).
  // The pet check applies to meal calls only: outside a feed the emitter is off and the sensor reads true.
  const uint32_t now = millis();
  const bool stop_on_pet =
      this->call_meal_ && (this->call_stop_on_pet_ == nullptr || this->call_stop_on_pet_->state);
  if (stop_on_pet && this->pet_ != nullptr && this->pet_->state && now - this->call_start_ms_ >= this->pet_settle_ms_) {
    this->call_stop("pet seen");
    return;
  }
  if (this->call_done_ >= this->call_total_) {
    this->call_active_ = false;
    this->status_("call finished (%d sounds)", this->call_total_ + 1);
    return;
  }
  float minutes = this->call_interval_ != nullptr ? this->call_interval_->state : NAN;
  if (std::isnan(minutes))
    minutes = 2.0f;  // stock default ringerInterval 2 min (Dev_Attr_Init @0x4200bd2c)
  if (minutes < 0.25f)
    minutes = 0.25f;  // OURS: never more often than every 15 s
  if (now - this->call_last_ms_ >= (uint32_t) (minutes * 60000.0f)) {
    this->call_last_ms_ = now;
    this->call_done_++;
    this->ring_();
    this->status_("call repeat %d/%d", this->call_done_, this->call_total_);
  }
}

// ------------------------------------------------------------------------------------------------ gate
bool PolarSound::in_quiet_hours_() const {
  if (this->quiet_en_ == nullptr || !this->quiet_en_->state || this->time_ == nullptr || this->quiet_start_ == nullptr ||
      this->quiet_end_ == nullptr || !this->quiet_start_->has_state() || !this->quiet_end_->has_state())
    return false;
  ESPTime t = this->time_->now();
  if (!t.is_valid())
    return false;  // no clock: quiet hours cannot be evaluated (the meal schedule needs the clock too)
  const int s = this->quiet_start_->hour * 60 + this->quiet_start_->minute;
  const int e = this->quiet_end_->hour * 60 + this->quiet_end_->minute;
  const int n = t.hour * 60 + t.minute;
  if (s == e)
    return false;
  return s < e ? (n >= s && n < e) : (n >= s || n < e);  // window may wrap midnight
}

bool PolarSound::allowed_(bool force, const char **why) const {
  // Stock gate FUN_ram_4200c276: soundSwitch and the soundAgingType window. Stock's window is where sound is
  // ALLOWED; ours is a quiet window where it is suppressed.
  if (this->enable_ != nullptr && !this->enable_->state) {
    *why = "sound disabled";
    return false;
  }
  if (!force && this->in_quiet_hours_()) {
    *why = "quiet hours";
    return false;
  }
  return true;
}

int PolarSound::volume_level_() const {
  if (this->volume_ == nullptr || !this->volume_->has_state() || std::isnan(this->volume_->state))
    return -1;
  int v = (int) std::lround(this->volume_->state);
  return v < 0 ? 0 : (v > VOLUME_MAX_LEVEL ? VOLUME_MAX_LEVEL : v);
}

uint8_t PolarSound::call_index() const {
  if (this->call_select_ != nullptr && this->call_select_->has_state()) {
    auto idx = this->call_select_->active_index();
    if (idx.has_value())
      return (uint8_t) idx.value();
  }
  return this->default_call_index_;
}

// ------------------------------------------------------------------------------------------------ commands
bool PolarSound::play(int index, bool force) {
  if (index < 0 || index > MAX_VOICE_INDEX) {
    this->status_("play %d refused: voice index is 0..%d", index, MAX_VOICE_INDEX);
    return false;
  }
  const char *why = "";
  if (!this->allowed_(force, &why)) {
    this->status_("play 0x%02X blocked: %s", index, why);
    return false;
  }
  const int vol = this->volume_before_play_ ? this->volume_level_() : -1;
  if (this->q_count_ + (vol >= 0 ? 2 : 1) > QUEUE_LEN) {
    ESP_LOGW(TAG, "Queue full: play 0x%02X dropped", index);
    return false;
  }
  if (vol >= 0)
    this->enqueue_(VOLUME_BASE + vol);
  this->enqueue_((uint8_t) index);
  if (vol >= 0) {
    this->status_("play 0x%02X at volume %d (0x%02X)", index, vol, VOLUME_BASE + vol);
  } else {
    this->status_("play 0x%02X", index);
  }
  return true;
}

bool PolarSound::set_volume(int level) {
  level = level < 0 ? 0 : (level > VOLUME_MAX_LEVEL ? VOLUME_MAX_LEVEL : level);
  if (this->volume_ != nullptr && (std::isnan(this->volume_->state) || std::lround(this->volume_->state) != level)) {
    auto call = this->volume_->make_call();
    call.set_value(level);
    call.perform();
  }
  this->status_("volume %d (0x%02X)", level, VOLUME_BASE + level);
  return this->enqueue_(VOLUME_BASE + level);
}

bool PolarSound::send(int code, bool force) {
  if (code < 0 || code > 0xFF) {
    this->status_("send %d refused: byte is 0..255", code);
    return false;
  }
  if (code <= MAX_VOICE_INDEX) {
    const char *why = "";
    if (!this->allowed_(force, &why)) {
      this->status_("send 0x%02X blocked: %s", code, why);
      return false;
    }
  }
  this->status_("send 0x%02X (raw)", code);
  return this->enqueue_((uint8_t) code);
}

void PolarSound::stop() {
  this->q_count_ = 0;
  this->call_active_ = false;
  this->status_("stop (0x%02X)", CMD_STOP);
  this->enqueue_(CMD_STOP);
}

bool PolarSound::loop_current() {
  const char *why = "";
  if (!this->allowed_(true, &why)) {
    this->status_("loop blocked: %s", why);
    return false;
  }
  this->status_("loop current (0x%02X)", CMD_LOOP);
  return this->enqueue_(CMD_LOOP);
}

void PolarSound::ring_() { this->play(this->call_index(), false); }

void PolarSound::call_start(bool meal) {
  if (meal && this->call_en_ != nullptr && !this->call_en_->state) {
    ESP_LOGD(TAG, "Meal served: call the pet is off");
    return;
  }
  const char *why = "";
  if (!this->allowed_(false, &why)) {
    this->status_("call skipped: %s", why);
    return;
  }
  float r = this->call_repeats_ != nullptr ? this->call_repeats_->state : NAN;
  this->call_total_ = std::isnan(r) || r < 0 ? 0 : (int) std::lround(r);
  this->call_done_ = 0;
  this->call_start_ms_ = this->call_last_ms_ = millis();
  this->call_meal_ = meal;
  this->call_active_ = true;
  this->ring_();
  this->status_("call started (%s): sound 0x%02X, %d repeats", meal ? "meal" : "manual", this->call_index(),
                this->call_total_);
}

void PolarSound::call_stop(const char *reason) {
  if (!this->call_active_)
    return;
  this->call_active_ = false;
  this->q_count_ = 0;
  this->status_("call ended: %s", reason);
}

void PolarSound::status_(const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ESP_LOGI(TAG, "%s", buf);
  if (this->status_text_ != nullptr)
    this->status_text_->publish_state(buf);
}

}  // namespace esphome::polar_sound

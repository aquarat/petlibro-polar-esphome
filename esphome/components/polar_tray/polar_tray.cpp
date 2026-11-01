#include "polar_tray.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome::polar_tray {

static const char *const TAG = "polar_tray";

// Stock constants (analysis/static/README.md section 2, NVS new_dev_attr defaults from DEV_ATTR_newAttrInit @0x4200bf94):
static const uint32_t MONITOR_PERIOD_MS = 500;      // Feeding_Monitor / Control_ThreadHandle loop, vTaskDelay(500) @0x4200f6d0
static const uint32_t STALL_BRAKE_MS = 1000;        // FEED_PLAN_plateHandle @0x4200b2c8: brake, delay 1000 ms
static const uint32_t STALL_REVERSE_MS = 2000;      // ... reverse for 4 loops (2 s); emulation saw ~1.8 s (5.3)
static const uint32_t STALL_SETTLE_MS = 100;        // OURS: short brake between reverse and forward (not in stock)
static const uint32_t RESUME_GATE_IGNORE_MS = 2000; // "GPIO10 IRQ re-enabled after 2000 ms" after restore
static const uint32_t HOME_MAX_PULSE_MS = 1000;     // gpio_task_example @0x4201d47c: dt < 1000 ms and pulses < 4
static const uint8_t HOME_MIN_PULSES = 3;           // ... pulses > 2
static const uint32_t HOME_WIDTH_TOLERANCE_MS = 150;  // ... dt >= max_dt - 150 ms
static const uint32_t BRAKE_REASSERT_MS = 1000;     // OURS: re-assert brake while idle (recovers a failed I2C write)
static const uint32_t JOG_MAX_MS = 3000;            // OURS: test-aid limit

void IRAM_ATTR GateStore::gpio_intr(GateStore *arg) {
  const uint32_t now = micros();
  const uint8_t h = arg->head;
  const uint8_t next = (h + 1) % SIZE;
  if (next == arg->tail) {
    arg->overflows = arg->overflows + 1;
    return;
  }
  arg->ts_us[h] = now;
  arg->level[h] = arg->pin.digital_read() ? 1 : 0;
  arg->head = next;
}

void PolarTray::setup() {
  this->pin_a_->setup();
  this->pin_b_->setup();
  this->motor_(Motor::BRAKE);  // stock Motor_init @0x4200d1a2: P0_5=1, P0_6=1

  this->gate_pin_->setup();  // input, pull-up (stock AllDet_init @0x4200ce24, mask 0x400, pull-up, any edge)
  this->store_.pin = this->gate_pin_->to_isr();
  this->gate_pin_->attach_interrupt(GateStore::gpio_intr, &this->store_, gpio::INTERRUPT_ANY_EDGE);
  this->stable_level_ = this->gate_pin_->digital_read();

  if (this->current_sensing_) {
    // Stock Read_PlateMotorAdc @0x4200d37e uses adc2_get_raw(ADC2_CH0) at 11 dB, 12 bit (Adc_init @0x4200d26e).
    // IDF 5.5 refuses ADC2 oneshot on the C3 unless CONFIG_ADC_ONESHOT_FORCE_USE_ADC2_ON_C3=y (set by __init__.py).
    // When Wi-Fi holds the ADC2 arbiter the sample is flagged invalid and adc_oneshot_read returns ESP_ERR_TIMEOUT;
    // such samples are retried and otherwise skipped (neither count towards nor reset the stall counter).
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = ADC_UNIT_2;
    ucfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &this->adc_);
    if (err == ESP_OK) {
      adc_oneshot_chan_cfg_t ccfg = {};
      ccfg.atten = ADC_ATTEN_DB_12;
      ccfg.bitwidth = ADC_BITWIDTH_12;
      err = adc_oneshot_config_channel(this->adc_, (adc_channel_t) this->adc_channel_, &ccfg);
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "ADC2 init failed (%d): current stall detection disabled, only timeouts protect the motor", err);
      this->current_sensing_ = false;
      this->adc_ = nullptr;
    }
  }
  this->set_state_(TrayState::IDLE);
  this->publish_position_();
  if (this->homing_text_ != nullptr)
    this->homing_text_->publish_state("not homed");
  if (this->homed_bs_ != nullptr)
    this->homed_bs_->publish_state(false);
  this->last_tick_ms_ = millis();
}

void PolarTray::dump_config() {
  ESP_LOGCONFIG(TAG, "Polar tray:");
  LOG_PIN("  Motor A (fwd): ", this->pin_a_);
  LOG_PIN("  Motor B (rev): ", this->pin_b_);
  LOG_PIN("  Gate: ", this->gate_pin_);
  ESP_LOGCONFIG(TAG,
                "  Compartments: %u%s\n"
                "  Current sensing: %s (ADC2 ch%d, threshold %d raw x %u samples)\n"
                "  Stall retries: %u\n"
                "  Edge timeout: %" PRIu32 " ms, max move: %" PRIu32 " ms, gate ignore: %" PRIu32 " ms",
                this->compartments_, this->compartments_ > 3 ? " (EXPERIMENTAL timed sub-steps)" : "",
                YESNO(this->current_sensing_), this->adc_channel_, this->stall_threshold_, this->stall_samples_,
                this->stall_retries_max_, this->edge_timeout_ms_, this->max_move_ms_, this->gate_ignore_ms_);
}

// ---------------------------------------------------------------------------------------------- motor
void PolarTray::motor_(Motor m) {
  // H-bridge on AW9523B P0_5 (A) / P0_6 (B): 10 forward, 01 reverse, 11 brake
  // (Device_Module_Ctrl @0x4200ea0e, "GPIO_MOTOR_BRAKE" in FEED_PLAN_plateHandle @0x4200b2c8, emulation 5.3).
  // Direction changes always pass through brake (1,1), never through 00 or a shoot-through state.
  this->pin_a_->digital_write(true);
  this->pin_b_->digital_write(true);
  if (m == Motor::FORWARD) {
    this->pin_b_->digital_write(false);
  } else if (m == Motor::REVERSE) {
    this->pin_a_->digital_write(false);
  }
  this->motor_state_ = m;
}

void PolarTray::power_on_() {
  // Stock keeps P0_4/P0_7 on whenever on DC, and on battery switches them on only while a motor moves
  // (CTRL_THREAD_batteryModeHandle @0x4200e4fc). What they power is unknown; switch them on before any move.
  for (auto *s : this->power_switches_) {
    if (!s->state)
      s->turn_on();
  }
}

// ---------------------------------------------------------------------------------------------- commands
bool PolarTray::start_move_(int target_index, bool homing, bool raw_step) {
  if (this->inhibited_) {
    ESP_LOGW(TAG, "Move refused: inhibited");
    return false;
  }
  if (this->state_ == TrayState::FAULT) {
    ESP_LOGW(TAG, "Move refused: in fault, clear it first");
    return false;
  }
  if (this->is_busy()) {
    ESP_LOGW(TAG, "Move refused: busy");
    return false;
  }
  const uint8_t k = this->k_();
  this->timed_phase_ = false;
  this->homing_ = homing;
  this->raw_step_ = raw_step;
  if (homing) {
    this->homed_ = false;
    this->mark_ = -1;
    this->pulses_ = 0;
    this->max_width_ms_ = 0;
    this->target_ = 0;
    this->marks_remaining_ = 0;
    if (this->homing_text_ != nullptr)
      this->homing_text_->publish_state("homing");
    if (this->homed_bs_ != nullptr)
      this->homed_bs_->publish_state(false);
  } else if (raw_step) {
    this->target_ = -1;
    this->marks_remaining_ = 1;
  } else {
    const int cur = this->get_position() - 1;
    if (cur < 0)
      return false;
    if (target_index == cur) {
      ESP_LOGI(TAG, "Already at compartment %d", cur + 1);
      this->arrived_trigger_.trigger();
      return true;
    }
    const int cur_mark = cur / k, cur_sub = cur % k;
    const int t_mark = target_index / k, t_sub = target_index % k;
    int m = (t_mark - cur_mark + 3) % 3;
    if (m == 0 && t_sub <= cur_sub)
      m = 3;  // behind us on the same segment: forward-only, so a full turn
    this->target_ = target_index;
    this->marks_remaining_ = m;
    if (m == 0) {
      this->timed_phase_ = true;
      this->timed_goal_ms_ = t_sub * this->sub_step_ms_();
    }
  }
  const uint32_t now = millis();
  this->edges_this_move_ = 0;
  this->position_suspect_ = false;
  this->consecutive_invalid_ = 0;
  this->move_adc_total_ = 0;
  this->move_adc_invalid_ = 0;
  this->retries_ = 0;
  this->over_count_ = 0;
  this->have_rise_ = false;
  this->prev_mark_valid_ = false;
  this->move_start_ms_ = now;
  this->last_progress_ms_ = now;
  this->last_sample_ms_ = now;
  this->last_tick_ms_ = now;
  // Stock start_move (Device_Module_Ctrl @0x4200ea0e): Close_SW_Interrupt @0x4201da2c, re-enable after 500 ms.
  // OURS: if a sub-step stop left the next mark closer than the ignore window, shorten the window.
  uint32_t ignore_ms = this->gate_ignore_ms_;
  if (!homing && !raw_step && this->homed_ && this->interval_ms_ > 0 && this->seg_run_ms_ < this->interval_ms_) {
    const uint32_t half_to_next = (this->interval_ms_ - this->seg_run_ms_) / 2;
    if (half_to_next < ignore_ms)
      ignore_ms = half_to_next;
  }
  this->ignore_until_us_ = micros() + ignore_ms * 1000;
  this->ignore_active_ = true;
  this->store_.tail = this->store_.head;  // drop edges captured while idle
  this->stable_level_ = this->gate_pin_->digital_read();
  this->cand_valid_ = false;
  this->power_on_();
  this->motor_(Motor::FORWARD);  // PLATE_READY_SET_POS: P0_5=1, P0_6=0
  this->hf_.start();
  ESP_LOGI(TAG, "Move start: %s, target %d, marks to pass %d%s", homing ? "homing" : raw_step ? "raw step" : "go to",
           this->get_target(), this->marks_remaining_, this->timed_phase_ ? ", timed sub-step" : "");
  this->set_state_(TrayState::MOVING, homing ? "homing" : "moving");
  if (this->target_sensor_ != nullptr)
    this->target_sensor_->publish_state(this->target_ < 0 ? NAN : this->target_ + 1);
  return true;
}

bool PolarTray::home() { return this->start_move_(0, true, false); }

bool PolarTray::step() {
  if (this->homed_)
    return this->go_to(((this->get_position()) % this->compartments_) + 1);
  if (this->compartments_ == 3) {
    // Stock rotate (SET_PLATE_POS_SERVICE / emulation 5.3): run to the next falling edge. Position stays unknown.
    ESP_LOGW(TAG, "Not homed: raw step to the next gate mark, position stays unknown");
    return this->start_move_(-1, false, true);
  }
  ESP_LOGW(TAG, "Step refused: sub-step mode needs homing first");
  return false;
}

bool PolarTray::go_to(int compartment) {
  if (compartment < 1 || compartment > this->compartments_) {
    ESP_LOGW(TAG, "Go to %d refused: valid range 1..%u", compartment, this->compartments_);
    return false;
  }
  if (!this->homed_) {
    ESP_LOGW(TAG, "Go to refused: not homed");
    return false;
  }
  if (this->k_() > 1 && this->interval_ms_ == 0) {
    ESP_LOGW(TAG, "Go to refused: mark interval not learned yet (home first)");
    return false;
  }
  return this->start_move_(compartment - 1, false, false);
}

bool PolarTray::jog(bool forward, uint32_t ms) {
  if (this->inhibited_ || this->is_busy() || this->state_ == TrayState::FAULT) {
    ESP_LOGW(TAG, "Jog refused (inhibited, busy or fault)");
    return false;
  }
  if (ms > JOG_MAX_MS)
    ms = JOG_MAX_MS;
  this->homed_ = false;  // any untracked motion invalidates the position
  this->mark_ = -1;
  if (this->homing_text_ != nullptr)
    this->homing_text_->publish_state("not homed");
  if (this->homed_bs_ != nullptr)
    this->homed_bs_->publish_state(false);
  this->jog_ms_ = ms;
  this->phase_start_ms_ = millis();
  this->move_start_ms_ = this->phase_start_ms_;
  this->power_on_();
  this->motor_(forward ? Motor::FORWARD : Motor::REVERSE);
  this->hf_.start();
  ESP_LOGI(TAG, "Jog %s %" PRIu32 " ms", forward ? "forward" : "reverse", ms);
  this->set_state_(TrayState::JOG, "jog");
  this->publish_position_();
  return true;
}

bool PolarTray::change_compartments(uint8_t n) {
  if (n != 3 && n != 6 && n != 9 && n != 12) {
    ESP_LOGW(TAG, "Compartments %u refused: must be 3, 6, 9 or 12", n);
    return false;
  }
  if (n == this->compartments_)
    return true;
  if (this->is_busy()) {
    ESP_LOGW(TAG, "Compartment change refused while moving");
    return false;
  }
  ESP_LOGW(TAG, "Compartments %u -> %u%s: homing required", this->compartments_, n,
           n > 3 ? " (EXPERIMENTAL timed sub-steps)" : "");
  this->compartments_ = n;
  this->homed_ = false;
  this->mark_ = -1;
  this->target_ = -1;
  if (this->homing_text_ != nullptr)
    this->homing_text_->publish_state("not homed");
  if (this->homed_bs_ != nullptr)
    this->homed_bs_->publish_state(false);
  this->publish_position_();
  return true;
}

void PolarTray::stop(const char *reason) {
  this->motor_(Motor::BRAKE);
  this->hf_.stop();
  if (this->is_busy()) {
    ESP_LOGW(TAG, "Stopped: %s", reason);
    if (this->homing_ || this->state_ != TrayState::MOVING) {
      // Interrupted homing or stall recovery: position not trusted.
      this->homed_ = false;
      this->mark_ = -1;
    }
    this->homing_ = false;
    this->raw_step_ = false;
    this->timed_phase_ = false;
    this->target_ = -1;
    this->set_state_(TrayState::IDLE);
    this->publish_position_();
    if (this->homing_text_ != nullptr)
      this->homing_text_->publish_state(this->homed_ ? "homed" : "not homed");
    if (this->homed_bs_ != nullptr)
      this->homed_bs_->publish_state(this->homed_);
  }
}

void PolarTray::clear_fault() {
  if (this->state_ == TrayState::FAULT) {
    ESP_LOGI(TAG, "Fault cleared (tray must be homed again)");
    this->set_state_(TrayState::IDLE);
  }
}

void PolarTray::set_inhibit(bool inhibit, const char *reason) {
  if (inhibit && !this->inhibited_) {
    ESP_LOGW(TAG, "Inhibited: %s", reason);
    this->stop(reason);
  } else if (!inhibit && this->inhibited_) {
    ESP_LOGI(TAG, "Inhibit released (%s)", reason);
  }
  this->inhibited_ = inhibit;
}

int PolarTray::get_position() const {
  if (!this->homed_ || this->mark_ < 0)
    return 0;
  const uint8_t k = this->k_();
  int sub = 0;
  if (k > 1 && this->interval_ms_ > 0) {
    const uint32_t ss = this->sub_step_ms_();
    sub = (int) ((this->seg_run_ms_ + ss / 2) / ss);
    if (sub > k - 1)
      sub = k - 1;
  }
  return this->mark_ * k + sub + 1;
}

// ---------------------------------------------------------------------------------------------- loop
void PolarTray::loop() {
  // Gate edges from the ISR
  // OURS: level-stable filter. A transition counts only once the new level has held for gate_debounce.
  while (this->store_.tail != this->store_.head) {
    const uint8_t t = this->store_.tail;
    const uint32_t ts = this->store_.ts_us[t];
    const bool level = this->store_.level[t] != 0;
    this->store_.tail = (t + 1) % GateStore::SIZE;
    if (level == this->stable_level_) {
      this->cand_valid_ = false;  // glitch: back to the stable level
    } else {
      this->cand_valid_ = true;
      this->cand_level_ = level;
      this->cand_ts_us_ = ts;
    }
  }
  if (this->cand_valid_ && micros() - this->cand_ts_us_ >= this->gate_debounce_us_) {
    this->cand_valid_ = false;
    this->stable_level_ = this->cand_level_;
    this->handle_edge_(this->cand_ts_us_, this->stable_level_);
  }
  if (this->store_.overflows != 0) {
    ESP_LOGW(TAG, "Gate edge buffer overflow (%" PRIu32 " edges lost): chattering sensor?", this->store_.overflows);
    this->store_.overflows = 0;
  }

  const uint32_t now = millis();
  const uint32_t dt = now - this->last_tick_ms_;
  this->last_tick_ms_ = now;

  // Hard limit on any motor activity (OURS; stock only has the 60 s plateErr timer).
  if (this->is_busy() && now - this->move_start_ms_ > this->max_move_ms_) {
    this->fault_("hard move timeout");
    return;
  }

  switch (this->state_) {
    case TrayState::MOVING: {
      this->seg_run_ms_ += dt;
      if (this->timed_phase_ && this->seg_run_ms_ >= this->timed_goal_ms_) {
        ESP_LOGI(TAG, "Timed sub-step reached (%" PRIu32 " ms after mark)", this->seg_run_ms_);
        this->arrive_();
        return;
      }
      // Stock plateErr timer (FUN_ram_4201dbd4, plateErrTimeout default 60 s): no gate edge -> error.
      // OURS: restarted at every counted edge instead of being stopped at the first one.
      // OURS: once the mark interval is known, also fault after 3x that interval (min 5 s) without an edge.
      uint32_t edge_to = this->edge_timeout_ms_;
      if (this->interval_ms_ > 0) {
        const uint32_t adaptive = std::max<uint32_t>(3 * this->interval_ms_, 5000);
        edge_to = std::min(edge_to, adaptive);
      }
      if (now - this->last_progress_ms_ > edge_to) {
        this->fault_("no gate edge (plateErr timeout)");
        return;
      }
      if (now - this->last_sample_ms_ >= MONITOR_PERIOD_MS) {
        this->last_sample_ms_ = now;
        this->stall_check_(now);
      }
      break;
    }
    case TrayState::STALL_BRAKE:
      if (now - this->phase_start_ms_ >= STALL_BRAKE_MS) {
        this->motor_(Motor::REVERSE);  // PLATE_READY_BACK_POS: P0_5=0, P0_6=1
        this->phase_start_ms_ = now;
        this->set_state_(TrayState::STALL_REVERSE, "stall recovery: reverse");
      }
      break;
    case TrayState::STALL_REVERSE:
      this->seg_run_ms_ = this->seg_run_ms_ > dt ? this->seg_run_ms_ - dt : 0;  // rough: assumes equal speed
      if (now - this->phase_start_ms_ >= STALL_REVERSE_MS) {
        this->motor_(Motor::BRAKE);
        this->phase_start_ms_ = now;
        this->set_state_(TrayState::STALL_SETTLE, "stall recovery: settle");
      }
      break;
    case TrayState::STALL_SETTLE:
      if (now - this->phase_start_ms_ >= STALL_SETTLE_MS) {
        ESP_LOGI(TAG, "plate motor restore: forward again (retry %u/%u)", this->retries_, this->stall_retries_max_);
        // OURS: edges may have been lost or double counted during the reverse; distrust the count.
        this->position_suspect_ = true;
        if (this->homing_) {
          this->pulses_ = 0;
          this->max_width_ms_ = 0;
          this->edges_this_move_ = 0;
        }
        this->ignore_until_us_ = micros() + RESUME_GATE_IGNORE_MS * 1000;
        this->ignore_active_ = true;
        this->store_.tail = this->store_.head;
        this->have_rise_ = false;
        this->over_count_ = 0;
        this->last_progress_ms_ = now;
        this->last_sample_ms_ = now;
        this->motor_(Motor::FORWARD);
        this->set_state_(TrayState::MOVING, this->homing_ ? "homing" : "moving");
      }
      break;
    case TrayState::JOG:
      if (now - this->phase_start_ms_ >= this->jog_ms_) {
        this->motor_(Motor::BRAKE);
        this->hf_.stop();
        this->set_state_(TrayState::IDLE);
      }
      break;
    case TrayState::IDLE:
    case TrayState::FAULT:
      if (now - this->phase_start_ms_ >= BRAKE_REASSERT_MS) {
        this->phase_start_ms_ = now;
        this->motor_(Motor::BRAKE);  // no I2C traffic unless a previous write failed
      }
      break;
  }
}

void PolarTray::handle_edge_(uint32_t ts_us, bool level) {
  if (this->state_ != TrayState::MOVING) {
    // Edges during stall reverse/brake or idle are ignored (stock disables the GPIO10 IRQ through recovery).
    this->have_rise_ = false;
    return;
  }
  if (this->ignore_active_) {
    if ((int32_t) (ts_us - this->ignore_until_us_) < 0)
      return;  // Close_SW_Interrupt window: the stock IRQ is disabled, so rising edges are lost too
    this->ignore_active_ = false;
  }
  if (level) {
    // Rising edge: start of a mark. Stock remembers the timestamp to time the high pulse (HW 5.2.0).
    this->last_rise_us_ = ts_us;
    this->have_rise_ = true;
    return;
  }
  const uint32_t width = this->have_rise_ ? (ts_us - this->last_rise_us_) / 1000 : UINT32_MAX;
  this->have_rise_ = false;
  this->counted_edge_(ts_us, width);
}

void PolarTray::counted_edge_(uint32_t ts_us, uint32_t width_ms) {
  // HW 5.2.0 acts on the falling edge (gpio_task_example @0x4201d47c), then Plate_Micro_Handle @0x4200e3b0.
  this->ignore_until_us_ = ts_us + this->gate_ignore_ms_ * 1000;  // stock: GPIO10 IRQ off for 500 ms after a step
  this->ignore_active_ = true;
  this->last_progress_ms_ = millis();
  this->edges_this_move_++;

  if (this->prev_mark_valid_) {
    const uint32_t iv = (ts_us - this->prev_mark_us_) / 1000;
    if (iv > 1000 && iv < 20000) {  // plausibility window (OURS); emulation model ~3.1 s per mark (5.3, inferred)
      this->interval_ms_ = this->interval_ms_ == 0 ? iv : (3 * this->interval_ms_ + iv) / 4;
      if (this->interval_sensor_ != nullptr)
        this->interval_sensor_->publish_state(this->interval_ms_);
    }
  }
  this->prev_mark_us_ = ts_us;
  this->prev_mark_valid_ = true;
  if (width_ms != UINT32_MAX && this->width_sensor_ != nullptr)
    this->width_sensor_->publish_state(width_ms);

  // step(): current = next(current) (FUN_ram_4200e39c). Time since the edge = latency of this loop.
  if (this->mark_ >= 0)
    this->mark_ = (this->mark_ + 1) % 3;
  this->seg_run_ms_ = (micros() - ts_us) / 1000;
  ESP_LOGD(TAG, "Gate mark: width %" PRIu32 " ms, mark %d, edges %u", width_ms == UINT32_MAX ? 0 : width_ms,
           this->mark_, this->edges_this_move_);

  if (this->timed_phase_) {
    ESP_LOGW(TAG, "Timed sub-step overran into the next mark: stopping at the mark");
    this->target_ = this->mark_ < 0 ? -1 : this->mark_ * this->k_();
    this->arrive_();
    return;
  }

  if (this->homing_) {
    // HW 5.2.0 home detection: the widest of the 3 marks is plate 1 (gpio_task_example @0x4201d47c).
    if (width_ms < HOME_MAX_PULSE_MS && this->pulses_ < 4) {
      this->widths_[this->pulses_] = width_ms;
      if (width_ms > this->max_width_ms_)
        this->max_width_ms_ = width_ms;
      this->pulses_++;
    }
    if (this->pulses_ >= HOME_MIN_PULSES && width_ms != UINT32_MAX &&
        width_ms + HOME_WIDTH_TOLERANCE_MS >= this->max_width_ms_) {
      // OURS: stock accepts any pulse within 150 ms of the widest; we also require the widest mark to be
      // distinct (>= home_width_ratio x the runner-up; emulation/NVS log: ~160 ms vs ~510 ms).
      // The runner-up is the widest mark clearly narrower than the widest (outside the tolerance): when homing
      // starts just before the home mark, the window holds it twice (e.g. 502, 161, 154, 497 ms, seen
      // 2026-11-01 from compartment 3) and the second pass of the same mark is not a competitor.
      uint32_t second = 0;
      for (uint8_t i = 0; i < this->pulses_; i++) {
        if (this->widths_[i] + HOME_WIDTH_TOLERANCE_MS < this->max_width_ms_)
          second = std::max(second, this->widths_[i]);
      }
      if (second > 0 && (float) this->max_width_ms_ >= this->home_ratio_ * (float) second) {
        ESP_LOGI(TAG, "Home mark found: width %" PRIu32 " ms (widest %" PRIu32 ", runner-up %" PRIu32 ") after %u edges",
                 width_ms, this->max_width_ms_, second, this->edges_this_move_);
        this->mark_ = 0;
        this->homed_ = true;
        this->homing_ = false;
        this->position_suspect_ = false;
        if (this->homing_text_ != nullptr)
          this->homing_text_->publish_state("homed");
        if (this->homed_bs_ != nullptr)
          this->homed_bs_->publish_state(true);
        // Stock stops only when current==target on a LATER edge (one more turn); we stop here, at the same
        // place (falling edge of the home mark), one turn earlier.
        this->arrive_();
        return;
      }
      if (this->pulses_ >= 4) {
        ESP_LOGE(TAG, "Mark widths not distinct: widest %" PRIu32 " ms, runner-up %" PRIu32 " ms", this->max_width_ms_,
                 second);
        this->fault_("home mark not found (widths not distinct)");
        return;
      }
    }
    if (this->edges_this_move_ >= this->home_max_edges_)
      this->fault_("home mark not found");  // OURS: stock has no such limit
    return;
  }

  if (this->raw_step_) {
    this->arrive_();
    return;
  }

  if (--this->marks_remaining_ <= 0) {
    const int sub = this->target_ % this->k_();
    if (sub == 0) {
      this->arrive_();
    } else {
      // EXPERIMENTAL: timed sub-step after the mark, using the learned mark-to-mark time.
      this->timed_phase_ = true;
      this->timed_goal_ms_ = sub * this->sub_step_ms_();
      this->last_progress_ms_ = millis();
    }
  }
}

void PolarTray::arrive_() {
  this->motor_(Motor::BRAKE);  // "GPIO_MOTOR_BRAKE": P0_5=1, P0_6=1
  this->hf_.stop();
  this->publish_adc_stats_();
  if (this->position_suspect_ && this->homed_) {
    ESP_LOGW(TAG, "Stall recovery happened during this move: position no longer trusted, home again");
    this->homed_ = false;
    this->mark_ = -1;
    if (this->homing_text_ != nullptr)
      this->homing_text_->publish_state("not homed");
    if (this->homed_bs_ != nullptr)
      this->homed_bs_->publish_state(false);
  }
  this->position_suspect_ = false;
  this->homing_ = false;
  this->raw_step_ = false;
  this->timed_phase_ = false;
  this->set_state_(TrayState::IDLE);
  this->publish_position_();
  ESP_LOGI(TAG, "Arrived: position %d (move took %" PRIu32 " ms)", this->get_position(),
           millis() - this->move_start_ms_);
  this->arrived_trigger_.trigger();
}

void PolarTray::fault_(const char *reason) {
  this->motor_(Motor::BRAKE);
  this->hf_.stop();
  this->publish_adc_stats_();
  ESP_LOGE(TAG, "FAULT: %s (brake on, homing required)", reason);
  this->homing_ = false;
  this->raw_step_ = false;
  this->timed_phase_ = false;
  this->homed_ = false;  // OURS: any fault invalidates the position
  this->mark_ = -1;
  this->target_ = -1;
  if (this->homing_text_ != nullptr)
    this->homing_text_->publish_state("not homed");
  if (this->homed_bs_ != nullptr)
    this->homed_bs_->publish_state(false);
  char buf[64];
  snprintf(buf, sizeof(buf), "fault: %s", reason);
  this->set_state_(TrayState::FAULT, buf);
  this->publish_position_();
  this->fault_trigger_.trigger(std::string(reason));
}

bool PolarTray::read_current_(int *raw) {
  for (int i = 0; i < 3; i++) {
    this->adc_total_++;
    int v = -1;
    if (adc_oneshot_read(this->adc_, (adc_channel_t) this->adc_channel_, &v) == ESP_OK && v >= 0) {
      *raw = v;
      return true;
    }
    this->adc_invalid_++;
  }
  return false;
}

void PolarTray::stall_check_(uint32_t now) {
  // Feeding_Monitor @0x4200b8dc: adc2_raw(GPIO5) > plateStuckCurrent (400) for 6 loops (3 s) while MOVING -> STUCK.
  if (!this->current_sensing_ || this->adc_ == nullptr)
    return;
  int raw;
  this->move_adc_total_++;
  if (!this->read_current_(&raw)) {
    this->move_adc_invalid_++;
    this->consecutive_invalid_++;
    ESP_LOGV(TAG, "ADC2 sample invalid (Wi-Fi arbiter), skipped");
    if (this->consecutive_invalid_ == this->stall_samples_)
      ESP_LOGW(TAG, "ADC2: %u consecutive invalid samples, current stall detection is blind", this->consecutive_invalid_);
    if (this->max_invalid_ > 0 && this->consecutive_invalid_ >= this->max_invalid_)
      this->fault_("ADC2 current unavailable (invalid samples)");  // OURS
    return;
  }
  this->consecutive_invalid_ = 0;
  if (this->current_sensor_ != nullptr)
    this->current_sensor_->publish_state(raw);
  if (raw > this->stall_threshold_) {
    this->over_count_++;
  } else {
    this->over_count_ = 0;
  }
  if (this->over_count_ < this->stall_samples_)
    return;
  this->over_count_ = 0;
  this->prev_mark_valid_ = false;
  if (this->retries_ < this->stall_retries_max_) {
    this->retries_++;
    ESP_LOGW(TAG, "MOTOR_STUCK (raw %d > %d): recovery %u/%u", raw, this->stall_threshold_, this->retries_,
             this->stall_retries_max_);
    this->motor_(Motor::BRAKE);
    this->phase_start_ms_ = now;
    this->set_state_(TrayState::STALL_BRAKE, "stall recovery: brake");
  } else {
    // "MOTOR_STUCK stop plate motor"; during a feed stock closes the record with error 0x7fc.
    this->fault_("motor stuck (current)");
  }
}

void PolarTray::set_state_(TrayState s, const char *text) {
  this->state_ = s;
  if (s == TrayState::IDLE || s == TrayState::FAULT)
    this->phase_start_ms_ = millis();
  if (this->state_text_ != nullptr) {
    const char *t = text;
    if (t == nullptr)
      t = s == TrayState::IDLE ? "idle" : s == TrayState::FAULT ? "fault" : "busy";
    if (this->state_text_->state != t)
      this->state_text_->publish_state(t);
  }
  if (this->fault_bs_ != nullptr)
    this->fault_bs_->publish_state(s == TrayState::FAULT);
  if (this->moving_bs_ != nullptr)
    this->moving_bs_->publish_state(this->is_busy());
}

void PolarTray::publish_adc_stats_() {
  if (this->move_adc_total_ == 0)
    return;
  const float pct = 100.0f * (float) this->move_adc_invalid_ / (float) this->move_adc_total_;
  if (this->move_adc_invalid_ > 0)
    ESP_LOGI(TAG, "ADC2 invalid samples this move: %" PRIu32 "/%" PRIu32, this->move_adc_invalid_, this->move_adc_total_);
  if (this->adc_invalid_sensor_ != nullptr)
    this->adc_invalid_sensor_->publish_state(pct);
  this->move_adc_total_ = 0;
  this->move_adc_invalid_ = 0;
}

void PolarTray::publish_position_() {
  if (this->position_sensor_ != nullptr) {
    const int p = this->get_position();
    const float v = p == 0 ? NAN : (float) p;
    if (!(std::isnan(v) && std::isnan(this->position_sensor_->state)) && v != this->position_sensor_->state)
      this->position_sensor_->publish_state(v);
  }
  if (this->target_sensor_ != nullptr && !this->is_busy())
    this->target_sensor_->publish_state(this->target_ < 0 ? NAN : this->target_ + 1);
}

}  // namespace esphome::polar_tray

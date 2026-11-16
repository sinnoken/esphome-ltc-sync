#include "ltc_gen.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <cinttypes>
#include <cstring>
#include <sys/time.h>
#include <time.h>

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include <driver/usb_serial_jtag.h>
#endif

namespace esphome {
namespace ltc_gen {

static const char *const TAG = "ltc_gen";
static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint32_t PREF_MAGIC = 0x4C54434D;  // "LTCM"
static const uint32_t CAL_OFFSETS_MS[] = {60000, 300000, 900000};
static const uint32_t TEMP_PERIOD_MS = 5000;

LtcGen *LtcGen::instance_ = nullptr;

using ltc_core::Rate;

static const char *rate_name(Rate r) {
  switch (r) {
    case Rate::FPS_24: return "24";
    case Rate::FPS_25: return "25";
    case Rate::FPS_2997_DF: return "29.97df";
    default: return "30";
  }
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void LtcGen::setup() {
  instance_ = this;
  boot_ms_ = millis();

  // timebase defaults from YAML, possibly replaced by synced settings
  shared_rate_ = rate_;
  base_origin_ = -(int64_t) llround((double) start_frames_ * ltc_core::frame_period_us(rate_));
  shared_origin_ = base_origin_;
  if (settings_enabled_()) load_settings_();

#ifdef USE_WIFI
  own_wifi_ = false;
  hybrid_ = role_ == Role::NODE && source_ == Source::HOTSPOT;
  if (hybrid_) {
    online_ = online_timeout_ms_ > 0;
    online_until_ms_ = boot_ms_ + online_timeout_ms_;
    if (!online_) {
      // straight to low power; ESPHome's wifi may already have started
      this->set_timeout(100, [this]() { wifi::global_wifi_component->disable(); });
    }
  }
#endif

  q_ = xQueueCreate(64, sizeof(Event));
  if (q_ == nullptr) {
    this->mark_failed();
    return;
  }

  // --- RMT ---
  rmt_tx_channel_config_t cfg = {};
  cfg.gpio_num = (gpio_num_t) pin_->get_pin();
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = 10000000;
  cfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
  cfg.trans_queue_depth = RING;
  cfg.flags.invert_out = invert_;
  if (rmt_new_tx_channel(&cfg, &chan_) != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_tx_channel failed");
    this->mark_failed();
    return;
  }
  rmt_copy_encoder_config_t ecfg = {};
  if (rmt_new_copy_encoder(&ecfg, &enc_) != ESP_OK) {
    this->mark_failed();
    return;
  }
  rmt_tx_event_callbacks_t cbs = {};
  cbs.on_trans_done = rmt_done_cb_;
  rmt_tx_register_event_callbacks(chan_, &cbs, this);
  rmt_enable(chan_);

  if (pps_pin_ != nullptr) {
    pps_pin_->setup();
    pps_pin_->digital_write(false);
    pps_gpio_ = pps_pin_->get_pin();
  }

  // --- engine ---
  engine_.set_depth(5);
  engine_.set_idle_level(false);
  engine_.clock().set_reference(role_ == Role::MASTER);
  if (role_ == Role::NODE && persist_model_) {
    pref_ = global_preferences->make_preference<ModelPref>(fnv1_hash("ltc_gen_temp_model"));
    ModelPref p;
    if (pref_.load(&p) && p.magic == PREF_MAGIC) {
      engine_.clock().model().set_coeffs(p.c0, p.c1, p.c2);
      ESP_LOGI(TAG, "Loaded temperature model c0=%.3f c1=%.4f c2=%.5f", p.c0, p.c1, p.c2);
    }
  }

  // --- master timebase ---
  session_ = esp_random();
  if (role_ == Role::MASTER || source_ != Source::ESPNOW) {
    // master: its own clock; hotspot / wifi node: timecode = AP TSF + start_timecode
    Event e = {};
    e.type = Event::TIMEBASE;
    e.local_us = shared_origin_ + output_offset_us_;
    e.value = (double) (int) rate_;
    post_(e);
  }

  // --- temperature sensor ---
#if SOC_TEMP_SENSOR_SUPPORTED
  temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  if (temperature_sensor_install(&tcfg, &tsens_) == ESP_OK) {
    temperature_sensor_enable(tsens_);
  } else {
    ESP_LOGW(TAG, "Temperature sensor unavailable (is internal_temperature also configured?)");
    tsens_ = nullptr;
  }
#endif

  // --- radio ---
  if (!(role_ == Role::NODE && source_ == Source::WIFI)) radio_init_();
  if (role_ == Role::MASTER) {
    radio_on_();
    esp_timer_create_args_t targs = {};
    targs.callback = beacon_timer_cb_;
    targs.arg = this;
    targs.name = "ltc_beacon";
    esp_timer_create(&targs, &beacon_timer_);
    esp_timer_start_periodic(beacon_timer_, 100000);
  }

  // --- real-time task (high priority, small) ---
  xTaskCreate(task_entry_, "ltc_gen", 6144, this, configMAX_PRIORITIES - 3, &task_handle_);
}

void LtcGen::dump_config() {
  ESP_LOGCONFIG(TAG, "LTC generator:");
  LOG_PIN("  Output pin: ", pin_);
  if (pps_pin_ != nullptr) LOG_PIN("  PPS pin: ", pps_pin_);
  ESP_LOGCONFIG(TAG, "  Role: %s", role_ == Role::MASTER ? "master" : "node");
  if (role_ == Role::NODE) {
    if (source_ == Source::HOTSPOT) {
      ESP_LOGCONFIG(TAG, "  Source: phone hotspot '%s'", hotspot_ssid_.c_str());
    } else if (source_ == Source::WIFI) {
      ESP_LOGCONFIG(TAG, "  Source: beacons of the connected access point");
    } else {
      ESP_LOGCONFIG(TAG, "  Source: ESP-NOW master");
    }
  }
  ESP_LOGCONFIG(TAG, "  Frame rate: %s", rate_name(rate_));
  ESP_LOGCONFIG(TAG, "  ESP-NOW channel: %u%s", channel_, own_wifi_ ? "" : " (follows the wifi component)");
  if (role_ == Role::NODE) {
    ESP_LOGCONFIG(TAG, "  Initial window: %" PRIu32 " ms, window: %" PRIu32 " ms, resync: %" PRIu32 " ms",
                  initial_window_ms_, window_ms_, resync_ms_);
    ESP_LOGCONFIG(TAG, "  Temperature compensation: %s", YESNO(temp_comp_));
  }
  ESP_LOGCONFIG(TAG, "  Output offset: %" PRId32 " us", output_offset_us_);
  if (hybrid_)
    ESP_LOGCONFIG(TAG, "  Hybrid: online %" PRIu32 " s after boot, %" PRIu32 " s after a change; USB host keeps online: %s",
                  online_timeout_ms_ / 1000, after_change_ms_ / 1000, YESNO(usb_keeps_online_));
}

// ---------------------------------------------------------------------------
// radio
// ---------------------------------------------------------------------------
void LtcGen::radio_init_() {
  if (own_wifi_) {
    esp_netif_init();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(err));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    if (esp_wifi_init(&cfg) != ESP_OK) {
      ESP_LOGE(TAG, "esp_wifi_init failed");
      return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
  }
  radio_ready_ = true;
}

// Timing always comes from the AP's beacon timestamps (promiscuous capture),
// in every mode, so nodes that are online and nodes in low-power mode share
// exactly the same reference (no beacon-airtime bias between them).
void LtcGen::start_beacon_capture_() {
  wifi_promiscuous_filter_t f = {};
  f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous_rx_cb(promisc_cb_);
  esp_wifi_set_promiscuous(true);
}

void LtcGen::radio_on_() {
  if (radio_on_flag_) return;
  if (role_ == Role::NODE && wifi_phase_()) {
    // Associated through ESPHome's wifi component: sniff our own AP's beacons
    // on the current channel. No channel change, connection stays up.
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;  // not connected: window will fail
    memcpy(hs_bssid_, ap.bssid, 6);
    hs_bssid_known_ = true;
    channel_ = ap.primary;
    hs_channel_known_ = true;
    learned_ssid_ = std::string((const char *) ap.ssid);
    esp_wifi_get_ps(&saved_ps_);
    esp_wifi_set_ps(WIFI_PS_NONE);  // receive every beacon during the window
    start_beacon_capture_();
    radio_on_flag_ = true;
    return;
  }
  if (!radio_ready_) return;
  bool start_radio = own_wifi_ || hybrid_;
  if (start_radio) {
    if (hybrid_) esp_wifi_set_mode(WIFI_MODE_STA);  // ESPHome's wifi is disabled now
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  if (role_ == Role::NODE && source_ == Source::HOTSPOT) {
    if (cfg_channel_ != 0) {
      channel_ = cfg_channel_;
      hs_channel_known_ = true;
    } else if (!hs_channel_known_) {
      hs_channel_known_ = hotspot_find_channel_();
    }
    // Phones may randomise the BSSID per hotspot session: match by SSID again,
    // unless we just learned it while online.
    if (!keep_bssid_) hs_bssid_known_ = false;
    start_beacon_capture_();
    esp_wifi_set_channel(channel_, WIFI_SECOND_CHAN_NONE);
    radio_on_flag_ = true;
    return;
  }
  if (own_wifi_) esp_wifi_set_channel(channel_, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_init failed");
    return;
  }
  if (role_ == Role::MASTER) {
    esp_now_register_send_cb(send_cb_);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;  // current channel
    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    peer.ifidx = (mode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  } else {
    esp_now_register_recv_cb(recv_cb_);
  }
  radio_on_flag_ = true;
}

void LtcGen::radio_off_() {
  if (!radio_on_flag_) return;
  radio_on_flag_ = false;
  if (role_ == Role::NODE && wifi_phase_()) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_ps(saved_ps_);
    return;
  }
  if (role_ == Role::NODE && source_ == Source::HOTSPOT) {
    esp_wifi_set_promiscuous(false);
    if (own_wifi_ || hybrid_) esp_wifi_stop();
    return;
  }
  esp_now_deinit();
  if (own_wifi_) esp_wifi_stop();
}

bool LtcGen::hotspot_find_channel_() {
  wifi_scan_config_t sc = {};
  sc.ssid = (uint8_t *) hotspot_ssid_.c_str();
  sc.show_hidden = false;
  // Passive: the node never transmits (no RF bursts next to the audio path).
  sc.scan_type = WIFI_SCAN_TYPE_PASSIVE;
  sc.scan_time.passive = 150;  // > one beacon interval (102.4 ms) per channel
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
  uint16_t n = 1;
  wifi_ap_record_t rec;
  bool found = esp_wifi_scan_get_ap_records(&n, &rec) == ESP_OK && n > 0;
  esp_wifi_clear_ap_list();
  if (!found) {
    ESP_LOGW(TAG, "Hotspot '%s' not found (2.4 GHz? hotspot screen open?)", hotspot_ssid_.c_str());
    return false;
  }
  channel_ = rec.primary;
  ESP_LOGI(TAG, "Hotspot '%s' on channel %u, RSSI %d", hotspot_ssid_.c_str(), channel_, rec.rssi);
  return true;
}

// Beacon frames of the phone hotspot: the 8-byte timestamp right after the
// 24-byte MAC header is the AP's TSF, inserted by the radio hardware.
void LtcGen::promisc_cb_(void *buf, wifi_promiscuous_pkt_type_t type) {
  int64_t now = esp_timer_get_time();
  LtcGen *self = instance_;
  if (self == nullptr || type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *) buf;
  int len = pkt->rx_ctrl.sig_len;
  const uint8_t *p = pkt->payload;
  if (p[0] == 0xD0) {  // action frame: maybe ESP-NOW settings from an online node
    const uint8_t *body;
    int n = ltc_core::espnow_payload(p, len, &body);
    if (n == (int) sizeof(ltc_core::SettingsPacket)) self->receive_settings_(body);
    return;
  }
  if (len < 40 || p[0] != 0x80) return;  // beacon
  const uint8_t *bssid = p + 16;
  if (self->hs_bssid_known_) {
    if (memcmp(bssid, self->hs_bssid_, 6) != 0) return;
  } else {
    // first IE must be the SSID
    const uint8_t *ie = p + 36;
    size_t sl = self->hotspot_ssid_.size();
    if (ie[0] != 0 || ie[1] != sl || 38 + (int) sl > len || memcmp(ie + 2, self->hotspot_ssid_.data(), sl) != 0) return;
    memcpy(self->hs_bssid_, bssid, 6);
    self->hs_bssid_known_ = true;
  }
  uint64_t tsf = 0;
  for (int i = 7; i >= 0; i--) tsf = (tsf << 8) | p[24 + i];
  Event e = {};
  e.type = Event::TSF_SAMPLE;
  e.local_us = now;
  e.aux = (int64_t) tsf;
  xQueueSend(self->q_, &e, 0);
}

void LtcGen::recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  int64_t now = esp_timer_get_time();  // first thing: this is the receive timestamp
  LtcGen *self = instance_;
  if (self == nullptr) return;
  if (len == (int) sizeof(ltc_core::SettingsPacket)) {
    self->receive_settings_(data);
    return;
  }
  if (len != (int) sizeof(ltc_core::Beacon)) return;
  Event e = {};
  e.type = Event::BEACON;
  e.local_us = now;
  memcpy(&e.beacon, data, sizeof(e.beacon));
  xQueueSend(self->q_, &e, 0);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void LtcGen::send_cb_(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
void LtcGen::send_cb_(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  int64_t now = esp_timer_get_time();
  LtcGen *self = instance_;
  if (self == nullptr) return;
  portENTER_CRITICAL(&self->lock_);
  self->tx_stamp_ = now;
  self->tx_stamp_seq_ = self->last_sent_seq_;
  portEXIT_CRITICAL(&self->lock_);
}

void LtcGen::beacon_timer_cb_(void *arg) { static_cast<LtcGen *>(arg)->send_beacon_(); }

void LtcGen::send_beacon_() {
  if (!radio_on_flag_) return;
  ltc_core::Beacon b = {};
  b.magic = ltc_core::BEACON_MAGIC;
  b.version = ltc_core::BEACON_VERSION;
  b.session = session_;
  portENTER_CRITICAL(&lock_);
  b.rate = (uint8_t) shared_rate_;
  b.origin_net = shared_origin_;
  b.seq = seq_++;
  b.prev_tx_net = (b.seq > 0 && tx_stamp_seq_ == b.seq - 1) ? tx_stamp_ : 0;
  last_sent_seq_ = b.seq;
  portEXIT_CRITICAL(&lock_);
  esp_now_send(BROADCAST, (const uint8_t *) &b, sizeof(b));
}

// ---------------------------------------------------------------------------
// real-time task
// ---------------------------------------------------------------------------
void LtcGen::post_(const Event &e) {
  if (q_ != nullptr) xQueueSend(q_, &e, pdMS_TO_TICKS(10));
}

bool LtcGen::rmt_done_cb_(rmt_channel_handle_t ch, const rmt_tx_done_event_data_t *ed, void *ctx) {
  LtcGen *self = static_cast<LtcGen *>(ctx);
  Event e;
  e.type = Event::FRAME_DONE;
  e.local_us = esp_timer_get_time();
  uint32_t started = ++self->isr_done_;
  if (self->pps_gpio_ >= 0) gpio_set_level((gpio_num_t) self->pps_gpio_, self->pps_flag_[started % RING]);
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(self->q_, &e, &woken);
  return woken == pdTRUE;
}

void LtcGen::task_entry_(void *arg) { static_cast<LtcGen *>(arg)->task_(); }

void LtcGen::task_() {
  Event e;
  for (;;) {
    if (xQueueReceive(q_, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
      switch (e.type) {
        case Event::FRAME_DONE:
          engine_.on_frame_done(e.local_us);
          break;
        case Event::BEACON: {
          int64_t lo, ne;
          if (pairer_.on_beacon(e.beacon, e.local_us, &lo, &ne)) engine_.clock().add_sample(lo, ne);
          if (source_ == Source::ESPNOW && pairer_.have_info()) {
            int64_t o = pairer_.origin() + output_offset_us_;
            if (pairer_.rate() != engine_.rate() || o != engine_.origin()) engine_.set_timebase(pairer_.rate(), o);
          }
          break;
        }
        case Event::TSF_SAMPLE:
          engine_.clock().add_sample(e.local_us, e.aux);
          break;
        case Event::WINDOW_BEGIN:
          engine_.clock().begin_window();
          break;
        case Event::WINDOW_END: {
          bool ok = engine_.clock().end_window();
          portENTER_CRITICAL(&lock_);
          window_result_ = ok ? 1 : -1;
          portEXIT_CRITICAL(&lock_);
          break;
        }
        case Event::TEMPERATURE:
          engine_.clock().update_temperature(e.local_us, e.value);
          break;
        case Event::JAM:
          if (engine_.clock().valid()) {
            int64_t o = (int64_t) llround(engine_.clock().to_net(esp_timer_get_time())) + base_origin_;
            portENTER_CRITICAL(&lock_);
            jam_origin_ = o;
            jam_pending_ = true;
            portEXIT_CRITICAL(&lock_);
          }
          break;
        case Event::TIMEBASE:
          engine_.set_timebase((Rate) (int) e.value, e.local_us);
          break;
      }
    }

    // stream management
    if (streaming_ && engine_.restart_pending() && engine_.in_flight() == 0) {
      streaming_ = false;
      engine_.stop();
    }
    bool have_timebase = role_ == Role::MASTER || source_ != Source::ESPNOW || pairer_.have_info();
    if (!streaming_ && have_timebase && engine_.clock().valid()) start_stream_();
    if (streaming_ && !engine_.restart_pending()) {
      while (engine_.in_flight() < engine_.depth()) queue_frame_();
    }

    // share statistics with the main loop
    const ltc_core::Stats &st = engine_.stats();
    int64_t now = esp_timer_get_time();
    int64_t max_age = resync_ms_ > 0 ? 3LL * resync_ms_ * 1000LL + 60000000LL : INT64_MAX;
    double c0, c1, c2;
    engine_.clock().model().get_coeffs(&c0, &c1, &c2);
    portENTER_CRITICAL(&lock_);
    stats_ = st;
    synced_ = engine_.clock().synced(now, max_age);
    last_anchor_local_ = engine_.clock().last_anchor_local();
    model_c_[0] = c0;
    model_c_[1] = c1;
    model_c_[2] = c2;
    model_learned_ = engine_.clock().model().learned_temperature_terms();
    portEXIT_CRITICAL(&lock_);
  }
}

void LtcGen::start_stream_() {
  int64_t start_local;
  if (!engine_.plan_start(esp_timer_get_time(), 5000, &start_local)) return;
  isr_done_ = 0;
  queued_tx_ = 0;
  int64_t wait = start_local - esp_timer_get_time();
  if (wait > 3000) vTaskDelay(pdMS_TO_TICKS((wait - 2000) / 1000));
  while (esp_timer_get_time() < start_local) {
  }
  for (int i = 0; i < engine_.depth(); i++) queue_frame_();
  streaming_ = true;
}

void LtcGen::queue_frame_() {
  ltc_core::QueuedFrame qf;
  engine_.next_frame(&qf);
  uint32_t q = queued_tx_++;
  rmt_symbol_word_t *b = buf_[q % RING];
  int nw = qf.nseg / 2;
  for (int i = 0; i < nw; i++) {
    b[i].duration0 = qf.seg[2 * i].ticks;
    b[i].level0 = qf.seg[2 * i].level;
    b[i].duration1 = qf.seg[2 * i + 1].ticks;
    b[i].level1 = qf.seg[2 * i + 1].level;
  }
  pps_flag_[q % RING] = qf.second_mark ? 1 : 0;
  rmt_transmit_config_t tc = {};
  tc.loop_count = 0;
  tc.flags.eot_level = 0;
  esp_err_t err = rmt_transmit(chan_, enc_, b, nw * sizeof(rmt_symbol_word_t), &tc);
  if (err != ESP_OK) ESP_LOGW(TAG, "rmt_transmit: %s", esp_err_to_name(err));
}

// ---------------------------------------------------------------------------
// main loop: radio schedule, temperature, master timebase, publishing
// ---------------------------------------------------------------------------
void LtcGen::loop() {
  uint32_t now = millis();
  if (role_ == Role::NODE) node_schedule_();
  if (hybrid_) power_manager_();
  if (settings_enabled_()) settings_loop_();
  if (role_ == Role::MASTER) master_timebase_();
  if (now - last_temp_ms_ >= TEMP_PERIOD_MS) {
    last_temp_ms_ = now;
    read_temperature_();
  }
  if (now - last_publish_ms_ >= 1000) {
    last_publish_ms_ = now;
    publish_();
  }
  if (role_ == Role::NODE && persist_model_ && now - last_save_ms_ >= 1800000) {
    last_save_ms_ = now;
    save_model_();
  }
}

void LtcGen::node_schedule_() {
  uint32_t t = millis() - boot_ms_;
  switch (win_state_) {
    case WinState::IDLE:
      if (sync_now_ || (next_win_ms_ != UINT32_MAX && t >= next_win_ms_)) {
        sync_now_ = false;
        radio_on_();
        Event e = {};
        e.type = Event::WINDOW_BEGIN;
        post_(e);
        win_end_ms_ = t + (ever_synced_ ? window_ms_ : initial_window_ms_);
        win_state_ = WinState::OPEN;
      }
      break;
    case WinState::OPEN:
      if (t >= win_end_ms_) {
        portENTER_CRITICAL(&lock_);
        window_result_ = 0;
        portEXIT_CRITICAL(&lock_);
        Event e = {};
        e.type = Event::WINDOW_END;
        post_(e);
        win_state_ = WinState::CLOSING;
      }
      break;
    case WinState::CLOSING: {
      portENTER_CRITICAL(&lock_);
      int r = window_result_;
      portEXIT_CRITICAL(&lock_);
      if (r == 0) break;
      if (r > 0) {
        radio_off_();
        if (!ever_synced_) {
          ever_synced_ = true;
          sched_idx_ = 0;
          ESP_LOGI(TAG, "Initial sync done, free-running");
        }
        if (sched_idx_ < 3) {
          // calibration windows, measured from the first sync
          if (sched_idx_ == 0) first_sync_ms_ = t;
          next_win_ms_ = first_sync_ms_ + CAL_OFFSETS_MS[sched_idx_];
          sched_idx_++;
        } else {
          uint32_t iv = resync_ms_;
          if (wifi_phase_() && (iv == 0 || iv > 60000)) iv = 60000;  // online: cheap, sync every minute
          next_win_ms_ = iv > 0 ? t + iv : UINT32_MAX;
        }
      } else if (source_ == Source::HOTSPOT && !hs_channel_known_) {
        // hotspot not found: rescan on the next window
        radio_off_();
        next_win_ms_ = t + (ever_synced_ ? 60000 : 5000);
        win_state_ = WinState::IDLE;
        break;
      } else if (!ever_synced_) {
        // nothing heard yet: keep listening (re-arm, e.g. wifi just connected)
        if (!radio_on_flag_) radio_on_();
        Event e = {};
        e.type = Event::WINDOW_BEGIN;
        post_(e);
        win_end_ms_ = t + initial_window_ms_;
        win_state_ = WinState::OPEN;
        if (source_ == Source::HOTSPOT) {
          hs_channel_known_ = false;
          ESP_LOGW(TAG, "No beacons from hotspot '%s' yet", hotspot_ssid_.c_str());
        } else {
          ESP_LOGW(TAG, "No master beacons received yet (channel %u)", channel_);
        }
        break;
      } else {
        radio_off_();
        hs_channel_known_ = false;  // the hotspot may have moved channel
        next_win_ms_ = t + 60000;   // retry in a minute
        ESP_LOGW(TAG, "Sync window failed, retrying in 60 s");
      }
      win_state_ = WinState::IDLE;
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// hybrid power management: online (ESPHome wifi: HA, OTA) <-> low power
// ---------------------------------------------------------------------------
bool LtcGen::usb_host_connected_() {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  return usb_keeps_online_ && usb_serial_jtag_is_connected();
#else
  return false;
#endif
}

void LtcGen::power_manager_() {
  uint32_t now = millis();
  bool hold = charging_ || ota_active_ || usb_host_connected_();
  if (online_) {
    if (hold) {
      if ((int32_t) (online_until_ms_ - (now + 60000)) < 0) online_until_ms_ = now + 60000;
      return;
    }
    bool can_sleep = ever_synced_ || !hotspot_ssid_.empty();
    if ((int32_t) (now - online_until_ms_) >= 0 && win_state_ == WinState::IDLE && can_sleep) go_low_power_();
  } else if ((hold || want_online_) && win_state_ == WinState::IDLE) {
    want_online_ = false;
    go_online_(now + 60000);
  }
}

void LtcGen::go_low_power_() {
#ifdef USE_WIFI
  if (hotspot_ssid_.empty()) hotspot_ssid_ = learned_ssid_;
  keep_bssid_ = hs_bssid_known_;  // same hotspot session as while online
  ESP_LOGI(TAG, "Entering low-power mode (hotspot '%s', channel %u)", hotspot_ssid_.c_str(), channel_);
  espnow_online_stop_();
  online_ = false;
  wifi::global_wifi_component->disable();
  // resync on the normal low-power schedule from now on
  next_win_ms_ = (millis() - boot_ms_) + (resync_ms_ > 0 ? resync_ms_ : UINT32_MAX / 2);
#endif
}

void LtcGen::go_online_(uint32_t until_ms) {
#ifdef USE_WIFI
  ESP_LOGI(TAG, "Going online (Wi-Fi, API, OTA)");
  radio_off_();
  online_ = true;
  keep_bssid_ = false;
  online_until_ms_ = until_ms;
  wifi::global_wifi_component->enable();
  next_win_ms_ = (millis() - boot_ms_) + 15000;  // resync once connected
#endif
}

void LtcGen::enter_low_power() {
  if (hybrid_ && online_) online_until_ms_ = millis();
}

void LtcGen::go_online() {
  if (!hybrid_) return;
  if (online_)
    online_until_ms_ = millis() + online_timeout_ms_;
  else
    want_online_ = true;
}

void LtcGen::master_timebase_() {
#ifdef LTC_GEN_USE_TIME
  if (time_anchored_ || time_ == nullptr || !time_->now().is_valid()) return;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t net = esp_timer_get_time();
  time_t secs = tv.tv_sec;
  struct tm lt;
  localtime_r(&secs, &lt);
  int64_t tod = ((int64_t) (lt.tm_hour * 60 + lt.tm_min) * 60 + lt.tm_sec) * 1000000LL + tv.tv_usec;
  portENTER_CRITICAL(&lock_);
  shared_origin_ = net - tod;
  Rate r = shared_rate_;
  int64_t o = shared_origin_;
  portEXIT_CRITICAL(&lock_);
  time_anchored_ = true;
  Event e = {};
  e.type = Event::TIMEBASE;
  e.local_us = o + output_offset_us_;
  e.value = (double) (int) r;
  post_(e);
  ESP_LOGI(TAG, "Timecode anchored to time of day");
#endif
}

void LtcGen::jam() {
  if (settings_enabled_()) {
    // timecode restarts at start_timecode now, on every node (synced)
    Event e = {};
    e.type = Event::JAM;
    post_(e);
    settings_changed_();
    return;
  }
  if (role_ != Role::MASTER) {
    sync_now();
    return;
  }
  time_anchored_ = false;
#ifdef LTC_GEN_USE_TIME
  if (time_ != nullptr && time_->now().is_valid()) return;  // loop() re-anchors to time of day
#endif
  int64_t net = esp_timer_get_time();
  portENTER_CRITICAL(&lock_);
  shared_origin_ = net + base_origin_;
  session_ = esp_random();
  Rate r = shared_rate_;
  int64_t o = shared_origin_;
  portEXIT_CRITICAL(&lock_);
  Event e = {};
  e.type = Event::TIMEBASE;
  e.local_us = o + output_offset_us_;
  e.value = (double) (int) r;
  post_(e);
}

bool LtcGen::set_fps(const std::string &fps) {
  Rate r;
  if (fps == "24")
    r = Rate::FPS_24;
  else if (fps == "25")
    r = Rate::FPS_25;
  else if (fps == "29.97df" || fps == "29.97")
    r = Rate::FPS_2997_DF;
  else if (fps == "30")
    r = Rate::FPS_30;
  else
    return false;
  if (settings_enabled_()) {
    if ((uint8_t) r != settings_.rate) {
      ltc_core::SettingsPacket s = settings_;
      s.rate = (uint8_t) r;
      local_change_(s);
    }
    settings_changed_();
    return true;
  }
  rate_ = r;
  if (role_ == Role::MASTER) {
    portENTER_CRITICAL(&lock_);
    // the origin is a point in time, independent of the rate, so every node
    // that switches to the same fps ends up with the same labels
    shared_rate_ = r;
    int64_t o = shared_origin_;
    portEXIT_CRITICAL(&lock_);
    Event e = {};
    e.type = Event::TIMEBASE;
    e.local_us = o + output_offset_us_;
    e.value = (double) (int) r;
    post_(e);
  }
  settings_changed_();
  return true;
}

std::string LtcGen::get_fps() const { return rate_name(rate_); }

// ---------------------------------------------------------------------------
// settings sync
// ---------------------------------------------------------------------------
static const uint32_t SETTINGS_TX_MS = 1000;

void LtcGen::load_settings_() {
  group_hash_ = fnv1_hash(sync_group_);
  uint8_t mac[6];
  get_mac_address_raw(mac);
  my_id_ = ((uint32_t) mac[2] << 24) | ((uint32_t) mac[3] << 16) | ((uint32_t) mac[4] << 8) | mac[5];
  settings_ = {};
  settings_.magic = ltc_core::SETTINGS_MAGIC;
  settings_.version = ltc_core::SETTINGS_VERSION;
  settings_.group = group_hash_;
  settings_.rev = 0;
  settings_.author = 0;
  settings_.rate = (uint8_t) rate_;
  settings_.origin = base_origin_;
  settings_.resync_ms = resync_ms_;
  settings_.online_ms = online_timeout_ms_;
  settings_pref_ = global_preferences->make_preference<ltc_core::SettingsPacket>(fnv1_hash("ltc_gen_settings"));
  ltc_core::SettingsPacket p;
  if (settings_pref_.load(&p) && ltc_core::settings_valid(p, group_hash_) && p.rev > 0) {
    apply_settings_(p, false);
    ESP_LOGI(TAG, "Restored synced settings rev %" PRIu32 " (fps %s)", p.rev, rate_name((Rate) p.rate));
  }
}

void LtcGen::apply_settings_(const ltc_core::SettingsPacket &s, bool save) {
  bool tb_changed = s.rate != settings_.rate || s.origin != settings_.origin;
  rate_ = (Rate) s.rate;
  resync_ms_ = s.resync_ms;
  online_timeout_ms_ = s.online_ms;
  portENTER_CRITICAL(&lock_);
  shared_rate_ = rate_;
  shared_origin_ = s.origin;
  portEXIT_CRITICAL(&lock_);
  settings_ = s;
  if (save) settings_pref_.save(&settings_);
  if (tb_changed && q_ != nullptr) {
    Event e = {};
    e.type = Event::TIMEBASE;
    e.local_us = s.origin + output_offset_us_;
    e.value = (double) (int) rate_;
    post_(e);
  }
}

void LtcGen::local_change_(ltc_core::SettingsPacket s) {
  s.magic = ltc_core::SETTINGS_MAGIC;
  s.version = ltc_core::SETTINGS_VERSION;
  s.group = group_hash_;
  s.rev = settings_.rev + 1;  // settings_ is always the newest we have seen
  s.author = my_id_;
  apply_settings_(s, true);
  ESP_LOGI(TAG, "Settings changed locally -> rev %" PRIu32 ", broadcasting", s.rev);
  last_settings_tx_ms_ = 0;  // send right away
}

void LtcGen::receive_settings_(const uint8_t *body) {
  ltc_core::SettingsPacket p;
  memcpy(&p, body, sizeof(p));
  portENTER_CRITICAL(&lock_);
  inbox_ = p;
  inbox_flag_ = true;
  portEXIT_CRITICAL(&lock_);
}

void LtcGen::settings_loop_() {
  // incoming
  portENTER_CRITICAL(&lock_);
  bool have = inbox_flag_;
  ltc_core::SettingsPacket in = inbox_;
  inbox_flag_ = false;
  bool jam = jam_pending_;
  int64_t jo = jam_origin_;
  jam_pending_ = false;
  portEXIT_CRITICAL(&lock_);
  if (have && ltc_core::settings_valid(in, group_hash_) && in.author != my_id_ && ltc_core::settings_newer(in, settings_)) {
    ESP_LOGI(TAG, "Adopting settings rev %" PRIu32 " from %08" PRIX32 " (fps %s)", in.rev, in.author,
             rate_name((Rate) in.rate));
    apply_settings_(in, true);
  }
  if (jam) {
    ltc_core::SettingsPacket s = settings_;
    s.origin = jo;
    local_change_(s);
  }
  // outgoing: only while online (connected), so low-power nodes never transmit
#ifdef USE_WIFI
  bool online = wifi_phase_() && wifi::global_wifi_component->is_connected();
  if (!online) {
    espnow_online_stop_();
    return;
  }
  espnow_online_start_();
  uint32_t now = millis();
  if (espnow_online_ && (last_settings_tx_ms_ == 0 || now - last_settings_tx_ms_ >= SETTINGS_TX_MS)) {
    last_settings_tx_ms_ = now;
    esp_now_send(BROADCAST, (const uint8_t *) &settings_, sizeof(settings_));
  }
#endif
}

void LtcGen::espnow_online_start_() {
  if (espnow_online_) return;
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(recv_cb_);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  espnow_online_ = true;
}

void LtcGen::espnow_online_stop_() {
  if (!espnow_online_) return;
  esp_now_deinit();
  espnow_online_ = false;
}

void LtcGen::set_resync_minutes(float m) {
  if (!settings_enabled_()) {
    resync_ms_ = (uint32_t) (m * 60000);
    return;
  }
  ltc_core::SettingsPacket s = settings_;
  s.resync_ms = (uint32_t) (m * 60000);
  if (s.resync_ms != settings_.resync_ms) local_change_(s);
  settings_changed_();
}

void LtcGen::set_online_minutes(float m) {
  if (!settings_enabled_()) {
    online_timeout_ms_ = (uint32_t) (m * 60000);
    return;
  }
  ltc_core::SettingsPacket s = settings_;
  s.online_ms = (uint32_t) (m * 60000);
  if (s.online_ms != settings_.online_ms) local_change_(s);
  settings_changed_();
}

void LtcGen::reset_settings() {
  if (!settings_enabled_()) return;
  ltc_core::SettingsPacket s = settings_;
  s.rev = 0;
  settings_pref_.save(&s);
  ESP_LOGI(TAG, "Synced settings cleared; YAML defaults apply after restart");
}

void LtcGen::settings_changed_() {
  if (hybrid_ && online_) online_until_ms_ = millis() + after_change_ms_;
}

void LtcGen::read_temperature_() {
#if SOC_TEMP_SENSOR_SUPPORTED
  if (tsens_ == nullptr) return;
  float t;
  if (temperature_sensor_get_celsius(tsens_, &t) != ESP_OK) return;
  last_temp_ = t;
  // Skip while the radio heats the die during a sync window.
  if (role_ == Role::NODE && temp_comp_ && !radio_on_flag_) {
    Event e = {};
    e.type = Event::TEMPERATURE;
    e.local_us = esp_timer_get_time();
    e.value = t;
    post_(e);
  }
#endif
}

std::string LtcGen::timecode() const {
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&lock_));
  ltc_core::Timecode tc = stats_.tc;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&lock_));
  char b[16];
  ltc_core::format_timecode(tc, b, sizeof b);
  return b;
}

bool LtcGen::is_locked() const {
  portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&lock_));
  bool l = stats_.locked && synced_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&lock_));
  return l;
}

void LtcGen::publish_() {
  portENTER_CRITICAL(&lock_);
  ltc_core::Stats st = stats_;
  bool synced = synced_;
  int64_t anchor = last_anchor_local_;
  portEXIT_CRITICAL(&lock_);
#ifdef USE_SENSOR
  if (ppm_sensor_ != nullptr && st.clock_valid) ppm_sensor_->publish_state(st.ppm);
  if (sync_error_sensor_ != nullptr && st.clock_valid) sync_error_sensor_->publish_state(st.sync_residual_us);
  if (phase_error_sensor_ != nullptr && st.clock_valid) phase_error_sensor_->publish_state(st.rms_err_us);
  if (temperature_sensor_ != nullptr && !std::isnan(last_temp_)) temperature_sensor_->publish_state(last_temp_);
#endif
#ifdef USE_TEXT_SENSOR
  if (timecode_sensor_ != nullptr) {
    char b[16];
    ltc_core::format_timecode(st.tc, b, sizeof b);
    timecode_sensor_->publish_state(b);
  }
  if (source_sensor_ != nullptr) {
    std::string s;
    if (role_ == Role::MASTER)
      s = "master";
    else if (!st.clock_valid)
      s = source_ == Source::ESPNOW ? "waiting for master" : (source_ == Source::WIFI ? "waiting for wifi" : "waiting for hotspot");
    else if (source_ == Source::WIFI)
      s = synced ? "online (beacon sync)" : "wifi lost, free-run";
    else if (hybrid_ && online_)
      s = synced ? "online, low power in " + to_string((int) ((int32_t) (online_until_ms_ - millis()) / 60000 + 1)) + " min"
                 : "online, waiting for sync";
    else if (radio_on_flag_)
      s = "syncing";
    else {
      int64_t mins = (esp_timer_get_time() - anchor) / 60000000LL;
      s = "free-run (" + to_string((int) mins) + " min since sync)";
    }
    if (source_sensor_->get_state() != s) source_sensor_->publish_state(s);
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (locked_sensor_ != nullptr) locked_sensor_->publish_state(st.locked && synced);
#endif
  (void) synced;
  (void) anchor;
}

void LtcGen::save_model_() {
  portENTER_CRITICAL(&lock_);
  bool learned = model_learned_;
  ModelPref p = {PREF_MAGIC, model_c_[0], model_c_[1], model_c_[2]};
  portEXIT_CRITICAL(&lock_);
  if (!learned) return;
  if (pref_.save(&p)) ESP_LOGI(TAG, "Saved temperature model c0=%.3f c1=%.4f c2=%.5f", p.c0, p.c1, p.c2);
}

}  // namespace ltc_gen
}  // namespace esphome

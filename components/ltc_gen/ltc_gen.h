#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef LTC_GEN_USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <string>

#include <driver/rmt_tx.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>

#if SOC_TEMP_SENSOR_SUPPORTED
#include <driver/temperature_sensor.h>
#endif

#include "ltc_core.h"

namespace esphome {
namespace ltc_gen {

enum class Role : uint8_t { NODE = 0, MASTER = 1 };
// Where a node gets its time from.
enum class Source : uint8_t { HOTSPOT = 0, ESPNOW = 1 };

// Everything the real-time task receives goes through one queue so it can
// block on a single object.
struct Event {
  enum Type : uint8_t { FRAME_DONE, BEACON, TSF_SAMPLE, WINDOW_BEGIN, WINDOW_END, TEMPERATURE, TIMEBASE };
  Type type;
  int64_t local_us;
  int64_t aux;  // TSF_SAMPLE: the hotspot's TSF
  double value;
  ltc_core::Beacon beacon;
};

struct ModelPref {
  uint32_t magic;
  double c0, c1, c2;
};

class LtcGen : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // --- configuration (code generation) ---
  void set_pin(InternalGPIOPin *pin) { pin_ = pin; }
  void set_pps_pin(InternalGPIOPin *pin) { pps_pin_ = pin; }
  void set_invert(bool v) { invert_ = v; }
  void set_role(Role r) { role_ = r; }
  void set_source(Source s) { source_ = s; }
  void set_hotspot_ssid(const std::string &s) { hotspot_ssid_ = s; }
  void set_rate(uint8_t r) { rate_ = (ltc_core::Rate) r; }
  void set_channel(uint8_t c) {
    channel_ = c;
    cfg_channel_ = c;
  }
  void set_initial_window(uint32_t ms) { initial_window_ms_ = ms; }
  void set_window(uint32_t ms) { window_ms_ = ms; }
  void set_resync_interval(uint32_t ms) { resync_ms_ = ms; }
  void set_temperature_compensation(bool v) { temp_comp_ = v; }
  void set_persist_model(bool v) { persist_model_ = v; }
  void set_output_offset(int32_t us) { output_offset_us_ = us; }
  void set_start_timecode_frames(int64_t f) { start_frames_ = f; }
#ifdef LTC_GEN_USE_TIME
  void set_time(time::RealTimeClock *t) { time_ = t; }
#endif
#ifdef USE_SENSOR
  void set_ppm_sensor(sensor::Sensor *s) { ppm_sensor_ = s; }
  void set_sync_error_sensor(sensor::Sensor *s) { sync_error_sensor_ = s; }
  void set_phase_error_sensor(sensor::Sensor *s) { phase_error_sensor_ = s; }
  void set_temperature_sensor(sensor::Sensor *s) { temperature_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_timecode_sensor(text_sensor::TextSensor *s) { timecode_sensor_ = s; }
  void set_state_sensor(text_sensor::TextSensor *s) { source_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_locked_sensor(binary_sensor::BinarySensor *s) { locked_sensor_ = s; }
#endif

  // --- runtime API (lambdas) ---
  // "24", "25", "29.97df", "30"
  bool set_fps(const std::string &fps);
  std::string get_fps() const;
  // Master: re-take the time of day (or restart at start_timecode).
  void jam();
  // Node: start a sync window now.
  void sync_now() { sync_now_ = true; }
  std::string timecode() const;
  bool is_locked() const;

 protected:
  // radio
  void radio_init_();
  void radio_on_();
  void radio_off_();
  static void recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  static void send_cb_(const esp_now_send_info_t *info, esp_now_send_status_t status);
#else
  static void send_cb_(const uint8_t *mac, esp_now_send_status_t status);
#endif
  static void promisc_cb_(void *buf, wifi_promiscuous_pkt_type_t type);
  bool hotspot_find_channel_();
  static void beacon_timer_cb_(void *arg);
  void send_beacon_();

  // real-time task
  static void task_entry_(void *arg);
  void task_();
  void start_stream_();
  void queue_frame_();
  static bool rmt_done_cb_(rmt_channel_handle_t ch, const rmt_tx_done_event_data_t *ed, void *ctx);
  void post_(const Event &e);

  // loop helpers
  void node_schedule_();
  void master_timebase_();
  void read_temperature_();
  void publish_();
  void save_model_();

  // config
  InternalGPIOPin *pin_{nullptr};
  InternalGPIOPin *pps_pin_{nullptr};
  int pps_gpio_{-1};
  bool invert_{false};
  Role role_{Role::NODE};
  Source source_{Source::HOTSPOT};
  std::string hotspot_ssid_;
  uint8_t cfg_channel_{0};  // 0 = scan for the hotspot
  ltc_core::Rate rate_{ltc_core::Rate::FPS_30};
  uint8_t channel_{1};
  uint32_t initial_window_ms_{12000};
  uint32_t window_ms_{3000};
  uint32_t resync_ms_{600000};
  bool temp_comp_{true};
  bool persist_model_{true};
  int32_t output_offset_us_{0};
  int64_t start_frames_{0};
#ifdef LTC_GEN_USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

  // real-time state (owned by the task)
  ltc_core::Engine engine_{10e6};
  ltc_core::BeaconPairer pairer_;
  bool streaming_{false};
  static const int RING = ltc_core::Engine::RING;
  rmt_symbol_word_t buf_[RING][ltc_core::MAX_SEGMENTS / 2];
  uint32_t queued_tx_{0};
  volatile uint8_t pps_flag_[RING] = {0};
  volatile uint32_t isr_done_{0};

  // handles
  rmt_channel_handle_t chan_{nullptr};
  rmt_encoder_handle_t enc_{nullptr};
  QueueHandle_t q_{nullptr};
  TaskHandle_t task_handle_{nullptr};
  esp_timer_handle_t beacon_timer_{nullptr};
  bool radio_ready_{false};
  bool radio_on_flag_{false};
  bool own_wifi_{true};  // false when ESPHome's wifi component owns the radio
  uint8_t hs_bssid_[6] = {0};
  volatile bool hs_bssid_known_{false};
  bool hs_channel_known_{false};
#if SOC_TEMP_SENSOR_SUPPORTED
  temperature_sensor_handle_t tsens_{nullptr};
#endif
  float last_temp_{NAN};

  // shared between task / callbacks / loop (guarded by lock_)
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  ltc_core::Stats stats_;
  bool synced_{false};
  int64_t last_anchor_local_{0};
  double model_c_[3] = {0, 0, 0};
  bool model_learned_{false};
  int window_result_{0};  // 0 pending, 1 ok, -1 failed
  ltc_core::Rate shared_rate_{ltc_core::Rate::FPS_30};
  int64_t shared_origin_{0};
  uint32_t session_{0};
  uint32_t seq_{0};
  uint32_t last_sent_seq_{0};
  uint32_t tx_stamp_seq_{UINT32_MAX};
  int64_t tx_stamp_{0};

  // loop state
  enum class WinState : uint8_t { IDLE, OPEN, CLOSING } win_state_{WinState::IDLE};
  uint32_t boot_ms_{0};
  uint32_t win_end_ms_{0};
  uint32_t first_sync_ms_{0};
  uint32_t next_win_ms_{0};
  int sched_idx_{0};
  bool ever_synced_{false};
  bool sync_now_{false};
  bool time_anchored_{false};
  uint32_t last_publish_ms_{0};
  uint32_t last_temp_ms_{0};
  uint32_t last_save_ms_{0};
  ESPPreferenceObject pref_;

  static LtcGen *instance_;

#ifdef USE_SENSOR
  sensor::Sensor *ppm_sensor_{nullptr};
  sensor::Sensor *sync_error_sensor_{nullptr};
  sensor::Sensor *phase_error_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *timecode_sensor_{nullptr};
  text_sensor::TextSensor *source_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *locked_sensor_{nullptr};
#endif
};

}  // namespace ltc_gen
}  // namespace esphome

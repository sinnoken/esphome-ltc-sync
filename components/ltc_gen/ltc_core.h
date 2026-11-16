// ltc_core.h - platform independent core of the LTC generator.
//
// Everything here is plain C++ (no ESP-IDF / ESPHome headers) so it can be
// unit-tested and simulated on a PC. The ESPHome glue (ltc_gen.cpp) only
// feeds clock samples / transmit-done timestamps in and pushes the produced
// pulse segments to the RMT peripheral.
//
// Time domains:
//   local : esp_timer_get_time() of this chip (µs, free running crystal)
//   net   : the master's esp_timer (µs). The master broadcasts it over ESP-NOW;
//           nodes learn offset + rate during short sync windows and free-run
//           on that model with the radio switched off in between.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace ltc_core {

// ---------------------------------------------------------------------------
// Frame rates & timecode
// ---------------------------------------------------------------------------
enum class Rate : uint8_t { FPS_24 = 0, FPS_25 = 1, FPS_2997_DF = 2, FPS_30 = 3 };

struct RateInfo {
  uint32_t num;     // frames per second = num / den
  uint32_t den;
  uint8_t nominal;  // frames counted per timecode second
  bool drop;        // drop-frame labelling
};

inline RateInfo rate_info(Rate r) {
  switch (r) {
    case Rate::FPS_24: return {24, 1, 24, false};
    case Rate::FPS_25: return {25, 1, 25, false};
    case Rate::FPS_2997_DF: return {30000, 1001, 30, true};
    case Rate::FPS_30:
    default: return {30, 1, 30, false};
  }
}

inline double frame_period_us(Rate r) {
  RateInfo i = rate_info(r);
  return 1e6 * (double) i.den / (double) i.num;
}

inline uint64_t frames_per_day(Rate r) {
  RateInfo i = rate_info(r);
  // 29.97 DF: 17982 frames per 10 minutes, 144 blocks of 10 minutes per day.
  return i.drop ? 17982ULL * 144ULL : (uint64_t) i.nominal * 86400ULL;
}

struct Timecode {
  uint8_t hours = 0, minutes = 0, seconds = 0, frames = 0;
  bool drop = false;
};

// frame: number of frames since 00:00:00:00 (wraps every 24h).
inline Timecode frames_to_timecode(int64_t frame, Rate r) {
  RateInfo i = rate_info(r);
  int64_t per_day = (int64_t) frames_per_day(r);
  frame %= per_day;
  if (frame < 0) frame += per_day;
  uint64_t fn = (uint64_t) frame;
  if (i.drop) {
    // Drop frame numbers 0 and 1 of every minute except every 10th minute.
    uint64_t d = fn / 17982, m = fn % 17982;
    fn += 18 * d + (m < 2 ? 0 : 2 * ((m - 2) / 1798));
  }
  Timecode tc;
  tc.drop = i.drop;
  uint64_t nom = i.nominal;
  tc.frames = (uint8_t) (fn % nom);
  tc.seconds = (uint8_t) ((fn / nom) % 60);
  tc.minutes = (uint8_t) ((fn / (nom * 60)) % 60);
  tc.hours = (uint8_t) ((fn / (nom * 3600)) % 24);
  return tc;
}

inline void format_timecode(const Timecode &tc, char *out, size_t len) {
  snprintf(out, len, "%02u:%02u:%02u%c%02u", tc.hours, tc.minutes, tc.seconds, tc.drop ? ';' : ':', tc.frames);
}

// ---------------------------------------------------------------------------
// SMPTE 12M LTC frame (80 bits, LSB first)
// ---------------------------------------------------------------------------
struct Frame {
  uint8_t bytes[10] = {0};
  bool bit(int i) const { return (bytes[i >> 3] >> (i & 7)) & 1; }
  void set(int i, bool v) {
    if (v)
      bytes[i >> 3] |= (uint8_t) (1u << (i & 7));
    else
      bytes[i >> 3] &= (uint8_t) ~(1u << (i & 7));
  }
  void put(int pos, int nbits, uint32_t val) {
    for (int b = 0; b < nbits; b++) set(pos + b, (val >> b) & 1);
  }
  int ones() const {
    int n = 0;
    for (int i = 0; i < 80; i++) n += bit(i);
    return n;
  }
};

static const uint16_t LTC_SYNC_WORD = 0xBFFC;  // bits 64..79 = 0011 1111 1111 1101

// user_bits: 8 nibbles, nibble 0 goes to user-bit group 1.
inline Frame encode_frame(const Timecode &tc, Rate r, uint32_t user_bits = 0) {
  Frame f;
  f.put(0, 4, tc.frames % 10);
  f.put(8, 2, tc.frames / 10);
  f.set(10, tc.drop);
  f.put(16, 4, tc.seconds % 10);
  f.put(24, 3, tc.seconds / 10);
  f.put(32, 4, tc.minutes % 10);
  f.put(40, 3, tc.minutes / 10);
  f.put(48, 4, tc.hours % 10);
  f.put(56, 2, tc.hours / 10);
  static const int ub_pos[8] = {4, 12, 20, 28, 36, 44, 52, 60};
  for (int g = 0; g < 8; g++) f.put(ub_pos[g], 4, (user_bits >> (4 * g)) & 0xF);
  f.put(64, 16, LTC_SYNC_WORD);
  // Polarity correction bit: make the number of ones even, so every frame has
  // an even number of transitions and starts with the same polarity.
  int parity_pos = (r == Rate::FPS_25) ? 59 : 27;
  if (f.ones() & 1) f.set(parity_pos, true);
  return f;
}

// ---------------------------------------------------------------------------
// Biphase-mark encoding into (duration, level) segments
// ---------------------------------------------------------------------------
struct Segment {
  uint16_t ticks;
  uint8_t level;
};

static const int MAX_SEGMENTS = 160;

// Encodes one frame lasting `frame_ticks` (fractional) ticks. `carry` keeps the
// fractional tick remainder between frames so that rounding never accumulates.
// The line is assumed to sit at `idle_level` before the frame; because the
// number of ones is even, it ends at `idle_level` again.
inline int biphase_encode(const Frame &f, double frame_ticks, double &carry, bool idle_level, Segment *out) {
  const double h = frame_ticks / 160.0;
  const double base = carry;  // in [0, 1)
  int64_t prev = (int64_t) std::floor(base);
  bool level = idle_level;
  int n = 0;
  for (int b = 0; b < 80; b++) {
    level = !level;  // transition at every bit boundary
    if (f.bit(b)) {
      int64_t mid = (int64_t) std::floor(base + (2 * b + 1) * h);
      out[n++] = {(uint16_t) (mid - prev), (uint8_t) level};
      prev = mid;
      level = !level;  // extra transition in the middle of a '1'
    }
    int64_t end = (int64_t) std::floor(base + (2 * b + 2) * h);
    out[n++] = {(uint16_t) (end - prev), (uint8_t) level};
    prev = end;
  }
  carry = base + 160.0 * h - (double) prev;
  return n;
}

// ---------------------------------------------------------------------------
// TempRateModel: crystal frequency error (ppm) as a function of temperature
// ---------------------------------------------------------------------------
// Every sync interval yields one measured average rate together with the
// average die temperature over that interval. Once enough temperature spread
// has been seen, a weighted least-squares line / parabola ppm(T) is fitted and
// used while free-running ("software TCXO"). Without temperature data, or
// with too little spread, only the offset c0 is updated and the slope terms
// (possibly loaded from flash) are kept.
class TempRateModel {
 public:
  static const int MAXP = 64;
  static constexpr double T_REF = 25.0;

  void reset() {
    n_ = 0;
    head_ = 0;
    c0_ = 0;
    c1_ = 0;
    c2_ = 0;
    have_ = false;
  }
  // Load slope terms learned on a previous day (c0 is re-learned).
  void set_coeffs(double c0, double c1, double c2) {
    c0_ = c0;
    c1_ = c1;
    c2_ = c2;
    have_ = true;
    loaded_ = true;
  }
  void get_coeffs(double *c0, double *c1, double *c2) const {
    *c0 = c0_;
    *c1 = c1_;
    *c2 = c2_;
  }
  bool has_temperature_terms() const { return c1_ != 0 || c2_ != 0; }
  bool learned_temperature_terms() const { return learned_; }
  bool have() const { return have_; }
  int points() const { return n_; }
  double temp_spread() const {
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < n_; i++) {
      if (std::isnan(pt_[i].t)) continue;
      if (pt_[i].t < lo) lo = pt_[i].t;
      if (pt_[i].t > hi) hi = pt_[i].t;
    }
    return hi > lo ? hi - lo : 0;
  }

  // temp may be NaN (no sensor).
  void add(double temp, double ppm, double weight) {
    // age the existing points so that crystal aging is followed
    for (int i = 0; i < n_; i++) pt_[i].w *= age_factor;
    pt_[head_] = {temp, ppm, weight};
    head_ = (head_ + 1) % MAXP;
    if (n_ < MAXP) n_++;
    refit_();
  }

  double ppm_at(double temp) const {
    if (std::isnan(temp)) temp = last_temp_;
    if (std::isnan(temp)) return c0_;
    double d = temp - T_REF;
    return c0_ + c1_ * d + c2_ * d * d;
  }

  double min_spread_linear = 2.0;     // °C
  double min_spread_quadratic = 8.0;  // °C
  double age_factor = 0.98;

 private:
  struct P {
    double t, ppm, w;
  };

  void refit_() {
    // statistics of points with temperature
    double sw = 0;
    int nt = 0;
    for (int i = 0; i < n_; i++) {
      if (!std::isnan(pt_[i].t)) {
        nt++;
        sw += pt_[i].w;
        last_temp_ = pt_[i].t;
      }
    }
    double spread = temp_spread();
    if (nt >= 6 && spread >= min_spread_quadratic && solve_(3)) {
      learned_ = true;
    } else if (nt >= 3 && spread >= min_spread_linear && solve_(2)) {
      learned_ = true;
    } else {
      // offset only, keep slope terms
      double num = 0, den = 0;
      for (int i = 0; i < n_; i++) {
        double d = std::isnan(pt_[i].t) ? (std::isnan(last_temp_) ? 0 : last_temp_ - T_REF) : pt_[i].t - T_REF;
        double resid = pt_[i].ppm - (c1_ * d + c2_ * d * d);
        num += pt_[i].w * resid;
        den += pt_[i].w;
      }
      if (den > 0) c0_ = num / den;
    }
    (void) sw;
    have_ = true;
  }

  // Weighted least squares with k = 2 (line) or 3 (parabola) terms.
  bool solve_(int k) {
    double A[3][4] = {{0}};
    for (int i = 0; i < n_; i++) {
      if (std::isnan(pt_[i].t)) continue;
      double d = pt_[i].t - T_REF;
      double x[3] = {1, d, d * d};
      for (int r = 0; r < k; r++) {
        for (int c = 0; c < k; c++) A[r][c] += pt_[i].w * x[r] * x[c];
        A[r][3] += pt_[i].w * x[r] * pt_[i].ppm;
      }
    }
    // Gaussian elimination with partial pivoting
    for (int c = 0; c < k; c++) {
      int piv = c;
      for (int r = c + 1; r < k; r++)
        if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
      if (std::fabs(A[piv][c]) < 1e-12) return false;
      if (piv != c)
        for (int j = 0; j < 4; j++) std::swap(A[c][j], A[piv][j]);
      for (int r = 0; r < k; r++) {
        if (r == c) continue;
        double f = A[r][c] / A[c][c];
        for (int j = c; j < 4; j++) A[r][j] -= f * A[c][j];
      }
    }
    double sol[3] = {0, 0, 0};
    for (int r = 0; r < k; r++) sol[r] = A[r][3] / A[r][r];
    c0_ = sol[0];
    c1_ = sol[1];
    if (k == 3)
      c2_ = sol[2];
    else if (!loaded_)
      c2_ = 0;
    return true;
  }

  P pt_[MAXP] = {};
  int n_ = 0, head_ = 0;
  double c0_ = 0, c1_ = 0, c2_ = 0;
  double last_temp_ = NAN;
  bool have_ = false, loaded_ = false, learned_ = false;
};

// ---------------------------------------------------------------------------
// SyncClock: maps this chip's local clock onto the network (master) clock
// ---------------------------------------------------------------------------
// The radio is only on during short sync windows. Each window yields one
// robust anchor point (local, net). The frequency error comes from a
// regression over the first window and is then measured between successive
// anchors, which over minutes is very precise. Between windows the clock
// free-runs; the rate follows the temperature model when a sensor is present.
class SyncClock {
 public:
  static const int MAXS = 512;

  // The master is the reference: net time == its local time.
  void set_reference(bool ref) {
    reference_ = ref;
    reset();
  }
  bool reference() const { return reference_; }

  void reset() {
    n_ = 0;
    have_anchor_ = false;
    rate_uncertain_ = false;
    cur_ppm_ = 0;
    windows_ok_ = 0;
    last_residual_ = 0;
    t_sum_ = 0;
    t_cnt_ = 0;
    temp_now_ = NAN;
    bool keep = model_.has_temperature_terms() && keep_model_on_reset;
    double c0, c1, c2;
    model_.get_coeffs(&c0, &c1, &c2);
    model_.reset();
    if (keep) model_.set_coeffs(c0, c1, c2);
  }

  TempRateModel &model() { return model_; }

  void begin_window() { n_ = 0; }
  int window_samples() const { return n_; }

  void add_sample(int64_t local_us, int64_t net_us) {
    if (n_ < MAXS) {
      loc_[n_] = local_us;
      net_[n_] = net_us;
      n_++;
    }
  }

  // Feed the die temperature regularly (e.g. every few seconds). The model is
  // re-based at this instant, so phase stays continuous when the rate changes.
  void update_temperature(int64_t local_us, double temp_c) {
    if (std::isnan(temp_c)) return;
    temp_now_ = temp_c;
    t_sum_ += temp_c;
    t_cnt_++;
    if (reference_ || !have_anchor_) return;
    rebase_(local_us);
    cur_ppm_ = model_.ppm_at(temp_c);
  }

  // Close the current window. Returns true if a new anchor was taken.
  bool end_window() {
    if (reference_) return true;
    if (n_ < min_samples) return false;
    int64_t span = loc_[n_ - 1] - loc_[0];
    bool first = !have_anchor_ || rate_uncertain_;
    if (first && !model_.have()) {
      if (span < min_rate_span_us) return false;
      double r;
      if (!robust_slope_(&r)) return false;
      // Short baseline -> low weight; later long-baseline points dominate.
      model_.add(temp_now_, (r - 1.0) * 1e6, initial_weight);
      cur_ppm_ = model_.ppm_at(temp_now_);
    }
    // Robust offset at the window centre using the current rate.
    double rate = 1.0 + cur_ppm_ * 1e-6;
    int64_t lc = loc_[n_ / 2];
    for (int i = 0; i < n_; i++) tmp_[i] = (double) (net_[i] - lc) - rate * (double) (loc_[i] - lc);
    double off = median_(tmp_, n_);
    double nc = (double) lc + off;  // net time at local lc

    double resid = 0;
    if (have_anchor_) {
      resid = nc - to_net(lc);
      if (std::fabs(resid) > jump_us) {
        // Master restarted or another master took over.
        rate_uncertain_ = true;
        windows_ok_ = 0;
      } else {
        double dl = (double) (lc - c_local_);
        if (dl >= min_rate_span_us) {
          double r_meas = (nc - c_net_) / dl;
          double ppm = (r_meas - 1.0) * 1e6;
          if (std::fabs(ppm) < 200) {
            double tmean = t_cnt_ > 0 ? t_sum_ / t_cnt_ : NAN;
            double base_s = dl / 1e6;
            double w = (base_s / 600.0) * (base_s / 600.0);
            if (w > 4) w = 4;
            if (rate_uncertain_) w = 1;
            model_.add(tmean, ppm, w);
            rate_uncertain_ = false;
          }
        }
      }
    }
    t_sum_ = 0;
    t_cnt_ = 0;
    c_local_ = lc;
    c_net_ = nc;
    a_local_ = lc;
    a_net_ = nc;
    cur_ppm_ = model_.ppm_at(temp_now_);
    have_anchor_ = true;
    last_residual_ = resid;
    last_anchor_local_ = lc;
    windows_ok_++;
    return true;
  }

  bool valid() const { return reference_ || have_anchor_; }
  bool synced(int64_t now_local, int64_t max_age_us) const {
    if (reference_) return true;
    return have_anchor_ && windows_ok_ > 0 && (now_local - last_anchor_local_) <= max_age_us;
  }
  double rate() const { return reference_ ? 1.0 : 1.0 + cur_ppm_ * 1e-6; }
  double ppm() const { return reference_ ? 0.0 : cur_ppm_; }
  double last_residual_us() const { return last_residual_; }
  int64_t last_anchor_local() const { return last_anchor_local_; }
  double temperature() const { return temp_now_; }

  double to_net(int64_t local_us) const {
    if (reference_) return (double) local_us;
    return a_net_ + rate() * (double) (local_us - a_local_);
  }
  double to_local(double net_us) const {
    if (reference_) return net_us;
    return (double) a_local_ + (net_us - a_net_) / rate();
  }

  int min_samples = 5;
  int64_t min_rate_span_us = 8000000;  // first window must span 8 s
  double jump_us = 50000;              // 50 ms
  double initial_weight = 0.01;
  bool keep_model_on_reset = true;

 private:
  void rebase_(int64_t local_us) {
    a_net_ = to_net(local_us);
    a_local_ = local_us;
  }

  static double median_(double *v, int n) {
    for (int i = 1; i < n; i++) {
      double x = v[i];
      int j = i - 1;
      while (j >= 0 && v[j] > x) {
        v[j + 1] = v[j];
        j--;
      }
      v[j + 1] = x;
    }
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
  }

  bool fit_(const bool *use, double *slope, double *icpt) const {
    int64_t lr = loc_[0], nr = net_[0];
    double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
    for (int i = 0; i < n_; i++) {
      if (!use[i]) continue;
      double x = (double) (loc_[i] - lr), y = (double) (net_[i] - nr) - x;
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
      n += 1;
    }
    if (n < min_samples) return false;
    double var = sxx - sx * sx / n;
    if (var <= 0) return false;
    double b = (sxy - sx * sy / n) / var;
    *slope = 1.0 + b;
    *icpt = (sy - b * sx) / n;
    return true;
  }

  bool robust_slope_(double *rate) {
    for (int i = 0; i < n_; i++) use_[i] = true;
    double s = 1, c = 0;
    int64_t lr = loc_[0], nr = net_[0];
    for (int pass = 0; pass < 3; pass++) {
      if (!fit_(use_, &s, &c)) return false;
      for (int i = 0; i < n_; i++) {
        double x = (double) (loc_[i] - lr);
        tmp_[i] = std::fabs((double) (net_[i] - nr) - x - c - (s - 1.0) * x);
      }
      double mad = median_(tmp_, n_);
      double thr = 3.0 * (mad > 1.0 ? mad : 1.0);
      for (int i = 0; i < n_; i++) {
        double x = (double) (loc_[i] - lr);
        double r = std::fabs((double) (net_[i] - nr) - x - c - (s - 1.0) * x);
        use_[i] = r <= thr;
      }
    }
    if (!(s > 0.9998 && s < 1.0002)) return false;  // ±200 ppm sanity
    *rate = s;
    return true;
  }

  bool reference_ = false;
  int64_t loc_[MAXS] = {0}, net_[MAXS] = {0};
  double tmp_[MAXS] = {0};
  bool use_[MAXS] = {false};
  int n_ = 0;
  bool have_anchor_ = false, rate_uncertain_ = false;
  double cur_ppm_ = 0;
  TempRateModel model_;
  int64_t a_local_ = 0;  // current free-run segment
  double a_net_ = 0;
  int64_t c_local_ = 0;  // last sync anchor (for rate measurement)
  double c_net_ = 0;
  int windows_ok_ = 0;
  double last_residual_ = 0;
  int64_t last_anchor_local_ = 0;
  double t_sum_ = 0;
  int t_cnt_ = 0;
  double temp_now_ = NAN;
};

// ---------------------------------------------------------------------------
// ESP-NOW beacon protocol (two-step, like PTP sync + follow-up)
// ---------------------------------------------------------------------------
// The master sends a beacon every 100 ms. It cannot know exactly when a
// beacon leaves the radio until the send-done callback fires, so beacon N
// carries the transmit time of beacon N-1. The node pairs it with the local
// receive time it recorded for N-1.
static const uint32_t BEACON_MAGIC = 0x3243544C;  // "LTC2" little endian
static const uint8_t BEACON_VERSION = 1;

#pragma pack(push, 1)
struct Beacon {
  uint32_t magic;
  uint8_t version;
  uint8_t rate;  // Rate enum
  uint16_t flags;
  uint32_t session;     // random per master boot
  uint32_t seq;
  int64_t prev_tx_net;  // net time when beacon seq-1 was sent, 0 = unknown
  int64_t origin_net;   // net time of timecode 00:00:00:00
};
#pragma pack(pop)

class BeaconPairer {
 public:
  static const int R = 16;
  void reset() {
    for (int i = 0; i < R; i++) seq_[i] = UINT32_MAX;
    have_session_ = false;
  }
  BeaconPairer() { reset(); }

  // Returns true and fills (local, net) when a complete sample is available.
  bool on_beacon(const Beacon &b, int64_t rx_local, int64_t *local_out, int64_t *net_out) {
    if (b.magic != BEACON_MAGIC || b.version != BEACON_VERSION) return false;
    if (!have_session_ || b.session != session_) {
      reset();
      session_ = b.session;
      have_session_ = true;
      session_changed_ = true;
    }
    rate_ = (Rate) b.rate;
    origin_ = b.origin_net;
    have_info_ = true;
    seq_[b.seq % R] = b.seq;
    rx_[b.seq % R] = rx_local;
    if (b.prev_tx_net == 0 || b.seq == 0) return false;
    uint32_t p = b.seq - 1;
    if (seq_[p % R] != p) return false;  // we missed the previous one
    *local_out = rx_[p % R];
    *net_out = b.prev_tx_net;
    return true;
  }

  bool have_info() const { return have_info_; }
  Rate rate() const { return rate_; }
  int64_t origin() const { return origin_; }
  uint32_t session() const { return session_; }
  bool take_session_changed() {
    bool c = session_changed_;
    session_changed_ = false;
    return c;
  }

 private:
  uint32_t seq_[R];
  int64_t rx_[R] = {0};
  bool have_session_ = false, have_info_ = false, session_changed_ = false;
  uint32_t session_ = 0;
  Rate rate_ = Rate::FPS_30;
  int64_t origin_ = 0;
};

// ---------------------------------------------------------------------------
// Phase controller with transport-delay compensation
// ---------------------------------------------------------------------------
// Frames are queued `depth` frames ahead of the one being transmitted, so a
// correction only shows up `depth` frames later. We therefore predict the
// error at the start of the frame we are about to queue by adding all the
// adjustments that are already in the pipe (Smith-predictor style).
class PhaseController {
 public:
  static const int MAXD = 16;

  void reset() {
    n_ = 0;
    integ_ = 0;
  }
  void reset_integral() { integ_ = 0; }

  // A frame finished: its adjustment is history now.
  // (pend_ holds one entry per frame queued but not yet finished.)
  void pop() {
    if (n_ > 0) {
      for (int i = 1; i < n_; i++) pend_[i - 1] = pend_[i];
      n_--;
    }
  }

  // err_us: actual - target start of the frame that just started (net µs).
  // Returns the duration adjustment (net µs) for the next frame to queue.
  double update(double err_us, double period_us) {
    double e = err_us;
    for (int i = 0; i < n_; i++) e += pend_[i];
    integ_ += e;
    double lim_i = max_frac * period_us / (ki > 0 ? ki : 1);
    if (integ_ > lim_i) integ_ = lim_i;
    if (integ_ < -lim_i) integ_ = -lim_i;
    double adj = -(kp * e + ki * integ_);
    double lim = max_frac * period_us;
    if (adj > lim) adj = lim;
    if (adj < -lim) adj = -lim;
    return adj;
  }

  // Register the adjustment that was actually applied to a queued frame.
  void commit(double adj) {
    if (n_ < MAXD) pend_[n_++] = adj;
  }

  double kp = 0.5;
  double ki = 0.05;
  double max_frac = 0.02;  // max ±2 % speed change while slewing

 private:
  double pend_[MAXD] = {0};
  int n_ = 0;
  double integ_ = 0;
};

// ---------------------------------------------------------------------------
// Engine: decides which frame goes out when and how long it lasts
// ---------------------------------------------------------------------------
struct QueuedFrame {
  int64_t index;  // frames since the virtual midnight (origin)
  Timecode tc;
  Segment seg[MAX_SEGMENTS];
  int nseg;
  bool second_mark;  // first frame of a timecode second (for the PPS pin)
};

struct Stats {
  double last_err_us = 0;
  double rms_err_us = 0;
  double ppm = 0;
  bool locked = false;
  bool clock_valid = false;
  Timecode tc;
  uint32_t jumps = 0;
  double sync_residual_us = 0;  // prediction error measured at the last sync window
};

class Engine {
 public:
  static const int RING = 8;

  explicit Engine(double tick_hz = 10e6) : tick_per_us_(tick_hz / 1e6) {}

  // --- configuration (call from the real-time context) ---
  // origin_net: network time (µs) that corresponds to timecode 00:00:00:00.
  void set_timebase(Rate r, int64_t origin_net) {
    if (r != rate_ && started_) restart_pending_ = true;
    rate_ = r;
    origin_ = origin_net;
  }
  Rate rate() const { return rate_; }
  int64_t origin() const { return origin_; }
  void set_idle_level(bool l) { idle_ = l; }
  int depth() const { return depth_; }
  void set_depth(int d) { depth_ = d < 2 ? 2 : (d > RING - 2 ? RING - 2 : d); }

  SyncClock &clock() { return clock_; }
  PhaseController &controller() { return ctrl_; }

  // --- stream control ---
  bool restart_pending() const { return restart_pending_; }
  int in_flight() const { return (int) (queued_ - done_); }

  // Target start of frame k in the network time domain.
  double target_net(int64_t k) const { return (double) origin_ + (double) k * frame_period_us(rate_); }

  // Prepare a new stream. Returns the local time at which the first frame
  // should start (caller waits until then and queues depth() frames).
  bool plan_start(int64_t now_local, int64_t lead_us, int64_t *start_local) {
    if (!clock_.valid()) return false;
    double p = frame_period_us(rate_);
    double t = clock_.to_net(now_local + lead_us);
    int64_t k = (int64_t) std::ceil((t - (double) origin_) / p);
    next_index_ = k;
    *start_local = (int64_t) std::llround(clock_.to_local(target_net(k)));
    queued_ = done_ = 0;
    carry_ = 0;
    ctrl_.reset();
    next_adj_ = 0;
    restart_pending_ = false;
    started_ = true;
    good_frames_ = 0;
    stats_.locked = false;
    return true;
  }

  void stop() { started_ = false; }

  // Produce the next frame to hand to the transmitter.
  void next_frame(QueuedFrame *out) {
    double p = frame_period_us(rate_);
    double adj = next_adj_;
    next_adj_ = 0;
    ctrl_.commit(adj);
    double dur_net = p + adj;
    double ticks = dur_net / clock_.rate() * tick_per_us_;
    out->index = next_index_++;
    out->tc = frames_to_timecode(out->index, rate_);
    Frame f = encode_frame(out->tc, rate_);
    out->nseg = biphase_encode(f, ticks, carry_, idle_, out->seg);
    out->second_mark = out->tc.frames == 0;
    ring_index_[queued_ % RING] = out->index;
    queued_++;
  }

  // A transmission finished at local time `local_us`; the next queued frame
  // (if any) starts right now.
  void on_frame_done(int64_t local_us) {
    done_++;
    ctrl_.pop();
    if (restart_pending_) return;  // draining the old stream
    if (done_ >= queued_) {  // underflow: nothing is playing
      stats_.locked = false;
      good_frames_ = 0;
      return;
    }
    if (!clock_.valid()) {
      stats_.locked = false;
      good_frames_ = 0;
    }
    double p = frame_period_us(rate_);
    int64_t k = ring_index_[done_ % RING];
    double err = clock_.to_net(local_us) - target_net(k);
    if (std::fabs(err) > p / 2) {
      // Way off (start-up, new origin, AP change): relabel whole frames and
      // slew the remainder.
      int64_t n = (int64_t) std::llround(err / p);
      for (uint32_t q = done_; q < queued_; q++) ring_index_[q % RING] += n;
      next_index_ += n;
      err -= (double) n * p;
      ctrl_.reset_integral();
      stats_.jumps++;
      good_frames_ = 0;
      stats_.locked = false;
    }
    next_adj_ = ctrl_.update(err, p);

    stats_.last_err_us = err;
    rms_acc_ = 0.98 * rms_acc_ + 0.02 * err * err;
    stats_.rms_err_us = std::sqrt(rms_acc_);
    double fa = std::fabs(err);
    if (fa < lock_us) {
      if (good_frames_ < 100000) good_frames_++;
    } else {
      good_frames_ = 0;
    }
    if (fa > unlock_us || !clock_.valid()) stats_.locked = false;
    if (good_frames_ >= lock_frames && clock_.valid()) stats_.locked = true;
    stats_.tc = frames_to_timecode(k, rate_);
  }

  const Stats &stats() {
    stats_.ppm = clock_.ppm();
    stats_.clock_valid = clock_.valid();
    stats_.sync_residual_us = clock_.last_residual_us();
    return stats_;
  }

  double lock_us = 100.0;
  double unlock_us = 1000.0;
  int lock_frames = 60;

 private:
  double tick_per_us_;
  Rate rate_ = Rate::FPS_30;
  int64_t origin_ = 0;
  bool idle_ = false;
  int depth_ = 4;
  SyncClock clock_;
  PhaseController ctrl_;
  bool started_ = false, restart_pending_ = false;
  int64_t next_index_ = 0;
  uint32_t queued_ = 0, done_ = 0;
  int64_t ring_index_[RING] = {0};
  double carry_ = 0;
  double next_adj_ = 0;
  double rms_acc_ = 0;
  int good_frames_ = 0;
  Stats stats_;
};

}  // namespace ltc_core

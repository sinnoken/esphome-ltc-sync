// Closed-loop simulation of a master + node over 24 h.
//
// Simulated physics (all assumptions, see docs/accuracy.md):
//   * node crystal: +17 ppm static, AT-cut-like temperature curve
//       ppm(T) = 17 - 0.15 (T-25) + 1.1e-4 (T-25)^3
//   * crystal temperature follows ambient with a 5 min lag
//   * die temperature sensor = crystal + 3 °C, 0.3 °C noise, 0.25 °C steps
//   * ESP-NOW: beacon every 100 ms, CSMA delay 0..500 µs, send-callback
//     latency exp(40 µs) with 3 % 1..5 ms outliers, receive-callback latency
//     exp(50 µs) with 3 % 1..5 ms outliers, 5 % packet loss
//   * RMT: 8..12 µs restart gap between transactions, ISR stamp jitter 0..6 µs
//
// The error reported is the node's actual LTC frame start minus the ideal
// frame start on the master's timebase.
#include "../components/ltc_gen/ltc_core.h"

#include <cstdio>
#include <algorithm>
#include <deque>
#include <random>
#include <vector>

using namespace ltc_core;

struct Env {
  std::mt19937_64 rng{12345};
  double uni(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
  double expo(double mean) { return std::exponential_distribution<double>(1.0 / mean)(rng); }
  double norm(double s) { return std::normal_distribution<double>(0, s)(rng); }
  double latency(double mean) {
    double d = expo(mean);
    if (uni(0, 1) < 0.03) d += uni(1000, 5000);
    return d;
  }
};

struct Scenario {
  const char *name;
  int64_t resync_s;      // 0 = never after calibration
  bool temp_comp;
  bool diurnal;          // 10 °C day/night swing, else constant 25 °C
  bool preload_model;    // start with the temperature model learned in a previous run
  double hours = 24;
};

struct Result {
  double max_err_after_cal = 0;  // µs, after the calibration phase
  double err_at_end = 0;
  double max_err_h[25] = {0};
  double c0 = 0, c1 = 0, c2 = 0;
  bool learned = false;
};

static double ambient(const Scenario &sc, double t_s) {
  if (!sc.diurnal) return 25.0;
  // 20 °C at t=0, 30 °C after 12 h, plus a faster ±1 °C wobble
  return 25.0 - 5.0 * std::cos(2 * M_PI * t_s / 86400.0) + 1.0 * std::sin(2 * M_PI * t_s / 5400.0);
}

static double crystal_ppm(double T) {
  double d = T - 25.0;
  return 17.0 - 0.15 * d + 1.1e-4 * d * d * d;
}

static Result run(const Scenario &sc, const Result *preload) {
  Env env;
  Result res;
  const double period_net = frame_period_us(Rate::FPS_30);
  Engine eng;
  eng.set_timebase(Rate::FPS_30, 0);
  eng.set_depth(4);
  SyncClock &clk = eng.clock();
  clk.set_reference(false);
  if (sc.preload_model && preload) clk.model().set_coeffs(preload->c0, preload->c1, preload->c2);
  BeaconPairer pairer;

  // windows: [start_s, length_s]
  std::vector<std::pair<double, double>> windows = {{0, 12}, {60, 3}, {300, 3}, {900, 3}};
  if (sc.resync_s > 0)
    for (double t = 900 + sc.resync_s; t < sc.hours * 3600 + 10; t += sc.resync_s) windows.push_back({t, 3});
  size_t wi = 0;
  bool radio = false;
  const double boot = 5e6;  // node boots 5 s after the master

  double T = 0;            // true time = master net (µs)
  double L = 7.77e6;       // node local clock (µs)
  double Tc = ambient(sc, 0);
  // master beacons
  double next_beacon = 0;
  uint32_t seq = 0;
  int64_t prev_tx_net = 0;
  // node RMT
  bool streaming = false;
  std::deque<std::pair<int64_t, double>> rmt;  // (frame index, local duration µs)
  double cur_end_T = 0;                         // true end of current frame
  double next_temp = boot;
  double cal_done_T = boot + 905e6;

  auto advance = [&](double t_new) {
    // integrate the node clock in ≤50 ms steps
    while (T < t_new) {
      double dt = std::min(50000.0, t_new - T);
      double ts = T / 1e6;
      Tc += (ambient(sc, ts) - Tc) * (dt / 300e6);
      L += dt * (1.0 + crystal_ppm(Tc) * 1e-6);
      T += dt;
    }
  };
  auto ppm_now = [&]() { return crystal_ppm(Tc); };

  auto queue_frames = [&]() {
    while (eng.in_flight() < eng.depth()) {
      QueuedFrame qf;
      eng.next_frame(&qf);
      double ticks = 0;
      for (int i = 0; i < qf.nseg; i++) ticks += qf.seg[i].ticks;
      rmt.push_back({qf.index, ticks / 10.0});
    }
  };

  const double end_T = sc.hours * 3600e6;
  while (T < end_T) {
    // next event: beacon, frame end, temperature tick, window edge
    double t_win = 1e30;
    if (wi < windows.size()) t_win = boot + (radio ? windows[wi].first + windows[wi].second : windows[wi].first) * 1e6;
    double t_frame = streaming && !rmt.empty() ? cur_end_T : 1e30;
    double t_next = std::min({next_beacon, t_frame, t_win, next_temp});
    advance(t_next);

    if (t_next == t_win) {
      if (!radio) {
        radio = true;
        clk.begin_window();
      } else {
        radio = false;
        clk.end_window();
        wi++;
        if (!streaming && clk.valid()) {
          int64_t start_local;
          if (eng.plan_start((int64_t) L, 2000, &start_local)) {
            double dl = (double) start_local - L;
            advance(T + dl / (1.0 + ppm_now() * 1e-6));
            rmt.clear();
            queue_frames();
            streaming = true;
            cur_end_T = T + rmt.front().second / (1.0 + ppm_now() * 1e-6);
            double err = T - (double) rmt.front().first * period_net;
            (void) err;
          }
        }
      }
      continue;
    }
    if (t_next == next_temp) {
      next_temp += 5e6;
      if (sc.temp_comp) {
        double sensor = std::round((Tc + 3.0 + env.norm(0.3)) * 4.0) / 4.0 - 3.0;  // calibrated offset
        clk.update_temperature((int64_t) L, sensor);
      }
      continue;
    }
    if (t_next == next_beacon) {
      next_beacon += 100000;
      double tx_end = T + env.uni(0, 500) + 300;  // CSMA + airtime
      Beacon b{};
      b.magic = BEACON_MAGIC;
      b.version = BEACON_VERSION;
      b.rate = (uint8_t) Rate::FPS_30;
      b.session = 42;
      b.seq = seq++;
      b.prev_tx_net = prev_tx_net;
      b.origin_net = 0;
      prev_tx_net = (int64_t) (tx_end + env.latency(40));  // master's send callback stamp
      if (radio && T > boot && env.uni(0, 1) > 0.05) {
        double rx_T = tx_end + 1;
        double rx_local = L + (rx_T - T) * (1 + ppm_now() * 1e-6) + env.latency(50);
        int64_t lo, ne;
        if (pairer.on_beacon(b, (int64_t) rx_local, &lo, &ne)) clk.add_sample(lo, ne);
      }
      continue;
    }
    // frame end
    {
      auto fr = rmt.front();
      rmt.pop_front();
      double stamp = L + env.uni(0, 6);
      if (!rmt.empty()) {
        double gap = env.uni(8, 12);
        advance(T + gap / (1 + ppm_now() * 1e-6));
      }
      eng.on_frame_done((int64_t) stamp);
      queue_frames();
      if (!rmt.empty()) {
        // the frame that starts now
        double err = T - (double) rmt.front().first * period_net;
        // ignore the start-up slew
        if (T > cal_done_T) {
          double ae = std::fabs(err);
          if (ae > res.max_err_after_cal) res.max_err_after_cal = ae;
          int h = (int) (T / 3600e6);
          if (h < 25 && ae > res.max_err_h[h]) res.max_err_h[h] = ae;
          res.err_at_end = err;
        }
        cur_end_T = T + rmt.front().second / (1.0 + ppm_now() * 1e-6);
      }
      (void) fr;
    }
  }
  clk.model().get_coeffs(&res.c0, &res.c1, &res.c2);
  res.learned = clk.model().learned_temperature_terms();
  return res;
}

int main() {
  Scenario s1{"A: resync every 10 min, temp comp, 10 C swing", 600, true, true, false};
  Scenario s2{"B1: calibrate once, no temp comp, 10 C swing", 0, false, true, false};
  Scenario s3{"B2: calibrate once, temp comp (model learned by A), 10 C swing", 0, true, true, true};
  Scenario s4{"B3: calibrate once, no temp comp, constant 25 C", 0, false, false, false};

  int fails = 0;
  Result r1 = run(s1, nullptr);
  printf("%s\n  max |err| = %.0f us, model c0=%.3f c1=%.4f c2=%.5f (learned=%d)\n", s1.name, r1.max_err_after_cal, r1.c0,
         r1.c1, r1.c2, r1.learned);
  if (r1.max_err_after_cal > 1000) fails++;

  for (Scenario *s : {&s2, &s3, &s4}) {
    Result r = run(*s, &r1);
    printf("%s\n  max |err| over 24 h = %.1f ms, error at 24 h = %.1f ms\n", s->name, r.max_err_after_cal / 1000,
           r.err_at_end / 1000);
    printf("  per-hour max (ms):");
    for (int h = 0; h < 24; h += 3) printf(" h%d=%.1f", h, r.max_err_h[h] / 1000);
    printf("\n");
  }
  printf(fails ? "test_sim: %d FAILURES\n" : "test_sim: resync scenario within 1 ms\n", fails);
  return fails ? 1 : 0;
}

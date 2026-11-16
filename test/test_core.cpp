// Unit tests for ltc_core.h, cross-checked against libltc (x42/libltc).
#include "../components/ltc_gen/ltc_core.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "ltc.h"
}

using namespace ltc_core;

static int failures = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      failures++;                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

static std::string tcs(const Timecode &tc) {
  char b[16];
  format_timecode(tc, b, sizeof b);
  return b;
}

static void test_timecode() {
  struct {
    int64_t frame;
    Rate r;
    const char *expect;
  } cases[] = {
      {0, Rate::FPS_30, "00:00:00:00"},
      {30 * 3600 + 29, Rate::FPS_30, "01:00:00:29"},
      {25 * 86400, Rate::FPS_25, "00:00:00:00"},  // wraps at 24h
      {24 * 61 + 5, Rate::FPS_24, "00:01:01:05"},
      {1799, Rate::FPS_2997_DF, "00:00:59;29"},
      {1800, Rate::FPS_2997_DF, "00:01:00;02"},
      {17982, Rate::FPS_2997_DF, "00:10:00;00"},
      {17981, Rate::FPS_2997_DF, "00:09:59;29"},
      {107892, Rate::FPS_2997_DF, "01:00:00;00"},
      {-1, Rate::FPS_25, "23:59:59:24"},
  };
  for (auto &c : cases) {
    std::string got = tcs(frames_to_timecode(c.frame, c.r));
    CHECK(got == c.expect, "frame %lld -> %s, expected %s", (long long) c.frame, got.c_str(), c.expect);
  }
  // Drop-frame never produces frames 0/1 at a non-10th minute.
  for (int64_t f = 0; f < (int64_t) frames_per_day(Rate::FPS_2997_DF); f += 7) {
    Timecode tc = frames_to_timecode(f, Rate::FPS_2997_DF);
    if (tc.seconds == 0 && tc.minutes % 10 != 0) CHECK(tc.frames >= 2, "dropped label used at %s", tcs(tc).c_str());
  }
}

static LTC_TV_STANDARD std_of(Rate r) {
  switch (r) {
    case Rate::FPS_24: return LTC_TV_FILM_24;
    case Rate::FPS_25: return LTC_TV_625_50;
    default: return LTC_TV_525_60;
  }
}

static void test_frame_vs_libltc() {
  Rate rates[] = {Rate::FPS_24, Rate::FPS_25, Rate::FPS_2997_DF, Rate::FPS_30};
  int compared = 0;
  for (Rate r : rates) {
    for (int64_t f = 0; f < (int64_t) frames_per_day(r); f += 9973) {
      Timecode tc = frames_to_timecode(f, r);
      Frame mine = encode_frame(tc, r);
      CHECK((mine.ones() & 1) == 0, "odd number of ones");

      LTCFrame ref;
      ltc_frame_reset(&ref);
      SMPTETimecode st = {};
      strcpy(st.timezone, "+0000");
      st.hours = tc.hours;
      st.mins = tc.minutes;
      st.secs = tc.seconds;
      st.frame = tc.frames;
      ltc_time_to_frame(&ref, &st, std_of(r), LTC_NO_PARITY);
      ref.dfbit = tc.drop ? 1 : 0;
      ltc_frame_set_parity(&ref, std_of(r));
      uint8_t rb[10];
      memcpy(rb, &ref, 10);
      CHECK(memcmp(rb, mine.bytes, 10) == 0, "frame bytes differ from libltc at %s", tcs(tc).c_str());
      compared++;
    }
  }
  printf("  compared %d frames bit-for-bit with libltc\n", compared);
}

static void test_biphase() {
  Timecode tc = frames_to_timecode(12345, Rate::FPS_25);
  Frame f = encode_frame(tc, Rate::FPS_25);
  Segment seg[MAX_SEGMENTS];
  double carry = 0;
  double ticks = 400000.0 / 1.0 + 0.37;  // 25 fps at 10 MHz + fraction
  double total = 0;
  for (int rep = 0; rep < 1000; rep++) {
    int n = biphase_encode(f, ticks, carry, false, seg);
    CHECK(n == 80 + f.ones(), "segment count");
    CHECK(seg[0].level == 1, "frame must start with a transition away from idle");
    CHECK(seg[n - 1].level == 0, "frame must end at idle level");
    for (int i = 1; i < n; i++) CHECK(seg[i].level != seg[i - 1].level, "levels must alternate");
    for (int i = 0; i < n; i++) total += seg[i].ticks;
  }
  // No accumulated rounding error over 1000 frames.
  CHECK(std::fabs(total + carry - 1000 * ticks) < 1e-6, "rounding accumulates: %f", total + carry - 1000 * ticks);
}

static void test_temp_model() {
  TempRateModel m;
  m.reset();
  // ppm(T) = 12 - 0.2 (T-25) + 0.01 (T-25)^2
  for (int i = 0; i < 20; i++) {
    double T = 15 + i;
    double d = T - 25;
    m.add(T, 12 - 0.2 * d + 0.01 * d * d, 1.0);
  }
  for (double T = 15; T <= 34; T += 3) {
    double d = T - 25;
    double exp = 12 - 0.2 * d + 0.01 * d * d;
    CHECK(std::fabs(m.ppm_at(T) - exp) < 1e-6, "temp model at %.1f: %.6f vs %.6f", T, m.ppm_at(T), exp);
  }
  // Offset-only update keeps the learned shape.
  double c0, c1, c2;
  m.get_coeffs(&c0, &c1, &c2);
  TempRateModel n;
  n.set_coeffs(c0, c1, c2);
  n.add(25.0, 12.5, 1.0);  // crystal aged by +0.5 ppm, single temperature
  CHECK(std::fabs(n.ppm_at(25.0) - 12.5) < 1e-6, "offset update");
  CHECK(std::fabs(n.ppm_at(35.0) - (12.5 - 2.0 + 1.0)) < 1e-6, "slope kept");
}

static void test_settings() {
  CHECK(sizeof(SettingsPacket) == 36, "settings packet size %d", (int) sizeof(SettingsPacket));
  SettingsPacket a = {}, b = {};
  a.rev = 3; a.author = 1;
  b.rev = 2; b.author = 9;
  CHECK(settings_newer(a, b) && !settings_newer(b, a), "revision ordering");
  b.rev = 3;
  CHECK(settings_newer(b, a) && !settings_newer(a, b), "author tie-break");
  // raw ESP-NOW action frame
  uint8_t f[39 + 36 + 4] = {0};
  f[0] = 0xD0; f[24] = 127; f[25] = 0x18; f[26] = 0xFE; f[27] = 0x34;
  f[32] = 221; f[33] = 5 + 36; f[34] = 0x18; f[35] = 0xFE; f[36] = 0x34; f[37] = 4; f[38] = 1;
  a.magic = SETTINGS_MAGIC; a.version = SETTINGS_VERSION; a.group = 77; a.rate = 1;
  memcpy(f + 39, &a, sizeof a);
  const uint8_t *body;
  int n = espnow_payload(f, sizeof f, &body);
  CHECK(n == 36 && settings_valid(*(const SettingsPacket *) body, 77), "espnow parse");
  CHECK(!settings_valid(*(const SettingsPacket *) body, 78), "group filter");
  f[37] = 5;
  CHECK(espnow_payload(f, sizeof f, &body) < 0, "reject non-espnow vendor frame");
}

int main() {
  test_settings();
  test_timecode();
  test_frame_vs_libltc();
  test_biphase();
  test_temp_model();
  printf(failures ? "test_core: %d FAILURES\n" : "test_core: all passed\n", failures);
  return failures ? 1 : 0;
}

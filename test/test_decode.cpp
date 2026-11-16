// End-to-end: our biphase segments -> 48 kHz audio -> libltc decoder.
// Also slews the frame length by ±2 % (what the phase controller may do) to
// make sure a standard reader still decodes every frame.
#include "../components/ltc_gen/ltc_core.h"

#include <cstdio>
#include <vector>

extern "C" {
#include "ltc.h"
}

using namespace ltc_core;

static int run(Rate r, double speed, int nframes) {
  const double fs = 48000.0;
  const double tick_hz = 10e6;
  double period_ticks = frame_period_us(r) * 10.0 / speed;
  std::vector<float> audio;
  double t_ticks = 0, carry = 0;
  bool level = false;
  Segment seg[MAX_SEGMENTS];
  // 10 frames of lead-in so the decoder can learn the speed
  int64_t first = 1000;
  for (int64_t k = first - 10; k < first + nframes; k++) {
    Frame f = encode_frame(frames_to_timecode(k, r), r);
    int n = biphase_encode(f, period_ticks, carry, false, seg);
    for (int i = 0; i < n; i++) {
      double t_end = t_ticks + seg[i].ticks;
      size_t s_end = (size_t) (t_end / tick_hz * fs);
      while (audio.size() < s_end) audio.push_back(seg[i].level ? 0.5f : -0.5f);
      t_ticks = t_end;
      level = seg[i].level;
    }
  }
  (void) level;
  LTCDecoder *d = ltc_decoder_create((int) (fs / rate_info(r).num * rate_info(r).den), 32);
  int decoded = 0, ok = 0;
  int64_t expect = -1;
  for (size_t off = 0; off < audio.size(); off += 1024) {
    size_t len = std::min<size_t>(1024, audio.size() - off);
    ltc_decoder_write_float(d, &audio[off], len, (ltc_off_t) off);
    LTCFrameExt fe;
    while (ltc_decoder_read(d, &fe)) {
      SMPTETimecode st;
      ltc_frame_to_time(&st, &fe.ltc, 0);
      decoded++;
      // map decoded label back to a frame number by searching forward
      Timecode tc;
      if (expect < 0) {
        for (int64_t k = first - 10; k < first + nframes; k++) {
          tc = frames_to_timecode(k, r);
          if (tc.hours == st.hours && tc.minutes == st.mins && tc.seconds == st.secs && tc.frames == st.frame) {
            expect = k;
            break;
          }
        }
      }
      tc = frames_to_timecode(expect, r);
      if (tc.hours == st.hours && tc.minutes == st.mins && tc.seconds == st.secs && tc.frames == st.frame &&
          fe.ltc.dfbit == (tc.drop ? 1 : 0))
        ok++;
      expect++;
    }
  }
  ltc_decoder_free(d);
  int want = nframes + 10 - 1;  // the decoder emits a frame once the next sync word arrives
  printf("  rate=%d speed=%.2f: decoded %d, sequential+correct %d (>= %d wanted)\n", (int) r, speed, decoded, ok, want - 2);
  return ok >= want - 2 && ok == decoded ? 0 : 1;
}

int main() {
  int fails = 0;
  Rate rates[] = {Rate::FPS_24, Rate::FPS_25, Rate::FPS_2997_DF, Rate::FPS_30};
  for (Rate r : rates)
    for (double sp : {0.98, 1.0, 1.02}) fails += run(r, sp, 600);
  printf(fails ? "test_decode: %d FAILURES\n" : "test_decode: all passed\n", fails);
  return fails ? 1 : 0;
}

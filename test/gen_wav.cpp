// Writes a stereo 48 kHz WAV with two LTC streams from ltc_core:
//   left  = ideal,  right = delayed by OFFSET_US and running DRIFT_PPM fast.
// Used to check tools/ltc_compare.py end to end.
//   gen_wav out.wav [offset_us] [drift_ppm] [seconds]
#include "../components/ltc_gen/ltc_core.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ltc_core;

static std::vector<int16_t> render(double offset_us, double drift_ppm, double seconds, double fs) {
  Rate r = Rate::FPS_25;
  std::vector<int16_t> out((size_t) (seconds * fs), 0);
  double t_us = offset_us;
  double carry = 0;
  Segment seg[MAX_SEGMENTS];
  double ticks_per_us = 10.0;
  double frame_ticks = frame_period_us(r) * ticks_per_us / (1.0 + drift_ppm * 1e-6);
  // levels before the first edge: idle (low)
  size_t s0 = 0;
  for (int64_t k = 90000; t_us < seconds * 1e6; k++) {
    Frame f = encode_frame(frames_to_timecode(k, r), r);
    int n = biphase_encode(f, frame_ticks, carry, false, seg);
    for (int i = 0; i < n; i++) {
      double t_end = t_us + seg[i].ticks / ticks_per_us;
      size_t s_end = std::min(out.size(), (size_t) (t_end * 1e-6 * fs));
      for (size_t s = s0; s < s_end; s++) out[s] = seg[i].level ? 8000 : -8000;
      s0 = s_end;
      t_us = t_end;
    }
  }
  return out;
}

int main(int argc, char **argv) {
  if (argc < 2) return 1;
  double off = argc > 2 ? atof(argv[2]) : 123.0;
  double drift = argc > 3 ? atof(argv[3]) : 2.0;
  double secs = argc > 4 ? atof(argv[4]) : 300.0;
  const double fs = 48000;
  auto a = render(0, 0, secs, fs);
  auto b = render(off, drift, secs, fs);
  FILE *fp = fopen(argv[1], "wb");
  uint32_t n = a.size(), data = n * 4, riff = 36 + data, sr = 48000, br = sr * 4, fmt = 16;
  uint16_t pcm = 1, ch = 2, ba = 4, bps = 16;
  fwrite("RIFF", 1, 4, fp);
  fwrite(&riff, 4, 1, fp);
  fwrite("WAVEfmt ", 1, 8, fp);
  fwrite(&fmt, 4, 1, fp);
  fwrite(&pcm, 2, 1, fp);
  fwrite(&ch, 2, 1, fp);
  fwrite(&sr, 4, 1, fp);
  fwrite(&br, 4, 1, fp);
  fwrite(&ba, 2, 1, fp);
  fwrite(&bps, 2, 1, fp);
  fwrite("data", 1, 4, fp);
  fwrite(&data, 4, 1, fp);
  for (uint32_t i = 0; i < n; i++) {
    fwrite(&a[i], 2, 1, fp);
    fwrite(&b[i], 2, 1, fp);
  }
  fclose(fp);
  return 0;
}

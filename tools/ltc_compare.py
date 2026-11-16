#!/usr/bin/env python3
"""Compare two LTC signals recorded on the left/right channels of one WAV file.

Record node A on the left and node B on the right channel of any stereo
recorder (or a computer line input), for as long as you like, then:

    python3 ltc_compare.py recording.wav            # summary
    python3 ltc_compare.py recording.wav --csv out.csv --plot out.png

For every timecode frame present on both channels it measures when the frame
started on each channel and reports the offset B - A over time. Because both
channels are sampled by the same converter, the recorder's own clock error
cancels out; the result is the true node-to-node synchronisation error.
Resolution is a small fraction of a sample (edges are interpolated).

Requires: numpy (matplotlib only for --plot).
"""

import argparse
import sys
import wave

import numpy as np

SYNC = [0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1]


def read_wav(path):
    with wave.open(path, "rb") as w:
        n_ch, width, fs, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float32)
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v & 0x800000, v - 0x1000000, v)
        data = v.astype(np.float32)
    elif width == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float32)
    elif width == 1:
        data = np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128
    else:
        sys.exit(f"unsupported sample width {width}")
    return fs, data.reshape(-1, n_ch)


def edges(x):
    """Zero-crossing times (in samples, interpolated) with hysteresis."""
    lo_p, hi_p = np.percentile(x, [1, 99])
    x = x - 0.5 * (lo_p + hi_p)
    amp = 0.5 * (hi_p - lo_p)
    if amp <= 0:
        return np.array([])
    thr = 0.25 * amp
    s = np.where(x > thr, 1, np.where(x < -thr, -1, 0)).astype(np.int8)
    # forward-fill the band so small wiggles around zero do not count
    idx = np.where(s != 0, np.arange(len(s)), 0)
    np.maximum.accumulate(idx, out=idx)
    sf = s[idx]
    trans = np.flatnonzero(np.diff(sf) != 0) + 1
    trans = trans[sf[trans - 1] != 0]
    if len(trans) == 0:
        return np.array([])
    # the actual zero crossing is the last sign change at or before each transition
    sign = x > 0
    zc = np.flatnonzero(sign[:-1] != sign[1:]) + 1  # crossing between zc-1 and zc
    k = np.searchsorted(zc, trans, side="right") - 1
    k = k[k >= 0]
    j = np.unique(zc[k])
    a, b = x[j - 1], x[j]
    frac = np.where(a != b, a / (a - b), 0.0)
    return (j - 1) + frac


def decode(fs, x):
    """Return list of (frame_label_tuple, start_time_seconds, drop_flag)."""
    e = edges(x)
    if len(e) < 200:
        return []
    d = np.diff(e)
    full = np.percentile(d, 90)  # rough full-bit length (long intervals)
    bits, starts = [], []
    i = 0
    while i < len(d):
        if d[i] > 0.75 * full:
            bits.append(0)
            starts.append(e[i])
            full = 0.9 * full + 0.1 * d[i]
            i += 1
        elif i + 1 < len(d) and d[i + 1] < 0.75 * full:
            bits.append(1)
            starts.append(e[i])
            full = 0.9 * full + 0.1 * (d[i] + d[i + 1])
            i += 2
        else:
            i += 1  # resync
    bits = np.array(bits, dtype=np.int8)
    frames = []
    k = 79
    sync = np.array(SYNC, dtype=np.int8)
    while k < len(bits):
        if np.array_equal(bits[k - 15 : k + 1], sync):
            b = bits[k - 79 : k + 1]

            def val(pos, n):
                return int(sum(int(b[pos + j]) << j for j in range(n)))

            ff = val(0, 4) + 10 * val(8, 2)
            ss = val(16, 4) + 10 * val(24, 3)
            mm = val(32, 4) + 10 * val(40, 3)
            hh = val(48, 4) + 10 * val(56, 2)
            frames.append(((hh, mm, ss, ff), starts[k - 79] / fs, bool(b[10])))
            k += 80
        else:
            k += 1
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--left", type=int, default=0, help="channel index of node A (default 0)")
    ap.add_argument("--right", type=int, default=1, help="channel index of node B (default 1)")
    ap.add_argument("--csv", help="write time,offset_us per matched frame")
    ap.add_argument("--plot", help="write a PNG plot of offset over time")
    args = ap.parse_args()

    fs, data = read_wav(args.wav)
    if data.shape[1] < 2:
        sys.exit("need a stereo (or multi-channel) file")
    fa = decode(fs, data[:, args.left])
    fb = decode(fs, data[:, args.right])
    print(f"{args.wav}: {data.shape[0] / fs / 60:.1f} min at {fs} Hz")
    print(f"  channel A: {len(fa)} frames decoded, channel B: {len(fb)} frames decoded")
    if not fa or not fb:
        sys.exit("no LTC found on one of the channels (check levels / cabling)")

    # match by label; the same label recurs only every 24 h
    tb = {}
    for lab, t, _ in fb:
        tb.setdefault(lab, []).append(t)
    rows = []
    for lab, t, _ in fa:
        cand = tb.get(lab)
        if not cand:
            continue
        tbest = min(cand, key=lambda u: abs(u - t))
        if abs(tbest - t) < 1.0:
            rows.append((t, (tbest - t) * 1e6))
    if not rows:
        # labels never matched: report the label offset
        print("  no common labels: the two nodes count different timecodes")
        print(f"  first A: {fa[0][0]}  first B: {fb[0][0]}")
        sys.exit(1)
    arr = np.array(rows)
    off = arr[:, 1]
    print(f"  matched frames: {len(arr)}  ({len(arr) / max(len(fa), 1) * 100:.1f} % of A)")
    print("  (positive offset = B starts its frames later than A)")
    print(f"  offset B-A: median {np.median(off):.1f} us, min {off.min():.1f} us, max {off.max():.1f} us")
    print(f"  peak-to-peak {off.max() - off.min():.1f} us, std {off.std():.1f} us")
    if len(arr) > 10 and arr[-1, 0] - arr[0, 0] > 60:
        slope = np.polyfit(arr[:, 0], off, 1)[0]  # us per second
        print(f"  trend {slope:+.4f} us/s = {slope * 86400 / 1000:+.2f} ms/day (B vs A: {-slope:+.4f} ppm fast)")
    per_frame = 1e6 / (len(fa) / (fa[-1][1] - fa[0][1])) if len(fa) > 1 else 0
    print(f"  (1 frame = {per_frame:.0f} us)")

    if args.csv:
        np.savetxt(args.csv, arr, delimiter=",", header="time_s,offset_us", comments="", fmt="%.6f")
        print(f"  wrote {args.csv}")
    if args.plot:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig, ax = plt.subplots(figsize=(9, 4))
        ax.plot(arr[:, 0] / 60, off, lw=0.8)
        ax.set_xlabel("recording time (min)")
        ax.set_ylabel("offset B - A (µs)")
        ax.set_title("LTC node-to-node offset")
        ax.grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(args.plot, dpi=120)
        print(f"  wrote {args.plot}")


if __name__ == "__main__":
    main()

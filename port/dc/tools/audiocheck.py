#!/usr/bin/env python3
"""Check a raw audio capture from flycast (FC_AUDIO_DUMP: 44.1 kHz s16 stereo, L R).

    audiocheck.py capture.raw [--fps 59.94] [--frame-offset N]

Prints loudness per 10 s, and lists
  * gaps: runs of >= 4 ms of near-silence (|x| < 64) with sound on both sides within 50 ms, i.e.
    drop-outs inside sound rather than pauses between sounds;
  * clicks: sample-to-sample steps (> 4096) more than 4x the largest step within 25 ms around
    them, i.e. isolated discontinuities rather than the edges of a waveform.
Times are also given as approximate game frames (capture time x fps + offset).
Needs numpy.
"""
import sys

import numpy as np

RATE = 44100


def main():
    path = sys.argv[1]
    fps = float(sys.argv[sys.argv.index("--fps") + 1]) if "--fps" in sys.argv else 59.94
    off = int(sys.argv[sys.argv.index("--frame-offset") + 1]) if "--frame-offset" in sys.argv else 0
    d = np.fromfile(path, dtype="<i2").reshape(-1, 2).astype(np.int32)
    n = len(d)
    print(f"{n / RATE:.1f} s captured")
    mono = np.abs(d).max(axis=1)
    for t in range(0, int(n / RATE), 10):
        seg = d[t * RATE:(t + 10) * RATE]
        if len(seg):
            print(f"  {t:4d}-{t + 10:4d} s: RMS L {np.sqrt((seg[:, 0] ** 2).mean()):7.0f}  R "
                  f"{np.sqrt((seg[:, 1] ** 2).mean()):7.0f}  peak {np.abs(seg).max():6d}")

    quiet = mono < 64
    gaps = []
    i = 0
    min_len, ctx = int(0.004 * RATE), int(0.05 * RATE)
    edges = np.flatnonzero(np.diff(quiet.astype(np.int8)))
    starts = edges[np.diff(quiet.astype(np.int8))[edges] == 1] + 1
    for s in starts:
        e = s
        while e < n and quiet[e]:
            e += 1
        if e - s >= min_len and e < n:
            before = mono[max(0, s - ctx):s].max(initial=0)
            after = mono[e:e + ctx].max(initial=0)
            if before > 1024 and after > 1024:
                gaps.append((s, e))
    print(f"gaps inside sound: {len(gaps)}")
    for s, e in gaps[:40]:
        print(f"  at {s / RATE:8.3f} s (~frame {int(s / RATE * fps) + off}), {1000 * (e - s) / RATE:.1f} ms")

    step = np.abs(np.diff(d, axis=0)).max(axis=1)
    w, guard = int(0.025 * RATE), 32
    clicks = []
    for k in np.flatnonzero(step > 4096):
        left = step[max(0, k - w):max(0, k - guard)]
        right = step[k + guard:k + w]
        ctx_max = max(left.max(initial=0), right.max(initial=0))
        if step[k] > 4 * max(ctx_max, 256) and (not clicks or k - clicks[-1] > w):
            clicks.append(k)
    print(f"clicks: {len(clicks)}")
    for k in clicks[:40]:
        print(f"  at {k / RATE:8.3f} s (~frame {int(k / RATE * fps) + off}), step {step[k]}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Summarise the PERF / SPIKE / MEM lines of a scripted run's serial log.

    perfstats.py serial.txt [--gameplay-from FRAME]

PERF lines cover 120-frame windows (hal_test.c). Windows that end at or before --gameplay-from
(default 2280: Vision 1-1 has faded in by then on the test route) count as "boot to level", the
rest as "gameplay". Frame rate is frames / total time, not the mean of the window rates.
"""
import re
import sys

PERF = re.compile(r"PERF frames=(\d+) fps=([\d.]+) frame=(\d+)us worst=(\d+)us game=(\d+)us render=(\d+)us "
                  r"submit=(\d+)us tex=(\d+)us wait=(\d+)us")
SPIKE = re.compile(r"SPIKE frame=(\d+) (\d+)us")


def summary(name, rows):
    if not rows:
        print(f"{name}: no windows")
        return
    n = 120 * len(rows)
    total = sum(r["frame"] * 120 for r in rows)
    print(f"{name}: frames {rows[0]['end'] - 119}-{rows[-1]['end']} ({n} frames, {len(rows)} windows)")
    print(f"  mean {1e6 * n / total:.1f} fps, slowest 120-frame window {min(r['fps'] for r in rows):.1f} fps, "
          f"windows below 59 fps: {sum(r['fps'] < 59 for r in rows)}")
    print(f"  worst single frame {max(r['worst'] for r in rows) / 1000:.1f} ms")
    for k in ("game", "render", "tex", "submit", "wait"):
        print(f"  {k:7s} {sum(r[k] for r in rows) / len(rows) / 1000:6.2f} ms/frame (mean of windows), "
              f"max window {max(r[k] for r in rows) / 1000:6.2f}")


def main():
    path = sys.argv[1]
    split = int(sys.argv[sys.argv.index("--gameplay-from") + 1]) if "--gameplay-from" in sys.argv else 2280
    rows, spikes = [], []
    for line in open(path, errors="ignore"):
        m = PERF.search(line)
        if m:
            v = list(map(float, m.groups()))
            rows.append(dict(end=int(v[0]), fps=v[1], frame=v[2], worst=v[3], game=v[4], render=v[5],
                             submit=v[6], tex=v[7], wait=v[8]))
        m = SPIKE.search(line)
        if m:
            spikes.append((int(m.group(1)), int(m.group(2))))
        if line.startswith(("MEM ", "stopping after")):
            print(line.strip())
    summary("all", rows)
    summary("boot to level", [r for r in rows if r["end"] <= split])
    summary("gameplay", [r for r in rows if r["end"] > split])
    late = [s for s in spikes if s[0] > split]
    print(f"frames over 33 ms: {len(spikes)} ({len(late)} after frame {split})"
          + (f"; worst {max(s[1] for s in spikes) / 1000:.1f} ms at frame {max(spikes, key=lambda s: s[1])[0]}"
             if spikes else ""))


if __name__ == "__main__":
    main()

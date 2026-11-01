#!/usr/bin/env python3
"""Stage-by-stage checks of the port's sound, from AUDIODUMP records and flycast captures.

    audiotrace.py probe CAPTURE
        CAPTURE from an AUDIOPROBE=1 build (the model's output replaced by 440 Hz left, 1 kHz right,
        8000 peak): checks the Dreamcast output path alone -- each tone on its own side at the right
        pitch and level, the other side silent, and no image of it mirrored about half the stream
        rate (what a sample-order error in the channel split produces).

    audiotrace.py engine RECS --rom ROM --elf KLEOD_ELF [--song 33]
        RECS from an AUDIODUMP=from-to build (written by flycast's FC_RAMDUMP hook). Checks every
        block in the ring was rendered whole between two VSyncs, and that each VSync had exactly one
        SoundMain behind it (no segment played twice or skipped). With --song: the pcmBuffer
        segments DMA played (the engine's output) against the cartridge's own engine running that
        song in an emulated ARM (Unicorn) from m4aSoundInit -- byte for byte, in order.

    audiotrace.py stream RECS CAPTURE [--frames A-B]
        The hardware model's output (the blocks the port queued, in ring order) against what the
        AICA played (CAPTURE: flycast FC_AUDIO_DUMP, 44.1 kHz s16 L,R). The AICA's resampling is
        reproduced (step 935/1024, linear interpolation with a 10-bit fraction -- what KallistiOS's
        40280 Hz request becomes) and each 4096-sample window is aligned on its own; reports gain
        and residual per window, and which model channel each output side matches.

    audiotrace.py capture CAPTURE [--wav OUT.wav]
        Format, duration, peak, clipped samples, DC, largest sample-to-sample step; optionally
        writes a WAV for listening.

Exit status 0 when every check passes. Needs numpy (and unicorn for engine --song).
"""
import argparse
import os
import struct
import sys
import wave

import numpy as np

SEG = 224          # GBASND_MAX_SEG
MAXS = 704         # GBASND_MAX_SAMPLES
HDR = struct.Struct("<IHHBBBBIII")  # struct dump_rec in hal_audio.c, up to the sample data
REC = HDR.size + 2 * SEG + 4 * MAXS
CAP_RATE = 44100
AICA_STEP = 935    # (1024 | FNS) >> 1 with FNS = ((40280 << 10) / 22050) & 1023 = 846, OCT -1
fails = []


def check(ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        fails.append(what)
    return ok


def load_records(path):
    d = open(path, "rb").read()
    if len(d) % REC:
        raise SystemExit(f"{path}: {len(d)} bytes is not a whole number of {REC}-byte records")
    out = []
    for i in range(len(d) // REC):
        r = d[i * REC:(i + 1) * REC]
        frame, spf, n, flags, dc, seg, tid, vsyncs, mains, lock = HDR.unpack_from(r)
        a = np.frombuffer(r, np.int8, SEG, HDR.size)[:spf]
        b = np.frombuffer(r, np.int8, SEG, HDR.size + SEG)[:spf]
        blk = np.frombuffer(r, np.int16, 2 * n, HDR.size + 2 * SEG).reshape(-1, 2)
        out.append(dict(frame=frame, spf=spf, n=n, flags=flags, dc=dc, seg=seg, tid=tid, vsyncs=vsyncs,
                        mains=mains, lock=lock, a=a, b=b, blk=blk))
    return out


def load_capture(path):
    d = open(path, "rb").read()
    if len(d) % 4:
        raise SystemExit(f"{path}: not 16-bit stereo ({len(d)} bytes)")
    return np.frombuffer(d, np.int16).reshape(-1, 2)


def aica_resample(x, phase=0):
    """What the AICA plays from x (one channel, the stream's rate) at 44.1 kHz: position advances
    935/1024 of a sample per output sample, linear interpolation on the 10-bit fraction."""
    x = x.astype(np.int64)
    pos = phase + AICA_STEP * np.arange((len(x) - 1) * 1024 // AICA_STEP)
    i, f = pos >> 10, pos & 1023
    return ((x[i] * (1024 - f) + x[i + 1] * f) >> 10).astype(float)


def lowpass(x, cutoff):
    X = np.fft.rfft(x)
    X[np.fft.rfftfreq(len(x), 1 / CAP_RATE) > cutoff] = 0
    return np.fft.irfft(X, len(x))


def xcorr_peak(a, b):
    """Offset of b inside a with the highest correlation."""
    n = len(a) + len(b)
    c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)[:len(a) - len(b) + 1]
    return int(np.argmax(c))


def resid_db(ref, got):
    g = (got @ ref) / max(ref @ ref, 1e-9)
    return g, 10 * np.log10(max(((got - g * ref) ** 2).sum(), 1e-9) / max((got ** 2).sum(), 1e-9))


# ---- probe ------------------------------------------------------------------------------------

def cmd_probe(a):
    cap = load_capture(a.capture).astype(float)
    print(f"{a.capture}: {len(cap)} frames ({len(cap) / CAP_RATE:.2f} s)")
    if not check(len(cap) > 5 * CAP_RATE, "at least 5 s captured"):
        return 1
    w = np.hanning(16384)
    fr = np.fft.rfftfreq(16384, 1 / CAP_RATE)
    stream_rate = CAP_RATE * AICA_STEP / 1024
    for side, f0 in ((0, 440.0), (1, 1000.0)):
        name = "left" if side == 0 else "right"
        worst_thd, worst_img, worst_leak, worst_df = -999, -999, -999, 0
        # from 2 s in (past the start-up underruns) to 1 s before the end (the run stops there)
        for st in range(2 * CAP_RATE, len(cap) - 16384 - CAP_RATE, CAP_RATE // 2):
            x, y = cap[st:st + 16384, side], cap[st:st + 16384, 1 - side]
            X, Y = np.abs(np.fft.rfft(x * w)), np.abs(np.fft.rfft(y * w))
            k = int(np.argmax(X))
            # parabolic interpolation of the peak for the frequency
            dk = 0.5 * (np.log(X[k - 1]) - np.log(X[k + 1])) / (np.log(X[k - 1]) - 2 * np.log(X[k]) + np.log(X[k + 1]))
            f = (k + dk) * CAP_RATE / 16384
            p = (X[k - 4:k + 5] ** 2).sum()
            tot = (X ** 2).sum()
            thd = 10 * np.log10(max(tot - p, 1e-9) / p)
            img = [stream_rate / 2 - f, stream_rate / 2 + f]  # f mirrored about half the stream rate
            imgdb = max(20 * np.log10(X[np.abs(fr - fi) < 30].max() / X[k]) for fi in img if fi < CAP_RATE / 2)
            leak = 20 * np.log10(max(Y[k - 4:k + 5].max(), 1e-9) / X[k])  # this tone on the other side
            worst_thd, worst_img, worst_leak = max(worst_thd, thd), max(worst_img, imgdb), max(worst_leak, leak)
            worst_df = max(worst_df, abs(f - f0 * stream_rate / 40280))
            amp = np.abs(x).max()
        print(f"  {name}: {f:.1f} Hz, peak {amp:.0f}; worst over the capture: THD+N {worst_thd:.1f} dB, "
              f"mirror image {worst_img:.1f} dB, other side {worst_leak:.1f} dB")
        check(worst_df < 1.5, f"{name}: the {f0:.0f} Hz tone, at the pitch the stream rate gives")
        check(7600 < amp < 8400, f"{name}: peak 8000 +- 5%")
        check(worst_thd < -35, f"{name}: THD+N below -35 dB")
        check(worst_img < -60, f"{name}: no image mirrored about half the stream rate (below -60 dB)")
        check(worst_leak < -60, f"{name}: the other side below -60 dB (channels not swapped or mixed)")
    return 1 if fails else 0


# ---- engine -----------------------------------------------------------------------------------

def reference_segments(rom_path, elf_path, song, frames):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "m4a_difftest"))
    import difftest as D
    syms = D.nm(elf_path)
    S = lambda n: syms[n][0]
    g = D.Gba(open(rom_path, "rb").read(), b"\0")
    g.hook_cpuset(S("CpuSet"))
    g.call(S("m4aSoundInit"))
    si = S("gSoundInfo")
    g.call(S("m4aSongNumStart"), song)
    out = []
    for _ in range(frames):
        g.call(S("m4aSoundVSync"))
        h = bytes(g.uc.mem_read(si, 0x14))
        dc, period, spf = h[4], h[11], struct.unpack_from("<I", h, 0x10)[0]
        seg = period - (dc - 1) if dc > 1 else 0  # the segment SoundMain mixes
        g.call(S("SoundMain"))
        pcm = bytes(g.uc.mem_read(si + 0x350, 1584 * 2))
        out.append((pcm[seg * spf:(seg + 1) * spf], pcm[1584 + seg * spf:1584 + (seg + 1) * spf]))
    return out


def cmd_engine(a):
    recs = load_records(a.records)
    print(f"{a.records}: {len(recs)} records, frames {recs[0]['frame']}..{recs[-1]['frame']}")
    period = max(r["dc"] for r in recs)  # framesPerDmaCycle: DMA plays segment period - dmaCounter
    torn = [i for i, r in enumerate(recs) if r["seg"] != 0xFF and r["dc"] and r["seg"] != period - r["dc"]]
    check(not torn, f"every block rendered whole between two VSyncs ({len(torn)} torn"
                    f"{': first at frame ' + str(recs[torn[0]]['frame']) if torn else ''})")
    bad = []
    for p, r in zip(recs, recs[1:]):
        dv, dm = r["vsyncs"] - p["vsyncs"], r["mains"] - p["mains"]
        loud = np.abs(r["a"].astype(int)).max() > 1 if r["flags"] & 1 else False
        if dv == 1 and dm != 1 and loud:
            bad.append(r["frame"])
    check(not bad, f"one SoundMain behind every VSync while sound plays ({len(bad)} exceptions"
                   f"{': frames ' + str(bad[:5]) if bad else ''})")
    lag = sum(1 for r in recs if not r["flags"] & 8)
    print(f"  {lag} lag-frame blocks, {sum(1 for r in recs if not r['flags'] & 4)} dropped")
    if a.song is None:
        return 1 if fails else 0
    # Every segment the engine handed to DMA, whether or not the ring kept its block (a lag frame's
    # block is dropped when the ring is full; the engine still advanced).
    played = [r for r in recs if r["flags"] & 1]
    first = next((i for i, r in enumerate(played) if np.abs(r["a"].astype(int)).max() > 1), None)
    if not check(first is not None, "the song is audible in the records"):
        return 1
    n = min(a.frames, len(played) - first)
    ref = reference_segments(a.rom, a.elf, a.song, n + 2)
    k0 = next(k for k, (x, _) in enumerate(ref) if np.abs(np.frombuffer(x, np.int8).astype(int)).max() > 1)
    same = 0
    for j in range(n):
        r, (ra, rb) = played[first + j], ref[k0 + j]
        if r["a"].tobytes() == ra and r["b"].tobytes() == rb:
            same += 1
        elif same == j:
            print(f"  first difference: frame {r['frame']} (song frame {k0 + j})")
    check(same == n, f"song {a.song}: {same}/{n} segments byte-identical to the cartridge's engine, in order "
                     f"(from frame {played[first]['frame']})")
    return 1 if fails else 0


# ---- stream -----------------------------------------------------------------------------------

def cmd_stream(a):
    recs = load_records(a.records)
    cap = load_capture(a.capture).astype(float)
    lo, hi = (int(x) for x in a.frames.split("-")) if a.frames else (0, 1 << 30)
    kept = [r for r in recs if r["flags"] & 4]
    blk = np.concatenate([r["blk"] for r in kept]).astype(np.int64)
    start = sum(r["n"] for r in kept if r["frame"] < lo)
    end = sum(r["n"] for r in kept if r["frame"] <= hi)
    model = [aica_resample(blk[:, c]) for c in (0, 1)]
    s0, s1 = int(start * 1024 / AICA_STEP), int(end * 1024 / AICA_STEP)
    print(f"{a.records} + {a.capture}: model frames {lo}..{min(hi, kept[-1]['frame'])}, "
          f"{(s1 - s0) / CAP_RATE:.2f} s")
    W = 4096
    rows = []
    # Windows are aligned in order from the start of the stream, even before frame A, so that a
    # repeating passage (looped music) cannot be matched to the wrong repeat. The capture position
    # minus the model position only grows, by the silence an underrun inserts; it starts at the
    # silence the AICA plays before the first block.
    shift, unaligned, quiet = 0, 0, 0
    for pos in range(0, s1 - W, W):
        m = [model[c][pos:pos + W] for c in (0, 1)]
        if min(np.abs(m[0]).max(), np.abs(m[1]).max()) < 1000:
            quiet += 1
            continue  # too quiet to align reliably
        if pos + shift + W > len(cap):
            break
        limit = min(4096 + quiet * W, 1 << 18)  # underruns happen during the quiet parts (loads)
        quiet = 0
        best = None
        src = slice(int(pos * AICA_STEP / 1024) - 2, int((pos + W) * AICA_STEP / 1024) + 8)

        def try_phases(phases, offsets):
            nonlocal best
            for ph in phases:  # the AICA's fraction at the window start is not known: search it
                mm = [aica_resample(blk[src, c], ph) for c in (0, 1)]
                for o in offsets:
                    if 0 <= o and o + W <= len(cap):
                        r0 = resid_db(mm[0][2:2 + W], cap[o:o + W, 0])[1]
                        if best is None or r0 < best[0]:
                            best = (r0, o, ph, mm)
        # The nearest match is the right one (a farther one can be a repeat of the passage), so
        # widen the search only while nothing near fits.
        reach = 4096
        while True:
            lo_ = max(0, pos + shift - 512)
            off = lo_ + xcorr_peak(cap[lo_:pos + shift + W + reach, 0], m[0])
            offs = {off, pos + shift}
            try_phases(range(0, 1024, 64), sorted(o_ + d for o_ in offs for d in (-1, 0, 1)))
            if (best is not None and best[0] < -25) or reach >= limit:
                break
            reach = min(reach * 4, limit)
        if best is None:
            print(f"  @{pos / CAP_RATE:7.3f}s  no alignment inside the capture (candidates {sorted(offs)})")
            unaligned += 1
            continue
        try_phases(range(best[2] - 60, best[2] + 61, 4), [best[1]])
        r0, o, ph, mm = best
        shift = o - pos
        if pos < s0:
            continue  # aligned only to keep track, before the frames asked for
        x = [mm[c][2:2 + W] for c in (0, 1)]
        y = [cap[o:o + W, c] for c in (0, 1)]
        g = [resid_db(x[c], y[c]) for c in (0, 1)]
        glp = [resid_db(lowpass(x[c], 5000), lowpass(y[c], 5000)) for c in (0, 1)]
        # which model channel each side matches; meaningless where the two are (nearly) the same
        swap = [resid_db(x[1 - c], y[c])[1] if resid_db(x[0], x[1])[1] > -40 else 0 for c in (0, 1)]
        rows.append((pos, g, glp, swap))
    if not check(len(rows) >= 3, f"{len(rows)} windows loud enough to compare (need 3)"):
        return 1
    for pos, g, glp, swap in (rows if a.verbose else rows[:6]):
        print(f"  @{pos / CAP_RATE:7.3f}s  L gain {g[0][0]:.3f} resid {g[0][1]:6.1f} dB (<5k {glp[0][1]:6.1f})  "
              f"R gain {g[1][0]:.3f} resid {g[1][1]:6.1f} dB (<5k {glp[1][1]:6.1f})")
    worst = max(max(g[0][1], g[1][1]) for _, g, _, _ in rows)
    worst_lp = max(max(glp[0][1], glp[1][1]) for _, _, glp, _ in rows)
    gains = [g[c][0] for _, g, _, _ in rows for c in (0, 1)]
    right_side = sum(1 for _, g, _, swap in rows for c in (0, 1) if g[c][1] <= swap[c])
    med = np.median([g[c][1] for _, g, _, _ in rows for c in (0, 1)])
    print(f"  {len(rows)} windows: median residual {med:.1f} dB; worst {worst:.1f} dB, below 5 kHz {worst_lp:.1f} dB, "
          f"gain {min(gains):.3f}..{max(gains):.3f}")
    check(unaligned == 0, f"every window found in the capture ({unaligned} not)")
    check(0.95 < min(gains) and max(gains) < 1.05, "gain from model to AICA output within 5%")
    check(worst < -30, "every window matches the model within -30 dB (whole band)")
    check(worst_lp < -40, "every window matches the model within -40 dB below 5 kHz")
    check(right_side == 2 * len(rows), "each side matches its own model channel better than the other one")
    return 1 if fails else 0


# ---- capture ----------------------------------------------------------------------------------

def cmd_capture(a):
    cap = load_capture(a.capture)
    x = cap.astype(np.int64)
    step = np.abs(np.diff(x, axis=0)).max(axis=0)
    print(f"{a.capture}: 16-bit stereo at {CAP_RATE} Hz, {len(cap)} frames = {len(cap) / CAP_RATE:.2f} s")
    print(f"  peak L {np.abs(x[:, 0]).max()} R {np.abs(x[:, 1]).max()}, clipped samples "
          f"{int((np.abs(x) >= 32767).sum())}, DC L {x[:, 0].mean():.1f} R {x[:, 1].mean():.1f}, "
          f"largest step L {step[0]} R {step[1]}")
    if a.wav:
        with wave.open(a.wav, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(CAP_RATE)
            w.writeframes(cap.astype("<i2").tobytes())
        print(f"  wrote {a.wav}")
    check(len(cap) > 0, "capture is not empty")
    check(int((np.abs(x) >= 32767).sum()) == 0, "no clipped samples")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("probe")
    p.add_argument("capture")
    p = sub.add_parser("engine")
    p.add_argument("records")
    p.add_argument("--rom")
    p.add_argument("--elf")
    p.add_argument("--song", type=int)
    p.add_argument("--frames", type=int, default=100, help="song frames to compare (default 100)")
    p = sub.add_parser("stream")
    p.add_argument("records")
    p.add_argument("capture")
    p.add_argument("--frames", help="game frames A-B of the model output to compare")
    p.add_argument("-v", "--verbose", action="store_true", help="print every window")
    p = sub.add_parser("capture")
    p.add_argument("capture")
    p.add_argument("--wav")
    a = ap.parse_args()
    rc = {"probe": cmd_probe, "engine": cmd_engine, "stream": cmd_stream, "capture": cmd_capture}[a.cmd](a)
    print("all checks passed" if not fails else f"{len(fails)} check(s) failed")
    return rc


if __name__ == "__main__":
    sys.exit(main())

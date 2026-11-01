/* Host test of the GBA sound model (src/hal_gbasnd.c) against GBATEK's figures.
 *
 *   cc -O2 -I src -DGBASND_TEST src/hal_gbasnd.c \
 *      tools/gbasnd_test.c -lm -o build/gbasnd_test && build/gbasnd_test
 *
 * Each case writes registers the way MP2K's CgbSound does, renders frames, and measures the
 * output: frequency (zero crossings), duty, level, envelope and length timing, wave volume codes,
 * noise periodicity, sweep, DirectSound routing and hold, and register side effects.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal.h"
#include "hal_gbasnd.h"

u8 gbasnd_test_io[0x400];
#define test_io gbasnd_test_io
#define R8(o) test_io[o]
#define FRAME 672
#define MAXF 400

static s16 buf[MAXF * FRAME * 2];
static int fails, checks;

static void check(int ok, const char *what, double got, double lo, double hi) {
    checks++;
    if (!ok) fails++;
    printf("  %-58s %10.3f  [%g, %g]  %s\n", what, got, lo, hi, ok ? "ok" : "FAIL");
}
#define EXPECT(what, v, lo, hi) check((v) >= (lo) && (v) <= (hi), what, (double)(v), lo, hi)

static void reset(void) {
    memset(test_io, 0, sizeof test_io);
    gbasnd_reset();
    R8(0x80) = 0x77;               /* NR50: both sides volume 7 */
    R8(0x82) = 0x02;               /* SOUNDCNT_H: PSG 100% */
    R8(0x84) = 0x80;               /* master on */
    test_io[0x88] = 0x00;          /* SOUNDBIAS = 0x200, 9-bit (the BIOS default) */
    test_io[0x89] = 0x02;
}

/* Render `frames` frames (with no DirectSound unless given) into buf; returns samples per side. */
static int render(int frames, const s8 *a, const s8 *b) {
    int f;
    for (f = 0; f < frames; f++) gbasnd_render(buf + f * FRAME * 2, FRAME, a, b, 224);
    return frames * FRAME;
}

static double freq_of(int side, int from, int n) { /* zero crossings of the DC-free signal */
    int i, cross = 0, first = -1, last = -1;
    double mean = 0;
    for (i = from; i < from + n; i++) mean += buf[2 * i + side];
    mean /= n;
    for (i = from + 1; i < from + n; i++) {
        double p = buf[2 * (i - 1) + side] - mean, q = buf[2 * i + side] - mean;
        if (p < 0 && q >= 0) {
            if (first < 0) first = i;
            last = i;
            cross++;
        }
    }
    return cross > 1 ? (cross - 1) * (double)GBASND_RATE / (last - first) : 0;
}

static double rms(int side, int from, int n) {
    double m = 0, s = 0;
    int i;
    for (i = from; i < from + n; i++) m += buf[2 * i + side];
    m /= n;
    for (i = from; i < from + n; i++) s += (buf[2 * i + side] - m) * (buf[2 * i + side] - m);
    return sqrt(s / n);
}

static double ptp(int side, int from, int n) {
    int i, lo = 32767, hi = -32768;
    for (i = from; i < from + n; i++) {
        if (buf[2 * i + side] < lo) lo = buf[2 * i + side];
        if (buf[2 * i + side] > hi) hi = buf[2 * i + side];
    }
    return hi - lo;
}

static double high_fraction(int side, int from, int n) {
    int i, hi = 0;
    double mid = 0;
    int lo_v = 32767, hi_v = -32768;
    for (i = from; i < from + n; i++) {
        if (buf[2 * i + side] < lo_v) lo_v = buf[2 * i + side];
        if (buf[2 * i + side] > hi_v) hi_v = buf[2 * i + side];
    }
    mid = (lo_v + hi_v) / 2.0;
    for (i = from; i < from + n; i++) hi += buf[2 * i + side] > mid;
    return (double)hi / n;
}

static void square2(u16 x, u8 duty, u8 nr22, u8 len, int len_en) {
    R8(0x68) = (u8)(duty << 6 | len);
    R8(0x69) = nr22;
    R8(0x6C) = (u8)x;
    R8(0x6D) = (u8)(0x80 | (len_en ? 0x40 : 0) | x >> 8);
    R8(0x81) = 0x22; /* NR51: channel 2 left and right */
}

int main(void) {
    int n, i;
    const double sec = GBASND_RATE;

    puts("square (channel 2): frequency 131072 / (2048 - x), duty, level");
    reset();
    square2(1750, 2, 0xF0, 0, 0);
    n = render(120, NULL, NULL);
    EXPECT("frequency for x = 1750 (439.84 Hz)", freq_of(0, 4000, n - 4000), 438.5, 441.2);
    EXPECT("50% duty: fraction above mid-level", high_fraction(0, 4000, n - 4000), 0.48, 0.52);
    EXPECT("level 15 at 100% PSG: RMS (+-120 DAC steps x 64 = 7680)", rms(0, 20000, n - 20000), 7400, 7900);
    EXPECT("trigger bit cleared after the frame (NR24 & 0x80)", R8(0x6D) & 0x80, 0, 0);
    reset();
    square2(1750, 0, 0xF0, 0, 0);
    n = render(120, NULL, NULL);
    EXPECT("12.5% duty: fraction above mid-level", high_fraction(0, 4000, n - 4000), 0.11, 0.14);
    reset();
    square2(1024, 2, 0xF0, 0, 0);
    n = render(60, NULL, NULL);
    EXPECT("frequency for x = 1024 (128 Hz)", freq_of(1, 4000, n - 4000), 127.5, 128.5);

    puts("mixer levels and routing");
    reset();
    square2(1750, 2, 0xF0, 0, 0);
    R8(0x81) = 0x02; /* right only */
    n = render(60, NULL, NULL);
    EXPECT("NR51 right only: left RMS", rms(0, 8000, n - 8000), 0, 1);
    EXPECT("NR51 right only: right RMS", rms(1, 8000, n - 8000), 6500, 8000);
    reset();
    R8(0x82) = 0x01; /* PSG 50% */
    square2(1750, 2, 0xF0, 0, 0);
    n = render(60, NULL, NULL);
    EXPECT("SOUNDCNT_H PSG 50%: RMS halves (3840)", rms(0, 20000, n - 20000), 3700, 3950);
    reset();
    R8(0x80) = 0x33; /* NR50 volume 3 of 7: x4 instead of x8 */
    square2(1750, 2, 0xF0, 0, 0);
    n = render(60, NULL, NULL);
    EXPECT("NR50 volume 3 of 7: RMS halves (3840)", rms(0, 20000, n - 20000), 3700, 3950);
    reset();
    R8(0x84) = 0x00;
    square2(1750, 2, 0xF0, 0, 0);
    n = render(30, NULL, NULL);
    EXPECT("SOUNDCNT_X master off: RMS", rms(0, 0, n), 0, 0.5);

    puts("envelope (64 Hz steps) and length (256 Hz)");
    reset();
    square2(1750, 2, 0xF1, 0, 0); /* 15, decreasing every 1/64 s: silent after 15/64 s */
    n = render(30, NULL, NULL);
    EXPECT("level 15 decreasing, step 1: RMS at 0.10-0.12 s (vol ~8)", rms(0, (int)(0.10 * sec), (int)(0.02 * sec)), 2800, 4800);
    EXPECT("level 15 decreasing, step 1: RMS at 0.25-0.40 s", rms(0, (int)(0.25 * sec), (int)(0.15 * sec)), 0, 60);
    reset();
    square2(1750, 2, 0x0F, 0, 0); /* 0, increasing every 7/64 s */
    n = render(120, NULL, NULL);
    EXPECT("level 0 increasing, step 7: RMS at 0-0.1 s", rms(0, 0, (int)(0.1 * sec)), 0, 60);
    EXPECT("level 0 increasing, step 7: RMS at 1.6-1.9 s (level 15)", rms(0, (int)(1.65 * sec), (int)(0.2 * sec)), 6500, 8000);
    reset();
    square2(1750, 2, 0xF0, 32, 1); /* length 64 - 32 = 32 / 256 s = 125 ms */
    n = render(30, NULL, NULL);
    EXPECT("length 32: RMS at 0.05-0.10 s", rms(0, (int)(0.05 * sec), (int)(0.05 * sec)), 6000, 8000);
    EXPECT("length 32: RMS at 0.14-0.30 s", rms(0, (int)(0.14 * sec), (int)(0.16 * sec)), 0, 200);

    puts("wave (channel 3)");
    reset();
    for (i = 0; i < 16; i++) { /* triangle: steps 0..15, 15..0 */
        int s0 = 2 * i < 16 ? 2 * i : 31 - 2 * i, s1 = 2 * i + 1 < 16 ? 2 * i + 1 : 30 - 2 * i;
        test_io[0x90 + i] = (u8)(s0 << 4 | s1);
    }
    R8(0x70) = 0x80;         /* DAC on */
    R8(0x72) = 0;            /* length */
    R8(0x73) = 0x20;         /* 100% */
    R8(0x74) = (u8)1750;
    R8(0x75) = (u8)(0x80 | 1750 >> 8);
    R8(0x81) = 0x44;
    n = render(60, NULL, NULL);
    EXPECT("frequency 65536 / (2048 - 1750) = 219.9 Hz", freq_of(0, 4000, n - 4000), 218.5, 221.5);
    {
        double full = ptp(0, 20000, n - 20000);
        R8(0x73) = 0x40; /* 50% */
        n = render(60, NULL, NULL);
        EXPECT("volume code 2 (50%) / 100% peak-to-peak", ptp(0, 20000, n - 20000) / full, 0.45, 0.55);
        R8(0x73) = 0x80; /* forced 75% */
        n = render(60, NULL, NULL);
        EXPECT("NR32 bit 7 (75%) / 100% peak-to-peak", ptp(0, 20000, n - 20000) / full, 0.70, 0.80);
        R8(0x70) = 0x00; /* DAC off */
        n = render(10, NULL, NULL);
        EXPECT("NR30 DAC off: RMS", rms(0, 1000, n - 1000), 0, 200);
    }

    puts("noise (channel 4)");
    reset();
    R8(0x79) = 0xF0;
    R8(0x7C) = 0x7B; /* s = 7, r = 3, 7-bit: 524288 / 3 / 256 = 682.7 steps/s, period 127 steps */
    R8(0x7D) = 0x80;
    R8(0x81) = 0x88;
    n = render(240, NULL, NULL);
    {
        /* autocorrelation peak near 127 steps = 7494 samples */
        int lag, best = 0;
        double bestv = -1e30;
        for (lag = 7000; lag < 8000; lag += 2) {
            double s = 0;
            for (i = 20000; i < 20000 + 30000; i += 3) s += (double)buf[2 * i] * buf[2 * (i + lag)];
            if (s > bestv) bestv = s, best = lag;
        }
        EXPECT("7-bit LFSR period (samples, 127 steps = 7494)", best, 7440, 7550);
    }
    EXPECT("noise level: RMS", rms(0, 20000, 40000), 3000, 9000);

    puts("sweep (channel 1)");
    reset();
    R8(0x60) = 0x11; /* period 1/128 s, increasing, shift 1 */
    R8(0x62) = 0x80;
    R8(0x63) = 0xF0;
    R8(0x64) = 0x00;
    R8(0x65) = 0x80 | 4; /* x = 1024 */
    R8(0x81) = 0x11;
    n = render(3, NULL, NULL);
    EXPECT("swept frequency written back to NR13/14 (1536)", R8(0x64) | (R8(0x65) & 7) << 8, 1536, 1536);
    EXPECT("overflow (1536 + 768 > 2047) silences the channel: RMS after 20 ms", rms(0, (int)(0.02 * sec), n - (int)(0.02 * sec)), 0, 300);

    puts("DirectSound");
    {
        s8 a[224], b[224];
        for (i = 0; i < 224; i++) a[i] = (i & 4) ? 64 : -64, b[i] = 0;
        reset();
        R8(0x82) = 0x0E;
        R8(0x83) = 0x31; /* SOUNDCNT_H high byte: A right (bit 8), B left (bit 13), FIFO resets */
        test_io[0x83] = 0x21;
        n = render(30, a, b);
        EXPECT("A routed right: RMS (+-64 x 4 DAC steps x 64 = 16384)", rms(1, 8000, n - 8000), 15800, 16700);
        EXPECT("A not routed left: left RMS", rms(0, 8000, n - 8000), 0, 1);
        R8(0x82) = 0x0A; /* A at 50% */
        n = render(30, a, b);
        EXPECT("A at 50%: RMS", rms(1, 8000, n - 8000), 7900, 8400);
        R8(0x82) = 0x0E;
        for (i = 0; i < 224; i++) a[i] = (i & 1) ? 64 : -64; /* alternate every DirectSound sample */
        n = render(30, a, b);
        {
            int held = 1, m;
            for (m = 3000; m < 3000 + 600; m++) {
                int s0 = buf[2 * (3 * m) + 1] > 0, s1 = buf[2 * (3 * m + 1) + 1] > 0, s2 = buf[2 * (3 * m + 2) + 1] > 0;
                int s3 = buf[2 * (3 * m + 3) + 1] > 0;
                held &= s0 == s1 && s1 == s2 && s2 != s3;
            }
            EXPECT("each DirectSound sample is held for 3 output samples", held, 1, 1);
        }
        n = render(10, NULL, b);
        EXPECT("DMA A stopped: right RMS", rms(1, 3000, n - 3000), 0, 300);
    }

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}

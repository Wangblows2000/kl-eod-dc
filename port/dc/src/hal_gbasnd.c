/* GBA sound hardware model: the four Game Boy PSG channels and the DirectSound/PSG mixer with the
 * GBA's DAC, driven by the I/O registers the game (MP2K's CgbSound) leaves behind each frame.
 *
 *   - A write of NRx4 bit 7 is a trigger. The registers are read once per frame, after CgbSound,
 *     and the bit is cleared again (it reads back as 0 on hardware). MP2K writes every channel's
 *     registers at most once per frame, so the frame is the resolution it needs.
 *   - Square, wave and noise follow GBATEK: 131072 / (2048 - f) Hz squares with 12.5/25/50/75%
 *     duty, the 32-step wave at 65536 / (2048 - f) Hz with 0/25/50/75/100% volume, the 15- or
 *     7-bit LFSR at 524288 / r / 2^(s+1) Hz, the 512 Hz frame sequencer (length 256 Hz, sweep
 *     128 Hz, envelope 64 Hz), and channel 1's sweep, which writes its frequency back to NR13/14.
 *   - Each output sample is the average of the waveform over its interval (area sampling), so high
 *     tones alias far less than point sampling at 40 kHz would.
 *   - Levels follow mGBA's GBA model: each channel 0..15, summed per side by NR51, << 3 and
 *     x (1 + NR50 volume), then >> (4 - SOUNDCNT_H ratio); DirectSound samples << 2 at 100% or
 *     << 1 at 50%, routed by SOUNDCNT_H; the sum gets the SOUNDBIAS bias, is clipped to 10 bits and
 *     reduced to the SOUNDBIAS resolution, and then AC-coupled (a ~25 Hz DC blocker).
 *
 * No KallistiOS dependency: tools/gbasnd_test.c runs this file on the host against GBATEK's
 * numbers (built with GBASND_TEST, which swaps the I/O page for an array).
 */
#include <string.h>

#include "hal.h"
#include "hal_gbasnd.h"

#if !defined(HAL_AUDIO) || HAL_AUDIO

#ifdef GBASND_TEST /* host test (tools/gbasnd_test.c): a plain array stands in for the I/O page */
extern u8 gbasnd_test_io[];
#define GBASND_IO ((uintptr_t)gbasnd_test_io)
#else
#define GBASND_IO GBA_IO
#endif
#undef IO16
#define IO8(off) (*(volatile u8 *)(GBASND_IO + (off)))
#define IO16(off) (*(volatile u16 *)(GBASND_IO + (off)))

u32 gbasnd_triggers;

#ifdef HAL_AUDIO_TONE
static u32 tone_phase;
#endif

/* ---- PSG (Game Boy channels 1-4, GBA wiring) --------------------------------------------- */

struct envelope {
    int vol, dir, period, timer;
};

struct psg_ch {
    int on;
    u32 phase, inc;  /* square/wave: position in the waveform, 2^32 = one period */
    u32 recip;       /* 2^40 / inc */
    int inc_over;    /* frequency at or above the output rate */
    int duty;        /* square: high part of the period, eighths */
    int length, len_en;
    struct envelope env;
    u16 freq, seen_freq; /* 11-bit period register; last value seen in the I/O registers */
    /* channel 1 sweep */
    int sweep_period, sweep_timer, sweep_dir, sweep_shift, sweep_en;
    u16 shadow;
    /* channel 3 */
    u8 wave[32];
    int wave_sum[33];
    /* channel 4 */
    u16 lfsr;
    u32 noise_acc, noise_inc, noise_recip; /* LFSR steps, 16.16; 2^32 / noise_inc */
    u8 noise_reg;
};

static struct psg_ch psg[4];
static u32 seq_acc; /* frame sequencer phase, 512 Hz: 2^32 per step */
static int seq_step;
static s32 hp_l, hp_r, hp_xl, hp_xr; /* output DC blocker (the GBA's AC coupling) */

static const int duty_eighths[4] = {1, 2, 4, 6};

/* Per-output-sample phase increment for a channel period register f, 2^32 = one waveform period;
 * inc_over when the waveform repeats faster than the output rate (inc would not fit). */
static void set_inc(struct psg_ch *c, u32 inc);
static void set_freq(struct psg_ch *c, u16 f, int wave) {
    u64 inc = ((wave ? 65536ull : 131072ull) << 32) / (2048 - f) / GBASND_RATE;
    c->inc_over = inc >= (1ull << 32);
    set_inc(c, c->inc_over ? 0xFFFFFFFFu : (u32)inc);
}

static void noise_setup(struct psg_ch *c, u8 nr43) {
    int s = nr43 >> 4, r = nr43 & 7;
    /* LFSR clock = 524288 / r / 2^(s+1) Hz, with r = 0 counting as 0.5 */
    u64 hz16 = r ? (524288ull << 16) / r : (1048576ull << 16);
    hz16 >>= s + 1;
    c->noise_inc = (u32)(hz16 / GBASND_RATE);
    c->noise_recip = c->noise_inc ? (u32)((1ull << 32) / c->noise_inc) : 0;
    c->noise_reg = nr43;
}

static void env_trigger(struct envelope *e, u8 nrx2) {
    e->vol = nrx2 >> 4;
    e->dir = (nrx2 & 8) != 0;
    e->period = nrx2 & 7;
    e->timer = e->period ? e->period : 8;
}

static void env_tick(struct envelope *e) {
    if (!e->period) return;
    if (--e->timer > 0) return;
    e->timer = e->period;
    if (e->dir && e->vol < 15) e->vol++;
    else if (!e->dir && e->vol > 0) e->vol--;
}

static int sweep_calc(struct psg_ch *c) {
    int d = c->shadow >> c->sweep_shift;
    int f = c->sweep_dir ? c->shadow - d : c->shadow + d;
    if (f > 2047) c->on = 0;
    return f;
}

static void write_back_freq(int ch, u16 f) {
    static const u8 lo[3] = {0x64, 0x6C, 0x74};
    IO8(lo[ch]) = (u8)f;
    IO8(lo[ch] + 1) = (u8)((IO8(lo[ch] + 1) & 0x40) | (f >> 8));
    psg[ch].seen_freq = f;
}

/* Frame sequencer: length 256 Hz, sweep 128 Hz, envelope 64 Hz. */
static void seq_tick(void) {
    int i;
    seq_step = (seq_step + 1) & 7;
    if (!(seq_step & 1))
        for (i = 0; i < 4; i++)
            if (psg[i].len_en && psg[i].length > 0 && --psg[i].length == 0) psg[i].on = 0;
    if (seq_step == 2 || seq_step == 6) {
        struct psg_ch *c = &psg[0];
        if (c->sweep_timer > 0 && --c->sweep_timer == 0) {
            c->sweep_timer = c->sweep_period ? c->sweep_period : 8;
            if (c->sweep_en && c->sweep_period) {
                int f = sweep_calc(c);
                if (f <= 2047 && c->sweep_shift) {
                    c->shadow = (u16)f;
                    c->freq = (u16)f;
                    set_freq(c, c->freq, 0);
                    write_back_freq(0, c->freq);
                    sweep_calc(c);
                }
            }
        }
    }
    if (seq_step == 7)
        for (i = 0; i < 4; i++)
            if (i != 2) env_tick(&psg[i].env);
}

/* Read the PSG registers once per frame (after CgbSound): triggers, frequency, wave pattern. */
static void psg_poll(void) {
    /* NRx1, NRx2, NRx3, NRx4 of each channel (NR3x: NR31 length, NR32 volume, NR33/34 frequency) */
    static const u8 reg[4][4] = {{0x62, 0x63, 0x64, 0x65}, {0x68, 0x69, 0x6C, 0x6D}, {0x72, 0x73, 0x74, 0x75},
                                 {0x78, 0x79, 0x7C, 0x7D}};
    int ch;
    for (ch = 0; ch < 4; ch++) {
        struct psg_ch *c = &psg[ch];
        u8 nrx1 = IO8(reg[ch][0]), nrx2 = IO8(reg[ch][1]), nrx3 = IO8(reg[ch][2]), nrx4 = IO8(reg[ch][3]);
        if (ch < 3) {
            u16 f = (u16)(nrx3 | (nrx4 & 7) << 8);
            if (f != c->seen_freq) {
                c->seen_freq = f;
                c->freq = f;
                set_freq(c, f, ch == 2);
            }
        } else if (nrx3 != c->noise_reg) {
            noise_setup(c, nrx3);
        }
        c->len_en = (nrx4 & 0x40) != 0;
        if (nrx4 & 0x80) {
            IO8(reg[ch][3]) = nrx4 & 0x7F; /* the trigger bit reads back as 0 */
            gbasnd_triggers++;
            c->on = 1;
            if (ch == 2) {
                int i;
                const volatile u8 *w = &IO8(0x90);
                c->wave_sum[0] = 0;
                for (i = 0; i < 16; i++) {
                    c->wave[2 * i] = w[i] >> 4;
                    c->wave[2 * i + 1] = w[i] & 15;
                }
                for (i = 0; i < 32; i++) c->wave_sum[i + 1] = c->wave_sum[i] + c->wave[i];
                c->phase = 0;
                c->length = 256 - nrx1;
            } else {
                env_trigger(&c->env, nrx2);
                c->length = 64 - (nrx1 & 63);
                if (ch < 2) c->duty = duty_eighths[nrx1 >> 6];
                if (ch == 3) {
                    c->lfsr = 0x7FFF;
                    c->noise_acc = 0;
                    noise_setup(c, nrx3);
                }
                if (ch == 0) {
                    u8 nr10 = IO8(0x60);
                    c->sweep_period = (nr10 >> 4) & 7;
                    c->sweep_dir = (nr10 & 8) != 0;
                    c->sweep_shift = nr10 & 7;
                    c->shadow = c->freq;
                    c->sweep_timer = c->sweep_period ? c->sweep_period : 8;
                    c->sweep_en = c->sweep_period || c->sweep_shift;
                    if (c->sweep_shift) sweep_calc(c);
                }
            }
        }
        /* DAC off silences the channel: NR30 bit 7, or NRx2 upper 5 bits all zero. */
        if (ch == 2 ? !(IO8(0x70) & 0x80) : !(nrx2 & 0xF8)) c->on = 0;
    }
}

/* The channels are area-sampled: each output sample is the average of the waveform over its
 * 1/GBASND_RATE interval, which keeps high tones from aliasing badly. Every average needs a division
 * by the per-sample increment; it is a multiplication by a reciprocal set whenever the frequency
 * changes (set_inc), and the common case -- no edge inside the interval -- needs none. */

static void set_inc(struct psg_ch *c, u32 inc) {
    c->inc = inc;
    c->recip = inc ? (u32)((1ull << 40) / inc) : 0; /* 2^40 / inc: area * recip >> 32 = 256 * area / inc */
}

/* Square: high for duty/8 of the period; 0..256 */
static int square_avg(struct psg_ch *c) {
    u32 p = c->phase, inc = c->inc, e = p + inc, hi = (u32)c->duty << 29, area;
    c->phase = e;
    if (c->inc_over) return c->duty * 32; /* above the output rate: its average */
    if (e >= p) { /* no wrap */
        if (e <= hi) return 256;
        if (p >= hi) return 0;
        area = hi - p;
    } else {
        area = (p < hi ? hi - p : 0) + (e < hi ? e : hi);
    }
    return (int)(((u64)area * c->recip) >> 32);
}

/* Wave: 32 4-bit steps of 2^27 each; 0..15 x 256 */
static u64 wave_cum(const struct psg_ch *c, u32 x) {
    u32 st = x >> 27;
    return ((u64)c->wave_sum[st] << 27) + (u64)(x & 0x7FFFFFF) * c->wave[st];
}

static int wave_avg(struct psg_ch *c) {
    u32 p = c->phase, inc = c->inc, e = p + inc;
    u64 area;
    c->phase = e;
    if (e >= p && (p >> 27) == ((e - 1) >> 27)) return c->wave[p >> 27] << 8;
    if (c->inc_over) return (c->wave_sum[32] << 8) / 32;
    area = e >= p ? wave_cum(c, e) - wave_cum(c, p) : ((u64)c->wave_sum[32] << 27) - wave_cum(c, p) + wave_cum(c, e);
    return (int)(((u64)(u32)(area >> 4) * c->recip) >> 28);
}

static int noise_step(struct psg_ch *c) {
    int x = (c->lfsr ^ (c->lfsr >> 1)) & 1;
    c->lfsr = (u16)((c->lfsr >> 1) | (x << 14));
    if (c->noise_reg & 8) c->lfsr = (u16)((c->lfsr & ~0x40) | (x << 6));
    return !(c->lfsr & 1);
}

/* Noise: LFSR output averaged over the sample, 0..256. noise_acc/noise_inc are LFSR steps in
 * 16.16; noise_recip = 2^32 / noise_inc turns a distance in steps into a fraction of the sample. */
static int noise_avg(struct psg_ch *c) {
    u32 acc = c->noise_acc, inc = c->noise_inc, left = 1u << 16, sum = 0;
    int out = !(c->lfsr & 1), guard = 64;
    if ((acc & 0xFFFF) + inc < 0x10000) { /* no LFSR step inside this sample */
        c->noise_acc = acc + inc;
        return out << 8;
    }
    while (left && guard--) {
        u32 to_step = 0x10000 - (acc & 0xFFFF);
        u32 span = (u32)(((u64)to_step * c->noise_recip) >> 16);
        if (span >= left) {
            sum += (u32)out * left;
            acc += (u32)(((u64)left * inc) >> 16);
            break;
        }
        if (!span) span = 1;
        sum += (u32)out * span;
        left -= span;
        acc = (acc | 0xFFFF) + 1;
        out = noise_step(c);
    }
    c->noise_acc = acc;
    return (int)(sum >> 8);
}

/* Render samples [i0, i1) of every sounding PSG channel into the left/right accumulators
 * (units: 4-bit level x 256, summed over the channels routed to that side by NR51). */
static s32 mix_l[GBASND_MAX_SAMPLES], mix_r[GBASND_MAX_SAMPLES];

static void psg_render(int i0, int i1, u8 nr51, u8 nr32) {
    int ch, i;
    for (ch = 0; ch < 4; ch++) {
        struct psg_ch *c = &psg[ch];
        int to_l = nr51 & (0x10 << ch), to_r = nr51 & (1 << ch), vol, shift = 0, mul = 1;
        if (!c->on) continue;
        vol = ch == 2 ? 1 : c->env.vol;
        if (ch == 2) {
            int code = (nr32 >> 5) & 3;
            if (nr32 & 0x80) mul = 3, shift = 2; /* 75% */
            else if (code) shift = code - 1;     /* 100 / 50 / 25% */
            else vol = 0;
        }
        if (!vol || !(to_l || to_r)) { /* silent: keep the oscillator running */
            if (ch < 3) c->phase += c->inc * (u32)(i1 - i0);
            continue;
        }
        for (i = i0; i < i1; i++) {
            s32 v;
            if (ch < 2) v = square_avg(c) * vol;
            else if (ch == 2) v = (wave_avg(c) * mul) >> shift;
            else v = noise_avg(c) * vol;
            if (to_l) mix_l[i] += v;
            if (to_r) mix_r[i] += v;
        }
    }
}

static int psg_any_on(void) { return psg[0].on | psg[1].on | psg[2].on | psg[3].on; }

/* ---- mixing -------------------------------------------------------------------------------- */

/* Render n stereo samples (interleaved L, R) of one frame: the DirectSound segments a (FIFO A) and
 * b (FIFO B), spf samples each or NULL when that DMA is stopped, held over the n output samples,
 * plus the PSG as its registers stand. Returns the peak absolute output sample. */
int gbasnd_render(s16 *out, int n, const s8 *seg_a, const s8 *seg_b, int spf) {
    u16 cnt_h = IO16(0x82);
    u8 nr50 = IO8(0x80), nr51 = IO8(0x81), master = IO8(0x84) & 0x80;
    int psg_shift = 4 - ((cnt_h & 3) == 3 ? 2 : (cnt_h & 3));
    int i;
    s32 peak = 0;
    if (n > GBASND_MAX_SAMPLES) n = GBASND_MAX_SAMPLES;
    if (spf > GBASND_MAX_SEG) spf = GBASND_MAX_SEG;
    psg_poll();
    {
        u16 sb = IO16(0x88);
        s32 bias = sb & 0x3FF, keep = ~((1 << (((sb >> 14) & 3) + 1)) - 1);
        u8 nr32 = IO8(0x73);
        s32 kl = 8 * (1 + ((nr50 >> 4) & 7)), kr = 8 * (1 + (nr50 & 7));
        int shift = 8 + psg_shift, psg_on = psg_any_on();
        s8 a[GBASND_MAX_SEG], b[GBASND_MAX_SEG];
        int a_r = seg_a && (cnt_h & 0x100), a_l = seg_a && (cnt_h & 0x200), b_r = seg_b && (cnt_h & 0x1000),
            b_l = seg_b && (cnt_h & 0x2000), a_sh = (cnt_h & 4) ? 2 : 1, b_sh = (cnt_h & 8) ? 2 : 1;
        u32 kacc = 0, kstep = n ? (((u32)spf << 16) + (u32)n - 1) / (u32)n : 0; /* rounded up: sample k spans [3k, 3k+3) */
        const u32 seq_inc = (u32)((512ull << 32) / GBASND_RATE);

        if (seg_a) memcpy(a, seg_a, spf);
        if (seg_b) memcpy(b, seg_b, spf);

        /* PSG, in spans between frame-sequencer ticks (a tick never turns a channel on) */
        if (psg_on) {
            memset(mix_l, 0, n * sizeof mix_l[0]);
            memset(mix_r, 0, n * sizeof mix_r[0]);
        }
        i = 0;
        while (i < n) {
            u32 to_tick = (u32)(((1ull << 32) - seq_acc + seq_inc - 1) / seq_inc); /* increments until a tick */
            u32 span = to_tick - 1;
            if (span > (u32)(n - i)) span = (u32)(n - i);
            if (span) {
                if (psg_on) psg_render(i, i + (int)span, nr51, nr32);
                seq_acc += span * seq_inc;
                i += (int)span;
            }
            if (i >= n) break;
            seq_acc += seq_inc; /* wraps: this sample starts after a tick */
            seq_tick();
            if (psg_on) psg_render(i, i + 1, nr51, nr32);
            i++;
        }

        for (i = 0; i < n; i++, kacc += kstep) {
            int k = kacc >> 16;
            s32 l = 0, r = 0, vl, vr;
            if (psg_on) {
                l = (mix_l[i] * kl) >> shift;
                r = (mix_r[i] * kr) >> shift;
            }
            if (a_r) r += a[k] << a_sh;
            if (a_l) l += a[k] << a_sh;
            if (b_r) r += b[k] << b_sh;
            if (b_l) l += b[k] << b_sh;
            if (!master) l = r = 0;
#ifdef HAL_AUDIO_TONE
            tone_phase += (u32)((440ull << 32) / GBASND_RATE); /* test build: 440 Hz square at 1/4 scale */
            l += (tone_phase >> 31) ? 128 : -128;
            r += (tone_phase >> 31) ? 128 : -128;
#endif
            /* GBA DAC: bias, clip to 10 bits, SOUNDBIAS resolution; then AC coupling (~25 Hz) */
            l += bias;
            r += bias;
            l = l < 0 ? 0 : l > 0x3FF ? 0x3FF : l;
            r = r < 0 ? 0 : r > 0x3FF ? 0x3FF : r;
            l = ((l & keep) - bias) << 8;
            r = ((r & keep) - bias) << 8;
            hp_l = l - hp_xl + hp_l - (hp_l >> 8);
            hp_xl = l;
            hp_r = r - hp_xr + hp_r - (hp_r >> 8);
            hp_xr = r;
            vl = hp_l >> 2;
            vr = hp_r >> 2;
            vl = vl > 32767 ? 32767 : vl < -32768 ? -32768 : vl;
            vr = vr > 32767 ? 32767 : vr < -32768 ? -32768 : vr;
            if (vl > peak || -vl > peak) peak = vl < 0 ? -vl : vl;
            if (vr > peak || -vr > peak) peak = vr < 0 ? -vr : vr;
            out[2 * i] = (s16)vl;
            out[2 * i + 1] = (s16)vr;
        }
    }
    return peak;
}

void gbasnd_reset(void) {
    memset(psg, 0, sizeof psg);
    psg[3].lfsr = 0x7FFF;
    seq_acc = 0;
    seq_step = 0;
    hp_l = hp_r = hp_xl = hp_xr = 0;
}

#endif

/* Audio output: the GBA's DMA sound playback, and the AICA stream.
 *
 * The game's MP2K engine (kleod src/m4a.c plus hal_m4a.c) does what it does on the GBA: once per
 * frame it runs the sequencer, writes the PSG registers (CgbSound) and mixes DirectSound into
 * gSoundInfo.pcmBuffer, a ring of framesPerDmaCycle segments of samplesPerFrame 8-bit samples
 * (first half FIFO A, second half FIFO B). On the GBA, DMA1/DMA2 then play one segment per frame
 * and the PSG plays whatever its registers say.
 *
 * hal_audio_frame() runs once per emulated frame, right after the VBlank handler. It picks the
 * pcmBuffer segment DMA would be playing this frame (none while DMA1/2 are stopped, as during
 * m4aSoundVSyncOff), has hal_gbasnd.c render the frame -- that segment plus the PSG, mixed as the
 * GBA mixes them -- and appends the result, 3 x 224 stereo samples, to a ring.
 *
 * Output: one 16-bit stereo KallistiOS snd_stream at 3 x 224 samples per Dreamcast frame
 * (~40.28 kHz). A dedicated thread polls it; the stream callback copies from the ring. The ring
 * is single-producer (game thread, once per frame) / single-consumer (poll thread): each side
 * only writes its own index, after the data it covers. The callback splits the two channels and
 * writes them into sound RAM itself (stream_cb). On an underrun the callback still returns
 * everything it was asked for, fading the last sample to silence, so the AICA never replays stale
 * buffer contents; the producer then
 * refills the ring to its target with silence rather than speeding up. A small trim of the block
 * length (at most 4 samples a frame, 0.6%) keeps the ring near its target and absorbs drift
 * between the Dreamcast's video and audio clocks.
 *
 * Everything here is compiled out with AUDIO=0 (HAL_AUDIO 0): the port is then silent, exactly
 * as before audio existed.
 */
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/spu.h>
#include <kos/thread.h>
#include <string.h>

#include "hal.h"
#include "hal_gbasnd.h"

#if HAL_AUDIO

#define ID_NUMBER 0x68736D53u

#define SEG_MAX 224          /* samplesPerFrame at 13379 Hz, the rate Klonoa uses */
#define OUT_MULT 3           /* output samples per DirectSound sample */
#define OUT_PER_FRAME (OUT_MULT * SEG_MAX)
#define OUT_RATE GBASND_RATE /* OUT_PER_FRAME x 59.94 Hz */
#define TRIM_MAX 4           /* +-samples per frame the drift trim may add or remove */
#define RING_FRAMES 8192     /* stereo frames; power of two */
#define TARGET_FILL 1600     /* ring fill the trim aims for, in stereo frames */
#define AICA_BUF 4096        /* bytes per channel of the AICA-side stream buffer (2048 samples) */
#define POLL_MS 5

/* Minimal view of the game's SoundMixerState (kleod include/gba/m4a_internal.h). */
struct mixer_view {
    u32 lockStatus;
    u8 dmaCounter, reverb, numChans, masterVol, freqOption, extensionFlags, cgbCounter15, framesPerDmaCycle;
    u8 maxScanlines, gap[3];
    s32 samplesPerFrame;
};
#define SOUND_INFO (*(struct mixer_view *volatile *)(GBA_IWRAM + 0x7FF0))
#define PCM_BUFFER_OFFSET 0x350
#define PCM_DMA_BUF_SIZE 1584

struct hal_audio_stats hal_audio_stats;

/* ---- ring (game thread -> poll thread) --------------------------------------------------- */

static s16 ring[RING_FRAMES * 2] __attribute__((aligned(32)));
static volatile u32 ring_wr, ring_rd; /* stereo frames written / read, free-running */
/* One request: up to AICA_BUF/2 samples per channel, split here into the two channel buffers. */
static s16 cb_l[AICA_BUF / 2] __attribute__((aligned(32))), cb_r[AICA_BUF / 2] __attribute__((aligned(32)));
static volatile int produced_any, underrun_pending, last_block_loud, underrun_to_log;
static s16 last_l, last_r;
static snd_stream_hnd_t stream = SND_STREAM_INVALID;

/* The stream callback, in KallistiOS's "direct" form: it is given the two channel buffers in sound
 * RAM and writes them itself. The port does not use the ordinary form (interleaved L,R handed back
 * to KallistiOS), because KallistiOS v2.3.0 splits interleaved 16-bit stereo with
 * snd_pcm16_split (snd_pcm_split.s), and under the SH-4's XTRCT semantics that routine puts the
 * right samples in the left buffer and vice versa, and swaps every pair of samples in time: a 1 kHz
 * tone came out on the wrong side with sidebands at fs/2 +- 1 kHz, and music lost its treble to a
 * mirrored copy of itself (fs - f) -- the "crunchy, bassy" sound. See docs/audio.md. */
static size_t stream_cb(snd_stream_hnd_t hnd, uintptr_t left, uintptr_t right, size_t size_req) {
    u32 want = (u32)size_req / 4, avail, n, i, rd = ring_rd; /* size_req: bytes for both channels */
    (void)hnd;
    if (want > AICA_BUF / 2) want = AICA_BUF / 2;
    avail = ring_wr - rd;
    n = avail < want ? avail : want;
    for (i = 0; i < n; i++) {
        u32 k = ((rd + i) & (RING_FRAMES - 1)) * 2;
        cb_l[i] = ring[k];
        cb_r[i] = ring[k + 1];
    }
    if (n) {
        last_l = cb_l[n - 1];
        last_r = cb_r[n - 1];
    }
    __asm__ volatile("" ::: "memory");
    ring_rd = rd + n;
    if (n < want) {
        /* Underrun: pad with a short fade from the last sample to silence. */
        for (i = n; i < want; i++) {
            last_l = (s16)(last_l - (last_l >> 5) - (last_l > 0) + (last_l < 0));
            last_r = (s16)(last_r - (last_r >> 5) - (last_r > 0) + (last_r < 0));
            cb_l[i] = last_l;
            cb_r[i] = last_r;
        }
        if (produced_any) {
            if (!underrun_pending) { /* count gaps, not the callbacks inside one gap */
                hal_audio_stats.underruns++;
                if (last_block_loud) hal_audio_stats.underruns_audible++;
            }
            hal_audio_stats.underrun_samples += want - n;
            hal_audio_stats.last_underrun_frame = hal_frame_count;
            underrun_pending = 1;
        }
    }
    /* Plain G2 writes (spu_memload): the store-queue copy waits on sq_lock while the renderer
     * uploads textures, which held this thread up for milliseconds at a time. left/right carry the
     * uncached-area base; spu_memload wants sound-RAM offsets. */
    {
        u64 t0 = hal_now_us();
        spu_memload(left & 0x1FFFFF, cb_l, want * 2);
        spu_memload(right & 0x1FFFFF, cb_r, want * 2);
        hal_audio_stats.spu_us += (u32)(hal_now_us() - t0);
    }
    return want * 4;
}

/* ---- keeping the sound in real time ----------------------------------------------------- *
 *
 * The sound engine advances once per emulated frame, and an emulated frame lasts as long as the
 * game takes over it. When a frame takes longer than 1/60 s on the Dreamcast -- a heavy scene, a
 * scene load -- that alone would leave the music behind and the ring dry. The GBA does not have
 * this problem: its VBlank interrupt keeps firing while the main loop is busy, and the game's
 * VBlank handlers run the sound engine themselves on such lag frames
 * (VBlankIntr_Common: `if (!gFrameFinished) m4aSoundMain()`).
 *
 * The port does the same. next_due_ns is when the next sound frame is due. Whenever real time has
 * passed it, a "lag frame" runs: SoundMain, m4aSoundVSync and one block, the order the GBA's
 * handler and DMA give. Two places run them:
 *   - the poll thread, every few milliseconds, once a frame is half a frame overdue -- so the
 *     music keeps going *during* a long frame.
 *     It has the higher priority, so like the GBA's interrupt it runs to completion while the game
 *     thread waits. It stays out while the game thread is inside the VBlank handler or its own
 *     audio frame (hal_audio_busy), or inside an MP2K call (the engine's lock word, which the
 *     engine already checks against interrupts on the GBA);
 *   - the game thread after its own frame, for anything the poll thread could not do.
 * A lag frame's block is queued only while the ring is below target plus one frame; otherwise it is
 * dropped (the engine still advances, so the music stays in time without adding latency). More
 * than CATCHUP_MAX frames behind -- a multi-second stall -- it resynchronises instead. The game
 * never reads sound state, so none of this changes game logic or the frame-indexed tests. */
#define FRAME_NS 16683350ull /* 1 / 59.94 Hz */
#define CATCHUP_MAX 8
void SoundMain(void);
void m4aSoundVSync(void);
static u64 next_due_ns;
volatile int hal_audio_busy;
static void produce(int keep);

static void run_lag_frames(u64 now, u32 *counter) {
    int n = 0;
    while (now >= next_due_ns && n++ < CATCHUP_MAX) {
        struct mixer_view *si = SOUND_INFO;
        if (!si || si->lockStatus != ID_NUMBER) return; /* not initialised, or inside the engine */
        SoundMain();
        m4aSoundVSync();
        produce((int)(ring_wr - ring_rd) < TARGET_FILL + OUT_PER_FRAME);
        (*counter)++;
        next_due_ns += FRAME_NS;
    }
}

static void *poll_thread(void *arg) {
    u64 last = 0;
    (void)arg;
    for (;;) {
        u64 t0 = hal_now_us();
        if (last && t0 - last > hal_audio_stats.max_poll_gap_us) hal_audio_stats.max_poll_gap_us = (u32)(t0 - last);
        last = t0;
        snd_stream_poll(stream);
        /* Half a frame of slack: an on-time game frame that arrives a little late is not a lag frame. */
        if (!hal_audio_busy && next_due_ns && hal_now_us() * 1000ull >= next_due_ns + FRAME_NS / 2)
            run_lag_frames(hal_now_us() * 1000ull - FRAME_NS / 2, &hal_audio_stats.lag_frames_thread);
        hal_audio_stats.poll_us += (u32)(hal_now_us() - t0);
        thd_sleep(POLL_MS);
    }
    return NULL;
}

/* ---- per-frame mix ------------------------------------------------------------------------- */

static int trim_ema = TARGET_FILL << 4;


/* Test aid: count "gaps inside sound" in what is produced, by the same rule as tools/audiocheck.py
 * applies to an emulator capture (>= 4 ms below 64 with > 1024 within 50 ms before and after). If
 * the two counts agree, the gaps a capture shows are in the game's sound itself, not made by the
 * ring or the stream. */
#ifdef HAL_AUDIO_CHECK
static void count_quiet_runs(const s16 *b, int n) {
    static int quiet_len, since_loud = 1 << 30, pending_wait = -1;
    const int min_len = GBASND_RATE * 4 / 1000, ctx = GBASND_RATE * 50 / 1000;
    int i;
    for (i = 0; i < n; i++) {
        int l = b[2 * i] < 0 ? -b[2 * i] : b[2 * i], r = b[2 * i + 1] < 0 ? -b[2 * i + 1] : b[2 * i + 1];
        int m = l > r ? l : r;
        if (pending_wait >= 0) {
            if (m > 1024) {
                hal_audio_stats.quiet_runs++;
                pending_wait = -1;
            } else if (--pending_wait < 0) {
                pending_wait = -1;
            }
        }
        if (m < 64) {
            quiet_len++;
        } else {
            if (quiet_len >= min_len && since_loud - quiet_len <= ctx) pending_wait = ctx;
            quiet_len = 0;
        }
        since_loud = m > 1024 ? 0 : since_loud + 1;
    }
}
#endif

/* Render the segment DMA plays now plus the PSG into one block; append it to the ring if `keep`. */
static void produce(int keep) {
    u64 t0 = hal_now_us();
    struct mixer_view *si = SOUND_INFO;
    const s8 *seg_a = NULL, *seg_b = NULL;
    int spf = SEG_MAX, n, i, fill, trim;
    u32 wr = ring_wr;

    if (si && (si->lockStatus - ID_NUMBER) < 20 && si->samplesPerFrame > 0 && si->samplesPerFrame <= SEG_MAX &&
        si->framesPerDmaCycle) {
        int seg = si->framesPerDmaCycle - si->dmaCounter;
        if (seg < 0) seg = 0;
        if (seg >= si->framesPerDmaCycle) seg = si->framesPerDmaCycle - 1;
        spf = si->samplesPerFrame;
        if (IO16(0xC6) & 0x8000)
            seg_a = (const s8 *)si + PCM_BUFFER_OFFSET + seg * spf;
        if (IO16(0xD2) & 0x8000)
            seg_b = (const s8 *)si + PCM_BUFFER_OFFSET + PCM_DMA_BUF_SIZE + seg * spf;
    }

    /* Block length: nominal 3 x 224, trimmed by up to TRIM_MAX toward the target fill. */
    fill = (int)(wr - ring_rd);
    if (underrun_pending && produced_any && !underrun_to_log) /* logged by the game thread (hal_audio_frame) */
        underrun_to_log = last_block_loud ? 2 : 1;
    if (underrun_pending || !produced_any) {
        /* After a gap (or at start): refill to the target with silence instead of speeding up. */
        int pad = TARGET_FILL - fill;
        for (i = 0; i < pad && (int)(wr - ring_rd) < RING_FRAMES; i++, wr++) {
            ring[(wr & (RING_FRAMES - 1)) * 2] = 0;
            ring[(wr & (RING_FRAMES - 1)) * 2 + 1] = 0;
        }
        fill = (int)(wr - ring_rd);
        trim_ema = fill << 4;
        underrun_pending = 0;
    }
    trim_ema += fill - (trim_ema >> 4);
    trim = (TARGET_FILL - (trim_ema >> 4)) / 128;
    if (trim > TRIM_MAX) trim = TRIM_MAX;
    if (trim < -TRIM_MAX) trim = -TRIM_MAX;
    n = spf * OUT_MULT + trim;
    if (!keep) {
        s16 scratch[GBASND_MAX_SAMPLES * 2];
        gbasnd_render(scratch, n, seg_a, seg_b, spf); /* keeps the PSG running in step */
        hal_audio_stats.dropped_frames++;
        hal_audio_stats.hw_us += (u32)(hal_now_us() - t0);
        return;
    }
    if ((int)(wr - ring_rd) + n > RING_FRAMES) {
        hal_audio_stats.overflows++;
        n = 0;
    }

    {
        s16 block[GBASND_MAX_SAMPLES * 2];
        int peak = gbasnd_render(block, n, seg_a, seg_b, spf), j;
#ifdef HAL_AUDIO_CHECK /* AUDIOCHECK=1 test builds only */
        count_quiet_runs(block, n);
#endif
        for (j = 0; j < n; j++, wr++) {
            u32 k2 = (wr & (RING_FRAMES - 1)) * 2;
            ring[k2] = block[2 * j];
            ring[k2 + 1] = block[2 * j + 1];
        }
        last_block_loud = peak >= 256; /* above about -42 dBFS: an underrun after it would be heard */
    }
    __asm__ volatile("" ::: "memory");
    ring_wr = wr;
    produced_any = 1;

    hal_audio_stats.frames++;
    hal_audio_stats.samples += (u32)n;
    if (trim < hal_audio_stats.trim_min) hal_audio_stats.trim_min = trim;
    if (trim > hal_audio_stats.trim_max) hal_audio_stats.trim_max = trim;
    fill = (int)(wr - ring_rd);
    if (fill < hal_audio_stats.fill_min) hal_audio_stats.fill_min = fill;
    if (fill > hal_audio_stats.fill_max) hal_audio_stats.fill_max = fill;
    hal_audio_stats.psg_triggers = gbasnd_triggers;
    hal_audio_stats.hw_us += (u32)(hal_now_us() - t0);
}

/* Called by hal_frame.c right after the VBlank handler, which it brackets with hal_audio_busy. */
void hal_audio_frame(void) {
    u64 now;
    produce(1);
    if (underrun_to_log) { /* only this thread writes to the serial log: it may be mid-way through a frame dump */
        hal_log("AUDIO underrun before frame %lu (%s)\n", (unsigned long)hal_frame_count,
                underrun_to_log == 2 ? "audible" : "during silence");
        underrun_to_log = 0;
    }
    now = hal_now_us() * 1000ull;
    if (!next_due_ns || now > next_due_ns + CATCHUP_MAX * FRAME_NS) {
        if (next_due_ns) hal_audio_stats.resyncs++;
        next_due_ns = now;
    }
    next_due_ns += FRAME_NS;
    run_lag_frames(now, &hal_audio_stats.catchup_frames);
    if (next_due_ns > now + 2 * FRAME_NS) next_due_ns = now + FRAME_NS; /* ahead of real time: never wait */
    hal_audio_busy = 0;
}

int hal_audio_init(void) {
    gbasnd_reset();
    if (snd_stream_init_ex(2, AICA_BUF) < 0) {
        hal_log("audio: snd_stream_init failed\n");
        return -1;
    }
    stream = snd_stream_alloc(NULL, AICA_BUF);
    if (stream == SND_STREAM_INVALID) {
        hal_log("audio: no stream\n");
        return -1;
    }
    snd_stream_set_callback_direct(stream, stream_cb);
    snd_stream_volume(stream, 255);
    snd_stream_start(stream, OUT_RATE, 1);
    {
        kthread_t *t = thd_create(true, poll_thread, NULL);
        if (t) thd_set_prio(t, PRIO_DEFAULT - 2);
    }
    hal_audio_stats_reset();
    hal_log("audio: AICA stream %d Hz stereo, %d-byte buffers, ring %d frames (target %d)\n", OUT_RATE, AICA_BUF,
            RING_FRAMES, TARGET_FILL);
    return 0;
}

void hal_audio_stats_reset(void) {
    u32 under = hal_audio_stats.underruns, under_s = hal_audio_stats.underrun_samples,
        last = hal_audio_stats.last_underrun_frame, trig = hal_audio_stats.psg_triggers,
        audible = hal_audio_stats.underruns_audible, catchup = hal_audio_stats.catchup_frames,
        dropped = hal_audio_stats.dropped_frames, resyncs = hal_audio_stats.resyncs,
        lag_thread = hal_audio_stats.lag_frames_thread,
        quiet = hal_audio_stats.quiet_runs,
        over = hal_audio_stats.overflows;
    memset(&hal_audio_stats, 0, sizeof hal_audio_stats);
    /* totals survive the per-window reset */
    hal_audio_stats.underruns = under;
    hal_audio_stats.underrun_samples = under_s;
    hal_audio_stats.last_underrun_frame = last;
    hal_audio_stats.psg_triggers = trig;
    hal_audio_stats.overflows = over;
    hal_audio_stats.underruns_audible = audible;
    hal_audio_stats.catchup_frames = catchup;
    hal_audio_stats.dropped_frames = dropped;
    hal_audio_stats.resyncs = resyncs;
    hal_audio_stats.lag_frames_thread = lag_thread;
    hal_audio_stats.quiet_runs = quiet;
    hal_audio_stats.fill_min = 1 << 30;
    hal_audio_stats.fill_max = -1;
    hal_audio_stats.trim_min = 99;
    hal_audio_stats.trim_max = -99;
}

#else /* !HAL_AUDIO */

struct hal_audio_stats hal_audio_stats;
int hal_audio_init(void) {
    hal_log("audio: disabled (AUDIO=0)\n");
    return 0;
}
volatile int hal_audio_busy;
void hal_m4a_report(void) {}
void hal_audio_frame(void) {}
void hal_audio_stats_reset(void) {}

#endif

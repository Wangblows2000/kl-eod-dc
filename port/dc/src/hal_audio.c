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
 * only writes its own index, after the data it covers, and both are static, so nothing the AICA
 * DMA reads can be freed (KallistiOS copies the callback's data into its own buffer before the
 * DMA starts). On an underrun the callback still returns everything it was asked for, fading the
 * last sample to silence, so the AICA never replays stale buffer contents; the producer then
 * refills the ring to its target with silence rather than speeding up. A small trim of the block
 * length (at most 4 samples a frame, 0.6%) keeps the ring near its target and absorbs drift
 * between the Dreamcast's video and audio clocks.
 *
 * Everything here is compiled out with AUDIO=0 (HAL_AUDIO 0): the port is then silent, exactly
 * as before audio existed.
 */
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
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
static s16 cb_buf[AICA_BUF] __attribute__((aligned(32))); /* one request: up to AICA_BUF/2 per channel */
static volatile int produced_any, underrun_pending, last_block_loud;
static s16 last_l, last_r;
static snd_stream_hnd_t stream = SND_STREAM_INVALID;

static void *stream_cb(snd_stream_hnd_t hnd, int bytes_req, int *bytes_recv) {
    u32 want = (u32)bytes_req / 4, avail, n, i, rd = ring_rd;
    s16 *out = cb_buf;
    (void)hnd;
    if (want > AICA_BUF / 2) want = AICA_BUF / 2;
    avail = ring_wr - rd;
    n = avail < want ? avail : want;
    for (i = 0; i < n; i++) {
        u32 k = ((rd + i) & (RING_FRAMES - 1)) * 2;
        out[2 * i] = ring[k];
        out[2 * i + 1] = ring[k + 1];
    }
    if (n) {
        last_l = out[2 * n - 2];
        last_r = out[2 * n - 1];
    }
    __asm__ volatile("" ::: "memory");
    ring_rd = rd + n;
    if (n < want) {
        /* Underrun: pad with a short fade from the last sample to silence. */
        for (i = n; i < want; i++) {
            last_l = (s16)(last_l - (last_l >> 5) - (last_l > 0) + (last_l < 0));
            last_r = (s16)(last_r - (last_r >> 5) - (last_r > 0) + (last_r < 0));
            out[2 * i] = last_l;
            out[2 * i + 1] = last_r;
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
    *bytes_recv = (int)(want * 4);
    return cb_buf;
}

static void *poll_thread(void *arg) {
    u64 last = 0;
    (void)arg;
    for (;;) {
        u64 t0 = hal_now_us();
        if (last && t0 - last > hal_audio_stats.max_poll_gap_us) hal_audio_stats.max_poll_gap_us = (u32)(t0 - last);
        last = t0;
        snd_stream_poll(stream);
        hal_audio_stats.poll_us += (u32)(hal_now_us() - t0);
        thd_sleep(POLL_MS);
    }
    return NULL;
}

/* ---- per-frame mix ------------------------------------------------------------------------- */

static int trim_ema = TARGET_FILL << 4;


void hal_audio_frame(void) {
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
    if (underrun_pending && produced_any)
        hal_log("AUDIO underrun before frame %lu (%s)\n", (unsigned long)hal_frame_count,
                last_block_loud ? "audible" : "during silence");
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
    if ((int)(wr - ring_rd) + n > RING_FRAMES) {
        hal_audio_stats.overflows++;
        n = 0;
    }

    {
        s16 block[GBASND_MAX_SAMPLES * 2];
        int peak = gbasnd_render(block, n, seg_a, seg_b, spf), j;
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

int hal_audio_init(void) {
    gbasnd_reset();
    if (snd_stream_init_ex(2, AICA_BUF) < 0) {
        hal_log("audio: snd_stream_init failed\n");
        return -1;
    }
    stream = snd_stream_alloc(stream_cb, AICA_BUF);
    if (stream == SND_STREAM_INVALID) {
        hal_log("audio: no stream\n");
        return -1;
    }
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
        audible = hal_audio_stats.underruns_audible,
        over = hal_audio_stats.overflows;
    memset(&hal_audio_stats, 0, sizeof hal_audio_stats);
    /* totals survive the per-window reset */
    hal_audio_stats.underruns = under;
    hal_audio_stats.underrun_samples = under_s;
    hal_audio_stats.last_underrun_frame = last;
    hal_audio_stats.psg_triggers = trig;
    hal_audio_stats.overflows = over;
    hal_audio_stats.underruns_audible = audible;
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
void hal_audio_frame(void) {}
void hal_audio_stats_reset(void) {}

#endif

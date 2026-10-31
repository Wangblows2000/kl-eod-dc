/* GBA DMA, performed in software.
 *
 * Register layout per channel n at 0x040000B0 + 12n: SAD (u32), DAD (u32), CNT_L (count, u16),
 * CNT_H (control, u16). Values are mirrored into the I/O page so code that reads them back, or
 * pauses a channel by clearing CNT_H bit 15 directly (save.c), sees and controls the same state.
 */
#include <string.h>

#include "hal.h"

#define DMA_BASE(ch) (0xB0u + 12u * (ch))
#define CNT_H_ENABLE 0x8000u
#define CNT_H_START_MASK 0x3000u
#define CNT_H_START_NOW 0x0000u
#define CNT_H_START_VBLANK 0x1000u
#define CNT_H_START_HBLANK 0x2000u
#define CNT_H_32BIT 0x0400u
#define CNT_H_REPEAT 0x0200u

struct dma_chan {
    u32 src;     /* internal (running) source */
    u32 dst;     /* internal destination */
    u32 dst_reload;
    u32 count;
    u16 ctrl;
};

static struct dma_chan chans[4];

static u32 transfer(struct dma_chan *c, int ch) {
    u32 n = c->count;
    int src_mode = (c->ctrl >> 7) & 3;
    int dst_mode = (c->ctrl >> 5) & 3;
    int wide = (c->ctrl & CNT_H_32BIT) != 0;
    int step = wide ? 4 : 2;
    int sstep = src_mode == 0 ? step : src_mode == 1 ? -step : 0;
    int dstep = (dst_mode == 0 || dst_mode == 3) ? step : dst_mode == 1 ? -step : 0;
    u32 s = c->src & (wide ? ~3u : ~1u);
    u32 d = c->dst & (wide ? ~3u : ~1u);

    if (n == 0) n = ch == 3 ? 0x10000u : 0x4000u;

    /* Fast paths for the overwhelmingly common fills and copies. */
    if (sstep == step && dstep == step) {
        memmove((void *)d, (const void *)s, n * step);
        s += n * step;
        d += n * step;
    } else if (sstep == 0 && dstep == step && wide) {
        u32 v = *(volatile u32 *)s, i;
        for (i = 0; i < n; i++) ((volatile u32 *)d)[i] = v;
        d += n * step;
    } else if (sstep == 0 && dstep == step) {
        u16 v = *(volatile u16 *)s;
        u32 i;
        for (i = 0; i < n; i++) ((volatile u16 *)d)[i] = v;
        d += n * step;
    } else {
        u32 i;
        for (i = 0; i < n; i++) {
            if (wide) *(volatile u32 *)d = *(volatile u32 *)s;
            else *(volatile u16 *)d = *(volatile u16 *)s;
            s += sstep;
            d += dstep;
        }
    }
    c->src = s;
    c->dst = dst_mode == 3 ? c->dst_reload : d;
    return n;
}

static int channel_enabled(int ch) { return (IO16(DMA_BASE(ch) + 10) & CNT_H_ENABLE) != 0; }

static void finish(struct dma_chan *c, int ch) {
    if (!(c->ctrl & CNT_H_REPEAT) || (c->ctrl & CNT_H_START_MASK) == CNT_H_START_NOW) {
        c->ctrl &= ~CNT_H_ENABLE;
        IO16(DMA_BASE(ch) + 10) &= ~CNT_H_ENABLE;
    }
}

void hal_dma_set(u32 ch, u32 src, u32 dest, u32 control) {
    struct dma_chan *c = &chans[ch & 3];
    u16 ctrl = (u16)(control >> 16);

    ch &= 3;
    IO32(DMA_BASE(ch) + 0) = src;
    IO32(DMA_BASE(ch) + 4) = dest;
    IO32(DMA_BASE(ch) + 8) = control;

    c->src = src;
    c->dst = dest;
    c->dst_reload = dest;
    c->count = control & 0xFFFF;
    c->ctrl = ctrl;

    if (!(ctrl & CNT_H_ENABLE)) return;
    switch (ctrl & CNT_H_START_MASK) {
    case CNT_H_START_NOW:
        transfer(c, ch);
        finish(c, ch);
        break;
    case CNT_H_START_VBLANK:
    case CNT_H_START_HBLANK:
        break; /* performed by hal_dma_vblank / hal_dma_hblank */
    default:
        /* START_SPECIAL: sound FIFO (ch1/2) -- the audio backend drains the mixer buffer itself. */
        break;
    }
}

static void run_timed(u16 timing) {
    int ch;
    for (ch = 0; ch < 4; ch++) {
        struct dma_chan *c = &chans[ch];
        if ((c->ctrl & (CNT_H_ENABLE | CNT_H_START_MASK)) != (CNT_H_ENABLE | timing)) continue;
        if (!channel_enabled(ch)) {
            c->ctrl &= ~CNT_H_ENABLE; /* stopped by a direct CNT_H write */
            continue;
        }
        transfer(c, ch);
        finish(c, ch);
    }
}

void hal_dma_hblank(void) { run_timed(CNT_H_START_HBLANK); }
void hal_dma_vblank(void) { run_timed(CNT_H_START_VBLANK); }

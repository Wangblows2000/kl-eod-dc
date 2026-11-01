/* Renderer selection and per-frame dispatch.
 *
 * In PowerVR mode, a frame that the test script marks for dumping is also rendered by the software
 * renderer, so both outputs come from exactly the same game state and can be compared pixel by
 * pixel (tools/framedump.py --compare sw pvr).
 */
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"

#ifndef HAL_DEFAULT_RENDERER
#define HAL_DEFAULT_RENDERER HAL_RENDER_PVR
#endif

int hal_renderer = HAL_DEFAULT_RENDERER;
static int sw_this_frame;
static u16 dump_buf[160 * 240];

void hal_video_init(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (dev) {
        cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
        if (st && (st->buttons & CONT_Y)) hal_renderer = !hal_renderer;
    }
    hal_log("video: %s renderer\n", hal_renderer == HAL_RENDER_PVR ? "PowerVR" : "software");
    if (hal_renderer == HAL_RENDER_PVR) hal_pvr_init();
    else hal_sw_init();
    hal_log("video: output %dx%d; game picture %dx%d scaled %d/3 to %.1fx%.1f at (%.1f, %.1f), rows %d-%d\n",
            vid_mode->width, vid_mode->height, HAL_GBA_W, HAL_GBA_H, (int)(HAL_VIEW_SCALE * 3 + 0.5f),
            (double)(HAL_GBA_W * HAL_VIEW_SCALE), (double)(HAL_GBA_H * HAL_VIEW_SCALE), (double)HAL_VIEW_X0,
            (double)HAL_VIEW_Y0, HAL_VIEW_ROW0, HAL_VIEW_ROW0 + HAL_VIEW_ROWS - 1);
    if (vid_mode->width != HAL_FB_W || vid_mode->height != HAL_FB_H)
        hal_log("video: unexpected output size; the picture geometry assumes %dx%d\n", HAL_FB_W, HAL_FB_H);
    sw_this_frame = hal_renderer == HAL_RENDER_SW || hal_test_dump_due(hal_frame_count + 1);
}

void hal_video_line(int line) {
    if (sw_this_frame) hal_sw_render_line(line);
    if (hal_renderer == HAL_RENDER_PVR) hal_pvr_line(line);
}

u32 hal_video_bar_pixels(const volatile u16 *fb, u32 *top, u32 *bottom) {
    int x, y;
    *top = *bottom = 0;
    for (y = 0; y < HAL_FB_H; y++) {
        if (y >= HAL_VIEW_ROW0 && y < HAL_VIEW_ROW0 + HAL_VIEW_ROWS) continue;
        if (y >= HAL_TEXT_Y && y < HAL_TEXT_Y + HAL_TEXT_ROWS) continue; /* status line */
        for (x = 0; x < HAL_FB_W; x++)
            if (fb[y * HAL_FB_W + x]) ++*(y < HAL_VIEW_ROW0 ? top : bottom);
    }
    return *top + *bottom;
}

static void log_bars(const char *tag, u32 frame, const volatile u16 *fb) {
    u32 top, bottom;
    hal_video_bar_pixels(fb, &top, &bottom);
    hal_log("BARS %s frame=%lu non-black: top=%lu bottom=%lu\n", tag, (unsigned long)frame, (unsigned long)top,
            (unsigned long)bottom);
}

#ifdef HAL_GEOM_PROBE
/* GEOMPROBE=1: at frame HAL_GEOM_PROBE the whole 640x480 frame goes to the host (flycast's
 * FC_RAMDUMP hook), for tools/geomcheck.py. The software renderer shows the same test card as
 * the PowerVR one (hal_video_pvr.c, draw_probe). */
static u16 probe_copy[HAL_FB_W * HAL_FB_H];
static void probe_card_sw(void) {
    int x, y;
    for (y = 0; y < HAL_GBA_H; y++)
        for (x = 0; x < HAL_GBA_W; x++) /* BGR555 of the card's RGB565: red 31, green 31 (of 63 -> 31), blue */
            hal_sw_frame[y][x] = (u16)(((x & 1) ? 31 : 0) | ((y & 1) ? 31 << 5 : 0) | (16 << 10));
}
static void probe_dump(u32 frame) {
    const volatile u16 *fb = hal_renderer == HAL_RENDER_PVR ? hal_pvr_front_buffer() : vram_s;
    int i;
    for (i = 0; i < HAL_FB_W * HAL_FB_H; i++) probe_copy[i] = fb[i];
    log_bars(hal_renderer == HAL_RENDER_PVR ? "pvr" : "sw", frame, fb);
    hal_log("GEOMPROBE %s frame=%lu %dx%d RGB565\n", hal_renderer == HAL_RENDER_PVR ? "pvr" : "sw",
            (unsigned long)frame, HAL_FB_W, HAL_FB_H);
    hal_log("FCDUMP %08lx %08lx\n", (unsigned long)(uintptr_t)probe_copy, (unsigned long)sizeof probe_copy);
}
#endif

void hal_video_present(void) {
    u32 frame = hal_frame_count + 1; /* number of the frame being finished */
    u64 t0 = hal_now_us(), t1;
#ifdef HAL_GEOM_PROBE
    if (hal_renderer == HAL_RENDER_SW) probe_card_sw();
#endif
    if (hal_renderer == HAL_RENDER_PVR) {
        hal_pvr_present(); /* times its own submit / texture / wait phases */
    } else {
        hal_sw_present();
        t1 = hal_now_us();
        hal_prof.submit_us += (u32)(t1 - t0);
        vid_waitvbl();
        hal_prof.wait_us += (u32)(hal_now_us() - t1);
    }

    if (hal_test_dump_due(frame)) {
        hal_test_dump("sw", frame, &hal_sw_frame[0][0]);
        if (hal_renderer == HAL_RENDER_PVR) {
            hal_pvr_readback(dump_buf);
            hal_test_dump("pvr", frame, dump_buf);
            log_bars("pvr", frame, hal_pvr_front_buffer());
        } else {
            log_bars("sw", frame, vram_s);
        }
    }
#ifdef HAL_GEOM_PROBE
    if (frame == HAL_GEOM_PROBE) probe_dump(frame);
#endif
    sw_this_frame = hal_renderer == HAL_RENDER_SW || hal_test_dump_due(frame + 1);
}

/* Renderer selection and per-frame dispatch.
 *
 * In PowerVR mode, a frame that the test script marks for dumping is also rendered by the software
 * renderer, so both outputs come from exactly the same game state and can be compared pixel by
 * pixel (tools/framedump.py --compare sw pvr).
 */
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>

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
    sw_this_frame = hal_renderer == HAL_RENDER_SW || hal_test_dump_due(hal_frame_count + 1);
}

void hal_video_line(int line) {
    if (sw_this_frame) hal_sw_render_line(line);
    if (hal_renderer == HAL_RENDER_PVR) hal_pvr_line(line);
}

void hal_video_present(void) {
    u32 frame = hal_frame_count + 1; /* number of the frame being finished */
    u64 t0 = hal_now_us(), t1;
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
        }
    }
    sw_this_frame = hal_renderer == HAL_RENDER_SW || hal_test_dump_due(frame + 1);
}

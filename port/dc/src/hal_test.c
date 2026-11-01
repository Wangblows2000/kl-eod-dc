/* Test and profiling support (compiled into every build; inert unless a script is linked in).
 *
 * Scripts (tests/*.txt, turned into build/hal/test_script.c by tools/script2c.py):
 *   press  <frame> <frames> <KEYS...>   hold keys (A B SELECT START RIGHT LEFT UP DOWN R L)
 *   dump   <frame>                      dump the finished frame over serial (base64 BGR555)
 *   end    <frame>                      stop the run (freeze or exit, see HAL_FREEZE)
 * Input is purely frame-indexed, so a given frame number is the same game state in every build
 * and with either renderer.
 */
#include <arch/timer.h>
#include <string.h>

#include "hal.h"

struct test_cmd {
    u32 frame;
    u32 len;
    u16 keys;
    u8 op; /* 1 press, 2 dump, 3 end */
};
extern const struct test_cmd hal_test_script[];
extern const u32 hal_test_script_len;

u16 hal_test_keys(u32 frame) {
    u16 k = 0;
    u32 i;
    for (i = 0; i < hal_test_script_len; i++) {
        const struct test_cmd *c = &hal_test_script[i];
        if (c->op == 1 && frame >= c->frame && frame < c->frame + c->len) k |= c->keys;
    }
    return k;
}

int hal_test_dump_due(u32 frame) {
    u32 i;
    for (i = 0; i < hal_test_script_len; i++)
        if (hal_test_script[i].op == 2 && hal_test_script[i].frame == frame) return 1;
    return 0;
}

u32 hal_test_end_frame(void) {
    u32 i;
    for (i = 0; i < hal_test_script_len; i++)
        if (hal_test_script[i].op == 3) return hal_test_script[i].frame;
    return 0;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* FRAMEDUMP <tag> <frame> 240 160 then base64 lines of 240x160 little-endian BGR555, then END. */
void hal_test_dump(const char *tag, u32 frame, const u16 *pix) {
    const u8 *p = (const u8 *)pix;
    u32 n = 240 * 160 * 2, i;
    char line[100];
    int o = 0;
    hal_log("FRAMEDUMP %s %lu 240 160\n", tag, (unsigned long)frame);
    for (i = 0; i < n; i += 3) {
        u32 v = (u32)p[i] << 16 | (u32)(i + 1 < n ? p[i + 1] : 0) << 8 | (i + 2 < n ? p[i + 2] : 0);
        line[o++] = b64[(v >> 18) & 63];
        line[o++] = b64[(v >> 12) & 63];
        line[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        line[o++] = i + 2 < n ? b64[v & 63] : '=';
        if (o >= 96) {
            line[o++] = '\n';
            line[o] = 0;
            hal_log("%s", line);
            o = 0;
        }
    }
    if (o) {
        line[o++] = '\n';
        line[o] = 0;
        hal_log("%s", line);
    }
    hal_log("FRAMEDUMP END\n");
}

/* ---- profiling -------------------------------------------------------------------------- */

struct hal_prof hal_prof;

u64 hal_now_us(void) { return timer_us_gettime64(); }

void hal_prof_frame_end(void) {
    static u64 last;
    u64 now = hal_now_us();
    if (last) {
        u32 ft = (u32)(now - last);
        static struct hal_prof prev;
        if (hal_prof.frames == 0) memset(&prev, 0, sizeof prev); /* accumulators were just reset */
        if (ft > 33000)
            hal_log("SPIKE frame=%lu %luus: game=%lu snapshots=%lu tex=%lu submit=%lu wait=%lu\n",
                    (unsigned long)hal_frame_count, (unsigned long)ft, (unsigned long)(hal_prof.game_us - prev.game_us),
                    (unsigned long)(hal_prof.render_us - prev.render_us), (unsigned long)(hal_prof.tex_us - prev.tex_us),
                    (unsigned long)(hal_prof.submit_us - prev.submit_us), (unsigned long)(hal_prof.wait_us - prev.wait_us));
        prev = hal_prof;
        hal_prof.frames++;
        hal_prof.frame_us += ft;
        if (ft > hal_prof.worst_us) hal_prof.worst_us = ft;
    }
    last = now;
    if (hal_prof.frames == 120) {
        u32 f = hal_prof.frames;
        hal_log("PERF frames=%lu fps=%lu.%lu frame=%luus worst=%luus game=%luus render=%luus submit=%luus "
                "tex=%luus wait=%luus\n",
                (unsigned long)hal_frame_count, (unsigned long)(1000000ull * f / hal_prof.frame_us),
                (unsigned long)((10000000ull * f / hal_prof.frame_us) % 10), (unsigned long)(hal_prof.frame_us / f),
                (unsigned long)hal_prof.worst_us, (unsigned long)(hal_prof.game_us / f),
                (unsigned long)(hal_prof.render_us / f), (unsigned long)(hal_prof.submit_us / f),
                (unsigned long)(hal_prof.tex_us / f), (unsigned long)(hal_prof.wait_us / f));
        memset(&hal_prof, 0, sizeof hal_prof);
    }
}

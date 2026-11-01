/* Dreamcast entry point: replaces the GBA crt0 (asm/crt0.s). Maps the GBA address space, puts
 * the hardware registers in their post-BIOS state, and hands control to the game's own
 * AgbMain(), which never returns. */
#include <dc/scif.h>
#include <kos.h>

#include "hal.h"
#include "hal_vmu_lcd.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

extern void AgbMain(void);
extern u32 hal_max_frames;
void hal_apply_code_ptr_fixups(void);

/* Debug watchdog: a preemptive KOS thread that reports progress and where the game thread is.
 * Map the PC to a function with build/klonoa-dc.map. */
static kthread_t *game_thread;
static void *watchdog(void *p) {
    u32 last = 0;
    (void)p;
    for (;;) {
        thd_sleep(2000);
        hal_log("watchdog: frames=%lu (+%lu) game pc=%08lx pr=%08lx\n", (unsigned long)hal_frame_count,
                (unsigned long)(hal_frame_count - last), (unsigned long)CONTEXT_PC(game_thread->context),
                (unsigned long)game_thread->context.pr);
        last = hal_frame_count;
    }
    return NULL;
}

/* Microbenchmark: memory-bound loop over a 64 KiB P1 buffer. Run before and after the MMU is
 * enabled to see how much the emulator's MMU mode costs (real hardware: no difference). */
static u8 bench_buf[65536] __attribute__((aligned(32)));
static void bench(const char *when) {
    u64 t0 = timer_us_gettime64();
    u32 i, k, sum = 0;
    for (k = 0; k < 16; k++)
        for (i = 0; i < sizeof bench_buf; i++) {
            bench_buf[(i * 37) & 0xFFFF] = (u8)(bench_buf[i] + k);
            sum += bench_buf[i];
        }
    hal_log("BENCH %s: 1M byte read-modify-writes in %lu us (checksum %lu)\n", when,
            (unsigned long)(timer_us_gettime64() - t0), (unsigned long)sum);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
#ifdef HAL_FAST_SERIAL
    /* Test builds: frame dumps are ~100 KB each, so raise the serial rate from 115200. */
    scif_set_parameters(HAL_FAST_SERIAL, 1);
    scif_init();
#endif
    hal_log("klonoa-dc: boot\n");
#ifdef HAL_FAST_SERIAL
    bench("MMU off");
#endif
    hal_audio_init(); /* before the MMU is on: the AICA driver is loaded through the store queues */
    if (hal_mem_init() < 0) {
        hal_log("klonoa-dc: fatal: memory mapping failed\n");
        return 1;
    }
    hal_apply_code_ptr_fixups();
#ifdef HAL_FAST_SERIAL
    bench("MMU on");
#endif

    IO16(0x000) = 0x0080; /* DISPCNT: forced blank, as the BIOS leaves it */
    hal_video_init();
    maple_wait_scan(); /* every VMU enumerated before the save is looked for */
    hal_save_init();
    hal_vmu_lcd_init();
    hal_frame_init();
    hal_input_poll();

#ifdef HAL_MAX_FRAMES
    hal_max_frames = HAL_MAX_FRAMES;
#endif
    game_thread = thd_get_current();
#ifndef HAL_FAST_SERIAL /* scripted runs report through PERF/SCENE lines; keep dumps unbroken */
    thd_create(true, watchdog, NULL);
#endif
    hal_report_memory();
    hal_log("klonoa-dc: entering AgbMain\n");
    AgbMain();
    return 0;
}

/* Dreamcast entry point: replaces the GBA crt0 (asm/crt0.s). Maps the GBA address space, puts
 * the hardware registers in their post-BIOS state, and hands control to the game's own
 * AgbMain(), which never returns. */
#include <kos.h>

#include "hal.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

extern void AgbMain(void);
extern u32 hal_max_frames;
void hal_apply_code_ptr_fixups(void);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    hal_log("klonoa-dc: boot\n");
    if (hal_mem_init() < 0) {
        hal_log("klonoa-dc: fatal: memory mapping failed\n");
        return 1;
    }
    hal_apply_code_ptr_fixups();

    IO16(0x000) = 0x0080; /* DISPCNT: forced blank, as the BIOS leaves it */
    hal_video_init();
    hal_frame_init();
    hal_input_poll();

#ifdef HAL_MAX_FRAMES
    hal_max_frames = HAL_MAX_FRAMES;
#endif
    hal_log("klonoa-dc: entering AgbMain\n");
    AgbMain();
    return 0;
}

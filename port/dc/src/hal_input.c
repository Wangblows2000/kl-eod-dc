/* Dreamcast controller -> GBA REG_KEYINPUT (0x04000130, active low).
 *
 * Mapping: A->A, B/X->B, Y->SELECT, START->START, L/R triggers->L/R, D-pad->D-pad.
 * START+A+B+X+Y together exits (KOS convention for homebrew).
 */
#include <dc/maple.h>
#include <dc/maple/controller.h>

#include "hal.h"

#define REG_KEYINPUT_OFF 0x130
#define KEY_A 0x001
#define KEY_B 0x002
#define KEY_SELECT 0x004
#define KEY_START 0x008
#define KEY_RIGHT 0x010
#define KEY_LEFT 0x020
#define KEY_UP 0x040
#define KEY_DOWN 0x080
#define KEY_R 0x100
#define KEY_L 0x200

static int quit_requested;
const u16 *hal_scripted_input; /* optional per-frame key script for headless tests (pressed bits) */
u32 hal_scripted_input_len;

void hal_input_poll(void) {
    u16 pressed = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);

    if (dev) {
        cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
        if (st) {
            u32 b = st->buttons;
            if (b & CONT_A) pressed |= KEY_A;
            if (b & (CONT_B | CONT_X)) pressed |= KEY_B;
            if (b & CONT_Y) pressed |= KEY_SELECT;
            if (b & CONT_START) pressed |= KEY_START;
            if (b & CONT_DPAD_RIGHT) pressed |= KEY_RIGHT;
            if (b & CONT_DPAD_LEFT) pressed |= KEY_LEFT;
            if (b & CONT_DPAD_UP) pressed |= KEY_UP;
            if (b & CONT_DPAD_DOWN) pressed |= KEY_DOWN;
            if (st->rtrig > 64) pressed |= KEY_R;
            if (st->ltrig > 64) pressed |= KEY_L;
            if ((b & (CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y)) == (CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y))
                quit_requested = 1;
        }
    }
    if (hal_scripted_input && hal_frame_count < hal_scripted_input_len) pressed |= hal_scripted_input[hal_frame_count];
    IO16(REG_KEYINPUT_OFF) = (u16)(~pressed & 0x3FF);
}

int hal_input_quit_requested(void) { return quit_requested; }

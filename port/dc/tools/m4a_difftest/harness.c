/* Support code for running hal_m4a.c on an ARM CPU next to the original GBA routines
 * (see difftest.py). */
#include "../../src/hal.h"
struct hal_audio_stats hal_audio_stats;
u64 hal_now_us(void) { return 0; }

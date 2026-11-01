/* GBA sound hardware model (hal_gbasnd.c). */
#ifndef KLONOA_DC_GBASND_H
#define KLONOA_DC_GBASND_H

#define GBASND_RATE 40280       /* output rate: 3 x 224 samples per 59.94 Hz frame */
#define GBASND_MAX_SEG 224      /* DirectSound samples per frame (13379 Hz mode) */
#define GBASND_MAX_SAMPLES 704  /* output samples per frame, with room for the drift trim */

int gbasnd_render(s16 *out, int n, const s8 *seg_a, const s8 *seg_b, int spf);
void gbasnd_reset(void);
extern u32 gbasnd_triggers;

#endif

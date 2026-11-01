/* Klonoa: Empire of Dreams -- Dreamcast hardware abstraction layer.
 *
 * The game logic (kleod src/ C files) is compiled unmodified for SH-4. It still addresses the GBA
 * memory map directly: RAM globals sit at their fixed IWRAM/EWRAM addresses, it writes
 * 0x04000000 I/O registers, VRAM/OAM/palette, and reads ROM data (including raw pointers inside
 * data blobs) at 0x08xxxxxx. hal_mem maps all of those virtual ranges onto Dreamcast RAM with
 * the SH-4 MMU, so every one of those accesses is an ordinary memory access. The rest of the HAL
 * gives the passive register state its meaning: DMA, the per-frame "interrupts", video, input,
 * save.
 */
#ifndef KLONOA_DC_HAL_H
#define KLONOA_DC_HAL_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

/* GBA address map as seen by the game. */
#define GBA_EWRAM 0x02000000u
#define GBA_IWRAM 0x03000000u
#define GBA_IO 0x04000000u
#define GBA_PLTT 0x05000000u
#define GBA_VRAM 0x06000000u
#define GBA_OAM 0x07000000u
#define GBA_ROM 0x08000000u

#define GBA_EWRAM_SIZE 0x40000u
#define GBA_IWRAM_SIZE 0x8000u
#define GBA_ROM_SIZE 0x400000u

int hal_gba_range_ok(u32 addr, u32 len, int write); /* hal_mem.c */

#define IO16(off) (*(volatile u16 *)(GBA_IO + (off)))
#define IO32(off) (*(volatile u32 *)(GBA_IO + (off)))

/* hal_mem.c */
int hal_mem_init(void); /* map GBA address space, load ROM image; 0 on success */

/* hal_dma.c */
void hal_dma_set(u32 ch, u32 src, u32 dest, u32 control);
void hal_dma_hblank(void);
void hal_dma_vblank(void);

/* hal_frame.c */
extern volatile u32 hal_frame_count;
void hal_frame_init(void);
void hal_report_memory(void);

/* Renderers. hal_video.c dispatches to the selected backend:
 *   HAL_RENDER_SW  -- reference scanline software renderer (hal_video_sw.c)
 *   HAL_RENDER_PVR -- PowerVR renderer (hal_video_pvr.c)
 * Selected at boot (build default HAL_DEFAULT_RENDERER, or hold Y while booting for the other one). */
enum { HAL_RENDER_SW = 0, HAL_RENDER_PVR = 1 };
extern int hal_renderer;
void hal_video_init(void);
void hal_video_line(int line); /* called for each visible line, before its HBlank */
void hal_video_present(void);  /* called once per frame, after line 227 */

/* software renderer */
extern u16 hal_sw_frame[160][240]; /* last rendered frame, BGR555 */
void hal_sw_init(void);
void hal_sw_render_line(int line);
void hal_sw_present(void);

/* PowerVR renderer */
void hal_pvr_init(void);
void hal_pvr_line(int line);
void hal_pvr_present(void);
void hal_pvr_readback(u16 *dst); /* 240x160 BGR555 of the last presented frame */
void hal_pvr_report_features(void);

/* hal_test.c */
typedef unsigned long long u64;
struct hal_prof {
    u32 frames, frame_us, worst_us, game_us, render_us, submit_us, tex_us, wait_us;
};
extern struct hal_prof hal_prof;
u64 hal_now_us(void);
void hal_prof_frame_end(void);
u16 hal_test_keys(u32 frame);
int hal_test_dump_due(u32 frame);
u32 hal_test_end_frame(void);
void hal_test_dump(const char *tag, u32 frame, const u16 *pix);

/* hal_input.c */
void hal_input_poll(void); /* writes REG_KEYINPUT */
int hal_input_quit_requested(void);

/* debug log over serial / dcload */
void hal_log(const char *fmt, ...);

#endif

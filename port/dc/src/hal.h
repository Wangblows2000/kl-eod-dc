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

/* Presentation, shared by both renderers. The output is 640x480 (vid_set_mode(DM_640x480)),
 * shown as 4:3, so its pixels are square. The 240x160 (3:2) GBA picture is scaled uniformly by
 * the largest factor that fits -- min(640/240, 480/160) = 8/3 -- and centred: 640 x 426.7 at
 * (0, 26.7), with black bars above and below. A 3:2 picture is wider than 4:3, so the bars are
 * horizontal; screen rows 27..452 show the game. Each screen pixel shows the game pixel under its
 * centre (nearest), so game pixels are 3, 3, 2, 3, 3, 2 ... screen pixels wide and tall. The game's
 * own coordinates are untouched: only these constants place its picture on the screen. */
#define HAL_FB_W 640
#define HAL_FB_H 480
#define HAL_GBA_W 240
#define HAL_GBA_H 160
#define HAL_VIEW_SCALE (8.0f / 3.0f)
#define HAL_VIEW_X0 0.0f
#define HAL_VIEW_Y0 (80.0f / 3.0f)
#define HAL_VIEW_ROW0 27   /* first screen row inside the picture */
#define HAL_VIEW_ROWS 426  /* rows 27..452 */
#define HAL_TEXT_Y 458     /* status line (VMU messages): 16 rows inside the bottom bar */
#define HAL_TEXT_ROWS 16
/* Test aid: non-black pixels in the bars of a 640x480 RGB565 frame, outside the status line. */
u32 hal_video_bar_pixels(const volatile u16 *fb, u32 *top, u32 *bottom);
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
const volatile u16 *hal_pvr_front_buffer(void); /* the last presented 640x480 frame (test dumps) */
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

/* hal_audio.c: GBA sound hardware model and AICA output; hal_m4a.c: the MP2K engine's asm parts */
struct hal_audio_stats {
    u32 frames, samples, hw_us, m4a_us, poll_us, max_poll_gap_us; /* per 120-frame window */
    int fill_min, fill_max, trim_min, trim_max;
    u32 underruns, underruns_audible, underrun_samples, last_underrun_frame, overflows, psg_triggers; /* totals */
    u32 catchup_frames, lag_frames_thread, dropped_frames, resyncs; /* totals: lag frames (game thread, poll
                                                                     thread), of them not queued */
    u32 spu_us; /* window: time writing the stream's sound RAM (in poll_us) */
    u32 quiet_runs; /* total: gaps inside sound in the produced stream (see hal_audio.c) */
};
extern struct hal_audio_stats hal_audio_stats;
int hal_audio_init(void);   /* before hal_mem_init: loads the AICA driver */
void hal_audio_frame(void); /* once per frame, after the VBlank handler; clears hal_audio_busy */
extern volatile int hal_audio_busy; /* set by hal_frame.c around the VBlank handler */
void hal_audio_stats_reset(void);
void hal_m4a_report(void); /* AUDIOCHECK=1: engine behaviours exercised (hal_m4a.c) */

/* hal_save.c: VMU saves of the EEPROM image (hal_eeprom.c) */
void hal_save_init(void);               /* before AgbMain: load the newest save, start the writer */
void hal_save_frame(void);              /* once per frame: hand a changed EEPROM image to the writer */
void hal_save_flush(u32 timeout_ms);    /* test runs, before exiting */
const char *hal_save_message(void);     /* status line for the letterbox bar, or NULL */

/* hal_input.c */
void hal_input_poll(void); /* writes REG_KEYINPUT */
int hal_input_quit_requested(void);

/* debug log over serial / dcload */
void hal_log(const char *fmt, ...);

#endif

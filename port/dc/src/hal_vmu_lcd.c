/* VMU LCD character display (docs/vmu-lcd.md).
 *
 * During gameplay every VMU screen shows Klonoa's current animation frame -- the very frame the
 * game's animation code (kleod src/anim.c) chose and DMA'd to OBJ VRAM this frame, with the hFlip
 * the game gives his OAM entry. On menus, cutscenes and the vision select it shows a Moo walking
 * with the game's own Moo walk/look-around animation and speed. hal_vmu_lcd_core.c does the
 * conversion, sequencing and scheduling; this file reads the game's state and talks to the VMUs.
 *
 * Only the maple LCD function is used (block write to MAPLE_FUNC_LCD): no storage access at all.
 * vmu_draw_lcd() only *tries* to lock the VMU's maple frame, so the game thread never waits: if
 * the frame is busy (a save writing a block to that VMU, or our previous image still in flight)
 * the image is sent on a later frame. Missing or non-VMU devices are skipped silently.
 */
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <kos.h>

#include "hal.h"
#include "hal_vmu_lcd.h"
#include "hal_vmu_lcd_core.h"

/* VMULCD=0 build option: leave the VMU screens alone. VMULCD_LOG=1: log every image and send. */
#ifndef HAL_VMULCD
#define HAL_VMULCD 1
#endif

/* ---- game state (kleod include/structs/variables.h) ------------------------------------------ */
extern u32 gCallbackQueue[];         /* struct CallbackQueue: current[1] is the scene handler */
extern u8 gUnk_03004C20[];           /* struct Unk_03004C20: +0x0C level (0 = none / vision select) */
extern u8 gEntityInfo[];             /* struct EntityInfo[], 0x1C bytes each; entity 0 is Klonoa */
extern u8 gEntityAnimationInfo[];    /* struct EntityAnimationInfo[]: state, timer, frame, pad */
extern u32 gUnk_03005418;            /* struct EntityAnimationData *: Klonoa's animation table */
/* The three tables it can point to. The port links its own copies of kleod's data (asm/data), so
 * the game stores these addresses, not the ROM ones; the bytes are the ROM's. */
extern u8 gUnk_0805553C[], gUnk_080555A8[], gUnk_08055614[];
#define ENTITY_ID_KLONOA 0x6E

/* Scenes that are not gameplay: everything here shows the Moo. Klonoa is shown only in a level
 * (gUnk_03004C20.level != 0) whose scene handler is not one of these -- the level loop, room
 * rotation, boss scenes, the death fade. A text box keeps whatever was showing: the vision select
 * opens one (with the next level already chosen) before the level loads, and signs open one in a
 * level. */
#define OVERLAY_SCENES(X) X(TextBoxHandler) X(TextBoxRestoreGameplay)
#define MENU_SCENES(X)                                                                                 \
    X(BootScreenHandler) X(NamcoScreenHandler) X(TitleScreenHandler)                                  \
    X(TransitionFromTitleScreenToFileSelect_FadeOut) X(TransitionFromDemoToTitleScreen_FadeOut)       \
    X(FileSelectScreenHandler) X(ButtonConfigurationScreenInit) X(ButtonConfigurationScreenHandler)   \
    X(DeleteAllSaveDataScreenHandler) X(TransitionFromFileSelectToLevel_FadeOut)                      \
    X(WorldMapScreenInit) X(WorldMapScreenHandler) X(WorldMapScreenCheckNewWorldUnlocked)             \
    X(WorldMapScreenUnlockNewWorld) X(TransitionFromWorldMapToVisionSelect_FadeOut)                   \
    X(TransitionFromWorldMapToLevel_FadeOut) X(TransitionFromVisionSelectToWorldMap_FadeOut)          \
    X(TransitionFromVisionSelectToLevel_FadeOut) X(TransitionFromVisionSelectToBootScreen_FadeOut)    \
    X(VisionSelectInit) X(VisionSelectHandler) X(VisionSelectWaitForNextFrame)                        \
    X(VisionSelectBeginTransitionToVision) X(sub_0804EB64) /* story cutscene */                     \
    X(PauseMenuScreenInit) X(PauseMenuScreenHandler) X(GameOverScreenHandler)                         \
    X(ClearedAllVisionsScreenInit) X(ClearedAllVisionsScreenHandler)                                  \
    X(TransitionFromLevelToClearedAllVisionsScreen_FadeOut)
#define DECLARE(f) extern void f(void);
MENU_SCENES(DECLARE)
OVERLAY_SCENES(DECLARE)
#define ADDRESS(f) (u32)f,
static const u32 menu_scenes[] = {MENU_SCENES(ADDRESS)};
static const u32 overlay_scenes[] = {OVERLAY_SCENES(ADDRESS)};

static int classify_scene(u32 handler) {
    unsigned i;
    for (i = 0; i < sizeof menu_scenes / sizeof menu_scenes[0]; i++)
        if (menu_scenes[i] == handler) return VMULCD_SCENE_MENU;
    for (i = 0; i < sizeof overlay_scenes / sizeof overlay_scenes[0]; i++)
        if (overlay_scenes[i] == handler) return VMULCD_SCENE_OVERLAY;
    return VMULCD_SCENE_OTHER;
}

static const void *resolve(u32 addr, u32 len) {
    return hal_gba_range_ok(addr, len, 0) ? (const void *)addr : NULL;
}

static void read_game(struct vmulcd_game *g) {
    const u8 *e = gEntityInfo; /* entity 0 */
    g->scene = classify_scene(gCallbackQueue[1]);
    g->level = gUnk_03004C20[0x0C];
    g->klonoa_entity = e[0x11] == ENTITY_ID_KLONOA;
    g->anims = gUnk_03005418 == (u32)gUnk_0805553C   ? VMULCD_KLONOA_ANIMS_NORMAL
               : gUnk_03005418 == (u32)gUnk_080555A8 ? VMULCD_KLONOA_ANIMS_BOARD
               : gUnk_03005418 == (u32)gUnk_08055614 ? VMULCD_KLONOA_ANIMS_GOAL
                                                      : gUnk_03005418;
    g->state = gEntityAnimationInfo[0];
    g->frame = gEntityAnimationInfo[2];
    /* TRANSFER_OAM (code_08003D58.c): affine sprites take hFlip from affineHFlip_matrixNum bit 3,
     * others from hvFlip bit 0. Byte 0x0C: priority:2 hvFlip:2; 0x0D: objMode:2 matrixNum:4;
     * 0x0E: affineEnable:1. */
    g->hflip = (e[0x0E] & 1) ? (e[0x0D] >> 5) & 1 : (e[0x0C] >> 2) & 1;
}

static struct vmulcd lcd;
static struct {
    u32 frames, us, worst_us, sends, busy, failed;
} stats;

void hal_vmu_lcd_init(void) {
    int p, u, n = 0;
    vmulcd_resolve = resolve;
    vmulcd_init(&lcd);
    for (p = 0; p < 4; p++)
        for (u = 1; u <= 2; u++) {
            maple_device_t *dev = maple_enum_dev(p, u);
            if (dev && (dev->info.functions & MAPLE_FUNC_LCD)) n++;
        }
    hal_log("VMULCD init: %d screen%s found%s\n", n, n == 1 ? "" : "s", HAL_VMULCD ? "" : " (disabled: VMULCD=0)");
}

/* A VMU in a controller sits upside down relative to the controller; as vmufb_present() does,
 * rotate unless the controller and the VMU face opposite ways (e.g. a light gun). */
static int needs_rotation(const maple_device_t *dev) {
    maple_device_t *cont = maple_enum_dev(dev->port, 0);
    return !(cont && (cont->info.functions & MAPLE_FUNC_CONTROLLER) &&
             cont->info.connector_direction != dev->info.connector_direction);
}

static void send_image(maple_device_t *dev, int slot) {
    int rot = needs_rotation(dev);
    int rv = rot ? vmu_draw_lcd_rotated(dev, lcd.image) : vmu_draw_lcd(dev, lcd.image);
    if (rv == MAPLE_EOK) {
        vmulcd_sent(&lcd, slot, hal_frame_count);
        stats.sends++;
    } else if (rv == MAPLE_EAGAIN) {
        stats.busy++; /* frame locked: retry next game frame */
    } else {
        vmulcd_sent(&lcd, slot, hal_frame_count); /* not a VMU (e.g. a VMS): don't retry this image */
        stats.failed++;
    }
#ifdef HAL_VMULCD_LOG
    hal_log("VMULCDSEND f=%lu gen=%lu slot=%d port=%c%d rot=%d rv=%d\n", (unsigned long)hal_frame_count,
            (unsigned long)lcd.gen, slot, 'A' + dev->port, dev->unit, rot, rv);
#endif
}

#ifdef HAL_VMULCD_LOG
/* Frames in which the game did not run its animation update (slot 0's state, frame and timer all
 * unchanged; its loop spanned two video frames): the frame on show then lasts one frame longer than
 * its ROM timer, on screen as on the LCD. Logged with each image; tools/vmulcd.py allows for them. */
static u32 lag;
static void count_lag_frames(void) {
    static u32 last = 0xFFFFFFFF;
    u32 now = gEntityAnimationInfo[0] | gEntityAnimationInfo[1] << 8 | gEntityAnimationInfo[2] << 16;
    if (now == last && gEntityAnimationInfo[1] != 0xFF) lag++;
    last = now;
}
#endif

void hal_vmu_lcd_frame(void) {
    struct vmulcd_game g;
    int p, u, mode = lcd.mode;
    u64 t0;
    u32 dt;
    if (!HAL_VMULCD) return;
    t0 = hal_now_us();
    read_game(&g);
#ifdef HAL_VMULCD_LOG
    count_lag_frames();
#endif
    if (vmulcd_update(&lcd, &g)) {
#ifdef HAL_VMULCD_LOG
        static const char hex[] = "0123456789abcdef";
        char img[VMULCD_BYTES * 2 + 1];
        int i;
        for (i = 0; i < VMULCD_BYTES; i++) {
            img[2 * i] = hex[lcd.image[i] >> 4];
            img[2 * i + 1] = hex[lcd.image[i] & 15];
        }
        img[2 * VMULCD_BYTES] = 0;
        hal_log("VMULCD f=%lu gen=%lu mode=%s src=%08lx pal=%08lx x=%d flip=%d state=%d frame=%d lag=%lu img=",
                (unsigned long)hal_frame_count, (unsigned long)lcd.gen, vmulcd_mode_name[lcd.mode],
                (unsigned long)lcd.src, (unsigned long)lcd.palette, lcd.x, lcd.hflip,
                lcd.mode == VMULCD_MODE_GAMEPLAY ? g.state : lcd.moo.anim.state,
                lcd.mode == VMULCD_MODE_GAMEPLAY ? g.frame : lcd.moo.anim.frame, (unsigned long)lag);
        hal_log("%.192s", img); /* hal_log() formats into 256 bytes */
        hal_log("%s\n", img + 192);
        lag = 0;
#endif
    }
    if (lcd.mode != mode)
        hal_log("VMULCD mode=%s frame=%lu scene=%08lx level=%d\n", vmulcd_mode_name[lcd.mode],
                (unsigned long)hal_frame_count, (unsigned long)gCallbackQueue[1], g.level);
    for (p = 0; p < 4; p++)
        for (u = 1; u <= 2; u++) {
            int slot = p * 2 + u - 1;
            maple_device_t *dev = maple_enum_dev(p, u);
            if (!dev || !(dev->info.functions & MAPLE_FUNC_LCD)) {
                vmulcd_gone(&lcd, slot); /* resend the current image when one is plugged in */
                continue;
            }
            if (vmulcd_due(&lcd, slot, hal_frame_count)) send_image(dev, slot);
        }
    dt = (u32)(hal_now_us() - t0);
    stats.us += dt;
    if (dt > stats.worst_us) stats.worst_us = dt;
#ifdef HAL_FAST_SERIAL /* scripted runs: a summary every 1200 frames */
    if (++stats.frames == 1200) {
        hal_log("VMULCD stats frames=%lu cost=%luus/frame worst=%luus changes=%lu sends=%lu busy=%lu failed=%lu "
                "conversions=%lu mode_switches=%lu\n",
                (unsigned long)hal_frame_count, (unsigned long)(stats.us / stats.frames), (unsigned long)stats.worst_us,
                (unsigned long)lcd.changes, (unsigned long)stats.sends, (unsigned long)stats.busy,
                (unsigned long)stats.failed, (unsigned long)vmulcd_cache_conversions, (unsigned long)lcd.mode_switches);
        stats.frames = stats.us = stats.worst_us = 0;
    }
#endif
}

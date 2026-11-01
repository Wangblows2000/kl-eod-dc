/* VMU LCD character display: the parts that do not touch the Dreamcast (sprite conversion, the
 * frame cache, the game's animation sequencing, the Moo walk, the choice between gameplay and
 * menus, and the send schedule). Compiled into the port (hal_vmu_lcd.c drives it) and into the
 * host test tools/vmu_lcd_test.c. Nothing here reads game RAM by itself: GBA addresses go through
 * the resolver the caller provides, so the host test can serve them from baserom.gba.
 */
#ifndef KLONOA_DC_HAL_VMU_LCD_CORE_H
#define KLONOA_DC_HAL_VMU_LCD_CORE_H

#include <stdint.h>

#define VMULCD_W 48
#define VMULCD_H 32
#define VMULCD_BYTES (VMULCD_W * VMULCD_H / 8) /* 192: 6 bytes per row, top row first, MSB = left */

/* A palette colour is "ink" (a dark LCD pixel) when its BT.601 luma, on a 0-255 scale, is below
 * this. Transparent (colour 0) is never ink. */
#define VMULCD_INK_THRESHOLD 100

/* Character sprites are one 32x32 4bpp OBJ (16 tiles, 1D mapping): 0x200 bytes per frame. */
#define VMULCD_SPRITE_BYTES 0x200
#define VMULCD_SPRITE_X 8 /* Klonoa's 32x32 box sits centred: columns 8-39, rows 0-31 (1:1) */

/* Never write a VMU screen more often than every VMULCD_MIN_INTERVAL game frames (30 Hz). */
#define VMULCD_MIN_INTERVAL 2
#define VMULCD_SLOTS 8 /* 4 maple ports x 2 expansion sockets */

/* ---- game data ------------------------------------------------------------------------------- */
/* Animation tables (struct EntityAnimationData / EntityAnimationFrameData in kleod's
 * include/structs/variables.h). Klonoa is animation slot 0; its table pointer gUnk_03005418 is one
 * of these three (normal levels, hover-board levels, the hover-board goal). */
#define VMULCD_KLONOA_ANIMS_NORMAL 0x0805553Cu
#define VMULCD_KLONOA_ANIMS_BOARD 0x080555A8u
#define VMULCD_KLONOA_ANIMS_GOAL 0x08055614u
#define VMULCD_KLONOA_PALETTE 0x08077E28u /* OBJ palette 0 (LoadEntityGraphics_Common) */
/* The Moo: enemy animation table shared by every level (slots 10-13 of Vision 1-1), and its
 * palette (LoadEntityGraphics_World1Level1, OBJ palette 2). */
#define VMULCD_MOO_FRAMES 0x081898E0u
#define VMULCD_MOO_PALETTE 0x08078328u
#define VMULCD_MOO_STATE_LOOK 0 /* stop and look around (9 frames), then "goto 1" */
#define VMULCD_MOO_STATE_WALK 1 /* walk cycle: 4 frames x 8 game frames, looping */

/* Frame-list sentinels (UpdateEntityAnimationInfoEntries): -1 loop, -2 hide, -3 hold; values
 * below 10000 jump to that state. */
#define VMULCD_SRC_LOOP 0xFFFFFFFFu
#define VMULCD_SRC_HIDE 0xFFFFFFFEu
#define VMULCD_SRC_HOLD 0xFFFFFFFDu
#define VMULCD_SRC_IS_FRAME(s) ((s) >= 10000u && (s) < 0xFFFFFFF0u)

/* Maps a GBA address to readable memory, or NULL when [addr, addr+len) is not mapped. */
typedef const void *(*vmulcd_resolve_fn)(uint32_t addr, uint32_t len);
extern vmulcd_resolve_fn vmulcd_resolve;

uint32_t vmulcd_rd32(uint32_t addr); /* 0 when unmapped */
uint16_t vmulcd_rd16(uint32_t addr);
uint8_t vmulcd_rd8(uint32_t addr);

/* ---- conversion ------------------------------------------------------------------------------ */
typedef struct {
    uint32_t row[32]; /* bit 31 = leftmost pixel */
} vmulcd_sprite_t;

uint16_t vmulcd_ink_mask(uint32_t palette_addr); /* bit i set: colour i is ink */
void vmulcd_convert(const uint8_t *tiles, uint16_t ink, vmulcd_sprite_t *out);
/* Clear out[] and draw the sprite with its 32x32 box's left edge at column x (0-16). */
void vmulcd_compose(uint8_t *out, const vmulcd_sprite_t *s, int x, int hflip);

/* Converted frames are cached by source address (and palette); a frame is converted once. */
#define VMULCD_CACHE_SIZE 64
const vmulcd_sprite_t *vmulcd_cache_get(uint32_t src, uint32_t palette_addr);
void vmulcd_cache_reset(void);
extern uint32_t vmulcd_cache_conversions; /* frames converted so far (statistics, tests) */

/* ---- animation sequencing: one slot of the game's UpdateEntityAnimationInfoEntries ------------- */
struct vmulcd_anim {
    uint32_t frames; /* GBA address of the state -> frame-list pointer table */
    uint8_t state, timer, frame;
    uint8_t hidden; /* reached a -2 entry */
};
void vmulcd_anim_set(struct vmulcd_anim *a, uint32_t frames, uint8_t state); /* SetEntityAnimationInfoState */
int vmulcd_anim_tick(struct vmulcd_anim *a); /* 1 when a new frame was selected */
uint32_t vmulcd_anim_src(const struct vmulcd_anim *a); /* current frame's tile data, 0 if none */

/* ---- the menu Moo ---------------------------------------------------------------------------- */
/* Patrols the screen at the game's Moo speed with the game's walk and look-around animations,
 * facing where it walks (Moo frames face right unflipped; the game sets hFlip to walk left). */
struct vmulcd_moo {
    struct vmulcd_anim anim;
    int x16; /* box left edge, 1/16 px */
    int dir; /* -1 left, +1 right */
};
#define VMULCD_MOO_SPEED16 8 /* 0.5 px per game frame, as Vision 1-1's Moos walk */
#define VMULCD_MOO_XMAX (VMULCD_W - 32)
void vmulcd_moo_reset(struct vmulcd_moo *m);
void vmulcd_moo_tick(struct vmulcd_moo *m);

/* ---- mode ------------------------------------------------------------------------------------ */
enum { VMULCD_MODE_NONE, VMULCD_MODE_MENU, VMULCD_MODE_GAMEPLAY };
extern const char *const vmulcd_mode_name[3];

/* The scene handler (gCallbackQueue.current[1]), classified by the port. */
enum {
    VMULCD_SCENE_OTHER,   /* level loop, room rotation, boss scenes, death fade...: gameplay if Klonoa is live */
    VMULCD_SCENE_MENU,    /* title, file select, world map, vision select, cutscene, pause, game over */
    VMULCD_SCENE_OVERLAY, /* text box: drawn over whatever was running, keeps the current mode */
};

/* What the port reads from the game each frame. */
struct vmulcd_game {
    int scene;          /* VMULCD_SCENE_* */
    int level;          /* gUnk_03004C20.level: 0 = vision select / no level */
    int klonoa_entity;  /* gEntityInfo[0].id == ENTITY_ID_KLONOA */
    uint32_t anims;     /* gUnk_03005418 */
    uint8_t state, frame; /* gEntityAnimationInfo[0] */
    int hflip;          /* as the game sets OAM hFlip for entity 0 */
};
int vmulcd_choose_mode(const struct vmulcd_game *g, int current_mode);
/* Klonoa's current frame: pFrames[state][frame].src, or 0 when the slot holds no frame yet. */
uint32_t vmulcd_klonoa_src(const struct vmulcd_game *g);

/* ---- the display ----------------------------------------------------------------------------- */
struct vmulcd {
    int mode;
    uint32_t src, palette; /* what is drawn */
    int x, hflip;
    struct vmulcd_moo moo;
    uint32_t gen; /* bumped whenever image[] changes */
    uint8_t image[VMULCD_BYTES] __attribute__((aligned(4)));
    /* send schedule, per maple slot */
    uint32_t sent_gen[VMULCD_SLOTS];
    uint32_t sent_frame[VMULCD_SLOTS];
    uint32_t changes, mode_switches;
};
void vmulcd_init(struct vmulcd *d);
/* One game frame: pick what to show, redraw when it changed. Returns 1 when image[] changed. */
int vmulcd_update(struct vmulcd *d, const struct vmulcd_game *g);
/* Whether slot should be sent the current image at game frame `now`; mark it sent / gone. */
int vmulcd_due(const struct vmulcd *d, int slot, uint32_t now);
void vmulcd_sent(struct vmulcd *d, int slot, uint32_t now);
void vmulcd_gone(struct vmulcd *d, int slot);

#endif

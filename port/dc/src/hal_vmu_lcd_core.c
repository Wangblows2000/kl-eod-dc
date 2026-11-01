/* VMU LCD character display: platform-independent part (see hal_vmu_lcd_core.h). */
#include <string.h>

#include "hal_vmu_lcd_core.h"

vmulcd_resolve_fn vmulcd_resolve;
uint32_t vmulcd_cache_conversions;
const char *const vmulcd_mode_name[3] = {"none", "menu", "gameplay"};

uint32_t vmulcd_rd32(uint32_t addr) {
    const uint8_t *p = vmulcd_resolve ? vmulcd_resolve(addr, 4) : 0;
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}

uint16_t vmulcd_rd16(uint32_t addr) {
    const uint8_t *p = vmulcd_resolve ? vmulcd_resolve(addr, 2) : 0;
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
}

uint8_t vmulcd_rd8(uint32_t addr) {
    const uint8_t *p = vmulcd_resolve ? vmulcd_resolve(addr, 1) : 0;
    return p ? p[0] : 0;
}

/* ---- conversion ------------------------------------------------------------------------------ */

uint16_t vmulcd_ink_mask(uint32_t palette_addr) {
    uint16_t mask = 0;
    int i;
    for (i = 1; i < 16; i++) {
        uint16_t c = vmulcd_rd16(palette_addr + 2 * i);
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        uint32_t luma = (299 * r + 587 * g + 114 * b) * 255 / (1000 * 31); /* BT.601, 0-255 */
        if (luma < VMULCD_INK_THRESHOLD) mask |= 1u << i;
    }
    return mask;
}

void vmulcd_convert(const uint8_t *tiles, uint16_t ink, vmulcd_sprite_t *out) {
    int t, y, x;
    memset(out, 0, sizeof *out);
    for (t = 0; t < 16; t++) { /* 4x4 tiles, row-major (1D OBJ mapping) */
        int tx = (t & 3) * 8, ty = (t >> 2) * 8;
        for (y = 0; y < 8; y++) {
            const uint8_t *p = tiles + t * 32 + y * 4;
            uint32_t bits = 0;
            for (x = 0; x < 8; x++) {
                int c = (p[x >> 1] >> ((x & 1) * 4)) & 15; /* low nibble = left pixel */
                if ((ink >> c) & 1) bits |= 0x80u >> x;
            }
            out->row[ty + y] |= bits << (24 - tx);
        }
    }
}

static uint32_t bitrev32(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

void vmulcd_compose(uint8_t *out, const vmulcd_sprite_t *s, int x, int hflip) {
    int y, k;
    if (x < 0) x = 0;
    if (x > VMULCD_W - 32) x = VMULCD_W - 32;
    for (y = 0; y < VMULCD_H; y++) {
        uint32_t r = hflip ? bitrev32(s->row[y]) : s->row[y];
        uint64_t v = ((uint64_t)r << 16) >> x; /* bit 47 = column 0 */
        for (k = 0; k < 6; k++) out[y * 6 + k] = (uint8_t)(v >> (40 - 8 * k));
    }
}

/* ---- cache ----------------------------------------------------------------------------------- */

static struct {
    uint32_t src, palette, sum;
    vmulcd_sprite_t sprite;
} cache[VMULCD_CACHE_SIZE];
static int cache_next;

void vmulcd_cache_reset(void) {
    memset(cache, 0, sizeof cache);
    cache_next = 0;
}

static int in_rom(uint32_t a) { return a >= 0x08000000u && a < 0x0A000000u; }

const vmulcd_sprite_t *vmulcd_cache_get(uint32_t src, uint32_t palette_addr) {
    const uint8_t *tiles = vmulcd_resolve ? vmulcd_resolve(src, VMULCD_SPRITE_BYTES) : 0;
    uint32_t sum = 0;
    int i;
    if (!tiles) return 0;
    if (!in_rom(src)) { /* decompressed into RAM: the same address can hold another frame later */
        for (i = 0; i < VMULCD_SPRITE_BYTES; i++) sum = sum * 31 + tiles[i];
        sum |= 1;
    }
    for (i = 0; i < VMULCD_CACHE_SIZE; i++)
        if (cache[i].src == src && cache[i].palette == palette_addr && cache[i].sum == sum) return &cache[i].sprite;
    i = cache_next;
    cache_next = (cache_next + 1) % VMULCD_CACHE_SIZE;
    cache[i].src = src;
    cache[i].palette = palette_addr;
    cache[i].sum = sum;
    vmulcd_convert(tiles, vmulcd_ink_mask(palette_addr), &cache[i].sprite);
    vmulcd_cache_conversions++;
    return &cache[i].sprite;
}

/* ---- animation sequencing (kleod src/anim.c, UpdateEntityAnimationInfoEntries) --------------- */

void vmulcd_anim_set(struct vmulcd_anim *a, uint32_t frames, uint8_t state) {
    a->frames = frames;
    a->state = state;
    a->timer = 1;
    a->frame = 0xFF;
    a->hidden = 0;
}

int vmulcd_anim_tick(struct vmulcd_anim *a) {
    uint32_t list, src;
    if (a->timer == 0xFF) return 0;
    if (--a->timer != 0) return 0;
    list = vmulcd_rd32(a->frames + 4 * a->state);
    a->frame++;
    src = vmulcd_rd32(list + 8 * a->frame);
    if (src == VMULCD_SRC_LOOP) {
        a->frame = 0;
    } else if (src == VMULCD_SRC_HIDE) {
        a->timer = 0xFF;
        a->hidden = 1;
        return 0;
    } else if (src > 9999) {
        if (src == VMULCD_SRC_HOLD) {
            a->timer = 0xFF;
            return 0;
        }
    } else {
        a->state = (uint8_t)src;
        a->frame = 0;
        list = vmulcd_rd32(a->frames + 4 * a->state);
    }
    a->timer = vmulcd_rd8(list + 8 * a->frame + 4);
    return 1;
}

uint32_t vmulcd_anim_src(const struct vmulcd_anim *a) {
    uint32_t src;
    if (a->frame == 0xFF || a->state == 0xFF) return 0;
    src = vmulcd_rd32(vmulcd_rd32(a->frames + 4 * a->state) + 8 * a->frame);
    return VMULCD_SRC_IS_FRAME(src) ? src : 0;
}

/* ---- the menu Moo ---------------------------------------------------------------------------- */

void vmulcd_moo_reset(struct vmulcd_moo *m) {
    vmulcd_anim_set(&m->anim, VMULCD_MOO_FRAMES, VMULCD_MOO_STATE_WALK);
    m->x16 = VMULCD_MOO_XMAX * 16; /* starts at the right edge, walking left */
    m->dir = -1;
}

/* What a Moo in Vision 1-1 does (measured, docs/vmu-lcd.md): walk (state 1, 0.5 px per frame);
 * at a ledge stop and look around (state 0, which hands back to state 1 by itself), then turn;
 * at a wall turn on the spot. Here the left edge of the screen is the ledge, the right the wall. */
void vmulcd_moo_tick(struct vmulcd_moo *m) {
    uint8_t before = m->anim.state;
    vmulcd_anim_tick(&m->anim);
    if (m->anim.state != VMULCD_MOO_STATE_WALK) return; /* looking around: stands still */
    if (before == VMULCD_MOO_STATE_LOOK) m->dir = -m->dir; /* done looking: turn round */
    m->x16 += m->dir * VMULCD_MOO_SPEED16;
    if (m->x16 <= 0) {
        m->x16 = 0;
        vmulcd_anim_set(&m->anim, VMULCD_MOO_FRAMES, VMULCD_MOO_STATE_LOOK);
        vmulcd_anim_tick(&m->anim);
    } else if (m->x16 >= VMULCD_MOO_XMAX * 16) {
        m->x16 = VMULCD_MOO_XMAX * 16;
        m->dir = -1;
    }
}

/* ---- mode ------------------------------------------------------------------------------------ */

static int klonoa_table(uint32_t a) {
    return a == VMULCD_KLONOA_ANIMS_NORMAL || a == VMULCD_KLONOA_ANIMS_BOARD || a == VMULCD_KLONOA_ANIMS_GOAL;
}

int vmulcd_choose_mode(const struct vmulcd_game *g, int current_mode) {
    int klonoa_live = g->level != 0 && g->klonoa_entity && klonoa_table(g->anims) && g->state != 0xFF;
    if (g->scene == VMULCD_SCENE_MENU || !klonoa_live) return VMULCD_MODE_MENU;
    if (g->scene == VMULCD_SCENE_OVERLAY && current_mode != VMULCD_MODE_GAMEPLAY) return VMULCD_MODE_MENU;
    return VMULCD_MODE_GAMEPLAY;
}

uint32_t vmulcd_klonoa_src(const struct vmulcd_game *g) {
    uint32_t src;
    if (g->state == 0xFF || g->frame == 0xFF || !klonoa_table(g->anims)) return 0;
    src = vmulcd_rd32(vmulcd_rd32(vmulcd_rd32(g->anims) + 4 * g->state) + 8 * g->frame);
    return VMULCD_SRC_IS_FRAME(src) ? src : 0;
}

/* ---- the display ----------------------------------------------------------------------------- */

void vmulcd_init(struct vmulcd *d) {
    memset(d, 0, sizeof *d);
    vmulcd_moo_reset(&d->moo);
}

int vmulcd_update(struct vmulcd *d, const struct vmulcd_game *g) {
    int mode = vmulcd_choose_mode(g, d->mode), x, hflip;
    uint32_t src, palette;
    uint8_t img[VMULCD_BYTES];
    const vmulcd_sprite_t *s;

    if (mode != d->mode) {
        if (d->mode != VMULCD_MODE_NONE) d->mode_switches++;
        if (mode == VMULCD_MODE_MENU) vmulcd_moo_reset(&d->moo);
        d->mode = mode;
        d->src = 0; /* redraw */
    }
    if (mode == VMULCD_MODE_GAMEPLAY) {
        src = vmulcd_klonoa_src(g);
        palette = VMULCD_KLONOA_PALETTE;
        x = VMULCD_SPRITE_X;
        hflip = g->hflip;
    } else {
        vmulcd_moo_tick(&d->moo);
        src = vmulcd_anim_src(&d->moo.anim);
        palette = VMULCD_MOO_PALETTE;
        x = d->moo.x16 >> 4;
        hflip = d->moo.dir < 0; /* Moo frames face right */
    }
    if (!src) return 0; /* no frame selected yet: keep what is shown */
    if (src == d->src && palette == d->palette && x == d->x && hflip == d->hflip) return 0;
    s = vmulcd_cache_get(src, palette);
    if (!s) return 0;
    d->src = src;
    d->palette = palette;
    d->x = x;
    d->hflip = hflip;
    vmulcd_compose(img, s, x, hflip);
    if (d->gen && !memcmp(img, d->image, sizeof img)) return 0;
    memcpy(d->image, img, sizeof img);
    d->gen++;
    d->changes++;
    return 1;
}

int vmulcd_due(const struct vmulcd *d, int slot, uint32_t now) {
    if (!d->gen || d->sent_gen[slot] == d->gen) return 0;
    return d->sent_gen[slot] == 0 || now - d->sent_frame[slot] >= VMULCD_MIN_INTERVAL;
}

void vmulcd_sent(struct vmulcd *d, int slot, uint32_t now) {
    d->sent_gen[slot] = d->gen;
    d->sent_frame[slot] = now;
}

void vmulcd_gone(struct vmulcd *d, int slot) { d->sent_gen[slot] = 0; }

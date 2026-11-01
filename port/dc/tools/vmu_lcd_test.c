/* Host test of the VMU LCD character display (src/hal_vmu_lcd_core.c), against the ROM's own
 * sprite and animation data.
 *
 *   make test-vmu-lcd            (or: cc -O2 -I src src/hal_vmu_lcd_core.c tools/vmu_lcd_test.c)
 *   build/vmu_lcd_test baserom.gba [--sheet out.pgm]
 *
 * Checks: 1-bit conversion (tile layout, nibble order, palette threshold) against a direct per-pixel
 * reference over every Klonoa and Moo frame; placement and mirroring on the 48x32 screen; frame
 * sequencing (the game's UpdateEntityAnimationInfoEntries rules) against a timeline expanded
 * straight from the ROM's frame lists, for every Klonoa state and both Moo states; the menu Moo's
 * walk; the gameplay/menu decision; redraw-only-on-change; the cache; and the send schedule.
 * --sheet writes every converted Klonoa (normal levels) and Moo frame as one PGM image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_vmu_lcd_core.h"

static uint8_t *rom;
static long rom_size;
static uint8_t ewram[0x40000];
static int checks, fails;

static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void check(int ok, const char *fmt, ...) {
    va_list ap;
    checks++;
    if (ok) return;
    fails++;
    va_start(ap, fmt);
    printf("  FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static const void *resolve(uint32_t addr, uint32_t len) {
    if (addr >= 0x08000000u && addr + len <= 0x08000000u + (uint32_t)rom_size) return rom + (addr - 0x08000000u);
    if (addr >= 0x02000000u && addr + len <= 0x02000000u + sizeof ewram) return ewram + (addr - 0x02000000u);
    return NULL;
}

/* ---- independent references ------------------------------------------------------------------ */

/* Colour index of pixel (x, y) of a 32x32 4bpp OBJ in 1D mapping. */
static int ref_pixel(const uint8_t *t, int x, int y) {
    int tile = (y / 8) * 4 + x / 8;
    uint8_t b = t[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
    return x & 1 ? b >> 4 : b & 15;
}

static int ref_ink(uint32_t pal, int c) {
    const uint8_t *p = resolve(pal + 2 * c, 2);
    unsigned v = p[0] | p[1] << 8;
    double luma = (0.299 * (v & 31) + 0.587 * ((v >> 5) & 31) + 0.114 * ((v >> 10) & 31)) / 31.0 * 255.0;
    return c != 0 && luma < VMULCD_INK_THRESHOLD - 1e-9;
}

static int lcd_px(const uint8_t *img, int x, int y) { return (img[y * 6 + x / 8] >> (7 - x % 8)) & 1; }

/* Frame list of one state as (src, duration) steps, following -1 (loop) and "goto" entries, the
 * way the game plays it: each frame shows for its timer; -3 holds the previous frame; -2 hides.
 * Returns the frame shown on each of the first n ticks after SetEntityAnimationInfoState (tick 0
 * is the first Update, which selects frame 0). 0 = nothing new selected yet / hidden. */
static void ref_timeline(uint32_t frames, int state, uint32_t *out, int n) {
    int t = 0, f = 0, guard = 0;
    uint32_t list = vmulcd_rd32(frames + 4 * state), shown = 0;
    while (t < n && guard++ < 100000) {
        uint32_t src = vmulcd_rd32(list + 8 * f);
        int dur;
        if (src == VMULCD_SRC_LOOP) { f = 0; continue; }
        if (src == VMULCD_SRC_HOLD) { while (t < n) out[t++] = shown; return; }
        if (src == VMULCD_SRC_HIDE) { while (t < n) out[t++] = 0; return; }
        if (src < 10000) { state = src; list = vmulcd_rd32(frames + 4 * state); f = 0; continue; }
        dur = vmulcd_rd8(list + 8 * f + 4);
        shown = src;
        while (dur-- > 0 && t < n) out[t++] = src;
        f++;
    }
}

/* Every frame src reachable from a state table (states 0..nstates-1), deduplicated. */
static int collect_frames(uint32_t frames, int nstates, uint32_t *out, int max) {
    int n = 0, s, k, i;
    for (s = 0; s < nstates; s++) {
        uint32_t list = vmulcd_rd32(frames + 4 * s);
        for (k = 0; k < 64; k++) {
            uint32_t src = vmulcd_rd32(list + 8 * k);
            if (!VMULCD_SRC_IS_FRAME(src)) break;
            for (i = 0; i < n && out[i] != src; i++) {}
            if (i == n && n < max && resolve(src, VMULCD_SPRITE_BYTES)) out[n++] = src;
        }
    }
    return n;
}

#define KLONOA_STATES 0x26 /* highest state the game sets for slot 0 is 0x25 */

/* ---- tests ----------------------------------------------------------------------------------- */

static void test_ink(void) {
    int c, pal;
    uint32_t pals[2] = {VMULCD_KLONOA_PALETTE, VMULCD_MOO_PALETTE};
    printf("palette threshold (luma < %d):\n", VMULCD_INK_THRESHOLD);
    for (pal = 0; pal < 2; pal++) {
        uint16_t m = vmulcd_ink_mask(pals[pal]);
        char s[17];
        for (c = 0; c < 16; c++) {
            check(((m >> c) & 1) == ref_ink(pals[pal], c), "ink colour %d of palette %08x", c, pals[pal]);
            s[c] = (m >> c) & 1 ? '#' : '.';
        }
        s[16] = 0;
        printf("  %s %08x: colours 0-15 %s\n", pal ? "Moo   " : "Klonoa", pals[pal], s);
        check(!(m & 1), "transparent colour is never ink");
        check((m >> 1) & 1, "outline colour 1 is ink");
    }
}

static void test_layout(void) {
    uint8_t t[VMULCD_SPRITE_BYTES], img[VMULCD_BYTES];
    vmulcd_sprite_t s;
    int x, y, ok = 1;
    /* Synthetic sprite: colour 1 (ink) on the diagonal and along row 3; colour 2 elsewhere at x=30. */
    memset(t, 0, sizeof t);
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            int c = (x == y || y == 3) ? 1 : x == 30 ? 2 : 0;
            int tile = (y / 8) * 4 + x / 8, o = tile * 32 + (y % 8) * 4 + (x % 8) / 2;
            t[o] |= x & 1 ? c << 4 : c;
        }
    vmulcd_convert(t, 1u << 1, &s);
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) ok &= ((s.row[y] >> (31 - x)) & 1) == (x == y || y == 3);
    check(ok, "synthetic sprite converts pixel-exact (tile order, nibble order, ink only)");
    vmulcd_compose(img, &s, VMULCD_SPRITE_X, 0);
    ok = 1;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 48; x++) {
            int sx = x - VMULCD_SPRITE_X, want = sx >= 0 && sx < 32 && (sx == y || y == 3);
            ok &= lcd_px(img, x, y) == want;
        }
    check(ok, "placed at columns 8-39, rows 0-31, 1:1");
    vmulcd_compose(img, &s, VMULCD_SPRITE_X, 1);
    ok = 1;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 48; x++) {
            int sx = 31 - (x - VMULCD_SPRITE_X), want = x >= 8 && x < 40 && (sx == y || y == 3);
            ok &= lcd_px(img, x, y) == want;
        }
    check(ok, "hFlip mirrors the 32x32 box in place (as OAM hFlip does)");
    vmulcd_compose(img, &s, 0, 0);
    check(lcd_px(img, 0, 0) && lcd_px(img, 31, 31) && !lcd_px(img, 32, 3), "x=0 places the box at columns 0-31");
    vmulcd_compose(img, &s, VMULCD_W - 32, 0);
    check(lcd_px(img, 16, 0) && lcd_px(img, 47, 31) && !lcd_px(img, 15, 3), "x=16 places the box at columns 16-47");
}

static void test_rom_frames(const char *sheet) {
    static uint32_t kl[512], moo[64];
    int nk = collect_frames(vmulcd_rd32(VMULCD_KLONOA_ANIMS_NORMAL), KLONOA_STATES, kl, 512);
    int nm = collect_frames(VMULCD_MOO_FRAMES, 2, moo, 64);
    int i, x, y, bad = 0, ink = 0;
    long px = 0;
    printf("ROM frames: %d Klonoa (normal levels, states 0-0x25), %d Moo (walk + look)\n", nk, nm);
    check(nk > 100, "Klonoa frame set found (%d)", nk);
    check(nm == 8, "Moo walk + look frames found (%d)", nm);
    for (i = 0; i < nk + nm; i++) {
        uint32_t src = i < nk ? kl[i] : moo[i - nk], pal = i < nk ? VMULCD_KLONOA_PALETTE : VMULCD_MOO_PALETTE;
        const uint8_t *t = resolve(src, VMULCD_SPRITE_BYTES);
        vmulcd_sprite_t s;
        vmulcd_convert(t, vmulcd_ink_mask(pal), &s);
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                int want = ref_ink(pal, ref_pixel(t, x, y)), got = (s.row[y] >> (31 - x)) & 1;
                bad += want != got;
                ink += got;
                px++;
            }
    }
    check(bad == 0, "%d pixels differ from the per-pixel reference", bad);
    printf("  %ld pixels converted, %d mismatches, %.1f%% ink\n", px, bad, 100.0 * ink / px);
    if (sheet) { /* contact sheet: 12 frames per row, each on a 48x32 screen, 2 px gutters */
        int cols = 12, n = nk + nm, rows = (n + cols - 1) / cols, W = cols * 50, H = rows * 34;
        uint8_t *im = malloc(W * H);
        FILE *f = fopen(sheet, "wb");
        memset(im, 128, W * H);
        for (i = 0; i < n; i++) {
            uint8_t img[VMULCD_BYTES];
            uint32_t src = i < nk ? kl[i] : moo[i - nk], pal = i < nk ? VMULCD_KLONOA_PALETTE : VMULCD_MOO_PALETTE;
            vmulcd_compose(img, vmulcd_cache_get(src, pal), VMULCD_SPRITE_X, 0);
            for (y = 0; y < 32; y++)
                for (x = 0; x < 48; x++) im[((i / cols) * 34 + y) * W + (i % cols) * 50 + x] = lcd_px(img, x, y) ? 40 : 205;
        }
        fprintf(f, "P5\n%d %d\n255\n", W, H);
        fwrite(im, 1, W * H, f);
        fclose(f);
        free(im);
        printf("  wrote %s\n", sheet);
    }
}

static void test_sequencing(void) {
    static uint32_t want[2000];
    uint32_t kfr = vmulcd_rd32(VMULCD_KLONOA_ANIMS_NORMAL);
    int s, t, total = 0, bad = 0, which;
    for (which = 0; which < 2; which++) {
        uint32_t frames = which ? VMULCD_MOO_FRAMES : kfr;
        int nst = which ? 2 : KLONOA_STATES;
        for (s = 0; s < nst; s++) {
            struct vmulcd_anim a;
            uint32_t shown = 0;
            if (!VMULCD_SRC_IS_FRAME(vmulcd_rd32(vmulcd_rd32(frames + 4 * s)))) continue;
            ref_timeline(frames, s, want, 2000);
            vmulcd_anim_set(&a, frames, s);
            for (t = 0; t < 2000; t++) {
                vmulcd_anim_tick(&a);
                if (a.hidden) shown = 0;
                else if (vmulcd_anim_src(&a)) shown = vmulcd_anim_src(&a);
                if (shown != want[t]) {
                    if (!bad) printf("  first mismatch: %s state %d tick %d: %08x, timeline %08x\n",
                                     which ? "Moo" : "Klonoa", s, t, shown, want[t]);
                    bad++;
                    break;
                }
            }
            total++;
        }
    }
    check(bad == 0, "frame sequence and timing match the ROM timelines (%d of %d states differ)", bad, total);
    printf("frame sequencing: %d states x 2000 ticks checked against the ROM frame lists\n", total);
    /* The timings measured on Vision 1-1's Moos in Flycast (docs/vmu-lcd.md). */
    {
        uint32_t walk = vmulcd_rd32(VMULCD_MOO_FRAMES + 4 * VMULCD_MOO_STATE_WALK);
        uint32_t look = vmulcd_rd32(VMULCD_MOO_FRAMES + 4 * VMULCD_MOO_STATE_LOOK);
        int k, ok = 1;
        for (k = 0; k < 4; k++) ok &= vmulcd_rd8(walk + 8 * k + 4) == 8;
        ok &= vmulcd_rd32(walk + 32) == VMULCD_SRC_LOOP;
        check(ok, "Moo walk: 4 frames x 8 game frames, looping");
        check(vmulcd_rd32(look + 8 * 9) == VMULCD_MOO_STATE_WALK, "Moo look-around ends in 'goto walk'");
    }
}

static void test_moo(void) {
    struct vmulcd_moo m;
    int t, ok_range = 1, ok_face = 1, ok_still = 1, ok_speed = 1, looks = 0, wall_turns = 0, prev_dir;
    int prev_x16, walk_frames_seen = 0;
    uint32_t prev_src = 0;
    vmulcd_moo_reset(&m);
    vmulcd_moo_tick(&m);
    check(m.anim.state == VMULCD_MOO_STATE_WALK && m.anim.frame == 0, "menu Moo starts on walk frame 0");
    check(m.x16 >> 4 == VMULCD_MOO_XMAX - 0 || m.x16 >> 4 == VMULCD_MOO_XMAX - 1, "menu Moo starts at the right edge");
    prev_dir = m.dir;
    prev_x16 = m.x16;
    for (t = 0; t < 3000; t++) {
        uint8_t st_before = m.anim.state;
        vmulcd_moo_tick(&m);
        ok_range &= m.x16 >= 0 && m.x16 <= VMULCD_MOO_XMAX * 16;
        if (m.anim.state == VMULCD_MOO_STATE_LOOK && st_before == VMULCD_MOO_STATE_LOOK) ok_still &= m.x16 == prev_x16;
        if (m.anim.state == VMULCD_MOO_STATE_WALK && st_before == VMULCD_MOO_STATE_WALK && m.dir == prev_dir &&
            m.x16 > 0 && m.x16 < VMULCD_MOO_XMAX * 16)
            ok_speed &= m.x16 - prev_x16 == m.dir * VMULCD_MOO_SPEED16;
        if (m.anim.state == VMULCD_MOO_STATE_LOOK && st_before != VMULCD_MOO_STATE_LOOK) {
            looks++;
            ok_face &= m.x16 == 0 && m.dir < 0; /* stops at the left edge, still facing left */
        }
        if (m.dir != prev_dir && st_before == VMULCD_MOO_STATE_WALK && m.x16 == VMULCD_MOO_XMAX * 16) wall_turns++;
        if (m.anim.state == VMULCD_MOO_STATE_WALK && vmulcd_anim_src(&m.anim) != prev_src) walk_frames_seen++;
        prev_src = vmulcd_anim_src(&m.anim);
        prev_dir = m.dir;
        prev_x16 = m.x16;
    }
    check(ok_range, "menu Moo stays on screen (box x 0-16)");
    check(ok_speed, "menu Moo walks 0.5 px per frame");
    check(ok_still, "menu Moo stands still while looking around");
    check(ok_face, "menu Moo looks around at the left edge");
    check(looks >= 10 && wall_turns >= 10, "menu Moo patrols: %d look-arounds, %d wall turns in 3000 frames", looks,
          wall_turns);
    check(walk_frames_seen > 50, "menu Moo walk frames advance (%d changes)", walk_frames_seen);
}

static struct vmulcd_game gameplay(uint8_t state, uint8_t frame, int flip) {
    struct vmulcd_game g = {0, 1, 1, VMULCD_KLONOA_ANIMS_NORMAL, state, frame, flip};
    return g;
}

static void test_mode(void) {
    const int G = VMULCD_MODE_GAMEPLAY, M = VMULCD_MODE_MENU;
    struct vmulcd_game g = gameplay(0, 0, 0);
    check(vmulcd_choose_mode(&g, M) == G, "level, Klonoa live, level scene: gameplay");
    g.scene = VMULCD_SCENE_MENU;
    check(vmulcd_choose_mode(&g, G) == M, "menu scene handler (pause, title...): menu");
    g.scene = VMULCD_SCENE_OVERLAY;
    check(vmulcd_choose_mode(&g, G) == G, "text box during gameplay: stays gameplay");
    check(vmulcd_choose_mode(&g, M) == M, "text box on the vision select (level already chosen): stays menu");
    check(vmulcd_choose_mode(&g, VMULCD_MODE_NONE) == M, "text box at boot: menu");
    g = gameplay(0, 0, 0), g.level = 0;
    check(vmulcd_choose_mode(&g, G) == M, "no level (vision select): menu");
    g = gameplay(0, 0, 0), g.klonoa_entity = 0;
    check(vmulcd_choose_mode(&g, G) == M, "entity 0 not Klonoa: menu");
    g = gameplay(0, 0, 0), g.anims = 0x08189A24;
    check(vmulcd_choose_mode(&g, G) == M, "slot 0 table not one of Klonoa's: menu");
    g = gameplay(0xFF, 0, 0);
    check(vmulcd_choose_mode(&g, G) == M, "animation slot unused (state 0xFF): menu");
    g = gameplay(0, 0, 0), g.anims = VMULCD_KLONOA_ANIMS_BOARD;
    check(vmulcd_choose_mode(&g, M) == G, "hover-board Klonoa table: gameplay");
}

/* Drive the display with a model of the game's slot-0 animation: menu -> gameplay (idle, run,
 * turn round) -> pause (menu) -> gameplay. */
static void test_display(void) {
    struct vmulcd d;
    struct vmulcd_anim kl;
    struct vmulcd_game g;
    uint8_t expect[VMULCD_BYTES];
    uint32_t kfr = vmulcd_rd32(VMULCD_KLONOA_ANIMS_NORMAL), gen;
    int t, redraws = 0, extra = 0, stale = 0;
    uint32_t conv0;
    vmulcd_cache_reset();
    conv0 = vmulcd_cache_conversions;
    vmulcd_init(&d);
    g = gameplay(0, 0, 0);
    g.scene = VMULCD_SCENE_MENU;
    for (t = 0; t < 120; t++) vmulcd_update(&d, &g);
    check(d.mode == VMULCD_MODE_MENU && d.palette == VMULCD_MOO_PALETTE && d.gen > 0, "menu shows the Moo");
    /* gameplay: idle (state 0) for 200 frames, run right (1) 200, run left 200 */
    vmulcd_anim_set(&kl, kfr, 0);
    for (t = 0; t < 600; t++) {
        uint32_t src;
        if (t == 200) vmulcd_anim_set(&kl, kfr, 1);
        vmulcd_anim_tick(&kl);
        g = gameplay(kl.state, kl.frame, t >= 400);
        gen = d.gen;
        vmulcd_update(&d, &g);
        src = vmulcd_anim_src(&kl);
        vmulcd_compose(expect, vmulcd_cache_get(src, VMULCD_KLONOA_PALETTE), VMULCD_SPRITE_X, t >= 400);
        stale += memcmp(expect, d.image, VMULCD_BYTES) != 0;
        if (d.gen != gen) redraws++;
        else if (t == 400) extra++; /* the turn must redraw */
    }
    check(stale == 0, "gameplay: the LCD always shows the game's current Klonoa frame and facing (%d stale)", stale);
    /* idle: 4 frames x 20 -> 10 changes in 200 frames (+1 from the Moo); run: 8 frames x 4 */
    check(redraws >= 10 + 50 + 50 && redraws <= 1 + 10 + 50 + 51, "redraws only when the frame changes (%d)", redraws);
    check(extra == 0, "turning round redraws at once");
    check(vmulcd_cache_conversions - conv0 <= 4 + 8 + 8, "each frame converted once (%u conversions)",
          vmulcd_cache_conversions - conv0);
    /* pause: menu again; the Moo restarts at the right edge on walk frame 0 */
    g.scene = VMULCD_SCENE_MENU;
    vmulcd_update(&d, &g);
    check(d.mode == VMULCD_MODE_MENU && d.palette == VMULCD_MOO_PALETTE && d.x >= VMULCD_MOO_XMAX - 1 &&
              d.moo.anim.state == VMULCD_MOO_STATE_WALK && d.moo.anim.frame == 0,
          "pause: switches to the Moo in the same frame, from its first walk frame");
    check(d.mode_switches == 2, "mode switches counted (%u)", d.mode_switches);
    /* frame 0xFF (just after SetEntityAnimationInfoState): keep showing the last frame */
    g = gameplay(1, 0xFF, 0);
    gen = d.gen;
    vmulcd_update(&d, &g);
    check(d.mode == VMULCD_MODE_GAMEPLAY && d.gen == gen, "no frame selected yet: nothing redrawn");
    g = gameplay(1, 0, 0);
    vmulcd_update(&d, &g);
    check(d.gen == gen + 1 && d.palette == VMULCD_KLONOA_PALETTE, "first selected frame shown");
}

static void test_ram_frames(void) {
    const uint8_t *src = resolve(0x0805C4E8, VMULCD_SPRITE_BYTES);
    const vmulcd_sprite_t *a, *b;
    vmulcd_sprite_t ca;
    uint32_t n;
    vmulcd_cache_reset();
    memcpy(ewram + 0x1000, src, VMULCD_SPRITE_BYTES);
    n = vmulcd_cache_conversions;
    a = vmulcd_cache_get(0x02001000, VMULCD_KLONOA_PALETTE);
    ca = *a;
    b = vmulcd_cache_get(0x02001000, VMULCD_KLONOA_PALETTE);
    check(vmulcd_cache_conversions == n + 1, "RAM frame cached while unchanged");
    memset(ewram + 0x1000, 0x11, VMULCD_SPRITE_BYTES);
    b = vmulcd_cache_get(0x02001000, VMULCD_KLONOA_PALETTE);
    check(vmulcd_cache_conversions == n + 2 && b->row[0] == 0xFFFFFFFFu && memcmp(&ca, b, sizeof ca),
          "new data at the same RAM address is converted again");
    check(vmulcd_cache_get(0x0A000000, VMULCD_KLONOA_PALETTE) == NULL, "unmapped source: no frame");
}

static void test_schedule(void) {
    struct vmulcd d;
    uint32_t now, sends[2] = {0, 0}, last[2] = {0, 0};
    int ok_rate = 1, slot;
    memset(&d, 0, sizeof d);
    /* the image changes every frame for 100 frames */
    for (now = 1; now <= 100; now++) {
        d.gen++;
        for (slot = 0; slot < 2; slot++)
            if (vmulcd_due(&d, slot, now)) {
                if (sends[slot]) ok_rate &= now - last[slot] >= VMULCD_MIN_INTERVAL;
                vmulcd_sent(&d, slot, now);
                sends[slot]++;
                last[slot] = now;
            }
    }
    check(ok_rate && sends[0] == 50 && sends[1] == 50, "at most one write per %d frames per VMU (%u, %u writes)",
          VMULCD_MIN_INTERVAL, sends[0], sends[1]);
    /* changes stop: the latest image still reaches each VMU within the interval */
    for (; now <= 103; now++)
        for (slot = 0; slot < 2; slot++)
            if (vmulcd_due(&d, slot, now)) vmulcd_sent(&d, slot, now);
    check(d.sent_gen[0] == d.gen && d.sent_gen[1] == d.gen, "the last image is always delivered (no stale screen)");
    check(!vmulcd_due(&d, 0, now + 50), "unchanged image is not resent");
    vmulcd_gone(&d, 1);
    check(vmulcd_due(&d, 1, now) && !vmulcd_due(&d, 0, now), "a VMU plugged in again gets the current image at once");
    memset(&d, 0, sizeof d);
    check(!vmulcd_due(&d, 0, 5), "nothing sent before the first image");
}

int main(int argc, char **argv) {
    FILE *f;
    const char *sheet = NULL;
    int i;
    if (argc < 2) {
        fprintf(stderr, "usage: %s baserom.gba [--sheet out.pgm]\n", argv[0]);
        return 2;
    }
    for (i = 2; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--sheet")) sheet = argv[i + 1];
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc(rom_size);
    if (fread(rom, 1, rom_size, f) != (size_t)rom_size) return 2;
    fclose(f);
    vmulcd_resolve = resolve;

    test_ink();
    test_layout();
    test_rom_frames(sheet);
    test_sequencing();
    test_moo();
    test_mode();
    test_display();
    test_ram_frames();
    test_schedule();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

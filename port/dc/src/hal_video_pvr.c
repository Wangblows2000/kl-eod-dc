/* PowerVR renderer for the GBA picture.
 *
 * The GBA's own data is handed to the PowerVR rather than computing pixels on the CPU:
 *
 *   BG layers   Each text or affine BG is mirrored into an 8bpp twiddled "map texture" the size of
 *               the whole BG (text: 256/512 square or rectangular; affine: 128-1024 square). A text
 *               BG texel is (palette bank << 4 | colour), so per-tile 4bpp palettes survive the
 *               conversion; 8bpp BGs store the colour index directly. Only map entries whose value
 *               or character data changed are rebuilt (64 bytes of plain 32-bit stores each).
 *               A BG is then one textured quad per run of lines: scrolling is a UV offset with
 *               texture repeat, and affine BGs are the same quad with affine UVs.
 *   Sprites     OBJ character data is mirrored into 4bpp and 8bpp tile atlases (only changed
 *               32-byte tiles are reconverted). Each sprite tile is a quad; affine sprites are
 *               transformed quads clipped to the GBA display box.
 *   Palettes    PVR palette RAM (ARGB1555): 0-255 BG and 256-511 OBJ as 16-colour banks (index 0
 *               of every bank transparent); 512-767 BG and 768-1023 OBJ as 256-colour palettes for
 *               8bpp data (only index 0 transparent).
 *   Priority    GBA priority becomes depth, so colour-keyed layers go to the punch-through list in
 *               any order. Alpha-blended layers, semi-transparent sprites and brightness fades go
 *               to the translucent list.
 *   Lines       hal_pvr_line() records the display registers of each visible line. Each layer is
 *               split into runs of lines over which *its* state is constant, so an HBlank wave on
 *               one BG does not multiply the geometry of the others. WIN0/WIN1 split a run into
 *               horizontal segments with their own layer and effect masks.
 *
 *   Mosaic      BG mosaic is drawn from a small block texture (one texel per mosaic block).
 *
 * Not implemented (logged once when the game uses it): OBJ mosaic, OBJ window, bitmap modes 3-5,
 * mid-frame palette changes, a BG switching between text and affine within one frame. GBA alpha
 * blending (A*EVA + B*EVB) is approximated as A*a + B*(1-a) with a = EVA/16, exact whenever
 * EVA + EVB = 16.
 */
#include <dc/pvr.h>
#include <dc/sq.h>
#include <kos.h>
#include <math.h>
#include <string.h>

#include "hal.h"

#define W 240
#define H 160
#define SCALE 2.0f
#define OX 80.0f
#define OY 80.0f

#define PLTT16 ((volatile u16 *)GBA_PLTT)
#define VRAM8 ((volatile u8 *)GBA_VRAM)
#define OAM16 ((volatile u16 *)GBA_OAM)

/* ---- per-line register snapshot ------------------------------------------------------------ */

struct lstate {
    u16 dispcnt, bgcnt[4], hofs[4], vofs[4];
    u16 winh[2], winv[2], winin, winout, bldcnt, bldalpha, bldy, mosaic;
    s16 pa[2], pb[2], pc[2], pd[2];
    s32 refx[2], refy[2]; /* internal affine reference point at this line (BG2, BG3) */
    u8 inwin[2];          /* line is inside the WIN0 / WIN1 vertical range */
};

static struct lstate lines[H];
static s32 affx[2], affy[2];
static u32 last_bgx[2], last_bgy[2];
static u32 unsupported_seen;

/* Frames in which each GBA feature was in use (printed by hal_pvr_report_features). */
enum { F_WIN, F_ALPHA, F_SEMI_OBJ, F_FADE_PAL, F_FADE_VTX, F_AFFINE_BG, F_AFFINE_OBJ, F_LINE_FX, F_BPP8_BG, F_MOSAIC,
       F_OBJWIN, F_OBJ_MOSAIC, F_COUNT };
static const char *const feature_name[F_COUNT] = {"windows", "alpha blend", "semi-transparent OBJ",
    "brightness (palette, exact)", "brightness (per-layer vertex colour)", "affine BG", "affine OBJ",
    "BG registers changed mid-frame", "8bpp BG", "BG mosaic", "OBJ window (not drawn)", "OBJ mosaic (not drawn)"};
static u32 feature_frames[F_COUNT];
static u32 feature_now;
#define USE(f) (feature_now |= 1u << (f))

static s32 sext28(u32 v) { return (s32)(v << 4) >> 4; }

static int in_vrange(u16 v, int line) {
    int y1 = v >> 8, y2 = v & 0xFF;
    return y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
}

void hal_pvr_line(int line) {
    struct lstate *s = &lines[line];
    int n;
    s->dispcnt = IO16(0x000);
    for (n = 0; n < 4; n++) {
        s->bgcnt[n] = IO16(0x008 + 2 * n);
        s->hofs[n] = IO16(0x010 + 4 * n) & 0x1FF;
        s->vofs[n] = IO16(0x012 + 4 * n) & 0x1FF;
    }
    for (n = 0; n < 2; n++) {
        u32 rx = IO32(0x028 + 0x10 * n), ry = IO32(0x02C + 0x10 * n);
        s->pa[n] = (s16)IO16(0x020 + 0x10 * n);
        s->pb[n] = (s16)IO16(0x022 + 0x10 * n);
        s->pc[n] = (s16)IO16(0x024 + 0x10 * n);
        s->pd[n] = (s16)IO16(0x026 + 0x10 * n);
        if (line == 0 || rx != last_bgx[n] || ry != last_bgy[n]) {
            affx[n] = sext28(rx);
            affy[n] = sext28(ry);
            last_bgx[n] = rx;
            last_bgy[n] = ry;
        }
        s->refx[n] = affx[n];
        s->refy[n] = affy[n];
        affx[n] += s->pb[n];
        affy[n] += s->pd[n];
        s->winh[n] = IO16(0x040 + 2 * n);
        s->winv[n] = IO16(0x044 + 2 * n);
        s->inwin[n] = (u8)in_vrange(s->winv[n], line);
    }
    s->winin = IO16(0x048);
    s->winout = IO16(0x04A);
    s->mosaic = IO16(0x04C);
    s->bldcnt = IO16(0x050);
    s->bldalpha = IO16(0x052);
    s->bldy = IO16(0x054);
}

/* State shared by all layers: display control, windows, blending. */
static int same_common(const struct lstate *a, const struct lstate *b) {
    return a->dispcnt == b->dispcnt && a->winin == b->winin && a->winout == b->winout && a->bldcnt == b->bldcnt &&
           a->bldalpha == b->bldalpha && a->bldy == b->bldy && a->inwin[0] == b->inwin[0] && a->inwin[1] == b->inwin[1] &&
           (!a->inwin[0] || a->winh[0] == b->winh[0]) && (!a->inwin[1] || a->winh[1] == b->winh[1]);
}

/* b (= a + 1 line) continues a's run for this layer (0-3 BG, 4 OBJ/effects). */
static int same_run(const struct lstate *a, const struct lstate *b, int layer) {
    if (!same_common(a, b)) return 0;
    if (layer == 4) return 1;
    if (a->bgcnt[layer] != b->bgcnt[layer]) return 0;
    if (layer >= 2 && (a->dispcnt & 7) != 0) {
        int n = layer - 2;
        return a->pa[n] == b->pa[n] && a->pb[n] == b->pb[n] && a->pc[n] == b->pc[n] && a->pd[n] == b->pd[n] &&
               a->refx[n] + a->pb[n] == b->refx[n] && a->refy[n] + a->pd[n] == b->refy[n];
    }
    return a->hofs[layer] == b->hofs[layer] && a->vofs[layer] == b->vofs[layer];
}

static int bg_exists(int mode, int n);
static int bg_is_affine(int mode, int n);

/* ---- textures ---------------------------------------------------------------------------- */

static u8 morton8[64]; /* texel (x + 8y) of an 8x8 block -> twiddled index */

/* Bit interleave (Morton) for twiddled addressing; arguments are always < 256 here. */
static u16 spread_tab[256];

static void init_spread(void) {
    u32 v, b;
    for (v = 0; v < 256; v++) {
        u32 r = 0;
        for (b = 0; b < 8; b++) r |= ((v >> b) & 1) << (2 * b);
        spread_tab[v] = (u16)r;
    }
}

static inline u32 spread(u32 v) { return spread_tab[v & 255]; }

/* Byte offset of the 8x8 block at tile (tx, ty) in a twiddled 8bpp texture of w x h pixels
 * (rectangular textures are a row/column of min(w,h) squares, as in pvr_txr_load_ex). */
static u32 block8_offset(u32 tx, u32 ty, u32 w, u32 h) {
    u32 m = (w < h ? w : h) >> 3; /* power of two: 16, 32, 64 or 128 tiles */
    u32 sh = m == 128 ? 7 : m == 64 ? 6 : m == 32 ? 5 : 4, mask = m - 1;
    return ((spread(ty & mask) | spread(tx & mask) << 1) + ((tx >> sh) + (ty >> sh)) * m * m) * 64;
}

/* Texture uploads use plain 32-bit stores to (uncached) texture memory, not the store queues:
 * with the MMU on, the store queues are translated through a TLB entry that KallistiOS remaps on
 * every sq_lock, and frequent small sq_cpy() calls between scenes froze PowerVR rendering
 * (first seen in the story cutscene, which streams sprite tiles every frame). */
static void upload(pvr_ptr_t dst, const u8 *src, u32 bytes) {
    volatile u32 *d = (volatile u32 *)dst;
    const u32 *s32 = (const u32 *)src;
    u32 i;
    for (i = 0; i < bytes / 4; i++) d[i] = s32[i];
}

static void upload64(pvr_ptr_t dst, const u8 *src) { upload(dst, src, 64); }

static u8 vram_shadow[0x18000];
/* Frame in which each 32-byte VRAM chunk last changed. A BG map texture compares this with the
 * frame it was last synced, so changes made while the layer was hidden are not lost. */
static u32 chunk_gen[3072];
static u32 sync_gen;    /* incremented once per presented frame */
static u32 bg_vram_gen; /* sync_gen of the last change anywhere in BG VRAM (0x0000-0xFFFF) */

/* OBJ atlases: 4bpp 1024 tiles (256x256), 8bpp 512 tiles (256x256, half used). */
static pvr_ptr_t obj4, obj8;

static void convert_obj4(u32 t) { /* t: OBJ tile 0-1023 */
    u8 out[32] __attribute__((aligned(32)));
    const u8 *src = vram_shadow + 0x10000 + t * 32;
    int i;
    memset(out, 0, 32);
    for (i = 0; i < 64; i++) {
        u8 px = (src[i >> 1] >> ((i & 1) * 4)) & 15, m = morton8[i];
        out[m >> 1] |= px << ((m & 1) * 4);
    }
    upload((u8 *)obj4 + (spread(t >> 5) | spread(t & 31) << 1) * 32, out, 32);
}

static void convert_obj8(u32 t) { /* t: 8bpp OBJ tile 0-511 (VRAM 0x10000 + t*64) */
    u8 out[64] __attribute__((aligned(32)));
    const u8 *src = vram_shadow + 0x10000 + t * 64;
    int i;
    for (i = 0; i < 64; i++) out[morton8[i]] = src[i];
    upload64((u8 *)obj8 + block8_offset(t & 31, t >> 5, 256, 256), out);
}

static u32 sync_vram(void) {
    const volatile u32 *v = (const volatile u32 *)GBA_VRAM;
    u32 *s = (u32 *)vram_shadow;
    u32 t, changed = 0;
    for (t = 0; t < 3072; t++, v += 8, s += 8) {
        if (v[0] == s[0] && v[1] == s[1] && v[2] == s[2] && v[3] == s[3] && v[4] == s[4] && v[5] == s[5] &&
            v[6] == s[6] && v[7] == s[7])
            continue;
        memcpy(s, (const void *)v, 32);
        chunk_gen[t] = sync_gen;
        if (t < 2048) bg_vram_gen = sync_gen;
        changed++;
        if (t >= 2048) {
            convert_obj4(t - 2048);
            convert_obj8((t - 2048) >> 1);
        }
    }
    return changed;
}

/* BG map textures. Text BGs: up to 512x512. Affine BGs: up to 1024x1024. */
struct bgtex {
    pvr_ptr_t tex;
    u32 alloc;           /* bytes allocated */
    u16 cfg;             /* BGCNT bits that define the layout (or 0xFFFF = invalid) */
    u8 affine;
    u16 w, h;            /* pixels */
    u32 synced;          /* sync_gen of the last sync */
    u16 shadow[64 * 64]; /* map entries the texture was built from (text) */
    u8 ashadow[128 * 128];
};
static struct bgtex bgt[4];

static int ensure_alloc(struct bgtex *b, u32 bytes) {
    if (b->alloc >= bytes) return 1;
    if (b->tex) pvr_mem_free(b->tex);
    b->tex = pvr_mem_malloc(bytes);
    b->alloc = b->tex ? bytes : 0;
    return b->tex != NULL;
}

/* mflip[f][i]: twiddled position of linear texel i (x + 8y) of a tile drawn with flips f (bit 0
 * horizontal, bit 1 vertical). */
static u8 mflip[4][64];

static void build_text_tile(struct bgtex *b, u16 e, u32 charbase, int bpp8, u32 tx, u32 ty) {
    u8 out[64] __attribute__((aligned(32)));
    const u8 *m = mflip[(e >> 10) & 3];
    int i;
    if (bpp8) {
        const u8 *src = vram_shadow + ((charbase + (e & 0x3FF) * 64) & 0xFFFF); /* 64-aligned: never wraps */
        for (i = 0; i < 64; i++) out[m[i]] = src[i];
    } else {
        const u8 *src = vram_shadow + ((charbase + (e & 0x3FF) * 32) & 0xFFFF);
        u8 bank = (u8)((e >> 12) << 4);
        for (i = 0; i < 32; i++) {
            u8 v = src[i], lo = v & 15, hi = v >> 4;
            out[m[2 * i]] = lo ? lo | bank : 0; /* index 0 stays transparent */
            out[m[2 * i + 1]] = hi ? hi | bank : 0;
        }
    }
    upload64((u8 *)b->tex + block8_offset(tx, ty, b->w, b->h), out);
}

static u32 rebuilt;
static u32 ph_vram, ph_maps, ph_mos; /* texture-sync phase times of the current frame (us) */

static int tile_dirty(const struct bgtex *b, u32 charbase, u16 tileno, int bpp8) {
    u32 c = (charbase + tileno * (bpp8 ? 64 : 32)) % 0x10000 / 32;
    return chunk_gen[c] > b->synced || (bpp8 && chunk_gen[c + 1] > b->synced);
}

static void sync_text_bg(int n, u16 cnt) {
    struct bgtex *b = &bgt[n];
    u32 charbase = ((cnt >> 2) & 3) * 0x4000, screenbase = ((cnt >> 8) & 0x1F) * 0x800;
    int bpp8 = (cnt >> 7) & 1, size = cnt >> 14;
    int wt = (size & 1) ? 64 : 32, ht = (size & 2) ? 64 : 32, tx, ty;
    u16 cfg = cnt & 0xFF8C; /* charbase, 8bpp, screenbase, size */
    int full = b->cfg != cfg || b->affine;
    if (full) {
        if (!ensure_alloc(b, wt * ht * 64)) return;
        b->cfg = cfg;
        b->affine = 0;
        b->w = wt * 8;
        b->h = ht * 8;
    }
    for (ty = 0; ty < ht; ty++)
        for (tx = 0; tx < wt; tx++) {
            u32 block = (u32)((tx >> 5) + (ty >> 5) * (wt >> 5));
            u16 e = *(const u16 *)(vram_shadow + screenbase + block * 0x800 + ((ty & 31) * 32 + (tx & 31)) * 2);
            u16 *sh = &b->shadow[ty * 64 + tx];
            if (!full && e == *sh && !tile_dirty(b, charbase, e & 0x3FF, bpp8)) continue;
            *sh = e;
            build_text_tile(b, e, charbase, bpp8, tx, ty);
            rebuilt++;
        }
    b->synced = sync_gen;
}

static void sync_affine_bg(int n, u16 cnt) {
    struct bgtex *b = &bgt[n];
    u32 charbase = ((cnt >> 2) & 3) * 0x4000, screenbase = ((cnt >> 8) & 0x1F) * 0x800;
    int sz = 128 << (cnt >> 14), tiles = sz >> 3, tx, ty;
    u16 cfg = cnt & 0xDF0C;
    int full = b->cfg != cfg || !b->affine;
    if (full) {
        if (!ensure_alloc(b, sz * sz)) return;
        b->cfg = cfg;
        b->affine = 1;
        b->w = b->h = sz;
    }
    for (ty = 0; ty < tiles; ty++)
        for (tx = 0; tx < tiles; tx++) {
            u8 tile = vram_shadow[screenbase + ty * tiles + tx];
            u8 *sh = &b->ashadow[ty * 128 + tx];
            if (!full && tile == *sh && !tile_dirty(b, charbase, tile, 1)) continue;
            *sh = tile;
            build_text_tile(b, (u16)tile, charbase, 1, tx, ty);
            rebuilt++;
        }
    b->synced = sync_gen;
}

/* ---- BG mosaic --------------------------------------------------------------------------- */
/* A mosaic BG is drawn from a small texture holding one texel per mosaic block: the BG pixel at the
 * block's top-left screen position, fetched from the VRAM mirror with the registers of the block's
 * first line (so HBlank and affine state are honoured). One quad then magnifies it by the block
 * size with nearest filtering, which is exactly the GBA's mosaic. */
#define MOS 128 /* >= 240/2 x 160/2 blocks */
static pvr_ptr_t mostex[4];
static u8 mosbuf[MOS * MOS] __attribute__((aligned(32)));
static int mos_active[4], mos_bw, mos_bh;

static u8 bg_pixel(const struct lstate *s, int n, int x, int y, int line_in_run) {
    u16 cnt = s->bgcnt[n];
    u32 charbase = ((cnt >> 2) & 3) * 0x4000, screenbase = ((cnt >> 8) & 0x1F) * 0x800;
    int mode = s->dispcnt & 7;
    if (!bg_is_affine(mode, n)) {
        int size = cnt >> 14, wt = (size & 1) ? 64 : 32, ht = (size & 2) ? 64 : 32;
        int px = (x + s->hofs[n]) & (wt * 8 - 1), py = (y + s->vofs[n]) & (ht * 8 - 1);
        int tx = px >> 3, ty = py >> 3, fx = px & 7, fy = py & 7;
        u32 block = (u32)((tx >> 5) + (ty >> 5) * (wt >> 5));
        u16 e = *(const u16 *)(vram_shadow + screenbase + block * 0x800 + ((ty & 31) * 32 + (tx & 31)) * 2);
        if (e & 0x400) fx = 7 - fx;
        if (e & 0x800) fy = 7 - fy;
        if (cnt & 0x80) return vram_shadow[(charbase + (e & 0x3FF) * 64 + fy * 8 + fx) % 0x10000];
        {
            u8 b = vram_shadow[(charbase + (e & 0x3FF) * 32 + fy * 4 + (fx >> 1)) % 0x10000];
            u8 c = (fx & 1) ? b >> 4 : b & 15;
            return c ? (u8)(c | (e >> 12) << 4) : 0;
        }
    } else {
        int a = n - 2, sz = 128 << (cnt >> 14), tiles = sz >> 3;
        s32 tx = (s->refx[a] + s->pa[a] * x + s->pb[a] * line_in_run) >> 8;
        s32 ty = (s->refy[a] + s->pc[a] * x + s->pd[a] * line_in_run) >> 8;
        u8 tile;
        if (cnt & 0x2000) {
            tx &= sz - 1;
            ty &= sz - 1;
        } else if (tx < 0 || ty < 0 || tx >= sz || ty >= sz) {
            return 0;
        }
        tile = vram_shadow[screenbase + (ty >> 3) * tiles + (tx >> 3)];
        return vram_shadow[(charbase + tile * 64 + (ty & 7) * 8 + (tx & 7)) % 0x10000];
    }
}

/* Inputs a mosaic texture depends on: mosaic size, the layer's registers on block-start lines,
 * and BG VRAM. Rebuild only when they change (transitions hold a size for several frames). */
static u32 mosaic_key(int n) {
    u32 h = 2166136261u, by;
    const u32 *p;
    unsigned k;
    h = (h ^ lines[0].mosaic) * 16777619u;
    h = (h ^ bg_vram_gen) * 16777619u;
    for (by = 0; by * mos_bh < H; by++) {
        const struct lstate *s = &lines[by * mos_bh];
        u32 v[6] = {s->dispcnt, s->bgcnt[n], (u32)s->hofs[n] << 16 | s->vofs[n], 0, 0, 0};
        if (n >= 2) {
            v[3] = (u32)(u16)s->pa[n - 2] << 16 | (u16)s->pc[n - 2];
            v[4] = (u32)s->refx[n - 2];
            v[5] = (u32)s->refy[n - 2];
        }
        for (p = v, k = 0; k < 6; k++) h = (h ^ p[k]) * 16777619u;
    }
    return h;
}

static void build_mosaic(int n) {
    static u32 built_key[4];
    int bx, by, i;
    u32 key;
    if (!mostex[n]) mostex[n] = pvr_mem_malloc(MOS * MOS);
    if (!mostex[n]) return;
    key = mosaic_key(n);
    if (key == built_key[n]) return;
    built_key[n] = key;

    memset(mosbuf, 0, sizeof mosbuf);
    for (by = 0; by * mos_bh < H; by++) {
        const struct lstate *s = &lines[by * mos_bh];
        u32 row = spread(by);
        if (!(s->dispcnt & (0x100 << n)) || !bg_exists(s->dispcnt & 7, n)) continue; /* row stays transparent */
        if (!bg_is_affine(s->dispcnt & 7, n)) {
            /* text BG: one map row and one tile row per block row */
            u16 cnt = s->bgcnt[n];
            u32 charbase = ((cnt >> 2) & 3) * 0x4000, screenbase = ((cnt >> 8) & 0x1F) * 0x800;
            int size = cnt >> 14, wt = (size & 1) ? 64 : 32, ht = (size & 2) ? 64 : 32, bpp8 = cnt & 0x80;
            int py = (by * mos_bh + s->vofs[n]) & (ht * 8 - 1), ty = py >> 3, fy = py & 7;
            u32 wmask = wt * 8 - 1, x = s->hofs[n];
            const u8 *maprow = vram_shadow + screenbase + (ty >> 5) * (wt >> 5) * 0x800 + (ty & 31) * 64;
            for (bx = 0; bx * mos_bw < W; bx++, x += mos_bw) {
                u32 px = x & wmask, tx = px >> 3;
                u16 e = *(const u16 *)(maprow + (tx >> 5) * 0x800 + (tx & 31) * 2);
                int fx = (e & 0x400) ? 7 - (px & 7) : (px & 7), ry = (e & 0x800) ? 7 - fy : fy;
                u8 c;
                if (bpp8) {
                    c = vram_shadow[(charbase + (e & 0x3FF) * 64 + ry * 8 + fx) & 0xFFFF];
                } else {
                    u8 b = vram_shadow[(charbase + (e & 0x3FF) * 32 + ry * 4 + (fx >> 1)) & 0xFFFF];
                    c = (fx & 1) ? b >> 4 : b & 15;
                    if (c) c |= (e >> 12) << 4;
                }
                mosbuf[row | spread(bx) << 1] = c;
            }
        } else {
            for (bx = 0; bx * mos_bw < W; bx++) mosbuf[row | spread(bx) << 1] = bg_pixel(s, n, bx * mos_bw, by * mos_bh, 0);
        }
    }
    for (i = 0; i < MOS * MOS; i += 64) upload64((u8 *)mostex[n] + i, mosbuf + i);
}

static void plan_mosaic(void) {
    const struct lstate *s = &lines[0];
    int n;
    mos_bw = (s->mosaic & 15) + 1;
    mos_bh = ((s->mosaic >> 4) & 15) + 1;
    for (n = 0; n < 4; n++) {
        mos_active[n] = (s->bgcnt[n] & 0x40) && (mos_bw > 1 || mos_bh > 1);
        if (mos_active[n]) build_mosaic(n);
    }
}

static u16 pal_shadow[512];
static u16 pal_sent[512];
static int pal_valid;

/* GBA brightness effect, exact (BLDY): c + (31 - c) * ey / 16 or c - c * ey / 16 per channel. */
static u16 bright(u16 c, int ey, int up) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (up) {
        r += ((31 - r) * ey) >> 4;
        g += ((31 - g) * ey) >> 4;
        b += ((31 - b) * ey) >> 4;
    } else {
        r -= (r * ey) >> 4;
        g -= (g * ey) >> 4;
        b -= (b * ey) >> 4;
    }
    return (u16)(r | (g << 5) | (b << 10));
}

static u32 argb1555(u16 c, int opaque) {
    return (opaque ? 0x8000 : 0) | ((c & 31) << 10) | (c & 0x3E0) | ((c >> 10) & 31);
}

/* Brightness folded into the palette for this frame (fade_mode 2 up / 3 down, fade_ey), with
 * per-palette enables: bit 0 BG colours, bit 1 backdrop, bit 2 OBJ. 0 = no palette fade. */
static int fade_mode, fade_ey, fade_where;

static void sync_palette(void) {
    int i;
    for (i = 0; i < 512; i++) {
        u16 c = PLTT16[i], out = c;
        int target = i >= 256 ? (fade_where & 4) : i == 0 ? (fade_where & 2) : (fade_where & 1);
        if (fade_mode && target) out = bright(c, fade_ey, fade_mode == 2);
        if (pal_valid && c == pal_shadow[i] && out == pal_sent[i]) continue;
        pal_shadow[i] = c;
        pal_sent[i] = out;
        pvr_set_pal_entry(i, argb1555(out, (i & 15) != 0));
        pvr_set_pal_entry(512 + i, argb1555(out, (i & 255) != 0));
    }
    pal_valid = 1;
}

/* Can this frame's brightness effect be applied exactly through the palette? Only when every line
 * has the same effect state, no window restricts it, and all displayed BG layers are either all
 * targets or all not (they share one palette). */
static void plan_fade(void) {
    const struct lstate *s = &lines[0];
    int eff = (s->bldcnt >> 6) & 3, y, l, bg_t = -1, mode;
    fade_mode = 0;
    if ((eff != 2 && eff != 3) || !(s->bldy & 31)) return;
    for (y = 1; y < H; y++)
        if (lines[y].bldcnt != s->bldcnt || lines[y].bldy != s->bldy) return;
    for (y = 0; y < H; y++) {
        const struct lstate *t = &lines[y];
        if (t->dispcnt & 0xE000) {
            /* Windows only matter if some visible region has the effect switched off. */
            if (!(t->winout & 0x20)) return;
            if ((t->dispcnt & 0x2000) && t->inwin[0] && !(t->winin & 0x20)) return;
            if ((t->dispcnt & 0x4000) && t->inwin[1] && !(t->winin & 0x2000)) return;
            if (t->dispcnt & 0x8000) return; /* OBJ window: not modelled */
        }
        mode = t->dispcnt & 7;
        for (l = 0; l < 4; l++) {
            int on = (t->dispcnt & (0x100 << l)) && bg_exists(mode, l), tg;
            if (!on) continue;
            tg = (s->bldcnt >> l) & 1;
            if (bg_t < 0) bg_t = tg;
            else if (bg_t != tg) return;
        }
    }
    fade_mode = eff;
    fade_ey = (s->bldy & 31) > 16 ? 16 : (s->bldy & 31);
    fade_where = (bg_t > 0 ? 1 : 0) | ((s->bldcnt >> 5) & 1 ? 2 : 0) | ((s->bldcnt >> 4) & 1 ? 4 : 0);
}

#ifndef HAL_PVR_DEBUG
#define HAL_PVR_DEBUG 0 /* bisect aid: 1 no TR content, 2 no sprites, 4 no windows, 8 no fade quads,
                          16 no texture/palette sync after frame 1300, 32 no PT/TR geometry after 1300 */
#endif

/* ---- geometry ---------------------------------------------------------------------------- */

struct vtx {
    float x, y, u, v;
};

struct clip {
    float x0, y0, x1, y1;
};

static pvr_list_t cur_list;
static u32 cur_key = 0xFFFFFFFF;
static u32 cur_argb = 0xFFFFFFFF;
static u32 prims;

/* Texture sources for headers. */
enum { SRC_NONE, SRC_OBJ4, SRC_OBJ8, SRC_BG0, SRC_BG1, SRC_BG2, SRC_BG3, SRC_MOS0 };

static u32 cur_oargb; /* offset colour (used for brightness increase) */
static int cur_spec;  /* 1 when the next header must enable the offset colour */

/* key = src | pal << 4 | translucent << 12 | specular << 13 | alpha << 16 */
static void use_header(int src, int pal, int translucent, u32 alpha8) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t *hdr;
    u32 key = (u32)src | (u32)pal << 4 | (u32)translucent << 12 | (u32)cur_spec << 13 | alpha8 << 16;
    if (key == cur_key) return;
    cur_key = key;
    if (src == SRC_NONE) {
        pvr_poly_cxt_col(&cxt, cur_list);
    } else if (src == SRC_OBJ4) {
        pvr_poly_cxt_txr(&cxt, cur_list, PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(pal) | PVR_TXRFMT_TWIDDLED, 256, 256,
                         obj4, PVR_FILTER_NONE);
    } else if (src == SRC_OBJ8) {
        pvr_poly_cxt_txr(&cxt, cur_list, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(3) | PVR_TXRFMT_TWIDDLED, 256, 256,
                         obj8, PVR_FILTER_NONE);
    } else if (src >= SRC_MOS0) {
        pvr_poly_cxt_txr(&cxt, cur_list, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(pal) | PVR_TXRFMT_TWIDDLED, MOS, MOS,
                         mostex[src - SRC_MOS0], PVR_FILTER_NONE);
    } else {
        struct bgtex *b = &bgt[src - SRC_BG0];
        pvr_poly_cxt_txr(&cxt, cur_list, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(pal) | PVR_TXRFMT_TWIDDLED, b->w, b->h,
                         b->tex, PVR_FILTER_NONE);
    }
    if (src != SRC_NONE) cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.specular = cur_spec ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    if (translucent) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    }
    hdr = pvr_dr_target();
    pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);
}

static u32 bad_vertices;

static inline void put_vertex(const struct vtx *p, float z, int last) {
    pvr_vertex_t *vx;
    if (!(p->x > -1000.0f && p->x < 1000.0f && p->y > -1000.0f && p->y < 1000.0f && p->u > -1e6f && p->u < 1e6f &&
          p->v > -1e6f && p->v < 1e6f)) {
        if (!bad_vertices++)
            hal_log("pvr: bad vertex x=%d y=%d u*1000=%d v*1000=%d\n", (int)p->x, (int)p->y, (int)(p->u * 1000),
                    (int)(p->v * 1000));
    }
    vx = pvr_dr_target();
    vx->flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vx->x = OX + p->x * SCALE;
    vx->y = OY + p->y * SCALE;
    vx->z = z;
    vx->u = p->u;
    vx->v = p->v;
    vx->argb = cur_argb;
    vx->oargb = cur_oargb;
    pvr_dr_commit(vx);
}

/* Clip a convex polygon against a screen rectangle and, optionally, the texture square
 * [0,1)x[0,1) (for non-wrapping affine BGs), then emit it as a strip. */
static void emit_poly(const struct vtx *in, int n, const struct clip *c, float z, int clip_uv) {
    struct vtx a[16], b[16], *src = a, *dst = b, *t;
    int i, k, m;
    memcpy(a, in, n * sizeof *in);
    for (k = 0; k < (clip_uv ? 8 : 4); k++) {
        m = 0;
        for (i = 0; i < n; i++) {
            const struct vtx *p = &src[i], *q = &src[(i + 1) % n];
            float dp, dq;
            switch (k) {
            case 0: dp = p->x - c->x0; dq = q->x - c->x0; break;
            case 1: dp = c->x1 - p->x; dq = c->x1 - q->x; break;
            case 2: dp = p->y - c->y0; dq = q->y - c->y0; break;
            case 3: dp = c->y1 - p->y; dq = c->y1 - q->y; break;
            case 4: dp = p->u; dq = q->u; break;
            case 5: dp = 1.0f - p->u; dq = 1.0f - q->u; break;
            case 6: dp = p->v; dq = q->v; break;
            default: dp = 1.0f - p->v; dq = 1.0f - q->v; break;
            }
            if (dp >= 0) dst[m++] = *p;
            if ((dp >= 0) != (dq >= 0) && m < 16) {
                float f = dp / (dp - dq);
                dst[m].x = p->x + (q->x - p->x) * f;
                dst[m].y = p->y + (q->y - p->y) * f;
                dst[m].u = p->u + (q->u - p->u) * f;
                dst[m].v = p->v + (q->v - p->v) * f;
                m++;
            }
        }
        n = m;
        if (n < 3) return;
        t = src;
        src = dst;
        dst = t;
    }
    for (i = 0; i < n; i++) {
        int idx = (i == 0) ? 0 : (i & 1) ? (i + 1) / 2 : n - i / 2;
        put_vertex(&src[idx], z, i == n - 1);
    }
    prims++;
}

static void emit_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z) {
    struct vtx q[4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1}, {x1, y1, u1, v1}};
    put_vertex(&q[0], z, 0);
    put_vertex(&q[1], z, 0);
    put_vertex(&q[2], z, 0);
    put_vertex(&q[3], z, 1);
    prims++;
}

/* ---- scene ------------------------------------------------------------------------------- */

/* Depth: higher is closer. Priority 0 is frontmost; within a priority OBJ > BG0 > ... > BG3. */
static float layer_z(int prio, int layer) { return 10.0f + (3 - prio) * 10.0f + (layer == 4 ? 5.0f : 3.0f - layer); }

struct seg {
    float x0, x1;
    u8 mask; /* bits 0-3 BG, 4 OBJ, 5 effects */
};

static int segments(const struct lstate *s, struct seg *out) {
    u16 dc = s->dispcnt;
    int edges[6], ne = 0, i, j, n = 0;
    if (!(dc & 0xE000) || (HAL_PVR_DEBUG & 4)) {
        out[0] = (struct seg){0, W, 0x3F};
        return 1;
    }
    USE(F_WIN);
    if (dc & 0x8000) {
        unsupported_seen |= 1; /* OBJ window: treated as "outside" */
        USE(F_OBJWIN);
    }
    edges[ne++] = 0;
    edges[ne++] = W;
    for (i = 0; i < 2; i++)
        if ((dc & (0x2000 << i)) && s->inwin[i]) {
            edges[ne++] = s->winh[i] >> 8;
            edges[ne++] = s->winh[i] & 0xFF;
        }
    for (i = 1; i < ne; i++)
        for (j = i; j > 0 && edges[j - 1] > edges[j]; j--) {
            int t = edges[j];
            edges[j] = edges[j - 1];
            edges[j - 1] = t;
        }
    for (i = 0; i + 1 < ne; i++) {
        int x0 = edges[i] > W ? W : edges[i], x1 = edges[i + 1] > W ? W : edges[i + 1], mid, w;
        u8 mask = s->winout & 0x3F;
        if (x1 <= x0) continue;
        mid = (x0 + x1) / 2;
        for (w = 1; w >= 0; w--) {
            int a = s->winh[w] >> 8, b = s->winh[w] & 0xFF, in;
            if (!(dc & (0x2000 << w)) || !s->inwin[w]) continue;
            in = a <= b ? (mid >= a && mid < b) : (mid >= a || mid < b);
            if (in) mask = (w ? s->winin >> 8 : s->winin) & 0x3F;
        }
        if (n && out[n - 1].mask == mask && out[n - 1].x1 == x0) out[n - 1].x1 = x1;
        else out[n++] = (struct seg){x0, x1, mask};
    }
    return n;
}

/* Is this layer alpha-blended by the GBA in this segment? alpha8 = EVA/16 * 255. */
static int layer_blends(const struct lstate *s, int layer, u8 segmask, u32 *alpha8) {
    int eff = (s->bldcnt >> 6) & 3, eva = s->bldalpha & 31;
    if (!(segmask & 0x20) || eff != 1 || !(s->bldcnt & (1 << layer)) || !(s->bldcnt & 0x3F00)) return 0;
    USE(F_ALPHA);
    if (eva > 16) eva = 16;
    *alpha8 = (u32)(eva * 255 / 16);
    return 1;
}

/* GBA brightness effect on one layer, when it could not be folded into the palette: modulate by
 * (1 - k) and, for an increase, add k * white through the offset colour. Sets cur_argb/cur_oargb/
 * cur_spec for the layer's polygons; alpha8 is kept in the top byte of cur_argb. */
static void layer_colour(const struct lstate *s, int layer, u8 segmask, u32 alpha8) {
    int eff = (s->bldcnt >> 6) & 3, ey = s->bldy & 31;
    cur_argb = (alpha8 << 24) | 0xFFFFFF;
    cur_oargb = 0;
    cur_spec = 0;
    if (fade_mode || !(segmask & 0x20) || (eff != 2 && eff != 3) || !ey || !(s->bldcnt & (1 << layer))) return;
    if (ey > 16) ey = 16;
    {
        u32 m = (u32)(255 * (16 - ey) / 16), k = (u32)(255 * ey / 16);
        cur_argb = (alpha8 << 24) | m << 16 | m << 8 | m;
        USE(F_FADE_VTX);
        if (eff == 2) {
            cur_oargb = k << 16 | k << 8 | k;
            cur_spec = 1;
        }
    }
}

static int bg_is_affine(int mode, int n) { return (mode == 1 && n == 2) || (mode == 2 && n >= 2); }
static int bg_exists(int mode, int n) { return mode == 0 || (mode == 1 && n <= 2) || (mode == 2 && n >= 2); }

static void draw_bg_run(const struct lstate *s, int n, int y0, int y1, int pass) {
    struct seg seg[8];
    int nseg = segments(s, seg), si, mode = s->dispcnt & 7;
    struct bgtex *b = &bgt[n];
    u16 cnt = s->bgcnt[n];
    float z = layer_z(cnt & 3, n);
    if (!b->tex) return;
    for (si = 0; si < nseg; si++) {
        struct clip c = {seg[si].x0, (float)y0, seg[si].x1, (float)y1};
        u32 alpha8 = 255;
        int blend;
        if (!(seg[si].mask & (1 << n))) continue;
        blend = layer_blends(s, n, seg[si].mask, &alpha8);
        if (blend != (pass == 1)) continue;
        layer_colour(s, n, seg[si].mask, alpha8);
        if (mos_active[n]) {
            float sx = 1.0f / (mos_bw * MOS), sy = 1.0f / (mos_bh * MOS);
            USE(F_MOSAIC);
            use_header(SRC_MOS0 + n, ((cnt & 0x80) || bg_is_affine(mode, n)) ? 2 : 0, blend, alpha8);
            emit_rect(c.x0, c.y0, c.x1, c.y1, c.x0 * sx, c.y0 * sy, c.x1 * sx, c.y1 * sy, z);
        } else if (!bg_is_affine(mode, n)) {
            float iw = 1.0f / b->w, ih = 1.0f / b->h;
            use_header(SRC_BG0 + n, (cnt & 0x80) ? 2 : 0, blend, alpha8);
            emit_rect(c.x0, c.y0, c.x1, c.y1, (c.x0 + s->hofs[n]) * iw, (c.y0 + s->vofs[n]) * ih,
                      (c.x1 + s->hofs[n]) * iw, (c.y1 + s->vofs[n]) * ih, z);
        } else {
            int a = n - 2, wrap = (cnt >> 13) & 1, k;
            USE(F_AFFINE_BG);
            float pa = s->pa[a] / 256.0f, pb = s->pb[a] / 256.0f, pc = s->pc[a] / 256.0f, pd = s->pd[a] / 256.0f;
            float rx = s->refx[a] / 256.0f, ry = s->refy[a] / 256.0f, isz = 1.0f / b->w;
            struct vtx q[4];
            for (k = 0; k < 4; k++) {
                /* The GBA samples texel ref + PA*x at integer pixel x; the 2x output samples a quarter
                 * GBA pixel further on, so evaluate the mapping a quarter pixel back. */
                float x = (k == 1 || k == 2) ? c.x1 : c.x0, y = (k >= 2) ? c.y1 : c.y0;
                float sx = x - 0.25f, sy = y - 0.25f - y0;
                q[k].x = x;
                q[k].y = y;
                /* + 1/512 texel: GBA coordinates are multiples of 1/256, so half a step keeps the
                 * sample inside the texel the GBA picks instead of on its edge */
                q[k].u = (rx + pa * sx + pb * sy + 1.0f / 512) * isz;
                q[k].v = (ry + pc * sx + pd * sy + 1.0f / 512) * isz;
            }
            use_header(SRC_BG0 + n, 2, blend, alpha8);
            emit_poly(q, 4, &c, z, !wrap);
        }
    }
}

static const u8 obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const u8 obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static void draw_sprites_seg(const struct lstate *s, const struct clip *c, int pass, u8 segmask) {
    int map1d = (s->dispcnt >> 6) & 1, i;
    int eva = s->bldalpha & 31;
    u32 semi8 = (u32)((eva > 16 ? 16 : eva) * 255 / 16), layer8 = 255;
    int obj_blends = layer_blends(s, 4, segmask, &layer8);
    for (i = 0; i < 128; i++) {
        u16 a0 = OAM16[i * 4], a1 = OAM16[i * 4 + 1], a2 = OAM16[i * 4 + 2];
        int affine = (a0 >> 8) & 1, dbl = (a0 >> 9) & 1, shape = a0 >> 14, size = a1 >> 14;
        int mode = (a0 >> 10) & 3, bpp8 = (a0 >> 13) & 1, w, h, bw, bh, x, y, prio, tile, pal, tx, ty, blend;
        u32 alpha8;
        float z;
        struct clip box;
        if ((!affine && dbl) || shape == 3) continue;
        if (mode == 2) {
            unsupported_seen |= 1;
            continue;
        }
        if ((a0 & 0x1000) && (s->mosaic & 0xFF00)) { /* OBJ mosaic with a non-zero size: not drawn */
            unsupported_seen |= 2;
            USE(F_OBJ_MOSAIC);
        }
        if (mode == 1) USE(F_SEMI_OBJ);
        if (affine) USE(F_AFFINE_OBJ);
        w = obj_w[shape][size];
        h = obj_h[shape][size];
        bw = w << (affine && dbl);
        bh = h << (affine && dbl);
        y = a0 & 0xFF;
        if (y >= 160) y -= 256;
        x = a1 & 0x1FF;
        if (x >= 240) x -= 512;
        if (x >= c->x1 || y >= c->y1 || x + bw <= c->x0 || y + bh <= c->y0) continue;
        blend = (mode == 1 && (segmask & 0x20)) || obj_blends;
        alpha8 = mode == 1 ? semi8 : layer8;
        if (blend != (pass == 1)) continue;
        layer_colour(s, 4, mode == 1 ? 0 : segmask, blend ? alpha8 : 255);
        prio = (a2 >> 10) & 3;
        tile = a2 & 0x3FF;
        pal = a2 >> 12;
        z = layer_z(prio, 4) + (127 - i) / 256.0f;
        box = (struct clip){x > c->x0 ? x : c->x0, y > c->y0 ? y : c->y0, x + bw < c->x1 ? x + bw : c->x1,
                            y + bh < c->y1 ? y + bh : c->y1};
        use_header(bpp8 ? SRC_OBJ8 : SRC_OBJ4, bpp8 ? 3 : 16 + pal, blend, blend ? alpha8 : 255);
        for (ty = 0; ty < h / 8; ty++) {
            for (tx = 0; tx < w / 8; tx++) {
                u32 t, ax, ay;
                float u0, v0, u1, v1;
                if (bpp8) {
                    u32 tn = map1d ? tile + (ty * (w / 8) + tx) * 2 : tile + ty * 32 + tx * 2;
                    t = (tn & 0x3FF) >> 1;
                    ax = t & 31;
                    ay = t >> 5;
                } else {
                    u32 tn = map1d ? tile + ty * (w / 8) + tx : tile + ty * 32 + tx;
                    t = tn & 0x3FF;
                    ax = t & 31;
                    ay = t >> 5;
                }
                u0 = ax * (8.0f / 256);
                v0 = ay * (8.0f / 256);
                u1 = u0 + 8.0f / 256;
                v1 = v0 + 8.0f / 256;
                if (!affine) {
                    int hf = (a1 >> 12) & 1, vf = (a1 >> 13) & 1;
                    float px = x + (hf ? (w / 8 - 1 - tx) : tx) * 8.0f, py = y + (vf ? (h / 8 - 1 - ty) : ty) * 8.0f;
                    struct vtx q[4];
                    if (px >= box.x1 || py >= box.y1 || px + 8 <= box.x0 || py + 8 <= box.y0) continue;
                    if (hf) {
                        float tmp = u0;
                        u0 = u1;
                        u1 = tmp;
                    }
                    if (vf) {
                        float tmp = v0;
                        v0 = v1;
                        v1 = tmp;
                    }
                    if (px >= box.x0 && py >= box.y0 && px + 8 <= box.x1 && py + 8 <= box.y1) {
                        emit_rect(px, py, px + 8, py + 8, u0, v0, u1, v1, z);
                    } else {
                        q[0] = (struct vtx){px, py, u0, v0};
                        q[1] = (struct vtx){px + 8, py, u1, v0};
                        q[2] = (struct vtx){px + 8, py + 8, u1, v1};
                        q[3] = (struct vtx){px, py + 8, u0, v1};
                        emit_poly(q, 4, &box, z, 0);
                    }
                } else {
                    int p = (a1 >> 9) & 0x1F, k;
                    float pa = (s16)OAM16[p * 16 + 3] / 256.0f, pb = (s16)OAM16[p * 16 + 7] / 256.0f;
                    float pc = (s16)OAM16[p * 16 + 11] / 256.0f, pd = (s16)OAM16[p * 16 + 15] / 256.0f;
                    float det = pa * pd - pb * pc, cx = x + bw / 2.0f, cy = y + bh / 2.0f;
                    struct vtx q[4];
                    if (det == 0.0f) continue;
                    for (k = 0; k < 4; k++) {
                        float du = (k == 1 || k == 2) ? 8.0f : 0.0f, dv = (k >= 2) ? 8.0f : 0.0f;
                        float lx = tx * 8 + du - w / 2.0f, ly = ty * 8 + dv - h / 2.0f;
                        /* texture = M * (screen - centre) + half size  =>  screen = M^-1 * (...) + centre */
                        q[k].x = cx + (pd * lx - pb * ly) / det + 0.25f; /* see the affine BG note */
                        q[k].y = cy + (-pc * lx + pa * ly) / det + 0.25f;
                        q[k].u = u0 + (du + 1.0f / 512) / 256;
                        q[k].v = v0 + (dv + 1.0f / 512) / 256;
                    }
                    emit_poly(q, 4, &box, z, 0);
                }
            }
        }
    }
}

static void draw_obj_and_effects_run(const struct lstate *s, int y0, int y1, int pass) {
    struct seg seg[8];
    int nseg = segments(s, seg), si;
    for (si = 0; si < nseg; si++) {
        struct clip c = {seg[si].x0, (float)y0, seg[si].x1, (float)y1};
        if ((s->dispcnt & 0x1000) && (seg[si].mask & 0x10) && !(HAL_PVR_DEBUG & 2)) draw_sprites_seg(s, &c, pass, seg[si].mask);
    }
}

/* Every list gets one invisible polygon outside the picture, so no list is ever submitted empty.
 * (Added while bisecting the cutscene freeze; that turned out to be store-queue texture uploads,
 * see upload(), but a guaranteed non-empty list is cheap insurance.) */
static void keepalive_poly(void) {
    cur_argb = 0x00000000;
    cur_oargb = 0;
    cur_spec = 0;
    use_header(SRC_NONE, 0, cur_list == PVR_LIST_TR_POLY, 0);
    emit_rect(-41, -41, -40, -40, 0, 0, 0, 0, 0.5f);
}

/* pass 0: punch-through list, pass 1: translucent list */
static void draw_scene(int pass) {
    int layer, y0, y1;
    cur_list = pass ? PVR_LIST_TR_POLY : PVR_LIST_PT_POLY;
    cur_key = 0xFFFFFFFF;
    keepalive_poly();
    for (layer = 0; layer < 5; layer++) {
        for (y0 = 0; y0 < H; y0 = y1) {
            const struct lstate *s = &lines[y0];
            int mode = s->dispcnt & 7;
            for (y1 = y0 + 1; y1 < H && same_run(&lines[y1 - 1], &lines[y1], (layer < 4 && mos_active[layer]) ? 4 : layer); y1++) {}
            if (s->dispcnt & 0x80) continue; /* forced blank: drawn as white in the OP pass */
            if (layer < 4) {
                if (mode > 2 && layer == 0) unsupported_seen |= 4;

                if ((s->dispcnt & (0x100 << layer)) && bg_exists(mode, layer)) {
                    if (y0 > 0 && y1 < H && same_common(&lines[y0 - 1], s) && same_common(&lines[y1 - 1], &lines[y1]))
                        USE(F_LINE_FX); /* a run bounded on both sides by this layer's own register changes */
                    if (s->bgcnt[layer] & 0x80) USE(F_BPP8_BG);
                    draw_bg_run(s, layer, y0, y1, pass);
                }
            } else {
                draw_obj_and_effects_run(s, y0, y1, pass);
            }
        }
    }
}

static void draw_opaque(void) {
    u16 bd = PLTT16[0];
    int y0, y1, si, nseg;
    struct seg seg[8];
    cur_list = PVR_LIST_OP_POLY;
    cur_key = 0xFFFFFFFF;
    cur_oargb = 0;
    cur_spec = 0;
    use_header(SRC_NONE, 0, 0, 255);
    /* backdrop (palette 0) behind the picture, with the GBA brightness effect applied exactly when
     * the backdrop is a target and the palette path is not in use */
    for (y0 = 0; y0 < H; y0 = y1) {
        const struct lstate *s = &lines[y0];
        int eff = (s->bldcnt >> 6) & 3, ey = (s->bldy & 31) > 16 ? 16 : (s->bldy & 31);
        for (y1 = y0 + 1; y1 < H && same_run(&lines[y1 - 1], &lines[y1], 4); y1++) {}
        nseg = segments(s, seg);
        for (si = 0; si < nseg; si++) {
            u16 c = bd;
            if (!fade_mode && (seg[si].mask & 0x20) && (eff == 2 || eff == 3) && (s->bldcnt & 0x20))
                c = bright(bd, ey, eff == 2);
            cur_argb = 0xFF000000 | ((c & 31) << 19) | (((c >> 5) & 31) << 11) | (((c >> 10) & 31) << 3);
            emit_rect(seg[si].x0, (float)y0, seg[si].x1, (float)y1, 0, 0, 0, 0, 1.0f);
        }
    }
    cur_argb = 0xFF000000;
    emit_rect(-40, -40, W + 40, 0, 0, 0, 0, 0, 100.0f);
    emit_rect(-40, H, W + 40, H + 40, 0, 0, 0, 0, 100.0f);
    emit_rect(-40, 0, 0, H, 0, 0, 0, 0, 100.0f);
    emit_rect(W, 0, W + 40, H, 0, 0, 0, 0, 100.0f);
    /* forced-blank lines are white */
    cur_argb = 0xFFFFFFFF;
    for (y0 = 0; y0 < H; y0 = y1) {
        for (y1 = y0 + 1; y1 < H && (lines[y1].dispcnt & 0x80) == (lines[y0].dispcnt & 0x80); y1++) {}
        if (lines[y0].dispcnt & 0x80) emit_rect(0, (float)y0, W, (float)y1, 0, 0, 0, 0, 95.0f);
    }
}

void hal_pvr_init(void) {
    pvr_init_params_t params = {
        {PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16},
        512 * 1024,
        0,
        0,
        0,
        3,
    };
    int i;
    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init(&params);
    /* GBA colours are 15-bit and fit the RGB565 framebuffer exactly; dithering only adds noise. */
    vid_set_dithering(false);
    pvr_set_bg_color(0, 0, 0);
    pvr_set_pal_format(PVR_PAL_ARGB1555);
    obj4 = pvr_mem_malloc(256 * 256 / 2);
    obj8 = pvr_mem_malloc(256 * 256);
    init_spread();
    for (i = 0; i < 64; i++) morton8[i] = (u8)(spread(i >> 3) | spread(i & 7) << 1);
    for (i = 0; i < 256; i++) {
        int f = i >> 6, t = i & 63, x = t & 7, y = t >> 3;
        if (f & 1) x = 7 - x;
        if (f & 2) y = 7 - y;
        mflip[f][t] = (u8)(spread(y) | spread(x) << 1);
    }
    memset(vram_shadow, 0, sizeof vram_shadow);
    for (i = 0; i < 1024; i++) convert_obj4(i);
    for (i = 0; i < 512; i++) convert_obj8(i);
    for (i = 0; i < 4; i++) bgt[i].cfg = 0xFFFF;
    pal_valid = 0;
    hal_log("pvr: renderer up, %lu KiB texture RAM free\n", (unsigned long)(pvr_mem_available() / 1024));
}

void hal_pvr_present(void) {
    static u32 reported;
    u64 t0 = hal_now_us(), t1, t2;
    u32 changed;
    int n, mode;
    u8 used[4] = {0, 0, 0, 0};

    pvr_wait_ready();
    t1 = hal_now_us();
    hal_prof.wait_us += (u32)(t1 - t0);

    /* texture and palette sync, while the previous frame's lists are no longer in use */
    rebuilt = 0;
    sync_gen++;
    {
        u64 a = hal_now_us();
        changed = (HAL_PVR_DEBUG & 16) && hal_frame_count > 1300 ? 0 : sync_vram();
        ph_vram = (u32)(hal_now_us() - a);
    }
    for (n = 0; n < H; n++) {
        int l;
        mode = lines[n].dispcnt & 7;
        for (l = 0; l < 4; l++)
            if ((lines[n].dispcnt & (0x100 << l)) && bg_exists(mode, l))
                used[l] |= bg_is_affine(mode, l) ? 2 : 1;
    }
    if ((HAL_PVR_DEBUG & 16) && hal_frame_count > 1300) memset(used, 0, sizeof used);
    ph_maps = (u32)hal_now_us();
    for (n = 0; n < 4; n++) {
        if (used[n] == 3) unsupported_seen |= 8; /* same BG text and affine in one frame */
        if (used[n] & 2) sync_affine_bg(n, lines[H - 1].bgcnt[n]);
        else if (used[n]) sync_text_bg(n, lines[H - 1].bgcnt[n]);
    }
    ph_maps = (u32)hal_now_us() - ph_maps;
    ph_mos = (u32)hal_now_us();
    plan_mosaic();
    ph_mos = (u32)hal_now_us() - ph_mos;
    plan_fade();
    if (fade_mode) USE(F_FADE_PAL);
    if (!((HAL_PVR_DEBUG & 16) && hal_frame_count > 1300)) sync_palette();
    t2 = hal_now_us();
    hal_prof.tex_us += (u32)(t2 - t1);
    if (t2 - t1 > 15000)
        hal_log("TEXSYNC frame=%lu %luus: vram=%lu (%lu chunks) maps=%lu (%lu tiles) mosaic=%lu\n",
                (unsigned long)hal_frame_count, (unsigned long)(t2 - t1), (unsigned long)ph_vram, (unsigned long)changed,
                (unsigned long)ph_maps, (unsigned long)rebuilt, (unsigned long)ph_mos);

    prims = 0;
    {
        static u32 ph[5], phn;
        u64 a = hal_now_us(), b2, c, d, e;
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_opaque();
        pvr_list_finish();
        b2 = hal_now_us();
        pvr_list_begin(PVR_LIST_PT_POLY);
        if (!((HAL_PVR_DEBUG & 32) && hal_frame_count > 1300)) draw_scene(0);
        else keepalive_poly();
        pvr_list_finish();
        c = hal_now_us();
        pvr_list_begin(PVR_LIST_TR_POLY);
        if (!(HAL_PVR_DEBUG & 1) && !((HAL_PVR_DEBUG & 32) && hal_frame_count > 1300)) draw_scene(1);
        else keepalive_poly();
        pvr_list_finish();
        d = hal_now_us();
        pvr_scene_finish();
        e = hal_now_us();
        ph[0] += (u32)(b2 - a);
        ph[1] += (u32)(c - b2);
        ph[2] += (u32)(d - c);
        ph[3] += (u32)(e - d);
        ph[4] += prims;
        if (++phn == 120) {
            pvr_stats_t st;
            pvr_get_stats(&st);
            hal_log("pvr phases (avg us): op+begin=%lu pt=%lu tr=%lu finish=%lu polys=%lu | last frame: reg=%luus "
                    "render=%luus ready-to-ready=%luus vbuf=%lu\n",
                    (unsigned long)(ph[0] / 120), (unsigned long)(ph[1] / 120), (unsigned long)(ph[2] / 120),
                    (unsigned long)(ph[3] / 120), (unsigned long)(ph[4] / 120), (unsigned long)(st.reg_last_time / 1000),
                    (unsigned long)(st.rnd_last_time / 1000), (unsigned long)(st.frame_last_time / 1000),
                    (unsigned long)st.vtx_buffer_used);
            memset(ph, 0, sizeof ph);
            phn = 0;
        }
    }
    hal_prof.submit_us += (u32)(hal_now_us() - t2);

    {
        static int was;
        int now = ((feature_now >> F_MOSAIC) | (feature_now >> F_OBJ_MOSAIC)) & 1;
        if (now != was) {
            u16 bgs = 0;
            int l;
            for (l = 0; l < 4; l++)
                if ((lines[0].bgcnt[l] & 0x40) && (lines[0].dispcnt & (0x100 << l))) bgs |= 1 << l;
            hal_log("pvr: mosaic %s at frame %lu (MOSAIC=%04x, mosaic BGs=%x)\n", now ? "on" : "off",
                    (unsigned long)hal_frame_count, lines[0].mosaic, bgs);
            was = now;
        }
    }
    {
        int f;
        for (f = 0; f < F_COUNT; f++)
            if (feature_now & (1u << f)) feature_frames[f]++;
        feature_now = 0;
    }
    if (unsupported_seen & ~reported) {
        if (unsupported_seen & 1) hal_log("pvr: OBJ window used (not implemented)\n");
        if (unsupported_seen & 2) hal_log("pvr: OBJ mosaic used (not implemented)\n");
        if (unsupported_seen & 4) hal_log("pvr: bitmap mode used (not implemented)\n");
        if (unsupported_seen & 8) hal_log("pvr: BG switches text/affine within a frame\n");
        reported |= unsupported_seen;
    }
    {
        static u16 last_dc, last_cnt[4];
        const struct lstate *s0 = &lines[0];
        if (s0->dispcnt != last_dc || memcmp(last_cnt, s0->bgcnt, sizeof last_cnt)) {
            last_dc = s0->dispcnt;
            memcpy(last_cnt, s0->bgcnt, sizeof last_cnt);
            hal_log("pvr: frame %lu DISPCNT=%04x BGCNT=%04x %04x %04x %04x BLDCNT=%04x tex %ux%u %ux%u %ux%u %ux%u\n",
                    (unsigned long)hal_frame_count, s0->dispcnt, s0->bgcnt[0], s0->bgcnt[1], s0->bgcnt[2], s0->bgcnt[3],
                    s0->bldcnt, bgt[0].w, bgt[0].h, bgt[1].w, bgt[1].h, bgt[2].w, bgt[2].h, bgt[3].w, bgt[3].h);
        }
    }
    if ((hal_frame_count % 120) == 0)
        hal_log("pvr: frame %lu: %lu polys, %lu VRAM chunks changed, %lu map tiles rebuilt\n",
                (unsigned long)hal_frame_count, (unsigned long)prims, (unsigned long)changed, (unsigned long)rebuilt);
}

/* Read the last rendered frame back (test dumps only). The framebuffer is linear in the 32-bit
 * VRAM area; pvr_get_front_buffer() returns it translated for the 64-bit area, so undo that.
 * Needs flycast's framebuffer emulation (-config config:rend.EmulateFramebuffer=yes): by default
 * flycast draws straight to the host window and never writes the picture back into VRAM. */
void hal_pvr_report_features(void) {
    int f;
    hal_log("FEATURES frames using each GBA effect (of %lu):\n", (unsigned long)hal_frame_count);
    for (f = 0; f < F_COUNT; f++) hal_log("FEATURE %s: %lu\n", feature_name[f], (unsigned long)feature_frames[f]);
}

void hal_pvr_readback(u16 *dst) {
    const volatile u16 *fb;
    u32 front, frame;
    int x, y;
    pvr_wait_render_done();
    front = (u32)pvr_get_front_buffer();
    frame = (front - PVR_RAM_BASE) / 2;
    fb = (const volatile u16 *)(PVR_RAM_BASE + frame);
    {
        static int logged;
        if (!logged++) {
            u32 sums[3] = {0, 0, 0}, cand[3] = {PVR_RAM_BASE + frame, front, PVR_RAM_INT_BASE + frame * 2};
            int k;
            for (k = 0; k < 3; k++)
                for (x = 0; x < 640 * 16; x++) sums[k] += ((const volatile u16 *)cand[k])[240 * 640 + x];
            hal_log("pvr readback: front=%08lx cand32=%08lx sum=%lu front sum=%lu int sum=%lu\n", (unsigned long)front,
                    (unsigned long)cand[0], (unsigned long)sums[0], (unsigned long)sums[1], (unsigned long)sums[2]);
        }
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            u16 p = fb[(80 + y * 2) * 640 + 80 + x * 2];
            dst[y * W + x] = (u16)(((p >> 11) & 31) | (((p >> 6) & 31) << 5) | ((p & 31) << 10));
        }
}

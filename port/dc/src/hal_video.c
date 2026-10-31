/* Software GBA PPU, one scanline at a time.
 *
 * Reads the game's own DISPCNT/BGxCNT/scroll/affine/window/blend registers, VRAM, OAM and
 * palette RAM through their GBA addresses (mapped by hal_mem) and produces 240 pixels per line.
 * Lines are rendered between the game's HBlank handlers (hal_frame), so per-line raster effects
 * work. Supported: modes 0-2 (text and affine BGs), regular and affine sprites (1D/2D mapping,
 * 4/8bpp, semi-transparent, OBJ window), priorities, WIN0/WIN1/OBJWIN, alpha blending and
 * brightness increase/decrease, forced blank. Not yet: mosaic, bitmap modes 3-5.
 *
 * Output: 240x160 scaled 2x into the 640x480 RGB565 framebuffer at (80, 80).
 *
 * This is a correctness-first reference renderer. It is far too slow for real hardware at 60
 * fps; a PowerVR tile renderer is the planned replacement.
 */
#include <dc/video.h>
#include <string.h>

#include "hal.h"

#define W 240
#define H 160

#define DISPCNT IO16(0x000)
#define BGCNT(n) IO16(0x008 + 2 * (n))
#define BGHOFS(n) IO16(0x010 + 4 * (n))
#define BGVOFS(n) IO16(0x012 + 4 * (n))
#define BGPA(n) (*(volatile s16 *)(GBA_IO + 0x020 + 0x10 * ((n)-2)))
#define BGPB(n) (*(volatile s16 *)(GBA_IO + 0x022 + 0x10 * ((n)-2)))
#define BGPC(n) (*(volatile s16 *)(GBA_IO + 0x024 + 0x10 * ((n)-2)))
#define BGPD(n) (*(volatile s16 *)(GBA_IO + 0x026 + 0x10 * ((n)-2)))
#define BGX(n) IO32(0x028 + 0x10 * ((n)-2))
#define BGY(n) IO32(0x02C + 0x10 * ((n)-2))
#define WINH(n) IO16(0x040 + 2 * (n))
#define WINV(n) IO16(0x044 + 2 * (n))
#define WININ IO16(0x048)
#define WINOUT IO16(0x04A)
#define BLDCNT IO16(0x050)
#define BLDALPHA IO16(0x052)
#define BLDY IO16(0x054)

#define PLTT16 ((volatile u16 *)GBA_PLTT)
#define VRAM8 ((volatile u8 *)GBA_VRAM)
#define VRAM16 ((volatile u16 *)GBA_VRAM)
#define OAM16 ((volatile u16 *)GBA_OAM)

/* layer ids: 0-3 BG, 4 OBJ, 5 backdrop */
#define L_OBJ 4
#define L_BD 5

static u16 bgline[4][W];
static u8 bgopaque[4][W];
static u16 objline[W];
static u8 objprio[W];
static u8 objsemi[W];
static u8 objopaque[W];
static u8 objwin[W];
static u16 out[W];
static s32 affx[4], affy[4];
static u32 last_bgx[4], last_bgy[4];

static void text_bg(int n, int line) {
    u16 cnt = BGCNT(n);
    u32 charbase = ((cnt >> 2) & 3) * 0x4000;
    u32 screenbase = ((cnt >> 8) & 0x1F) * 0x800;
    int bpp8 = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int wtiles = (size & 1) ? 64 : 32, htiles = (size & 2) ? 64 : 32;
    int y = (line + BGVOFS(n)) & (htiles * 8 - 1);
    int ty = y >> 3, x, sx = BGHOFS(n);

    for (x = 0; x < W; x++) {
        int px = (x + sx) & (wtiles * 8 - 1);
        int tx = px >> 3;
        u32 block = (u32)((tx >> 5) + ((ty >> 5) * (wtiles >> 5)));
        u16 e = VRAM16[(screenbase + block * 0x800) / 2 + (ty & 31) * 32 + (tx & 31)];
        int fx = px & 7, fy = y & 7, c;
        if (e & 0x400) fx = 7 - fx;
        if (e & 0x800) fy = 7 - fy;
        if (bpp8) {
            c = VRAM8[(charbase + (e & 0x3FF) * 64 + fy * 8 + fx) & 0xFFFF];
            bgopaque[n][x] = c != 0;
            bgline[n][x] = PLTT16[c];
        } else {
            u8 b = VRAM8[(charbase + (e & 0x3FF) * 32 + fy * 4 + (fx >> 1)) & 0xFFFF];
            c = (fx & 1) ? b >> 4 : b & 0xF;
            bgopaque[n][x] = c != 0;
            bgline[n][x] = PLTT16[((e >> 12) << 4) + c];
        }
    }
}

static s32 sext28(u32 v) { return (s32)(v << 4) >> 4; }

static void affine_latch(int n, int line) {
    u32 rx = BGX(n), ry = BGY(n);
    if (line == 0 || rx != last_bgx[n] || ry != last_bgy[n]) {
        affx[n] = sext28(rx);
        affy[n] = sext28(ry);
        last_bgx[n] = rx;
        last_bgy[n] = ry;
    }
}

static void affine_bg(int n) {
    u16 cnt = BGCNT(n);
    u32 charbase = ((cnt >> 2) & 3) * 0x4000;
    u32 screenbase = ((cnt >> 8) & 0x1F) * 0x800;
    int wrap = (cnt >> 13) & 1;
    int sz = 128 << (cnt >> 14);
    s32 pa = BGPA(n), pc = BGPC(n);
    s32 x = affx[n], y = affy[n];
    int i;

    for (i = 0; i < W; i++, x += pa, y += pc) {
        int px = x >> 8, py = y >> 8, c;
        if (wrap) {
            px &= sz - 1;
            py &= sz - 1;
        } else if (px < 0 || py < 0 || px >= sz || py >= sz) {
            bgopaque[n][i] = 0;
            continue;
        }
        {
            u8 tile = VRAM8[screenbase + (py >> 3) * (sz >> 3) + (px >> 3)];
            c = VRAM8[(charbase + tile * 64 + (py & 7) * 8 + (px & 7)) & 0xFFFF];
        }
        bgopaque[n][i] = c != 0;
        bgline[n][i] = PLTT16[c];
    }
    affx[n] += BGPB(n);
    affy[n] += BGPD(n);
}

static const u8 obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const u8 obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static void sprites(int line) {
    int map1d = (DISPCNT >> 6) & 1;
    int i;
    memset(objopaque, 0, sizeof objopaque);
    memset(objwin, 0, sizeof objwin);
    for (i = 0; i < W; i++) objprio[i] = 4;

    for (i = 0; i < 128; i++) {
        u16 a0 = OAM16[i * 4], a1 = OAM16[i * 4 + 1], a2 = OAM16[i * 4 + 2];
        int affine = (a0 >> 8) & 1, dbl = (a0 >> 9) & 1;
        int shape = a0 >> 14, size = a1 >> 14;
        int mode = (a0 >> 10) & 3, bpp8 = (a0 >> 13) & 1;
        int w, h, bw, bh, y, x, sy, prio, tile, pal, cx, cy;
        s16 pa = 0x100, pb = 0, pc = 0, pd = 0x100;

        if (!affine && dbl) continue; /* disabled */
        if (shape == 3) continue;
        w = obj_w[shape][size];
        h = obj_h[shape][size];
        bw = w << (affine && dbl);
        bh = h << (affine && dbl);
        y = a0 & 0xFF;
        if (y >= 160) y -= 256;
        sy = line - y;
        if (sy < 0 || sy >= bh) continue;
        x = a1 & 0x1FF;
        if (x >= 240) x -= 512;
        prio = (a2 >> 10) & 3;
        tile = a2 & 0x3FF;
        pal = a2 >> 12;
        if (affine) {
            int p = (a1 >> 9) & 0x1F;
            pa = (s16)OAM16[p * 16 + 3];
            pb = (s16)OAM16[p * 16 + 7];
            pc = (s16)OAM16[p * 16 + 11];
            pd = (s16)OAM16[p * 16 + 15];
        }
        cx = bw / 2;
        cy = bh / 2;
        {
            int sx;
            for (sx = 0; sx < bw; sx++) {
                int px = x + sx, tx, ty, c;
                u32 addr;
                if (px < 0 || px >= W) continue;
                if (affine) {
                    tx = ((pa * (sx - cx) + pb * (sy - cy)) >> 8) + w / 2;
                    ty = ((pc * (sx - cx) + pd * (sy - cy)) >> 8) + h / 2;
                    if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
                } else {
                    tx = (a1 & 0x1000) ? w - 1 - sx : sx;
                    ty = (a1 & 0x2000) ? h - 1 - sy : sy;
                }
                if (bpp8) {
                    u32 t = map1d ? tile + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2 : tile + (ty >> 3) * 32 + (tx >> 3) * 2;
                    addr = 0x10000 + (t & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7);
                    c = VRAM8[addr];
                    if (!c) continue;
                    c += 256;
                } else {
                    u32 t = map1d ? tile + (ty >> 3) * (w >> 3) + (tx >> 3) : tile + (ty >> 3) * 32 + (tx >> 3);
                    u8 b;
                    addr = 0x10000 + (t & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
                    b = VRAM8[addr];
                    c = (tx & 1) ? b >> 4 : b & 0xF;
                    if (!c) continue;
                    c += 256 + pal * 16;
                }
                if (mode == 2) {
                    objwin[px] = 1;
                    continue;
                }
                if (objopaque[px] && objprio[px] <= prio) continue;
                objline[px] = PLTT16[c];
                objprio[px] = (u8)prio;
                objsemi[px] = mode == 1;
                objopaque[px] = 1;
            }
        }
    }
}

static u16 blend_alpha(u16 a, u16 b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (u16)(r | (g << 5) | (bl << 10));
}

static u16 brighten(u16 c, int ey, int up) {
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

static int in_win(int n, int x, int line) {
    int x1 = WINH(n) >> 8, x2 = WINH(n) & 0xFF, y1 = WINV(n) >> 8, y2 = WINV(n) & 0xFF;
    int inx = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
    int iny = y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
    return inx && iny;
}

void hal_video_render_line(int line) {
    u16 dc = DISPCNT;
    int mode = dc & 7, n, x;
    int bg_on[4] = {0, 0, 0, 0};
    int bg_aff[4] = {0, 0, 0, 0};
    u16 bldcnt, backdrop;
    int eva, evb, ey, effect;

    if (dc & 0x80) {
        for (x = 0; x < W; x++) out[x] = 0x7FFF;
        goto emit;
    }
    if (mode <= 2) {
        for (n = 0; n < 4; n++) {
            if (!(dc & (0x100 << n))) continue;
            if (mode == 0 || (mode == 1 && n < 2)) {
                bg_on[n] = 1;
                text_bg(n, line);
            } else if ((mode == 1 && n == 2) || (mode == 2 && n >= 2)) {
                bg_on[n] = 1;
                bg_aff[n] = 1;
            }
        }
        for (n = 2; n < 4; n++) {
            if (mode >= 1) affine_latch(n, line);
            if (bg_aff[n]) affine_bg(n);
        }
    }
    if (dc & 0x1000) sprites(line);
    else {
        memset(objopaque, 0, sizeof objopaque);
        memset(objwin, 0, sizeof objwin);
    }

    bldcnt = BLDCNT;
    effect = (bldcnt >> 6) & 3;
    eva = BLDALPHA & 31;
    evb = (BLDALPHA >> 8) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    ey = BLDY & 31;
    if (ey > 16) ey = 16;
    backdrop = PLTT16[0];

    for (x = 0; x < W; x++) {
        int mask = 0x3F, top = L_BD, second = L_BD, tp = 4, sp = 4, fx;
        u16 ct = backdrop, cs = backdrop;
        if (dc & 0xE000) {
            if ((dc & 0x2000) && in_win(0, x, line)) mask = WININ & 0x3F;
            else if ((dc & 0x4000) && in_win(1, x, line)) mask = (WININ >> 8) & 0x3F;
            else if ((dc & 0x8000) && objwin[x]) mask = (WINOUT >> 8) & 0x3F;
            else mask = WINOUT & 0x3F;
        }
        /* find the top two layers: lower priority value wins; OBJ beats BG on ties; lower BG index
         * beats higher. */
        if ((mask & 0x10) && objopaque[x]) {
            top = L_OBJ;
            tp = objprio[x];
            ct = objline[x];
        }
        for (n = 0; n < 4; n++) {
            int p;
            if (!bg_on[n] || !bgopaque[n][x] || !(mask & (1 << n))) continue;
            p = BGCNT(n) & 3;
            if (p < tp || (p == tp && top != L_OBJ && n < top)) {
                second = top;
                sp = tp;
                cs = ct;
                top = n;
                tp = p;
                ct = bgline[n][x];
            } else if (p < sp || (p == sp && second != L_OBJ && n < second)) {
                second = n;
                sp = p;
                cs = bgline[n][x];
            }
        }
        if (top != L_OBJ && (mask & 0x10) && objopaque[x] && objprio[x] <= sp) {
            second = L_OBJ;
            cs = objline[x];
        }
        fx = (mask & 0x20) ? effect : 0;
        if (top == L_OBJ && objsemi[x] && (bldcnt & (0x100 << second))) {
            ct = blend_alpha(ct, cs, eva, evb);
        } else if (fx && (bldcnt & (1 << top))) {
            if (fx == 1 && (bldcnt & (0x100 << second))) ct = blend_alpha(ct, cs, eva, evb);
            else if (fx == 2) ct = brighten(ct, ey, 1);
            else if (fx == 3) ct = brighten(ct, ey, 0);
        }
        out[x] = ct;
    }

emit : {
    volatile u16 *fb = vram_s + (80 + line * 2) * 640 + 80;
    for (x = 0; x < W; x++) {
        u16 c = out[x];
        u16 p = (u16)(((c & 31) << 11) | (((c >> 5) & 31) << 6) | ((c >> 10) & 31));
        fb[x * 2] = fb[x * 2 + 1] = p;
        fb[640 + x * 2] = fb[640 + x * 2 + 1] = p;
    }
}
}

void hal_video_init(void) {
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 0);
}

void hal_video_present(void) { vid_waitvbl(); }

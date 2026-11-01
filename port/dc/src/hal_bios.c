/* GBA BIOS calls used by the game (libagbsyscall.s in kleod), plus the small hand-written
 * helpers from util.s, heap_arm.s and rand_arm.s, reimplemented in C with identical results. */
#include <arch/arch.h>
#include <string.h>

#include "hal.h"

/* ---- BIOS ------------------------------------------------------------------------------- */

void CpuSet(const void *src, void *dest, u32 control) {
    u32 n = control & 0x1FFFFF;
    int fixed = (control & 0x01000000) != 0;
    u32 unit = control & 0x04000000 ? 4 : 2;
    /* The BIOS refuses a source in its own region; writes to unmapped memory have no effect. */
    if (!hal_gba_range_ok((u32)(uintptr_t)src, fixed ? unit : n * unit, 0) ||
        !hal_gba_range_ok((u32)(uintptr_t)dest, n * unit, 1)) {
        static int logged;
        if (logged < 4) {
            logged++;
            hal_log("CpuSet: %p -> %p (%lu units) outside mapped memory, ignored\n", src, dest, (unsigned long)n);
        }
        return;
    }
    if (control & 0x04000000) {
        const volatile u32 *s = (const volatile u32 *)((uintptr_t)src & ~3u);
        volatile u32 *d = (volatile u32 *)((uintptr_t)dest & ~3u);
        u32 v = *s;
        while (n--) {
            *d++ = fixed ? v : *s++;
        }
    } else {
        const volatile u16 *s = (const volatile u16 *)((uintptr_t)src & ~1u);
        volatile u16 *d = (volatile u16 *)((uintptr_t)dest & ~1u);
        u16 v = *s;
        while (n--) {
            *d++ = fixed ? v : *s++;
        }
    }
}

/* Header: u32 (type<<4 | size<<8). LZ77 type 1. Decompresses byte-wise ("Wram" variant). */
void LZ77UnCompWram(const void *src, void *dest) {
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dest;
    u32 size = (s[1] | (s[2] << 8) | (s[3] << 16));
    u8 *end = d + size;
    s += 4;
    while (d < end) {
        u8 flags = *s++;
        int i;
        for (i = 0; i < 8 && d < end; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = (((s[0] & 0xF) << 8) | s[1]) + 1;
                s += 2;
                while (len-- && d < end) {
                    *d = *(d - disp);
                    d++;
                }
            } else {
                *d++ = *s++;
            }
        }
    }
}

/* GBA BIOS Huffman: header u32 (bits | 0x20 | size<<8), tree size byte, tree, then 32-bit LE
 * bitstream words read MSB first. Output is assembled into 32-bit words. */
void HuffUnComp(const void *src, void *dest) {
    const u8 *s = (const u8 *)src;
    u32 *d = (u32 *)dest;
    u32 bits = s[0] & 0xF;
    u32 size = s[1] | (s[2] << 8) | (s[3] << 16);
    const u8 *tree = s + 5;          /* root node */
    const u32 *stream = (const u32 *)(s + 4 + (s[4] + 1) * 2);
    u32 out = 0, outbits = 0, written = 0;
    const u8 *node = tree;
    u32 word = 0;
    int left = 0;

    {
        static int calls;
        if (calls++ < 12)
            hal_log("HuffUnComp #%d src=%08lx hdr=%02x%02x%02x%02x ts=%02x dst=%08lx\n", calls, (unsigned long)(uintptr_t)src, s[3],
                    s[2], s[1], s[0], s[4], (unsigned long)(uintptr_t)dest);
    }
    if ((s[0] & 0xF0) != 0x20 || (bits != 4 && bits != 8)) {
        hal_log("HuffUnComp: not a Huffman stream at %08lx, skipped\n", (unsigned long)(uintptr_t)src);
        return;
    }
    while (written < size) {
        u32 bit;
        u8 n;
        int is_leaf;
        if (left == 0) {
            const u8 *w = (const u8 *)stream;
            word = w[0] | (w[1] << 8) | (w[2] << 16) | ((u32)w[3] << 24);
            stream++;
            left = 32;
        }
        bit = (word >> 31) & 1;
        word <<= 1;
        left--;
        n = *node;
        is_leaf = bit ? (n & 0x40) : (n & 0x80);
        node = (const u8 *)(((uintptr_t)node & ~(uintptr_t)1) + ((n & 0x3F) + 1) * 2 + bit);
        if (is_leaf) {
            out |= (u32)(*node) << outbits;
            outbits += bits;
            node = tree;
            if (outbits == 32) {
                *d++ = out;
                written += 4;
                out = 0;
                outbits = 0;
            }
        }
    }
}

u16 Sqrt(u32 num) {
    u32 res = 0, bit = 1u << 30;
    while (bit > num) bit >>= 2;
    while (bit) {
        if (num >= res + bit) {
            num -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (u16)res;
}

void SoftResetRom(u32 resetFlags) {
    (void)resetFlags;
    hal_log("SoftResetRom: returning to Dreamcast menu\n");
    arch_menu();
}

/* m4a0.s helper: high 32 bits of an unsigned 32x32 multiply. */
u32 umul3232H32(u32 a, u32 b) { return (u32)(((uint64_t)a * b) >> 32); }

/* ---- util.s ----------------------------------------------------------------------------- */

s32 Abs(s32 n) { return n < 0 ? -n : n; }

s32 StringCompare(u8 *s1, u8 *s2) {
    for (;;) {
        u8 a = *s1++, b = *s2++;
        if (a != b) return 1;
        if (b == 0) return 0;
    }
}

void StringCopy(u8 *dest, u8 *src) {
    u8 c;
    do {
        c = *src++;
        *dest++ = c;
    } while (c);
}

/* ---- heap_arm.s: bump allocator in EWRAM ------------------------------------------------- */

extern u8 *gHeapPtr;

void HeapInit(void) { gHeapPtr = (u8 *)GBA_EWRAM; }

void *HeapAlloc(s32 count, s32 size_shift) {
    u32 size = (11 + ((u32)count << size_shift)) & ~3u;
    u8 *blk = gHeapPtr;
    *(u32 *)blk = size;
    *(u32 *)(blk + size - 4) = (u32)(uintptr_t)blk;
    gHeapPtr = blk + size;
    return blk + 4;
}

void HeapFree(void *heapPtr) { gHeapPtr = (u8 *)heapPtr - 4; }
u8 *HeapGetPtr(void) { return gHeapPtr; }
u32 HeapGetUsed(void) { return (u32)(uintptr_t)gHeapPtr - GBA_EWRAM; }
u32 HeapGetFree(void) { return GBA_EWRAM + GBA_EWRAM_SIZE - (u32)(uintptr_t)gHeapPtr; }

/* ---- rand_arm.s: 8-bit table RNG (bit-exact; attract-mode demos depend on it) ------------- */

extern u8 gRngState;
extern u8 gRngCount;
#define RNG_TABLE ((const volatile u8 *)0x08000318) /* gRngTable, read from the mapped ROM */

void UpdateRng(void) {
    gRngCount += 1;
    gRngState = (u8)(gRngState * 5 + 0x11);
}

u8 GetRandomValue(void) {
    u8 v = RNG_TABLE[gRngState];
    gRngState = (u8)(gRngState + (gRngCount | 1));
    return v;
}

u8 GetRandomValueEx(void) {
    u8 v = RNG_TABLE[gRngState];
    u8 st = (u8)(gRngState + (gRngCount | 1));
    gRngCount += 1;
    gRngState = (u8)(st * 5 + 0x11);
    return v;
}

/* thunk.s: on the GBA these jump to the ARM copies in IWRAM. */
void thunk_HeapInit(void) { HeapInit(); }
void *thunk_HeapAlloc(s32 count, s32 size_shift) { return HeapAlloc(count, size_shift); }
void thunk_HeapFree(void *p) { HeapFree(p); }
void thunk_UpdateRng(void) { UpdateRng(); }
u8 thunk_GetRandomValue(void) { return GetRandomValue(); }
u8 thunk_GetRandomValueEx(void) { return GetRandomValueEx(); }

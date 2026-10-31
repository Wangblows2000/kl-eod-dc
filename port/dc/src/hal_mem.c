/* Map the GBA address space onto Dreamcast RAM with static SH-4 TLB entries.
 *
 * KallistiOS runs in P1 (0x8C000000, untranslated), so the whole P0 range is free. With the MMU
 * on, the game's raw GBA addresses -- 2,800 literal I/O-register accesses, fixed IWRAM globals,
 * VRAM/OAM/palette, and pointers embedded in ROM data -- all resolve to these buffers.
 *
 * Buffers are aligned to their page size, so a buffer's P1 alias and its GBA alias share the
 * same low address bits and the same operand-cache index (no synonyms).
 */
#include <arch/mmu.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "hal.h"

extern const u8 hal_rom_image[];     /* rom_blob.S: the user's baserom.gba */
extern const u8 hal_rom_image_end[];

#define P1_TO_PHYS(p) ((uintptr_t)(p) & 0x1FFFFFFFu)
#define K64 0x10000u
#define M1 0x100000u

static u8 *small_arena; /* 640 KiB: EWRAM 256K, IWRAM 64K, IO 64K, PLTT 64K, VRAM 128K, OAM 64K */
static u8 *rom_arena;   /* 4 MiB */

static int map_range(u32 virt, u8 *buf, u32 size, page_size_t ps, u32 psize) {
    u32 off;
    for (off = 0; off < size; off += psize) {
        if (mmu_page_map_static(virt + off, P1_TO_PHYS(buf + off), ps, MMU_ALL_RDWR, true) < 0) {
            hal_log("mmu: map %08lx -> %08lx failed\n", (unsigned long)(virt + off),
                    (unsigned long)P1_TO_PHYS(buf + off));
            return -1;
        }
    }
    return 0;
}

int hal_mem_init(void) {
    u32 romlen = (u32)(hal_rom_image_end - hal_rom_image);
    u8 *p;

    if (romlen != GBA_ROM_SIZE) {
        hal_log("rom image is %lu bytes, expected %lu\n", (unsigned long)romlen, (unsigned long)GBA_ROM_SIZE);
        return -1;
    }

    small_arena = memalign(K64, 10 * K64);
    rom_arena = memalign(M1, GBA_ROM_SIZE);
    if (!small_arena || !rom_arena) {
        hal_log("mem: out of memory\n");
        return -1;
    }
    memset(small_arena, 0, 10 * K64);
    memcpy(rom_arena, hal_rom_image, GBA_ROM_SIZE);

    mmu_init_basic();

    p = small_arena;
    if (map_range(GBA_EWRAM, p, 4 * K64, PAGE_SIZE_64K, K64) < 0) return -1;
    p += 4 * K64;
    if (map_range(GBA_IWRAM, p, K64, PAGE_SIZE_64K, K64) < 0) return -1;
    p += K64;
    if (map_range(GBA_IO, p, K64, PAGE_SIZE_64K, K64) < 0) return -1;
    p += K64;
    if (map_range(GBA_PLTT, p, K64, PAGE_SIZE_64K, K64) < 0) return -1;
    p += K64;
    if (map_range(GBA_VRAM, p, 2 * K64, PAGE_SIZE_64K, K64) < 0) return -1;
    p += 2 * K64;
    if (map_range(GBA_OAM, p, K64, PAGE_SIZE_64K, K64) < 0) return -1;
    if (map_range(GBA_ROM, rom_arena, GBA_ROM_SIZE, PAGE_SIZE_1M, M1) < 0) return -1;

    /* Smoke test through the GBA aliases: the ROM header's fixed byte and an IWRAM round trip. */
    if (*(volatile u8 *)(GBA_ROM + 0xB2) != 0x96) {
        hal_log("mem: ROM alias check failed (got %02x)\n", *(volatile u8 *)(GBA_ROM + 0xB2));
        return -1;
    }
    *(volatile u32 *)(GBA_IWRAM + 0x7FF0) = 0xC0FFEE11;
    if (*(volatile u32 *)(small_arena + 4 * K64 + 0x7FF0) != 0xC0FFEE11) {
        hal_log("mem: IWRAM alias check failed\n");
        return -1;
    }
    *(volatile u32 *)(GBA_IWRAM + 0x7FF0) = 0;
    hal_log("mem: GBA address space mapped (14 static TLB entries)\n");
    return 0;
}

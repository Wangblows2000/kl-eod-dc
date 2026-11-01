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
static u8 *rom_arena;   /* 4 MiB: the embedded image itself when it is 1 MiB-aligned */

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
    if (!small_arena) {
        hal_log("mem: out of memory\n");
        return -1;
    }
    memset(small_arena, 0, 10 * K64);
    /* The embedded image is 1 MiB-aligned (rom_blob.S): map it in place. */
    if ((uintptr_t)hal_rom_image & (M1 - 1)) {
        rom_arena = memalign(M1, GBA_ROM_SIZE);
        if (!rom_arena) {
            hal_log("mem: out of memory\n");
            return -1;
        }
        memcpy(rom_arena, hal_rom_image, GBA_ROM_SIZE);
        hal_log("mem: ROM image not 1 MiB-aligned, copied\n");
    } else {
        rom_arena = (u8 *)hal_rom_image;
    }

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

/* Is [addr, addr + len) backed by memory the game may write (write = 1) or read (write = 0)?
 *
 * On the GBA, a DMA or CpuSet that targets the BIOS (0x00000000) or an unused region simply has no
 * effect, and the game relies on that: VBlankIntr_Common copies BG1's tilemap to
 * gBgInfo[1].pTilemap every frame, and during the boot screen that pointer is still NULL. Here
 * those addresses have no TLB entry, so the write must be dropped before it reaches the MMU --
 * under flycast it landed in unrelated Dreamcast memory (which variable depended on the link
 * layout), and on hardware it would raise a TLB-miss exception. Pointers into Dreamcast RAM
 * (P1 0x8C000000-0x8CFFFFFF: the game's stack and statics) are accepted as they are.
 */
int hal_gba_range_ok(u32 addr, u32 len, int write) {
    u32 end = addr + len;
    if (end < addr) return 0;
    if (addr >= 0x8C000000u && end <= 0x8D000000u) return 1;
    switch (addr >> 24) {
    case 0x02: return end <= GBA_EWRAM + 4 * K64;
    case 0x03: return end <= GBA_IWRAM + K64;
    case 0x04: return end <= GBA_IO + K64;
    case 0x05: return end <= GBA_PLTT + K64;
    case 0x06: return end <= GBA_VRAM + 2 * K64;
    case 0x07: return end <= GBA_OAM + K64;
    case 0x08: return !write && end <= GBA_ROM + GBA_ROM_SIZE;
    default: return 0;
    }
}

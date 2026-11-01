/* The user's own baserom.gba, embedded at build time (never distributed). It is aligned to
 * 1 MiB so hal_mem can map it at 0x08000000 in place with 1 MiB pages (no 4 MiB copy). */
    .section .rodata.gbarom, "a"
    .balign 0x100000
    .global _hal_rom_image
    .global _hal_rom_image_end
_hal_rom_image:
    .incbin "baserom.gba"
_hal_rom_image_end:

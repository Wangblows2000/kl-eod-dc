/* The user's own baserom.gba, embedded at build time (never distributed). hal_mem copies it
 * into a 1 MiB-aligned buffer and maps that at 0x08000000. */
    .section .rodata
    .balign 32
    .global _hal_rom_image
    .global _hal_rom_image_end
_hal_rom_image:
    .incbin "baserom.gba"
_hal_rom_image_end:

# Build-time edits to the game sources, applied to a copy of each kleod src/*.c before
# preprocessing (kleod's files on disk are never changed). Every rule names the one construct it
# replaces and why; tools/census.py counts them.

# m4a.c MusicPlayerJumpTableCopy: `asm("swi 0x2A")` is a GBA BIOS call (copy the BIOS's MP2K
# jump table) with no SH-4 meaning. The DC sound backend does not use it.
s/asm("swi 0x2A");//

# m4a.c SampleFreqSet: busy-waits for scanline 159 to start the sound timers in phase with
# VBlank. On the DC the scanline counter only advances when the game waits on it, so the wait
# becomes a HAL call that runs the emulated beam to that line.
s/while (\*(vu8 \*)REG_ADDR_VCOUNT == 159);/{ extern void hal_wait_vcount_not(int); hal_wait_vcount_not(159); }/
s/while (\*(vu8 \*)REG_ADDR_VCOUNT != 159);/{ extern void hal_wait_vcount(int); hal_wait_vcount(159); }/

# rotation.c: waits for scanline 0 before enabling the VBlank interrupt (rotating rooms).
s/while (REG_VCOUNT_L != 0);/{ extern void hal_wait_vcount(int); hal_wait_vcount(0); }/

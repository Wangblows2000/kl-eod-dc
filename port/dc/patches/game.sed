# Build-time edits to the preprocessed game sources (kleod's files on disk are never changed).
# Each line must name the one construct it fixes.

# m4a.c: MusicPlayerJumpTableCopy is `asm("swi 0x2A")`, a GBA BIOS call with no SH-4 meaning
# (it copies the BIOS's MP2K jump table). The DC sound backend does not use it.
s/asm("swi 0x2A");//

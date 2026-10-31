/* MP2K ("m4a") sound engine pieces that kleod has only as Thumb assembly (asm/m4a0.s) and that
 * the C sequencer in m4a.c calls through its jump table. Translated instruction-for-instruction
 * with the same structure offsets. The PCM mixer itself (SoundMain/SoundMainRAM) and the other
 * sequencer events are still stubs (tools/gen_symbols.py), so the game runs silently.
 */
#include <string.h>

#include "hal.h"

extern void *const gMPlayJumpTableTemplate[36];

/* 0x0804F724: copy the 36-entry template (on hardware the BIOS supplies part of it). */
void MPlayJumpTableCopy(void **dest) { memcpy(dest, gMPlayJumpTableTemplate, 36 * sizeof(void *)); }

/* 0x0804F6BC (jump table slot 35, used by Clear64byte): zero 64 bytes. */
void SoundMainBTM(void *p) { memset(p, 0, 64); }

/* 0x0804F6D4: unlink a mixer channel from its track's channel chain. Offsets: channel +0x2C
 * track, +0x30 prev, +0x34 next; track +0x20 chain head. */
void MP2KClearChain(void *chan) {
    u8 *c = (u8 *)chan;
    u8 *track = *(u8 **)(c + 0x2C);
    u8 *next, *prev;
    if (!track) return;
    next = *(u8 **)(c + 0x34);
    prev = *(u8 **)(c + 0x30);
    if (prev) *(u8 **)(prev + 0x34) = next;
    else *(u8 **)(track + 0x20) = next;
    if (next) *(u8 **)(next + 0x30) = prev;
    *(u8 **)(c + 0x2C) = 0;
}

/* 0x0804F6F4: FINE event -- release every channel of the track and stop it. */
void MP2K_event_fine(void *mplay, void *track) {
    u8 *t = (u8 *)track;
    u8 *ch = *(u8 **)(t + 0x20);
    (void)mplay;
    while (ch) {
        if (*ch & 0xC7) *ch |= 0x40;
        MP2KClearChain(ch);
        ch = *(u8 **)(ch + 0x34);
    }
    *t = 0;
}

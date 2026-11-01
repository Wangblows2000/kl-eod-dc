/* The parts of the MP2K ("m4a") sound engine that kleod has only as GBA assembly (asm/m4a0.s):
 * the sequencer (MP2KPlayerMain and the track commands), note-on, TrackStop, m4aSoundVSync, and
 * the DirectSound mixer (SoundMain / SoundMainRAM).
 *
 * Each function is a translation of the corresponding routine in asm/m4a0.s, keeping its
 * control flow, arithmetic and the structure offsets it uses, so that the sequencer state and
 * the 8-bit pcmBuffer contents match what the GBA computes. The comments name the asm labels
 * where the structure is not obvious. The engine's C half (kleod src/m4a.c: the public API,
 * SoundInit, CgbSound, TrkVolPitSet, MidiKeyToFreq, ...) is compiled as is and called from here.
 *
 * The structures are declared here rather than taken from kleod's headers (HAL files are built
 * against KallistiOS, not the game's include tree); their layout is checked against the offsets
 * the assembly uses.
 */
#include <stddef.h>

#include "hal.h"

#if HAL_AUDIO

#define ID_NUMBER 0x68736D53u

struct MP2KTrack;
struct MP2KPlayerState;

struct MixerSource {
    u8 status, type, rightVol, leftVol;
    u8 attack, decay, sustain, release;
    u8 key, envelopeVol, envelopeVolR, envelopeVolL; /* CGB: key, envelopeVol, envelopeGoal, envelopeCtr */
    u8 echoVol, echoLen, pad1, pad2;
    u8 gateTime, untransposedKey, velocity, priority;
    u8 rhythmPan, pad3, pad4, pad5;
    u32 ct;  /* samples left; CGB: pad6, sustainGoal, nrx4, pan */
    u32 fw;  /* fractional position (23 bits); CGB: panMask, cgbStatus, length, sweep */
    u32 freq;
    const void *wav;
    const s8 *current;
    struct MP2KTrack *track;
    struct MixerSource *prev, *next;
    u32 pad7, blockCount;
};

struct WaveData {
    u16 type, status;
    u32 freq, loopStart, size;
    s8 data[1];
};

struct ToneData {
    u8 type, key, length, pan_sweep;
    const void *wav; /* WaveData, or the sub-voicegroup for split/rhythm voices */
    u32 attack_decay_sustain_release;
};

struct MP2KTrack {
    u8 status, wait, patternLevel, repeatCount, gateTime, key, velocity, runningStatus;
    s8 keyShiftCalculated;
    u8 pitchCalculated;
    s8 keyShift, keyShiftPublic, tune;
    u8 pitchPublic;
    s8 bend;
    u8 bendRange, volRightCalculated, volLeftCalculated, vol, volPublic;
    s8 pan, panPublic, modCalculated;
    u8 modDepth, modType, lfoSpeed, lfoSpeedCounter, lfoDelay, lfoDelayCounter, priority, echoVolume, echoLength;
    struct MixerSource *chan;
    struct ToneData tone;
    u8 gap[10];
    u16 unk_3A;
    u32 unk_3C;
    const u8 *cmdPtr;
    const u8 *patternStack[3];
};

typedef void (*MPlayMainFunc)(struct MP2KPlayerState *);

struct MP2KPlayerState {
    const void *songHeader;
    u32 status;
    u8 trackCount, priority, cmd, checkSongPriority;
    u32 clock;
    u8 padding[8];
    u8 *memAccArea;
    u16 tempoRawBPM, tempoScale, tempoInterval, tempoCounter, fadeInterval, fadeCounter, fadeOV;
    struct MP2KTrack *tracks;
    const struct ToneData *voicegroup;
    u32 lockStatus;
    MPlayMainFunc nextPlayerFunc;
    struct MP2KPlayerState *nextPlayer;
};

typedef void (*EventFunc)(struct MP2KPlayerState *, struct MP2KTrack *);
typedef void (*NoteFunc)(u8, struct MP2KPlayerState *, struct MP2KTrack *);

struct SoundMixerState {
    u32 lockStatus;
    volatile u8 dmaCounter;
    u8 reverb, numChans, masterVol, freqOption, extensionFlags, cgbCounter15, framesPerDmaCycle;
    u8 maxScanlines, gap[3];
    s32 samplesPerFrame, sampleRate, sampleRateReciprocal;
    struct MixerSource *cgbChans;
    MPlayMainFunc MPlayMainHead;
    struct MP2KPlayerState *musicPlayerHead;
    void (*CgbSound)(void);
    void (*CgbOscOff)(u8);
    u32 (*MidiKeyToCgbFreq)(u8, u8, u8);
    void **MPlayJumpTable;
    NoteFunc plynote;
    void (*ExtVolPit)(void);
    void *reserved[4];
    struct MixerSource chans[12];
    s8 pcmBuffer[1584 * 2];
};

_Static_assert(sizeof(struct MixerSource) == 0x40, "MixerSource");
_Static_assert(offsetof(struct MixerSource, ct) == 0x18 && offsetof(struct MixerSource, wav) == 0x24, "MixerSource");
_Static_assert(offsetof(struct MixerSource, track) == 0x2C && offsetof(struct MixerSource, next) == 0x34, "MixerSource");
_Static_assert(sizeof(struct MP2KTrack) == 0x50, "MP2KTrack");
_Static_assert(offsetof(struct MP2KTrack, chan) == 0x20 && offsetof(struct MP2KTrack, tone) == 0x24, "MP2KTrack");
_Static_assert(offsetof(struct MP2KTrack, cmdPtr) == 0x40 && offsetof(struct MP2KTrack, priority) == 0x1D, "MP2KTrack");
_Static_assert(offsetof(struct MP2KPlayerState, tempoRawBPM) == 0x1C && offsetof(struct MP2KPlayerState, tracks) == 0x2C,
               "MP2KPlayerState");
_Static_assert(offsetof(struct MP2KPlayerState, lockStatus) == 0x34 && sizeof(struct MP2KPlayerState) == 0x40,
               "MP2KPlayerState");
_Static_assert(offsetof(struct SoundMixerState, samplesPerFrame) == 0x10 &&
                   offsetof(struct SoundMixerState, MPlayMainHead) == 0x20 &&
                   offsetof(struct SoundMixerState, plynote) == 0x38 &&
                   offsetof(struct SoundMixerState, chans) == 0x50 &&
                   offsetof(struct SoundMixerState, pcmBuffer) == 0x350,
               "SoundMixerState");

#define SOUND_INFO (*(struct SoundMixerState *volatile *)(GBA_IWRAM + 0x7FF0))
#define IO8(off) (*(volatile u8 *)(GBA_IO + (off)))
#define VCOUNT8 (*(volatile u8 *)(GBA_IO + 6))

/* kleod src/m4a.c and sound data */
extern const u8 gClockTable[];
extern char gMaxLines[];
void FadeOutBody(struct MP2KPlayerState *);
void TrkVolPitSet(struct MP2KPlayerState *, struct MP2KTrack *);
u32 MidiKeyToFreq(const void *wav, u8 key, u8 fineAdjust);
void ClearChain(struct MixerSource *);
void Clear64byte(void *);
void MP2K_event_fine(struct MP2KPlayerState *, struct MP2KTrack *);

/* ---- sequencer --------------------------------------------------------------------------- */

static inline u8 read_byte(struct MP2KTrack *t) { return *t->cmdPtr++; } /* sub_0804F758 */

static void clear_modM(struct MP2KTrack *t) {
    t->modCalculated = 0;
    t->lfoSpeedCounter = 0;
    t->status |= t->modType == 0 ? 0x0C : 0x03;
}

/* ChnVolSetAsm: channel volumes from the track's calculated volume and the note's velocity/pan. */
static void chn_vol_set(struct MixerSource *c, struct MP2KTrack *t) {
    s32 v = c->velocity, pan = (s8)c->rhythmPan;
    u32 r = (u32)((s32)(t->volRightCalculated * (0x80 + pan) * v) >> 14);
    u32 l = (u32)((s32)(t->volLeftCalculated * (0x7F - pan) * v) >> 14);
    c->rightVol = (u8)(r > 0xFF ? 0xFF : r);
    c->leftVol = (u8)(l > 0xFF ? 0xFF : l);
}

void MP2K_event_goto(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    const u8 *q = t->cmdPtr;
    (void)p;
    t->cmdPtr = (const u8 *)(uintptr_t)(q[0] | q[1] << 8 | q[2] << 16 | (u32)q[3] << 24);
}

void MP2K_event_patt(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    if (t->patternLevel < 3) {
        t->patternStack[t->patternLevel] = t->cmdPtr + 4;
        t->patternLevel++;
        MP2K_event_goto(p, t);
    } else {
        MP2K_event_fine(p, t);
    }
}

void MP2K_event_pend(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    if (t->patternLevel) {
        t->patternLevel--;
        t->cmdPtr = t->patternStack[t->patternLevel];
    }
}

void MP2K_event_rept(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    const u8 *q = t->cmdPtr;
    if (*q == 0) { /* repeat forever */
        t->cmdPtr = q + 1;
        MP2K_event_goto(p, t);
        return;
    }
    t->repeatCount++;
    if (t->repeatCount < read_byte(t)) {
        MP2K_event_goto(p, t);
    } else {
        t->repeatCount = 0;
        t->cmdPtr = q + 5;
    }
}

void MP2K_event_prio(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->priority = read_byte(t);
}

void MP2K_event_tempo(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    u32 bpm = (u32)read_byte(t) << 1;
    p->tempoRawBPM = (u16)bpm;
    p->tempoInterval = (u16)((bpm * p->tempoScale) >> 8);
}

void MP2K_event_keysh(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->keyShift = (s8)read_byte(t);
    t->status |= 0x0C;
}

void MP2K_event_voice(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    t->tone = p->voicegroup[read_byte(t)];
}

void MP2K_event_vol(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->vol = read_byte(t);
    t->status |= 0x03;
}

void MP2K_event_pan(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->pan = (s8)(read_byte(t) - 0x40);
    t->status |= 0x03;
}

void MP2K_event_bend(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->bend = (s8)(read_byte(t) - 0x40);
    t->status |= 0x0C;
}

void MP2K_event_bendr(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->bendRange = read_byte(t);
    t->status |= 0x0C;
}

void MP2K_event_lfodl(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->lfoDelay = read_byte(t);
}

void MP2K_event_modt(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    u8 v = read_byte(t);
    (void)p;
    if (t->modType != v) {
        t->modType = v;
        t->status |= 0x0F;
    }
}

void MP2K_event_tune(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->tune = (s8)(read_byte(t) - 0x40);
    t->status |= 0x0C;
}

/* Write a PSG register directly: <reg offset from 0x04000060> <value>. */
void MP2K_event_port(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    u8 reg = read_byte(t), val = read_byte(t);
    (void)p;
    IO8(0x60 + reg) = val;
}

void MP2K_event_lfos(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->lfoSpeed = read_byte(t);
    if (!t->lfoSpeed) clear_modM(t);
}

void MP2K_event_mod(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    (void)p;
    t->modDepth = read_byte(t);
    if (!t->modDepth) clear_modM(t);
}

/* EOT: release the playing note with this key (or the last key). */
void MP2K_event_endtie(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    struct MixerSource *c;
    u8 key;
    (void)p;
    if (*t->cmdPtr < 0x80) {
        key = *t->cmdPtr++;
        t->key = key;
    } else {
        key = t->key;
    }
    for (c = t->chan; c; c = c->next) {
        if ((c->status & 0x83) && !(c->status & 0x40) && c->untransposedKey == key) {
            c->status |= 0x40;
            return;
        }
    }
}

void TrackStop(struct MP2KPlayerState *p, struct MP2KTrack *t) {
    struct MixerSource *c;
    (void)p;
    if (!(t->status & 0x80)) return;
    for (c = t->chan; c; c = c->next) {
        if (c->status) {
            if (c->type & 7) SOUND_INFO->CgbOscOff(c->type & 7);
            c->status = 0;
        }
        c->track = NULL;
    }
    t->chan = NULL;
}

/* Note on (MP2K_event_nxx): `clk` indexes gClockTable for the gate time. */
void MP2K_event_nxx(u8 clk, struct MP2KPlayerState *p, struct MP2KTrack *t) {
    struct SoundMixerState *si = SOUND_INFO;
    const struct ToneData *tone;
    struct MixerSource *c;
    const u8 *q;
    u32 prio;
    int cgb, key, rhythm_pan = 0;

    t->gateTime = gClockTable[clk];
    q = t->cmdPtr;
    if (*q < 0x80) {
        t->key = *q++;
        if (*q < 0x80) {
            t->velocity = *q++;
            if (*q < 0x80) t->gateTime += *q++;
        }
        t->cmdPtr = q;
    }

    if (t->tone.type & 0xC0) { /* key split (0x40) or rhythm (0x80) voice */
        u8 idx = (t->tone.type & 0x40) ? ((const u8 *)(uintptr_t)t->tone.attack_decay_sustain_release)[t->key] : t->key;
        tone = (const struct ToneData *)t->tone.wav + idx;
        if (tone->type & 0xC0) return;
        if (t->tone.type & 0x80) {
            if (tone->pan_sweep & 0x80) rhythm_pan = (int)(tone->pan_sweep - 0xC0) << 1;
            key = tone->key;
        } else {
            key = t->key;
        }
    } else {
        tone = &t->tone;
        key = t->key;
    }

    prio = t->priority + p->priority;
    if (prio > 0xFF) prio = 0xFF;

    cgb = tone->type & 7;
    if (cgb) {
        if (!si->cgbChans) return;
        c = &si->cgbChans[cgb - 1];
        if ((c->status & 0xC7) && !(c->status & 0x40)) {
            if (c->priority > prio) return;
            if (c->priority == prio && (uintptr_t)c->track < (uintptr_t)t) return;
        }
    } else {
        /* Pick a DirectSound channel: a free one, else the lowest-priority releasing one, else the
         * lowest-priority playing one that does not outrank this note (ties: the later track). */
        struct MixerSource *best = NULL;
        u32 best_prio = prio;
        uintptr_t best_track = (uintptr_t)t;
        int releasing = 0, n;
        c = si->chans;
        for (n = si->numChans; n > 0; n--, c++) {
            if (!(c->status & 0xC7)) {
                best = c;
                break;
            }
            if (c->status & 0x40) {
                if (!releasing) {
                    releasing = 1;
                    best_prio = c->priority;
                    best_track = (uintptr_t)c->track;
                    best = c;
                    continue;
                }
            } else if (releasing) {
                continue;
            }
            if (c->priority < best_prio) {
                best_prio = c->priority;
                best_track = (uintptr_t)c->track;
                best = c;
            } else if (c->priority == best_prio) {
                if ((uintptr_t)c->track > best_track) {
                    best_track = (uintptr_t)c->track;
                    best = c;
                } else if ((uintptr_t)c->track == best_track) {
                    best = c;
                }
            }
        }
        if (!best) return;
        c = best;
    }

    /* Attach the channel to the track. */
    ClearChain(c);
    c->prev = NULL;
    c->next = t->chan;
    if (t->chan) t->chan->prev = c;
    t->chan = c;
    c->track = t;

    t->lfoDelayCounter = t->lfoDelay;
    if (t->lfoDelay) clear_modM(t);
    TrkVolPitSet(p, t);

    c->gateTime = t->gateTime;
    c->untransposedKey = t->key;
    c->velocity = t->velocity;
    c->priority = (u8)prio;
    c->key = (u8)key;
    c->rhythmPan = (u8)rhythm_pan;
    c->type = tone->type;
    c->wav = tone->wav;
    *(u32 *)&c->attack = tone->attack_decay_sustain_release;
    c->echoVol = t->echoVolume;
    c->echoLen = t->echoLength;
    chn_vol_set(c, t);

    key = c->key + t->keyShiftCalculated;
    if (key < 0) key = 0;
    if (cgb) {
        u8 sweep = tone->pan_sweep;
        ((u8 *)&c->fw)[2] = tone->length; /* CGB length  (+0x1E) */
        if ((sweep & 0x80) || !(sweep & 0x70)) sweep = 8;
        ((u8 *)&c->fw)[3] = sweep;        /* CGB sweep   (+0x1F) */
        c->freq = si->MidiKeyToCgbFreq((u8)cgb, (u8)key, t->pitchCalculated);
    } else {
        c->freq = MidiKeyToFreq(c->wav, (u8)key, t->pitchCalculated);
    }
    c->status = 0x80;
    t->status &= 0xF0;
}

/* One sequencer tick for every track of a player. Returns the bitmask of tracks still running. */
static u32 player_tick(struct SoundMixerState *si, struct MP2KPlayerState *p) {
    struct MP2KTrack *t = p->tracks;
    u32 bit = 1, active = 0;
    int i;
    for (i = p->trackCount; i > 0; i--, t++, bit <<= 1) {
        struct MixerSource *c;
        if (!(t->status & 0x80)) continue;
        active |= bit;

        for (c = t->chan; c; c = c->next) {
            if (c->status & 0xC7) {
                if (c->gateTime && --c->gateTime == 0) c->status |= 0x40;
            } else {
                ClearChain(c);
            }
        }

        if (t->status & 0x40) { /* track (re)start */
            Clear64byte(t);
            t->status = 0x80;
            t->bendRange = 2;
            t->volPublic = 64;
            t->lfoSpeed = 22;
            t->tone.type = 1;
        }

        while (!t->wait) {
            u8 cmd = *t->cmdPtr;
            if (cmd < 0x80) {
                cmd = t->runningStatus;
            } else {
                t->cmdPtr++;
                if (cmd >= 0xBD) t->runningStatus = cmd;
            }
            if (cmd >= 0xCF) {
                si->plynote(cmd - 0xCF, p, t);
            } else if (cmd > 0xB0) {
                p->cmd = cmd - 0xB1;
                ((EventFunc)si->MPlayJumpTable[p->cmd])(p, t);
                if (!t->status) goto next_track;
            } else {
                t->wait = gClockTable[cmd - 0x80];
            }
        }

        t->wait--;
        if (t->lfoSpeed && t->modDepth) {
            if (t->lfoDelayCounter) {
                t->lfoDelayCounter--;
            } else {
                u8 ctr = (u8)(t->lfoSpeedCounter + t->lfoSpeed);
                s32 tri, m;
                t->lfoSpeedCounter = ctr;
                tri = ((u8)(ctr - 0x40) & 0x80) ? (s8)ctr : 0x80 - ctr; /* triangle, -64..64 */
                m = (s32)(t->modDepth * tri) >> 6;
                if ((u8)(t->modCalculated ^ m)) {
                    t->modCalculated = (s8)m;
                    t->status |= t->modType == 0 ? 0x0C : 0x03;
                }
            }
        }
    next_track:;
    }
    return active;
}

void MP2KPlayerMain(struct MP2KPlayerState *p) {
    struct SoundMixerState *si;
    struct MP2KTrack *t;
    int i;

    if (p->lockStatus != ID_NUMBER) return;
    p->lockStatus++;
    if (p->nextPlayerFunc) p->nextPlayerFunc(p->nextPlayer);

    if ((s32)p->status < 0) goto done;
    si = SOUND_INFO;
    FadeOutBody(p);
    if ((s32)p->status < 0) goto done;

    p->tempoCounter += p->tempoInterval;
    while (p->tempoCounter >= 150) {
        u32 active = player_tick(si, p);
        p->clock++;
        if (!active) {
            p->status = 0x80000000u;
            goto done;
        }
        p->status = active;
        p->tempoCounter -= 150;
    }

    /* Apply volume/pitch changes to the playing channels. */
    for (i = p->trackCount, t = p->tracks; i > 0; i--, t++) {
        struct MixerSource *c;
        if (!(t->status & 0x80) || !(t->status & 0x0F)) continue;
        TrkVolPitSet(p, t);
        for (c = t->chan; c; c = c->next) {
            int cgb;
            if (!(c->status & 0xC7)) {
                ClearChain(c);
                continue;
            }
            cgb = c->type & 7;
            if (t->status & 0x03) {
                chn_vol_set(c, t);
                if (cgb) ((u8 *)&c->fw)[1] |= 1; /* cgbStatus |= volume changed (+0x1D) */
            }
            if (t->status & 0x0C) {
                int key = c->key + t->keyShiftCalculated;
                if (key < 0) key = 0;
                if (cgb) {
                    c->freq = si->MidiKeyToCgbFreq((u8)cgb, (u8)key, t->pitchCalculated);
                    ((u8 *)&c->fw)[1] |= 2; /* cgbStatus |= pitch changed */
                } else {
                    c->freq = MidiKeyToFreq(c->wav, (u8)key, t->pitchCalculated);
                }
            }
        }
        t->status &= 0xF0;
    }
done:
    p->lockStatus = ID_NUMBER;
}

/* ---- DMA ring ---------------------------------------------------------------------------- */

void m4aSoundVSync(void) {
    struct SoundMixerState *si = SOUND_INFO;
    if (si->lockStatus - ID_NUMBER > 1) return;
    if ((s32)(si->dmaCounter - 1) > 0) {
        si->dmaCounter--;
        return;
    }
    si->dmaCounter = si->framesPerDmaCycle;
    /* Restart DMA1/DMA2 at the start of pcmBuffer (hal_audio.c reads the enable bits). */
    if (IO32(0xC4) & (0x0200u << 16)) IO32(0xC4) = 0x84400004;
    if (IO32(0xD0) & (0x0200u << 16)) IO32(0xD0) = 0x84400004;
    IO16(0xC6) = 0x0400;
    IO16(0xD2) = 0x0400;
    IO16(0xC6) = 0xB600;
    IO16(0xD2) = 0xB600;
}

/* ---- DirectSound mixer ------------------------------------------------------------------- */

static inline int vcount_adj(void) {
    int v = VCOUNT8;
    return v < 160 ? v + 228 : v;
}

/* 8-bit wraparound add of (vol * sample) >> 8, as the ARM code's packed-byte arithmetic does. */
#define MIX(dst, vol, s) ((dst) = (s8)((dst) + (s8)(((vol) * (s)) >> 8)))

static void mix_channel(struct SoundMixerState *si, struct MixerSource *c, s8 *buf, int spf) {
    const struct WaveData *w = c->wav;
    s8 *right = buf, *left = buf + 1584;
    const s8 *loop_start = NULL;
    s32 loop_len = 0, ct = (s32)c->ct;
    int vr = c->envelopeVolR, vl = c->envelopeVolL, i;
    const s8 *q = c->current;

    if (c->status & 0x10) {
        loop_start = w->data + w->loopStart;
        loop_len = (s32)(w->size - w->loopStart);
    }

    if (c->type & 8) { /* fixed frequency: one source sample per output sample */
        for (i = 0; i < spf; i++) {
            int s = *q++;
            MIX(right[i], vr, s);
            MIX(left[i], vl, s);
            if (--ct == 0) {
                if (loop_len) {
                    q = loop_start;
                    ct = loop_len;
                } else {
                    c->status = 0;
                    return;
                }
            }
        }
    } else { /* linear interpolation, position step = sampleRateReciprocal * freq (23-bit fraction) */
        u32 fw = c->fw, step = (u32)si->sampleRateReciprocal * c->freq;
        s32 s0 = q[0], d = q[1] - s0;
        q++; /* q = current sample + 1 throughout, as in the asm */
        for (i = 0; i < spf; i++) {
            s32 v = s0 + ((s32)(fw * (u32)d) >> 23);
            u32 adv;
            MIX(right[i], vr, v);
            MIX(left[i], vl, v);
            fw += step;
            adv = fw >> 23;
            if (!adv) continue;
            fw &= ~0x3F800000u;
            ct -= (s32)adv;
            if (ct <= 0) {
                s32 back;
                if (!loop_len) {
                    c->status = 0;
                    return;
                }
                back = -ct;
                for (;;) {
                    ct += loop_len;
                    if (ct > 0) break;
                    back -= loop_len;
                }
                q = loop_start + back;
                s0 = *q;
            } else if (adv == 1) {
                s0 += d; /* the next sample, which q already points at */
            } else {
                q += adv - 1;
                s0 = *q;
            }
            d = q[1] - s0;
            q++;
        }
        c->fw = fw;
        q--;
    }
    c->ct = (u32)ct;
    c->current = q;
}

static void sound_main_ram(struct SoundMixerState *si, int line_limit) {
    int spf = si->samplesPerFrame, n;
    u8 dc = si->dmaCounter;
    s8 *seg = si->pcmBuffer;
    struct MixerSource *c;

    if (dc > 1) seg += (si->framesPerDmaCycle - (dc - 1)) * spf;

    if (si->reverb) {
        /* Reverb: feed back this segment and the next one (pcmBuffer start when wrapping). */
        s8 *nxt = dc == 2 ? si->pcmBuffer : seg + spf;
        int i;
        for (i = 0; i < spf; i++) {
            s32 v = seg[i + 1584] + seg[i] + nxt[i + 1584] + nxt[i];
            v = (v * si->reverb) >> 9;
            if (v & 0x80) v++;
            seg[i + 1584] = seg[i] = (s8)v;
        }
    } else {
        int i;
        for (i = 0; i < spf; i++) seg[i] = seg[i + 1584] = 0;
    }

    for (n = si->numChans, c = si->chans; n > 0; n--, c++) {
        const struct WaveData *w;
        u8 st;
        u32 env;
        if (line_limit && vcount_adj() >= line_limit) break; /* out of time for this frame */
        st = c->status;
        if (!(st & 0xC7)) continue;
        w = c->wav;
        if (st & 0x80) { /* note start */
            if (st & 0x40) {
                c->status = 0;
                continue;
            }
            st = 3;
            c->current = w->data;
            c->ct = w->size;
            c->envelopeVol = 0;
            c->fw = 0;
            if ((w->status >> 8) & 0xC0) st |= 0x10;
            c->status = st;
            env = 0;
            goto attack;
        }
        env = c->envelopeVol;
        if (st & 0x04) { /* pseudo-echo */
            if (c->echoLen-- <= 1) {
                c->status = 0;
                continue;
            }
        } else if (st & 0x40) { /* release */
            env = (env * c->release) >> 8;
            if (env <= c->echoVol) {
            echo_start:
                env = c->echoVol;
                if (!env) {
                    c->status = 0;
                    continue;
                }
                st |= 0x04;
                c->status = st;
            }
        } else if ((st & 3) == 2) { /* decay */
            env = (env * c->decay) >> 8;
            if (env <= c->sustain) {
                env = c->sustain;
                if (!env) goto echo_start;
                st--;
                c->status = st;
            }
        } else if ((st & 3) == 3) {
        attack:
            env += c->attack;
            if (env >= 0xFF) {
                env = 0xFF;
                st--;
                c->status = st;
            }
        }
        c->envelopeVol = (u8)env;
        env = ((si->masterVol + 1) * env) >> 4;
        c->envelopeVolR = (u8)((c->rightVol * env) >> 8);
        c->envelopeVolL = (u8)((c->leftVol * env) >> 8);
        mix_channel(si, c, seg, spf);
    }
}

void SoundMain(void) {
    struct SoundMixerState *si = SOUND_INFO;
    u64 t0;
    int line_limit = 0;
    if (si->lockStatus != ID_NUMBER) return;
    t0 = hal_now_us();
    si->lockStatus++;
    if (si->maxScanlines) line_limit = si->maxScanlines + vcount_adj();
    if (si->MPlayMainHead) si->MPlayMainHead(si->musicPlayerHead);
    si->CgbSound();
    sound_main_ram(si, line_limit);
    si->lockStatus = ID_NUMBER;
    hal_audio_stats.m4a_us += (u32)(hal_now_us() - t0);
}

/* The GBA copies SoundMainRAM to IWRAM and jumps there; the copy is never executed here. */
void SoundMainRAM(void) {}

#endif /* HAL_AUDIO */

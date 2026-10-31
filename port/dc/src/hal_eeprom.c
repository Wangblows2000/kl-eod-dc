/* Replacement for kleod src/eeprom.c (which bit-bangs the cartridge's serial EEPROM through
 * DMA3 and a timer interrupt). Same API, backed by a 512-byte RAM image (4 Kbit EEPROM,
 * 64 double-words). VMU persistence is a TODO; the image starts blank, as a new cartridge's
 * EEPROM does (all 0xFF).
 */
#include <string.h>

#include "hal.h"

#define EEPROM_OUT_OF_RANGE 0x80FF
#define EEPROM_VERIFY_FAIL 0x8000

typedef struct EepromConfig {
    u32 size;
    u16 nbrAddresses;
    u16 waitcnt;
    u8 addressWidth;
} EepromConfig;

static u8 eeprom[0x2000];
static u16 nbr_addresses = 64;
const EepromConfig *gEepromConfig;
static const EepromConfig cfg4k = {512, 64, 3, 6};
static const EepromConfig cfg64k = {8192, 1024, 3, 14};

u16 IdentifyEeprom(u16 sizeInKbit) {
    static int init;
    if (!init) {
        memset(eeprom, 0xFF, sizeof eeprom);
        init = 1;
    }
    gEepromConfig = sizeInKbit == 4 ? &cfg4k : &cfg64k;
    nbr_addresses = gEepromConfig->nbrAddresses;
    return sizeInKbit == 4 || sizeInKbit == 64 ? 0 : 1;
}

void EepromTimerIntr(void) {}
u16 SetEepromTimerIntr(u8 timerNo, u32 *timerPtr) {
    (void)timerNo;
    (void)timerPtr;
    return 0;
}
void StartEepromTimer(const u16 *timerConfig) { (void)timerConfig; }
void StopEepromTimer(void) {}
void Dma3Transmit(void *src, void *dest, u16 size) { memcpy(dest, src, size * 2u); }

u16 ReadEepromDword(u16 address, u16 *dest) {
    if (address >= nbr_addresses) return EEPROM_OUT_OF_RANGE;
    memcpy(dest, eeprom + address * 8u, 8);
    return 0;
}

u16 ProgramEepromDword(u16 address, u16 *src) {
    if (address >= nbr_addresses) return EEPROM_OUT_OF_RANGE;
    memcpy(eeprom + address * 8u, src, 8);
    return 0;
}

u16 VerifyEepromDword(u16 address, u16 *src) {
    if (address >= nbr_addresses) return EEPROM_OUT_OF_RANGE;
    return memcmp(eeprom + address * 8u, src, 8) ? EEPROM_VERIFY_FAIL : 0;
}

u16 ProgramEepromDwordEx(u16 address, u16 *src) {
    u16 r = ProgramEepromDword(address, src);
    return r ? r : VerifyEepromDword(address, src);
}

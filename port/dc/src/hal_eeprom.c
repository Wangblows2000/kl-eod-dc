/* Replacement for kleod src/eeprom.c (which bit-bangs the cartridge's serial EEPROM through
 * DMA3 and a timer interrupt). Same API, backed by a 512-byte RAM image (4 Kbit EEPROM,
 * 64 double-words), which hal_save.c loads from and saves to the VMU. A write that changes the
 * image bumps hal_eeprom_gen; hal_save.c picks the change up at the end of the frame.
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

/* Klonoa identifies a 4 Kbit part (main.c: IdentifyEeprom(4)); the image holds exactly that. */
static u8 eeprom[512];
static u16 nbr_addresses = 64;
const EepromConfig *gEepromConfig;
static const EepromConfig cfg4k = {512, 64, 3, 6};
volatile u32 hal_eeprom_gen;

const u8 *hal_eeprom_image(void) { return eeprom; }
u8 *hal_eeprom_image_rw(void) { return eeprom; }

u16 IdentifyEeprom(u16 sizeInKbit) {
    /* The image is set up by hal_save_init() (VMU save, or blank). Only the 4 Kbit part exists
     * here; a request for another size is refused, as the cartridge's detection would. */
    gEepromConfig = &cfg4k;
    nbr_addresses = cfg4k.nbrAddresses;
    if (sizeInKbit != 4) hal_log("eeprom: IdentifyEeprom(%u): only 4 Kbit is supported\n", sizeInKbit);
    return sizeInKbit == 4 ? 0 : 1;
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
    if (memcmp(eeprom + address * 8u, src, 8)) {
        memcpy(eeprom + address * 8u, src, 8);
        hal_eeprom_gen++;
    }
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

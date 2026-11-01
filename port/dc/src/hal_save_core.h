/* Save records and the two-file store behind VMU saves; no KallistiOS here, so the host test
 * (tools/save_test.c) can run it against a mock VMU.
 *
 * What is saved is the game's whole persistent state: the 512-byte image of the cartridge's
 * 4 Kbit EEPROM, exactly as the game's own code (kleod src/save.c) reads and writes it -- its slot
 * headers, scene and file records and their add/xor checksums stay the game's business.
 *
 * Record (little-endian), SAVE_REC_BYTES = 32 + 512:
 *    0  char  magic[8]      "KLEODSAV"
 *    8  u16   version       SAVE_VERSION (1)
 *   10  u16   header_bytes  32
 *   12  u32   data_bytes    512
 *   16  u32   sequence      +1 per save; the newer of the two files wins (wrapping compare)
 *   20  u32   data_crc      CRC-32 (IEEE) of the 512 data bytes
 *   24  u32   header_crc    CRC-32 of bytes 0..23
 *   28  u32   reserved      0
 *   32  u8    data[512]
 *
 * The store keeps two files, A and B. A save goes to the one that does not hold the newest valid
 * record, is read back and compared, and only then becomes current; until then the previous save
 * in the other file is untouched. Loading takes the valid record with the higher sequence. */
#ifndef HAL_SAVE_CORE_H
#define HAL_SAVE_CORE_H

#include <stdint.h>

#define SAVE_DATA_BYTES 512
#define SAVE_HEADER_BYTES 32
#define SAVE_REC_BYTES (SAVE_HEADER_BYTES + SAVE_DATA_BYTES)
#define SAVE_VERSION 1

/* save_rec_decode() results */
enum save_rec_result {
    REC_OK = 0,
    REC_TRUNCATED,   /* shorter than a record */
    REC_BAD_MAGIC,   /* not ours */
    REC_NEWER,       /* a later version of this port wrote it: do not read, do not overwrite */
    REC_BAD_VERSION, /* version 0 / unknown older layout */
    REC_BAD_LENGTH,  /* header or data length not what this version writes */
    REC_BAD_CRC,     /* header or data checksum wrong */
};

uint32_t save_crc32(const uint8_t *p, int n);
void save_rec_encode(uint8_t *rec, const uint8_t *data, uint32_t seq);
enum save_rec_result save_rec_decode(const uint8_t *rec, int len, uint8_t *data, uint32_t *seq);
const char *save_rec_result_name(enum save_rec_result r);

/* Does the EEPROM image hold a started game file (the game's "K_KLONOA" slot header in any of its
 * three slots)? A blank or freshly formatted image does not, and is not worth creating a file for. */
int save_image_has_game(const uint8_t *data);

/* Storage the store runs on. Return codes: */
#define SB_OK 0
#define SB_MISSING (-1)  /* read: no such file */
#define SB_IO (-2)       /* the device answered with an error */
#define SB_NO_DEVICE (-3)
#define SB_FULL (-4)     /* write: not enough free blocks */
#define SB_CORRUPT (-5)  /* read: the file's own container (VMS header CRC) is damaged */
struct save_backend {
    /* read the named file's payload into buf (capacity cap); *len = its length */
    int (*read)(void *ctx, const char *name, uint8_t *buf, int cap, int *len);
    /* replace the named file's payload */
    int (*write)(void *ctx, const char *name, const uint8_t *buf, int len);
    void *ctx;
};

#define SAVE_FILE_A "KLONOAEOD_A"
#define SAVE_FILE_B "KLONOAEOD_B"

enum save_status {
    SS_LOADED,      /* load: a save was found and loaded */
    SS_NONE,        /* load: no save files */
    SS_DAMAGED,     /* load: save files exist but none is valid; nothing loaded */
    SS_NEWER,       /* load/save: a file from a newer version exists; saving is off to protect it */
    SS_SAVED,       /* save: written and read back identical */
    SS_NO_DEVICE,
    SS_FULL,
    SS_IO,          /* save: the write failed */
    SS_VERIFY,      /* save: the write reported success but reading back differed */
};

struct save_store {
    const struct save_backend *be;
    uint32_t seq;  /* sequence of the current record */
    int current;   /* 0 = A, 1 = B, -1 = no valid record */
    int exists;    /* bit 0: A exists, bit 1: B exists (as far as known) */
    int blocked;   /* a newer version's file is on the device */
};

void save_store_init(struct save_store *s, const struct save_backend *be);
/* Load the newest valid record into data (left untouched unless SS_LOADED). */
enum save_status save_store_load(struct save_store *s, uint8_t *data);
/* Save data as the next record. */
enum save_status save_store_write(struct save_store *s, const uint8_t *data);
const char *save_status_name(enum save_status st);

#endif

/* Host test of the save record and the two-file store (src/hal_save_core.c) against a mock VMU.
 *
 *     make test-save
 *
 * The mock keeps files in memory, counts 512-byte blocks the way the VMU does (with the VMS
 * header hal_save.c adds, a save file is 3 blocks), and can be told to: have no device, fail a
 * write, write only part of a file and report success, write and report failure, flip a stored
 * byte, or refuse for lack of space. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_save_core.h"

#define MAXF 4
#define FILE_BLOCKS 3 /* VMS header 128 + icon 512 + record 544 = 1184 bytes */

struct mfile {
    char name[16];
    uint8_t data[SAVE_REC_BYTES + 64];
    int len, used;
};
struct mock {
    struct mfile f[MAXF];
    int present, free_blocks;
    int fail_write;     /* next write: report SB_IO, leave the old file deleted */
    int partial_write;  /* next write: store only half, report success */
    int fail_read;      /* reads report SB_IO */
    int writes;
} M;

static struct mfile *find(const char *name) {
    int i;
    for (i = 0; i < MAXF; i++)
        if (M.f[i].used && !strcmp(M.f[i].name, name)) return &M.f[i];
    return NULL;
}

static int m_read(void *ctx, const char *name, uint8_t *buf, int cap, int *len) {
    struct mfile *f;
    (void)ctx;
    if (!M.present) return SB_NO_DEVICE;
    if (!(f = find(name))) return SB_MISSING;
    if (M.fail_read) return SB_IO;
    if (f->len > cap) return SB_IO;
    memcpy(buf, f->data, f->len);
    *len = f->len;
    return SB_OK;
}

static int m_write(void *ctx, const char *name, const uint8_t *buf, int len) {
    struct mfile *f = find(name);
    int i, avail;
    (void)ctx;
    if (!M.present) return SB_NO_DEVICE;
    avail = M.free_blocks + (f ? FILE_BLOCKS : 0); /* overwriting frees the old file's blocks */
    if (avail < FILE_BLOCKS) return SB_FULL;
    M.writes++;
    if (f) { /* like vmufs_write with VMUFS_OVERWRITE: the old file is deleted first */
        f->used = 0;
        M.free_blocks += FILE_BLOCKS;
    }
    if (M.fail_write) {
        M.fail_write = 0;
        return SB_IO;
    }
    for (i = 0; i < MAXF && M.f[i].used; i++) {}
    if (i == MAXF) return SB_FULL;
    f = &M.f[i];
    f->used = 1;
    strcpy(f->name, name);
    f->len = M.partial_write ? len / 2 : len;
    M.partial_write = 0;
    memcpy(f->data, buf, f->len);
    M.free_blocks -= FILE_BLOCKS;
    return SB_OK;
}

static const struct save_backend BE = {m_read, m_write, NULL};
static int failures, checks;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                    \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

static void reset_vmu(int free_blocks) {
    memset(&M, 0, sizeof M);
    M.present = 1;
    M.free_blocks = free_blocks;
}

static void image(uint8_t *d, int seed) {
    int i;
    memset(d, 0xFF, SAVE_DATA_BYTES);
    memcpy(d, "K_KLONOA", 8); /* slot 0 started */
    for (i = 8; i < 0x78; i++) d[i] = (uint8_t)(seed * 31 + i);
}

static enum save_status fresh_load(uint8_t *out) {
    struct save_store s;
    save_store_init(&s, &BE);
    return save_store_load(&s, out);
}

int main(void) {
    struct save_store s;
    uint8_t a[SAVE_DATA_BYTES], b[SAVE_DATA_BYTES], out[SAVE_DATA_BYTES], rec[SAVE_REC_BYTES];
    uint32_t seq;
    int i;

    printf("record format\n");
    image(a, 1);
    save_rec_encode(rec, a, 7);
    CHECK(save_rec_decode(rec, SAVE_REC_BYTES, out, &seq) == REC_OK && seq == 7 && !memcmp(out, a, sizeof a),
          "round trip");
    CHECK(save_rec_decode(rec, SAVE_REC_BYTES - 1, out, &seq) == REC_TRUNCATED, "truncated data");
    CHECK(save_rec_decode(rec, 10, out, &seq) == REC_TRUNCATED, "truncated header");
    rec[0] ^= 1;
    CHECK(save_rec_decode(rec, SAVE_REC_BYTES, out, &seq) == REC_BAD_MAGIC, "magic");
    rec[0] ^= 1;
    rec[SAVE_HEADER_BYTES + 100] ^= 0x40;
    CHECK(save_rec_decode(rec, SAVE_REC_BYTES, out, &seq) == REC_BAD_CRC, "data byte flipped");
    rec[SAVE_HEADER_BYTES + 100] ^= 0x40;
    rec[16] ^= 1; /* sequence changed without fixing the header CRC */
    CHECK(save_rec_decode(rec, SAVE_REC_BYTES, out, &seq) == REC_BAD_CRC, "header byte flipped");
    rec[16] ^= 1;
    {
        uint8_t r2[SAVE_REC_BYTES];
        uint32_t c;
        memcpy(r2, rec, sizeof r2);
        r2[8] = 2; /* version 2, header CRC fixed up */
        c = save_crc32(r2, 24);
        r2[24] = (uint8_t)c, r2[25] = (uint8_t)(c >> 8), r2[26] = (uint8_t)(c >> 16), r2[27] = (uint8_t)(c >> 24);
        CHECK(save_rec_decode(r2, SAVE_REC_BYTES, out, &seq) == REC_NEWER, "newer version");
        r2[8] = 0;
        c = save_crc32(r2, 24);
        r2[24] = (uint8_t)c, r2[25] = (uint8_t)(c >> 8), r2[26] = (uint8_t)(c >> 16), r2[27] = (uint8_t)(c >> 24);
        CHECK(save_rec_decode(r2, SAVE_REC_BYTES, out, &seq) == REC_BAD_VERSION, "version 0");
        r2[8] = 1;
        r2[13] = 1; /* data length 0x100 = 256 */
        c = save_crc32(r2, 24);
        r2[24] = (uint8_t)c, r2[25] = (uint8_t)(c >> 8), r2[26] = (uint8_t)(c >> 16), r2[27] = (uint8_t)(c >> 24);
        CHECK(save_rec_decode(r2, SAVE_REC_BYTES, out, &seq) == REC_BAD_LENGTH, "wrong data length");
    }
    CHECK(save_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    memset(out, 0xFF, sizeof out);
    CHECK(!save_image_has_game(out), "blank EEPROM has no game");
    memset(out, 0, sizeof out);
    memcpy(out + 0x20 * 8, "K_KLONOA", 8);
    CHECK(save_image_has_game(out), "slot 3 header found");

    printf("empty VMU, then round trip across a restart\n");
    reset_vmu(200);
    memset(out, 0x5A, sizeof out);
    CHECK(fresh_load(out) == SS_NONE && out[0] == 0x5A, "empty VMU: nothing loaded, buffer untouched");
    save_store_init(&s, &BE);
    save_store_load(&s, out);
    CHECK(save_store_write(&s, a) == SS_SAVED && find(SAVE_FILE_A) && !find(SAVE_FILE_B), "first save -> A");
    CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, a, sizeof a), "reloaded after restart");

    printf("repeated saves alternate, newest wins, old copy kept\n");
    for (i = 2; i <= 9; i++) {
        image(b, i);
        CHECK(save_store_write(&s, b) == SS_SAVED, "save %d", i);
        CHECK(find(SAVE_FILE_A) && (i < 2 || find(SAVE_FILE_B)), "both files after save %d", i);
        CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, b, sizeof b), "load after save %d", i);
    }
    CHECK(s.seq == 9 && M.free_blocks == 200 - 2 * FILE_BLOCKS, "9 saves, 2 files (seq %u, free %d)",
          (unsigned)s.seq, M.free_blocks);

    printf("failed write keeps the previous save\n");
    image(a, 20);
    M.fail_write = 1;
    CHECK(save_store_write(&s, a) == SS_IO, "write error reported");
    CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, b, sizeof b), "previous save still loads");
    CHECK(save_store_write(&s, a) == SS_SAVED, "retry succeeds");
    CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, a, sizeof a), "retried save loads");

    printf("a write that silently stores half a file is caught by the read-back\n");
    image(b, 21);
    M.partial_write = 1;
    CHECK(save_store_write(&s, b) == SS_VERIFY, "verify failure reported");
    CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, a, sizeof a), "previous save still loads");
    CHECK(save_store_write(&s, b) == SS_SAVED, "retry succeeds");

    printf("damaged files\n");
    {
        struct mfile *fa = find(SAVE_FILE_A), *fb = find(SAVE_FILE_B), *newest = s.current ? fb : fa;
        CHECK(fa && fb && newest, "both files exist before the damage test");
        if (!fa || !fb || !newest) {
            printf("%d/%d checks passed\n", checks - failures, checks);
            return 1;
        }
        newest->data[SAVE_HEADER_BYTES + 3] ^= 0xFF;
        CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, a, sizeof a), "newest corrupt -> older loads");
        (newest == fa ? fb : fa)->len = 20;
        memset(out, 0x5A, sizeof out);
        CHECK(fresh_load(out) == SS_DAMAGED && out[0] == 0x5A, "both bad -> damaged, nothing loaded");
    }
    save_store_init(&s, &BE);
    CHECK(save_store_load(&s, out) == SS_DAMAGED, "damaged");
    CHECK(save_store_write(&s, a) == SS_SAVED && fresh_load(out) == SS_LOADED && !memcmp(out, a, sizeof a),
          "a new save over damaged files loads");
    M.fail_read = 1;
    CHECK(fresh_load(out) == SS_DAMAGED, "unreadable files -> damaged");
    M.fail_read = 0;
    {
        struct mfile *f = find(SAVE_FILE_A);
        memcpy(f->data, "NOTOURS!", 8);
        f = find(SAVE_FILE_B);
        if (f) f->used = 0;
        CHECK(fresh_load(out) == SS_DAMAGED, "foreign file under our name -> damaged");
    }

    printf("version mismatch\n");
    reset_vmu(200);
    save_store_init(&s, &BE);
    save_store_load(&s, out);
    save_store_write(&s, a);
    {
        struct mfile *f = find(SAVE_FILE_A);
        uint32_t c;
        f->data[8] = 2;
        c = save_crc32(f->data, 24);
        f->data[24] = (uint8_t)c, f->data[25] = (uint8_t)(c >> 8), f->data[26] = (uint8_t)(c >> 16),
        f->data[27] = (uint8_t)(c >> 24);
    }
    save_store_init(&s, &BE);
    memset(out, 0x5A, sizeof out);
    CHECK(save_store_load(&s, out) == SS_NEWER && out[0] == 0x5A, "newer version: not loaded");
    i = M.writes;
    CHECK(save_store_write(&s, b) == SS_NEWER && M.writes == i, "newer version: never overwritten");

    printf("full VMU\n");
    reset_vmu(2);
    save_store_init(&s, &BE);
    CHECK(save_store_load(&s, out) == SS_NONE, "nothing there");
    CHECK(save_store_write(&s, a) == SS_FULL && !find(SAVE_FILE_A), "2 free blocks: full, nothing written");
    reset_vmu(3);
    save_store_init(&s, &BE);
    save_store_load(&s, out);
    CHECK(save_store_write(&s, a) == SS_SAVED, "3 free blocks: first save fits");
    CHECK(save_store_write(&s, b) == SS_FULL, "second file does not fit");
    CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, a, sizeof a), "first save intact");

    printf("no VMU\n");
    reset_vmu(200);
    M.present = 0;
    save_store_init(&s, &BE);
    CHECK(save_store_load(&s, out) == SS_NO_DEVICE, "load");
    CHECK(save_store_write(&s, a) == SS_NO_DEVICE, "save");
    M.present = 1;
    CHECK(save_store_write(&s, a) == SS_SAVED, "VMU inserted later: save succeeds");

    printf("sequence wraps\n");
    reset_vmu(200);
    save_store_init(&s, &BE);
    save_store_load(&s, out);
    s.seq = 0xFFFFFFFEu;
    save_store_write(&s, a); /* A: 0xFFFFFFFF */
    save_store_write(&s, b); /* B: 0 */
    CHECK(fresh_load(out) == SS_LOADED && !memcmp(out, b, sizeof b), "0 is newer than 0xFFFFFFFF");

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

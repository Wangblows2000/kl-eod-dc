/* Save records and the two-file store; see hal_save_core.h for the format. */
#include <string.h>

#include "hal_save_core.h"

static const char MAGIC[8] = {'K', 'L', 'E', 'O', 'D', 'S', 'A', 'V'};

uint32_t save_crc32(const uint8_t *p, int n) {
    uint32_t c = 0xFFFFFFFFu;
    int i, k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

void save_rec_encode(uint8_t *rec, const uint8_t *data, uint32_t seq) {
    memcpy(rec, MAGIC, 8);
    put16(rec + 8, SAVE_VERSION);
    put16(rec + 10, SAVE_HEADER_BYTES);
    put32(rec + 12, SAVE_DATA_BYTES);
    put32(rec + 16, seq);
    put32(rec + 20, save_crc32(data, SAVE_DATA_BYTES));
    put32(rec + 24, save_crc32(rec, 24));
    put32(rec + 28, 0);
    memcpy(rec + SAVE_HEADER_BYTES, data, SAVE_DATA_BYTES);
}

enum save_rec_result save_rec_decode(const uint8_t *rec, int len, uint8_t *data, uint32_t *seq) {
    uint32_t version;
    if (len < SAVE_HEADER_BYTES) return REC_TRUNCATED;
    if (memcmp(rec, MAGIC, 8)) return REC_BAD_MAGIC;
    if (get32(rec + 24) != save_crc32(rec, 24)) return REC_BAD_CRC;
    version = get16(rec + 8);
    if (version > SAVE_VERSION) return REC_NEWER;
    if (version < 1) return REC_BAD_VERSION;
    /* version 1: the only layout so far; a later version converts older ones here */
    if (get16(rec + 10) != SAVE_HEADER_BYTES || get32(rec + 12) != SAVE_DATA_BYTES) return REC_BAD_LENGTH;
    if (len < SAVE_REC_BYTES) return REC_TRUNCATED;
    if (get32(rec + 20) != save_crc32(rec + SAVE_HEADER_BYTES, SAVE_DATA_BYTES)) return REC_BAD_CRC;
    if (data) memcpy(data, rec + SAVE_HEADER_BYTES, SAVE_DATA_BYTES);
    if (seq) *seq = get32(rec + 16);
    return REC_OK;
}

const char *save_rec_result_name(enum save_rec_result r) {
    static const char *n[] = {"ok", "truncated", "not a save record", "from a newer version", "unknown version",
                              "wrong length", "checksum mismatch"};
    return (unsigned)r < sizeof n / sizeof n[0] ? n[r] : "?";
}

int save_image_has_game(const uint8_t *data) {
    static const char tag[8] = {'K', '_', 'K', 'L', 'O', 'N', 'O', 'A'};
    int slot;
    for (slot = 0; slot < 3; slot++)
        if (!memcmp(data + slot * 0x10 * 8, tag, 8)) return 1;
    return 0;
}

static const char *file_name(int i) { return i ? SAVE_FILE_B : SAVE_FILE_A; }

void save_store_init(struct save_store *s, const struct save_backend *be) {
    memset(s, 0, sizeof *s);
    s->be = be;
    s->current = -1;
}

enum save_status save_store_load(struct save_store *s, uint8_t *data) {
    uint8_t buf[SAVE_REC_BYTES + 64], img[2][SAVE_DATA_BYTES];
    uint32_t seq[2];
    int ok[2] = {0, 0}, i, found = 0;
    s->current = -1;
    s->exists = 0;
    s->blocked = 0;
    for (i = 0; i < 2; i++) {
        int len = 0, r = s->be->read(s->be->ctx, file_name(i), buf, sizeof buf, &len);
        if (r == SB_NO_DEVICE) return SS_NO_DEVICE;
        if (r == SB_MISSING) continue;
        s->exists |= 1 << i;
        found = 1;
        if (r != SB_OK) continue; /* unreadable or damaged container: treat the file as invalid */
        switch (save_rec_decode(buf, len, img[i], &seq[i])) {
        case REC_OK:
            ok[i] = 1;
            break;
        case REC_NEWER:
            s->blocked = 1;
            break;
        default:
            break;
        }
    }
    if (s->blocked) return SS_NEWER;
    if (ok[0] && ok[1])
        i = (int32_t)(seq[1] - seq[0]) > 0;
    else if (ok[0] || ok[1])
        i = ok[1];
    else
        return found ? SS_DAMAGED : SS_NONE;
    s->current = i;
    s->seq = seq[i];
    memcpy(data, img[i], SAVE_DATA_BYTES);
    return SS_LOADED;
}

enum save_status save_store_write(struct save_store *s, const uint8_t *data) {
    uint8_t rec[SAVE_REC_BYTES], back[SAVE_REC_BYTES + 64], chk[SAVE_DATA_BYTES];
    uint32_t seq = s->seq + 1, got_seq;
    int target = s->current == 0 ? 1 : 0, len = 0, r;
    if (s->blocked) return SS_NEWER;
    save_rec_encode(rec, data, seq);
    r = s->be->write(s->be->ctx, file_name(target), rec, SAVE_REC_BYTES);
    if (r == SB_NO_DEVICE) return SS_NO_DEVICE;
    if (r == SB_FULL) return SS_FULL;
    if (r != SB_OK) {
        s->exists |= 1 << target; /* may now hold a partial file */
        return SS_IO;
    }
    s->exists |= 1 << target;
    r = s->be->read(s->be->ctx, file_name(target), back, sizeof back, &len);
    if (r == SB_NO_DEVICE) return SS_NO_DEVICE;
    if (r != SB_OK || save_rec_decode(back, len, chk, &got_seq) != REC_OK || got_seq != seq ||
        memcmp(chk, data, SAVE_DATA_BYTES))
        return SS_VERIFY;
    s->current = target;
    s->seq = seq;
    return SS_SAVED;
}

const char *save_status_name(enum save_status st) {
    static const char *n[] = {"loaded", "no save", "damaged", "newer version", "saved",
                              "no VMU", "VMU full", "write failed", "verify failed"};
    return (unsigned)st < sizeof n / sizeof n[0] ? n[st] : "?";
}

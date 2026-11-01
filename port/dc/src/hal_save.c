/* VMU saves.
 *
 * What is saved: the 512-byte image of the cartridge's 4 Kbit EEPROM (hal_eeprom.c), which is all
 * the game keeps -- three save files and the last-used file number, in the game's own layout and
 * with its own checksums (kleod src/save.c). The game writes it synchronously, a few double-words
 * at a time, at scene changes (world map, vision select, entering a vision, game over) and when
 * deleting all data; it never reads a result back beyond retrying a failed double-word.
 *
 * How: hal_save_frame(), once per frame on the game thread, notices that the image changed and
 * copies it to a staging buffer (seqlock: the game never waits). A writer thread waits until the image has been still for SAVE_SETTLE_MS -- so the scene record and the
 * file record a transition writes become one VMU write -- and saves it through the two-file store
 * of hal_save_core.c: the record goes into whichever of KLONOAEOD_A / KLONOAEOD_B does not hold the
 * newest valid save, is read back and compared, and only then counts as saved. A failure (no VMU,
 * VMU full, write or verify error) leaves the previous save intact, is shown in the letterbox bar,
 * and is retried every SAVE_RETRY_MS while there is unsaved progress. VMU I/O never runs on the
 * game thread or inside the frame loop.
 *
 * Each file is a VMS file (VMU file manager header with a one-frame icon, see vmu_pkg_build) around
 * the record: 128 + 512 + 544 = 1,184 bytes, 3 blocks; both files together use 6 of the VMU's 200.
 *
 * At boot, hal_save_init() reads every VMU, loads the newest valid save into the EEPROM image
 * before the game first reads it, and remembers that VMU for saving. A blank image (no game file
 * started) is not written to a VMU that has no save yet, so merely booting does not create files.
 */
#include <dc/maple.h>
#include <dc/minifont.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <kos/thread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal.h"
#include "hal_save_core.h"

#define SAVE_SETTLE_MS 500
#define SAVE_RETRY_MS 3000
#define SAVE_POLL_MS 100
#define MSG_SHORT_US 2500000ull
#define MSG_LONG_US 5000000ull

/* ---- EEPROM image hand-over ------------------------------------------------------------------ */

static u8 staged[SAVE_DATA_BYTES];
static volatile u32 staged_seq; /* seqlock: odd while the game thread is copying */
static volatile u32 staged_gen, saved_gen;
static volatile u64 staged_at_us;
static u32 seen_gen;
static volatile int flush_req;

/* ---- messages (writer -> renderer) and log lines (writer -> game thread) --------------------- */

#define LOGQ 8
static char logq[LOGQ][120];
static volatile u32 logq_w, logq_r;
static void qlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void qlog(const char *fmt, ...) {
    va_list ap;
    if (logq_w - logq_r >= LOGQ) return;
    va_start(ap, fmt);
    vsnprintf(logq[logq_w % LOGQ], sizeof logq[0], fmt, ap);
    va_end(ap);
    __asm__ volatile("" ::: "memory");
    logq_w++;
}

static char msg_buf[2][64];
static volatile int msg_idx = -1;
static volatile u64 msg_until;
static volatile int msg_sticky;

static void set_msg(int sticky, u64 dur_us, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_msg(int sticky, u64 dur_us, const char *fmt, ...) {
    int n = msg_idx < 0 ? 0 : !msg_idx;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_buf[n], sizeof msg_buf[n], fmt, ap);
    va_end(ap);
    msg_sticky = sticky;
    msg_until = hal_now_us() + dur_us;
    __asm__ volatile("" ::: "memory");
    msg_idx = n;
    qlog("save: status line \"%s\"\n", msg_buf[n]);
}

const char *hal_save_message(void) {
    int i = msg_idx;
    if (i < 0) return NULL;
    if (!msg_sticky && hal_now_us() > msg_until) return NULL;
    return msg_buf[i];
}

/* ---- the VMU backend for hal_save_core.c ------------------------------------------------------ */

struct vmu_ctx {
    int port, unit; /* the VMU saves go to; port < 0: none chosen */
};
static struct vmu_ctx vctx = {-1, 0};

static maple_device_t *vmu_dev(const struct vmu_ctx *c) {
    maple_device_t *d;
    if (c->port < 0) return NULL;
    d = maple_enum_dev(c->port, c->unit);
    return d && d->valid && (d->info.functions & MAPLE_FUNC_MEMCARD) ? d : NULL;
}

static u8 icon_data[512];
static vmu_pkg_t pkg_template;

static void build_icon(void) {
    u16 pix[16 * 16];
    int x, y;
    memset(pix, 0, sizeof pix);
    minifont_set_color(255, 255, 255);
    minifont_draw_str(pix, 16, "KL");
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x += 2) {
            int a = pix[(y / 2) * 16 + x / 2] ? 1 : 0, b = pix[(y / 2) * 16 + (x + 1) / 2] ? 1 : 0;
            icon_data[y * 16 + x / 2] = (u8)(a << 4 | b); /* 4bpp, left pixel in the high nibble */
        }
    strcpy(pkg_template.desc_short, "KLONOA EOD");
    strcpy(pkg_template.desc_long, "Klonoa: Empire of Dreams (DC port)");
    strcpy(pkg_template.app_id, "KLONOA_EOD_DC");
    pkg_template.icon_cnt = 1;
    pkg_template.icon_anim_speed = 0;
    pkg_template.eyecatch_type = VMUPKG_EC_NONE;
    pkg_template.icon_pal[0] = 0xF14A; /* ARGB4444: dark blue */
    pkg_template.icon_pal[1] = 0xFFFF; /* white */
    pkg_template.icon_data = icon_data;
}

static int be_read(void *ctx, const char *name, u8 *buf, int cap, int *len) {
    maple_device_t *d = vmu_dev(ctx);
    void *raw = NULL;
    int size = 0, r;
    vmu_pkg_t pkg;
    if (!d) return SB_NO_DEVICE;
    r = vmufs_read(d, name, &raw, &size);
    if (r == -2) return SB_MISSING;
    if (r < 0) return vmu_dev(ctx) ? SB_IO : SB_NO_DEVICE;
    r = vmu_pkg_parse(raw, (size_t)size, &pkg);
    if (r < 0 || pkg.data_len < 0 || pkg.data_len > cap) {
        free(raw);
        return SB_CORRUPT;
    }
    memcpy(buf, pkg.data, pkg.data_len);
    *len = pkg.data_len;
    free(raw);
    return SB_OK;
}

static int be_write(void *ctx, const char *name, const u8 *buf, int len) {
    maple_device_t *d = vmu_dev(ctx);
    vmu_pkg_t pkg = pkg_template;
    u8 *out = NULL, *padded;
    int size = 0, r, blocks;
    if (!d) return SB_NO_DEVICE;
    pkg.data_len = len;
    pkg.data = buf;
    if (vmu_pkg_build(&pkg, &out, &size) < 0) return SB_IO;
    blocks = (size + 511) / 512;
    padded = calloc(blocks, 512); /* whole blocks: vmufs_write would pad (and log about it) */
    if (!padded) {
        free(out);
        return SB_IO;
    }
    memcpy(padded, out, size);
    free(out);
    r = vmufs_write(d, name, padded, blocks * 512, VMUFS_OVERWRITE);
    free(padded);
    if (r == -7) return SB_FULL;
    if (r < 0) return vmu_dev(ctx) ? SB_IO : SB_NO_DEVICE;
    return SB_OK;
}

static const struct save_backend backend = {be_read, be_write, &vctx};
static struct save_store store;
static int store_port = -1, store_unit; /* the VMU whose A/B state `store` describes */

static const char *vmu_name(int port, int unit) {
    static char n[4];
    n[0] = (char)('A' + port);
    n[1] = (char)('0' + unit);
    n[2] = 0;
    return n;
}

/* Point the store at a VMU: the remembered one if it is still there, else the first one present.
 * On a different VMU, read its files first so the save goes to the right one of A/B. */
static int pick_vmu(void) {
    maple_device_t *d = vmu_dev(&vctx);
    if (!d) {
        d = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
        if (!d) return 0;
        vctx.port = d->port;
        vctx.unit = d->unit;
    }
    if (store_port != vctx.port || store_unit != vctx.unit) {
        u8 tmp[SAVE_DATA_BYTES];
        enum save_status st;
        save_store_init(&store, &backend);
        st = save_store_load(&store, tmp);
        if (st == SS_NO_DEVICE) return 0;
        store_port = vctx.port;
        store_unit = vctx.unit;
        qlog("save: now saving to VMU %s (%s there)\n", vmu_name(vctx.port, vctx.unit), save_status_name(st));
    }
    return 1;
}

/* ---- writer thread ----------------------------------------------------------------------------- */

static void *writer(void *arg) {
    u8 img[SAVE_DATA_BYTES];
    u64 retry_at = 0;
    u32 write_ms = 0;
    int last_fail = -1;
    (void)arg;
    for (;;) {
        u32 g, s1, s2;
        u64 now;
        enum save_status st;
        thd_sleep(SAVE_POLL_MS);
        g = staged_gen;
        if (g == saved_gen) continue;
        now = hal_now_us();
        if (!flush_req && (now - staged_at_us < SAVE_SETTLE_MS * 1000ull || now < retry_at)) continue;
        do { /* consistent copy of the staging buffer */
            s1 = staged_seq;
            memcpy(img, staged, sizeof img);
            g = staged_gen;
            s2 = staged_seq;
        } while ((s1 & 1) || s1 != s2);

        if (!store.exists && !save_image_has_game(img)) {
            saved_gen = g; /* no game started and no save file yet: nothing worth creating a file for */
            continue;
        }
        {
            u64 t0 = hal_now_us();
            st = pick_vmu() ? save_store_write(&store, img) : SS_NO_DEVICE;
            write_ms = (u32)((hal_now_us() - t0) / 1000);
        }
        if (st == SS_SAVED) {
            saved_gen = g;
            flush_req = 0;
            retry_at = 0;
            last_fail = -1;
            set_msg(0, MSG_SHORT_US, "VMU %s: SAVED", vmu_name(vctx.port, vctx.unit));
            qlog("save: VMU %s %s seq %lu written and verified in %lu ms, at frame %lu (EEPROM crc %08lx)\n",
                 vmu_name(vctx.port, vctx.unit), store.current ? SAVE_FILE_B : SAVE_FILE_A, (unsigned long)store.seq,
                 (unsigned long)write_ms, (unsigned long)hal_frame_count, (unsigned long)save_crc32(img, SAVE_DATA_BYTES));
            continue;
        }
        if ((int)st == last_fail) { /* same failure again: no new message or log line */
            retry_at = now + SAVE_RETRY_MS * 1000ull;
            if (flush_req) flush_req = 0;
            continue;
        }
        if (st == SS_NEWER) {
            saved_gen = g; /* never retried: the newer file must not be overwritten */
            set_msg(1, 0, "VMU %s: SAVE FROM A NEWER VERSION - SAVING OFF", vmu_name(vctx.port, vctx.unit));
        } else if (st == SS_NO_DEVICE) {
            set_msg(1, 0, "NO VMU - PROGRESS NOT SAVED");
        } else if (st == SS_FULL) {
            set_msg(1, 0, "VMU %s FULL (3 FREE BLOCKS NEEDED) - NOT SAVED", vmu_name(vctx.port, vctx.unit));
        } else {
            set_msg(1, 0, "VMU %s: SAVE FAILED - RETRYING", vmu_name(vctx.port, vctx.unit));
        }
        qlog("save: %s; previous save kept, retrying every %d s\n", save_status_name(st), SAVE_RETRY_MS / 1000);
        last_fail = st;
        retry_at = now + SAVE_RETRY_MS * 1000ull;
        if (flush_req) flush_req = 0; /* tried once for the flush; report it */
    }
    return NULL;
}

/* ---- game-thread side --------------------------------------------------------------------------- */

const u8 *hal_eeprom_image(void);
u8 *hal_eeprom_image_rw(void);
extern volatile u32 hal_eeprom_gen;

void hal_save_init(void) {
    u8 best[SAVE_DATA_BYTES], tmp[SAVE_DATA_BYTES];
    u32 best_seq = 0;
    int n, found = -1, bport = -1, bunit = 0;
    enum save_status first_st = SS_NO_DEVICE;
    maple_device_t *d;
    kthread_t *t;

    build_icon();
    memset(hal_eeprom_image_rw(), 0xFF, SAVE_DATA_BYTES); /* a new cartridge's EEPROM */
    for (n = 0; (d = maple_enum_type(n, MAPLE_FUNC_MEMCARD)) != NULL; n++) {
        struct save_store s;
        enum save_status st;
        vctx.port = d->port;
        vctx.unit = d->unit;
        save_store_init(&s, &backend);
        st = save_store_load(&s, tmp);
        hal_log("save: VMU %s: %s\n", vmu_name(d->port, d->unit), save_status_name(st));
        if (n == 0) first_st = st;
        if (st == SS_LOADED && (found < 0 || (int32_t)(s.seq - best_seq) > 0)) {
            found = n;
            best_seq = s.seq;
            bport = d->port;
            bunit = d->unit;
            memcpy(best, tmp, sizeof best);
            store = s;
        }
    }
    if (found >= 0) {
        vctx.port = bport;
        vctx.unit = bunit;
        store_port = bport;
        store_unit = bunit;
        memcpy(hal_eeprom_image_rw(), best, SAVE_DATA_BYTES);
        hal_log("save: loaded seq %lu from VMU %s (EEPROM crc %08lx)\n", (unsigned long)best_seq,
                vmu_name(bport, bunit), (unsigned long)save_crc32(best, SAVE_DATA_BYTES));
        set_msg(0, MSG_LONG_US, "VMU %s: SAVE LOADED", vmu_name(bport, bunit));
    } else {
        d = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
        vctx.port = d ? d->port : -1;
        vctx.unit = d ? d->unit : 0;
        if (!d) {
            hal_log("save: no VMU found; progress will not be saved until one is inserted\n");
            set_msg(0, MSG_LONG_US, "NO VMU - PROGRESS WILL NOT BE SAVED");
        } else if (first_st == SS_DAMAGED) {
            set_msg(0, MSG_LONG_US, "VMU %s: SAVE DATA DAMAGED - NOT LOADED", vmu_name(d->port, d->unit));
        } else if (first_st == SS_NEWER) {
            set_msg(1, 0, "VMU %s: SAVE FROM A NEWER VERSION - SAVING OFF", vmu_name(d->port, d->unit));
        } else {
            set_msg(0, MSG_LONG_US, "VMU %s: NO SAVE YET", vmu_name(d->port, d->unit));
        }
        hal_log("save: starting with a blank EEPROM (%s)\n", d ? save_status_name(first_st) : "no VMU");
    }
    memcpy(staged, hal_eeprom_image(), SAVE_DATA_BYTES);
    seen_gen = hal_eeprom_gen;
    /* Above the game thread (below the audio poll thread): the writer spends its time asleep or
     * blocked on the maple bus, and its bursts of CPU are short (a package and two CRCs). Below
     * the game thread it starved: each maple step waited for the scheduler's priority ageing, and
     * a save took until the game stopped to finish. */
    t = thd_create(true, writer, NULL);
    if (t) thd_set_prio(t, PRIO_DEFAULT - 1);
}

void hal_save_frame(void) {
    u32 g = hal_eeprom_gen;
    while (logq_r != logq_w) {
        hal_log("%s", logq[logq_r % LOGQ]);
        logq_r++;
    }
    if (g == seen_gen) return;
    seen_gen = g;
    staged_seq++;
    __asm__ volatile("" ::: "memory");
    memcpy(staged, hal_eeprom_image(), SAVE_DATA_BYTES);
    __asm__ volatile("" ::: "memory");
    staged_seq++;
    staged_at_us = hal_now_us();
    staged_gen++;
}

/* Test runs end with arch_exit(): give a pending save up to timeout_ms to reach the VMU. */
void hal_save_flush(u32 timeout_ms) {
    u64 end = hal_now_us() + timeout_ms * 1000ull;
    hal_save_frame();
    if (staged_gen == saved_gen) {
        hal_log("save: at exit everything saved\n");
        return;
    }
    flush_req = 1;
    while (staged_gen != saved_gen && flush_req && hal_now_us() < end) thd_sleep(20);
    hal_save_frame(); /* print what the writer logged */
    hal_log("save: at exit %s\n", staged_gen == saved_gen ? "everything saved" : "unsaved progress (see above)");
}

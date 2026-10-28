#pragma GCC optimize ("Os") /* main loop only: built for size */
/**
 * @file    mod_sd_store.c
 * @brief   Settings and calibration records in the SD card's system area.
 *
 * Layout (mod_sd_store.h): copy A at LBA 256..511, copy B at 512..767, each
 * a header sector and then one slot per record. A record is a 24-byte header
 * (magic, slot, format, sequence number, store instance, length, CRC-32 of
 * header and payload) followed by its payload, over as many sectors as it
 * needs. The newest valid copy of a slot is the record; a save goes to the
 * other copy with the next sequence number and counts once it reads back, so
 * whatever cuts a save short -- a power loss, a card pulled -- leaves the
 * previous version readable. The store instance (random, in both headers and
 * every record) keeps a record of an earlier store on the same card from
 * ever being read as this one's; the MCU's unique ID in the header keeps a
 * card moved to another radio from being used there silently.
 *
 * The card is reached only through SD_StoreDev* (mod_sdcard_service.c, or
 * the host suite's RAM disk), block by block, through one 512-byte buffer.
 */

#include "mod_sd_store.h"
#include "fw_update_format.h"
#include <stddef.h>
#include <string.h>

#define STORE_MAGIC   0x5332334DUL /* "M32S" */
#define REC_MAGIC     0x5232334DUL /* "M32R" */
#define STORE_LAYOUT  1U
#define REC_FORMAT    1U

typedef struct {
    uint32_t magic;
    uint16_t layout;
    uint16_t copy;
    uint32_t instance;
    uint32_t uid[3];
    uint32_t created_fw;
    uint32_t origin;
    uint32_t crc32;
} store_header_t;

typedef struct {
    uint32_t magic;
    uint16_t slot;
    uint16_t format;
    uint32_t seq;
    uint32_t instance;
    uint32_t length;
    uint32_t crc32;
} rec_header_t;

_Static_assert(sizeof(rec_header_t) == SD_STORE_REC_HEADER, "record header size");
_Static_assert(sizeof(store_header_t) <= 512U, "store header fits a sector");
_Static_assert(1U + SD_STORE_SETTINGS_SECTORS + (SD_STORE_SLOTS - 1U) <= SD_STORE_COPY_SECTORS,
               "slots fit a copy");
_Static_assert(2UL * SD_STORE_COPY_SECTORS == SD_SYSTEM_SECTORS, "two copies fill the area");

/* One copy of one slot, as read. */
typedef struct {
    uint8_t valid;    /* magic, slot, format, instance, length and CRC all right */
    uint8_t damaged;  /* a record header of this store was there, the record is not valid */
    uint8_t stale;    /* a record of another store instance (an earlier store) */
    uint8_t io;       /* the card did not read */
    uint32_t seq;
    uint32_t length;
    uint32_t instance;
} copy_t;

static sd_store_info_t info;
static store_header_t head;

static uint32_t copy_lba(uint8_t copy)
{
    return SD_SYSTEM_FIRST_LBA + (uint32_t)copy * SD_STORE_COPY_SECTORS;
}

static uint32_t slot_lba(uint8_t copy, uint16_t slot)
{
    return copy_lba(copy) + 1UL + ((slot == 0U) ? 0UL : (SD_STORE_SETTINGS_SECTORS + (uint32_t)slot - 1UL));
}

static uint32_t slot_cap(uint16_t slot)
{
    return (slot == 0U) ? SD_STORE_SLOT0_MAX : SD_STORE_SLOT_MAX;
}

/* The card's initialisation the store was read under: another one (the
 * driver's retry, FatFs after an error, a card put back) may be another card. */
static uint32_t open_session;

static uint8_t ready_for_io(void)
{
    return (uint8_t)(info.state == SD_STORE_READY && SD_StoreDevAvailable() && SD_StoreDevIoAllowed() &&
                     SD_StoreDevSession() == open_session);
}

/* After a read that failed: the card still there, still the initialisation
 * the store was read under. Then the failure was the card's own answer about
 * that copy (an error token, CRC errors through every retry). Otherwise the
 * transfer was cut short -- the I/O gate, a timeout, the card pulled -- and
 * says nothing about the copy, which may well be the newer one. */
static uint8_t card_unchanged(void)
{
    return (uint8_t)(SD_StoreDevAvailable() && SD_StoreDevSession() == open_session);
}

uint8_t SD_StoreNeedsReopen(void)
{
    return (uint8_t)(info.state == SD_STORE_READY && SD_StoreDevSession() != open_session);
}

static uint8_t dev_read(uint32_t lba, uint8_t *b)
{
    if (SD_StoreDevRead(lba, b)) return 1U;
    ++info.io_errors;
    return 0U;
}

static uint8_t dev_write(uint32_t lba, const uint8_t *b)
{
    if (SD_StoreDevWrite(lba, b)) return 1U;
    ++info.io_errors;
    return 0U;
}

/* Read one copy of a slot and check it; the payload goes to `out` when one
 * is given (and fits `cap`). `any` accepts a record of any store instance
 * (looking for the records of a store whose headers are gone). */
static void read_copy(uint8_t copy, uint16_t slot, uint8_t *out, uint32_t cap, uint8_t any, copy_t *c)
{
    uint8_t *b = SD_StoreDevBuffer();
    rec_header_t h;
    memset(c, 0, sizeof(*c));
    uint32_t lba = slot_lba(copy, slot);
    if (!dev_read(lba, b)) { c->io = 1U; return; }
    memcpy(&h, b, sizeof(h));
    if (h.magic != REC_MAGIC) return;
    /* A record an earlier store left (`storage new` over it) is not this
     * store's, and not damage either. */
    if (!any && h.instance != head.instance) { c->stale = 1U; return; }
    c->damaged = 1U;
    if (h.slot != slot || h.format != REC_FORMAT || h.length > slot_cap(slot) || h.length == 0U) return;
    if (out && h.length > cap) return;
    const uint32_t want = h.crc32;
    h.crc32 = 0U;
    uint32_t crc = fw_crc32_continue(0xFFFFFFFFUL, &h, sizeof(h));
    const uint8_t *p = b + sizeof(h);
    uint32_t room = 512U - (uint32_t)sizeof(h), done = 0U;
    for (;;) {
        uint32_t take = h.length - done;
        if (take > room) take = room;
        crc = fw_crc32_continue(crc, p, take);
        if (out) memcpy(out + done, p, take);
        done += take;
        if (done >= h.length) break;
        if (!dev_read(++lba, b)) { c->io = 1U; c->damaged = 0U; return; }
        p = b;
        room = 512U;
    }
    if (~crc != want) return;
    c->valid = 1U;
    c->damaged = 0U;
    c->seq = h.seq;
    c->length = h.length;
    c->instance = h.instance;
}

/* Write one copy of a slot: header and payload, zero padded. */
static uint8_t write_copy(uint8_t copy, uint16_t slot, const uint8_t *data, uint32_t len, uint32_t seq)
{
    uint8_t *b = SD_StoreDevBuffer();
    rec_header_t h = {REC_MAGIC, slot, REC_FORMAT, seq, head.instance, len, 0U};
    uint32_t crc = fw_crc32_continue(0xFFFFFFFFUL, &h, sizeof(h));
    h.crc32 = ~fw_crc32_continue(crc, data, len);
    const uint32_t sectors = ((uint32_t)sizeof(h) + len + 511U) / 512U;
    uint32_t done = 0U;
    for (uint32_t s = 0U; s < sectors; ++s) {
        memset(b, 0, 512U);
        uint32_t at = 0U;
        if (s == 0U) {
            memcpy(b, &h, sizeof(h));
            at = sizeof(h);
        }
        uint32_t take = len - done;
        if (take > 512U - at) take = 512U - at;
        memcpy(b + at, data + done, take);
        done += take;
        if (!dev_write(slot_lba(copy, slot) + s, b)) return 0U;
    }
    return 1U;
}

/* ---- Headers ------------------------------------------------------------- */

static uint32_t header_crc(const store_header_t *h)
{
    return fw_crc32(h, offsetof(store_header_t, crc32));
}

static uint8_t header_read(uint8_t copy, store_header_t *h, uint8_t *io)
{
    uint8_t *b = SD_StoreDevBuffer();
    *io = 0U;
    if (!dev_read(copy_lba(copy), b)) { *io = 1U; return 0U; }
    memcpy(h, b, sizeof(*h));
    return (uint8_t)(h->magic == STORE_MAGIC && h->layout == STORE_LAYOUT && h->copy == copy &&
                     h->crc32 == header_crc(h));
}

static uint8_t header_write(uint8_t copy)
{
    uint8_t *b = SD_StoreDevBuffer();
    store_header_t h = head;
    h.copy = copy;
    h.crc32 = header_crc(&h);
    memset(b, 0, 512U);
    memcpy(b, &h, sizeof(h));
    return dev_write(copy_lba(copy), b);
}

static uint8_t headers_write(void)
{
    SD_StoreDevHold(1U);
    const uint8_t ok = (uint8_t)(header_write(0U) && header_write(1U));
    SD_StoreDevHold(0U);
    if (ok) info.headers = 3U;
    return ok;
}

/* ---- Opening a card ---------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* LBA 0 holds a partition table and nothing in it covers the system area:
 * not a bare FAT volume ("superfloppy", its boot sector at LBA 0), not GPT. */
static uint8_t mbr_ok(const uint8_t *b)
{
    if (b[510] != 0x55U || b[511] != 0xAAU) return 0U;
    const uint16_t bps = (uint16_t)(b[11] | (b[12] << 8));
    if ((b[0] == 0xEBU || b[0] == 0xE9U) && bps == 512U && b[21] >= 0xF0U) return 0U;
    for (uint8_t i = 0U; i < 4U; ++i) {
        const uint8_t *e = b + 446U + 16U * i;
        if (e[4] == 0U) continue;
        if (e[4] == 0xEEU) return 0U;
        if (SD_SystemAreaOverlaps(le32(e + 8), le32(e + 12))) return 0U;
    }
    return 1U;
}

/* Both headers gone: does the area hold records (a store to repair) or
 * nothing (a card new to the store)? The first valid record found gives the
 * instance a repair keeps. */
static uint8_t find_records(uint32_t *instance)
{
    uint8_t seen = 0U;
    for (uint8_t copy = 0U; copy < 2U; ++copy) {
        for (uint16_t slot = 0U; slot < SD_STORE_SLOTS; ++slot) {
            copy_t c;
            read_copy(copy, slot, NULL, 0U, 1U, &c);
            if (c.valid) {
                *instance = c.instance;
                return 1U;
            }
            if (c.damaged) seen = 1U;
        }
    }
    *instance = 0U;
    return seen;
}

static sd_store_state_t set_state(sd_store_state_t s)
{
    if (s != info.state) SD_StoreDevEvent(SD_STORE_EV_STATE, (uint32_t)s, info.state);
    info.state = s;
    info.instance = (s == SD_STORE_READY || s == SD_STORE_FOREIGN) ? head.instance : 0U;
    info.origin = (s == SD_STORE_READY || s == SD_STORE_FOREIGN) ? head.origin : 0U;
    info.created_fw = (s == SD_STORE_READY || s == SD_STORE_FOREIGN) ? head.created_fw : 0U;
    return s;
}

static uint8_t uid_is_ours(const store_header_t *h)
{
    uint32_t uid[3];
    SD_StoreDevUid(uid);
    return (uint8_t)(memcmp(h->uid, uid, sizeof(uid)) == 0);
}

sd_store_state_t SD_StoreOpen(void)
{
    uint8_t *b = SD_StoreDevBuffer();
    if (!SD_StoreDevAvailable()) return set_state(SD_STORE_NO_CARD);
    open_session = SD_StoreDevSession();
    if (!dev_read(0UL, b)) return set_state(SD_STORE_IO);
    if (!mbr_ok(b)) return set_state(SD_STORE_LAYOUT);

    store_header_t ha, hb;
    uint8_t ioa = 0U, iob = 0U;
    const uint8_t va = header_read(0U, &ha, &ioa);
    const uint8_t vb = header_read(1U, &hb, &iob);
    info.headers = (uint8_t)((va ? 1U : 0U) | (vb ? 2U : 0U));
    if (!va && !vb) {
        if (ioa || iob) return set_state(SD_STORE_IO);
        uint32_t instance = 0U;
        memset(&head, 0, sizeof(head));
        return set_state(find_records(&instance) ? SD_STORE_DAMAGED : SD_STORE_BLANK);
    }
    /* Copy A is written first: when both are valid but differ, A is the
     * newer (a create or adopt cut short between the two). */
    head = va ? ha : hb;
    if (!uid_is_ours(&head)) return set_state(SD_STORE_FOREIGN);
    set_state(SD_STORE_READY);
    if (!va || !vb || memcmp(&ha.instance, &hb.instance, sizeof(ha.instance)) != 0 ||
        ha.origin != hb.origin) {
        ++info.bad_copies;
        if (SD_StoreDevSupplyOk()) {
            SD_StoreDevHold(1U);
            if (header_write(va ? 1U : 0U)) {
                ++info.repairs;
                info.headers = 3U;
            }
            SD_StoreDevHold(0U);
        }
    }
    return info.state;
}

void SD_StoreClose(void)
{
    info.headers = 0U;
    (void)set_state(SD_STORE_NO_CARD);
}

sd_store_state_t SD_StoreState(void)
{
    return info.state;
}

const char *SD_StoreStateName(sd_store_state_t state)
{
    switch (state) {
        case SD_STORE_READY:   return "READY";
        case SD_STORE_BLANK:   return "BLANK";
        case SD_STORE_FOREIGN: return "OTHER RADIO";
        case SD_STORE_DAMAGED: return "DAMAGED";
        case SD_STORE_LAYOUT:  return "CARD LAYOUT";
        case SD_STORE_IO:      return "READ ERROR";
        case SD_STORE_NO_CARD:
        default:               return "NO CARD";
    }
}

void SD_StoreGetInfo(sd_store_info_t *out)
{
    if (out) *out = info;
}

/* ---- Creating, adopting -------------------------------------------------- */

static uint8_t open_for_change(void)
{
    return (uint8_t)(SD_StoreDevAvailable() && SD_StoreDevIoAllowed() && SD_StoreDevSupplyOk() &&
                     SD_StoreDevSession() == open_session && /* the card that state was read from */
                     info.state != SD_STORE_NO_CARD && info.state != SD_STORE_LAYOUT &&
                     info.state != SD_STORE_IO);
}

uint8_t SD_StoreCreate(uint32_t origin)
{
    if (!open_for_change()) return 0U;
    const uint32_t old = head.instance;
    uint32_t instance = SD_StoreDevEntropy();
    SD_StoreDevUid(head.uid);
    instance ^= fw_crc32(head.uid, sizeof(head.uid));
    if (instance == 0U || instance == old) instance ^= 0xA5C3E1F7UL;
    head.magic = STORE_MAGIC;
    head.layout = STORE_LAYOUT;
    head.instance = instance;
    head.created_fw = SD_StoreDevFirmware();
    head.origin = origin;
    if (!headers_write()) {
        (void)SD_StoreOpen();
        return 0U;
    }
    set_state(SD_STORE_READY);
    SD_StoreDevEvent(SD_STORE_EV_CREATED, instance, origin);
    return 1U;
}

uint8_t SD_StoreAdopt(void)
{
    if (!open_for_change()) return 0U;
    if (info.state == SD_STORE_DAMAGED) {
        uint32_t instance = 0U;
        if (!find_records(&instance) || instance == 0U) return 0U;
        memset(&head, 0, sizeof(head));
        head.magic = STORE_MAGIC;
        head.layout = STORE_LAYOUT;
        head.instance = instance;
        head.created_fw = SD_StoreDevFirmware();
    } else if (info.state != SD_STORE_FOREIGN && info.state != SD_STORE_READY) {
        return 0U;
    }
    SD_StoreDevUid(head.uid);
    head.origin |= SD_STORE_ORIGIN_ADOPTED;
    if (!headers_write()) {
        (void)SD_StoreOpen();
        return 0U;
    }
    set_state(SD_STORE_READY);
    return 1U;
}

uint8_t SD_StoreSetOrigin(uint32_t set, uint32_t clear)
{
    if (!ready_for_io() || !SD_StoreDevSupplyOk()) return 0U;
    head.origin = (head.origin & ~clear) | set;
    info.origin = head.origin;
    return headers_write();
}

/* ---- Records ------------------------------------------------------------- */

sd_store_rec_t SD_StoreRead(uint16_t slot, void *data, uint32_t cap, uint32_t *len, uint32_t *seq)
{
    if (len) *len = 0U;
    if (seq) *seq = 0U;
    if (slot >= SD_STORE_SLOTS || data == NULL || !ready_for_io()) return SD_STORE_REC_ERROR;
    uint8_t *out = (uint8_t *)data;
    copy_t a, b;
    read_copy(0U, slot, out, cap, 0U, &a);
    read_copy(1U, slot, NULL, 0U, 0U, &b);
    uint8_t io = (uint8_t)(a.io | b.io);
    /* A copy whose read was cut short may be the newer one: the other is no
     * answer then (it returned the older version on the bench's gate-abort
     * stress, 2026-10-01). */
    if (io && !card_unchanged()) return SD_STORE_REC_ERROR;
    if (b.valid && b.length > cap) b.valid = 0U;
    if ((a.valid && b.damaged) || (b.valid && a.damaged)) ++info.bad_copies;
    const copy_t *pick = NULL;
    if (b.valid && (!a.valid || b.seq > a.seq)) {
        copy_t again;
        read_copy(1U, slot, out, cap, 0U, &again);
        io |= again.io;
        if (again.valid && again.seq == b.seq) pick = &b;
        else if (a.valid && card_unchanged()) {
            read_copy(0U, slot, out, cap, 0U, &again);
            io |= again.io;
            if (again.valid && again.seq == a.seq) pick = &a;
        }
    } else if (a.valid) {
        pick = &a;
    }
    if (pick) {
        ++info.reads;
        if (len) *len = pick->length;
        if (seq) *seq = pick->seq;
        return SD_STORE_REC_OK;
    }
    /* "None" only for a slot read whole and holding nothing: one that held
     * a record a moment ago, or did not read, is an error -- a caller seeds
     * an empty slot with what it has. */
    return (io || a.valid || b.valid) ? SD_STORE_REC_ERROR : SD_STORE_REC_NONE;
}

sd_store_rec_t SD_StoreReadOlder(uint16_t slot, void *data, uint32_t cap, uint32_t *len, uint32_t *seq)
{
    if (len) *len = 0U;
    if (seq) *seq = 0U;
    if (slot >= SD_STORE_SLOTS || data == NULL || !ready_for_io()) return SD_STORE_REC_ERROR;
    copy_t a, b;
    read_copy(0U, slot, NULL, 0U, 0U, &a);
    read_copy(1U, slot, NULL, 0U, 0U, &b);
    if (a.io || b.io) return SD_STORE_REC_ERROR;
    if (!a.valid || !b.valid || a.seq == b.seq) return SD_STORE_REC_NONE;
    const uint8_t older = (uint8_t)((a.seq < b.seq) ? 0U : 1U);
    const copy_t *want = older ? &b : &a;
    if (want->length > cap) return SD_STORE_REC_ERROR;
    copy_t again;
    read_copy(older, slot, (uint8_t *)data, cap, 0U, &again);
    if (again.io) return SD_STORE_REC_ERROR;
    if (!again.valid || again.seq != want->seq) return SD_STORE_REC_NONE;
    ++info.reads;
    if (len) *len = again.length;
    if (seq) *seq = again.seq;
    return SD_STORE_REC_OK;
}

uint8_t SD_StoreWrite(uint16_t slot, const void *data, uint32_t len)
{
    if (slot >= SD_STORE_SLOTS || data == NULL || len == 0U || len > slot_cap(slot)) return 0U;
    if (!ready_for_io() || !SD_StoreDevSupplyOk()) return 0U;
    SD_StoreDevHold(1U);
    copy_t a, b;
    read_copy(0U, slot, NULL, 0U, 0U, &a);
    read_copy(1U, slot, NULL, 0U, 0U, &b);
    if ((a.io || b.io) && !card_unchanged()) {
        SD_StoreDevHold(0U);
        return 0U; /* the card went away: not a write that failed */
    }
    /* The copy that does not hold the newest valid version. */
    const uint8_t target = (!a.valid) ? 0U : (!b.valid) ? 1U : (a.seq <= b.seq) ? 0U : 1U;
    uint32_t top = a.valid ? a.seq : 0U;
    if (b.valid && b.seq > top) top = b.seq;
    const uint32_t seq = top + 1U;
    uint8_t ok = 0U;
    for (uint8_t attempt = 0U; attempt < 2U && !ok; ++attempt) {
        if (write_copy(target, slot, (const uint8_t *)data, len, seq)) {
            copy_t v;
            read_copy(target, slot, NULL, 0U, 0U, &v);
            ok = (uint8_t)(v.valid && v.seq == seq && v.length == len);
        }
        if (!ok) {
            ++info.verify_failures;
            SD_StoreDevEvent(SD_STORE_EV_VERIFY, slot, slot_lba(target, slot));
        }
    }
    SD_StoreDevHold(0U);
    if (ok) {
        ++info.writes;
    } else {
        info.degraded = 1U;
        SD_StoreDevEvent(SD_STORE_EV_WRITE_FAILED, slot, slot_lba(target, slot));
    }
    return ok;
}

uint8_t SD_StoreErase(uint16_t slot)
{
    if (slot >= SD_STORE_SLOTS || !ready_for_io() || !SD_StoreDevSupplyOk()) return 0U;
    uint8_t *b = SD_StoreDevBuffer();
    SD_StoreDevHold(1U);
    memset(b, 0, 512U);
    uint8_t ok = dev_write(slot_lba(0U, slot), b);
    memset(b, 0, 512U);
    ok = (uint8_t)(dev_write(slot_lba(1U, slot), b) && ok);
    SD_StoreDevHold(0U);
    return ok;
}

uint8_t SD_StoreScrubStep(void)
{
    if (!ready_for_io()) return 0U;
    const uint16_t slot = info.scrub_slot;
    info.scrub_slot = (uint16_t)((slot + 1U) % SD_STORE_SLOTS);
    copy_t a, b;
    read_copy(0U, slot, NULL, 0U, 0U, &a);
    read_copy(1U, slot, NULL, 0U, 0U, &b);
    /* A read cut short is no verdict on that copy: copying the other over
     * it could replace the newer version with the older one. */
    if ((a.io || b.io) && !card_unchanged()) return 0U;
    if (a.valid == b.valid) return 0U; /* both good (the usual pair), or an empty slot */
    if (!SD_StoreDevSupplyOk()) return 0U;
    const uint8_t from = (uint8_t)(a.valid ? 0U : 1U), to = (uint8_t)(1U - from);
    const copy_t *good = a.valid ? &a : &b;
    /* The other copy is damaged or will not read (a repair, logged), or
     * simply has never been written since this store began -- a record
     * saved once, an import -- and gets its first copy (a mirror, counted
     * only). */
    const uint8_t damaged = (uint8_t)(a.valid ? (b.damaged | b.io) : (a.damaged | a.io));
    if (damaged) ++info.bad_copies;
    uint8_t *buf = SD_StoreDevBuffer();
    const uint32_t sectors = (SD_STORE_REC_HEADER + good->length + 511U) / 512U;
    uint8_t ok = 1U;
    SD_StoreDevHold(1U);
    for (uint32_t s = 0U; s < sectors && ok; ++s) {
        ok = (uint8_t)(dev_read(slot_lba(from, slot) + s, buf) && dev_write(slot_lba(to, slot) + s, buf));
    }
    copy_t v;
    if (ok) {
        read_copy(to, slot, NULL, 0U, 0U, &v);
        ok = (uint8_t)(v.valid && v.seq == good->seq);
    }
    SD_StoreDevHold(0U);
    if (!ok) return 0U;
    if (damaged) {
        ++info.repairs;
        SD_StoreDevEvent(SD_STORE_EV_REPAIRED, slot, to);
    } else {
        ++info.mirrors;
    }
    return 1U;
}

void SD_StoreSlotCopies(uint16_t slot, sd_store_copy_t out[2])
{
    for (uint8_t copy = 0U; copy < 2U; ++copy) {
        copy_t c;
        memset(&c, 0, sizeof(c));
        out[copy].seq = 0U;
        if (slot >= SD_STORE_SLOTS || !ready_for_io()) {
            out[copy].state = SD_STORE_COPY_IO;
            continue;
        }
        read_copy(copy, slot, NULL, 0U, 0U, &c);
        out[copy].state = c.io ? SD_STORE_COPY_IO : c.valid ? SD_STORE_COPY_OK :
                          c.damaged ? SD_STORE_COPY_BAD : c.stale ? SD_STORE_COPY_OTHER : SD_STORE_COPY_EMPTY;
        if (c.valid) out[copy].seq = c.seq;
    }
}

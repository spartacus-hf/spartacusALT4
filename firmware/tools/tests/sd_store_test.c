/* The SD system store (mod_sd_store.c) against a RAM card: every way a save
 * can be cut short leaves either the old or the new version readable, never
 * neither and never a mixture; a damaged copy is read past and repaired; a
 * card from another radio, a store without headers and a card whose layout
 * leaves no room are all recognised; nothing is ever written outside the
 * system area. */
#include "mod_sd_store.h"
#include "stubs/sd_store_disk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fill(uint8_t *p, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0U; i < len; ++i) p[i] = (uint8_t)(seed + i * 7U + (i >> 3));
}

static void fresh_card(void)
{
    disk_reset();
    disk_mbr(2048U, 960000U, 0x0CU);
    assert(SD_StoreOpen() == SD_STORE_BLANK);
    assert(SD_StoreCreate(SD_STORE_ORIGIN_NEW));
    assert(SD_StoreState() == SD_STORE_READY);
}

static void power_cycle(void)
{
    faults.powered = 1U;
    faults.cut_after_writes_set = 0U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_READY);
}

static uint32_t read_seed(uint16_t slot, uint32_t len)
{
    /* Which of the test versions (seed 1..255) the slot holds, 0 for none. */
    uint8_t got[SD_STORE_SLOT0_MAX], want[SD_STORE_SLOT0_MAX];
    uint32_t n = 0U;
    sd_store_rec_t r = SD_StoreRead(slot, got, sizeof(got), &n, NULL);
    if (r == SD_STORE_REC_NONE) return 0U;
    assert(r == SD_STORE_REC_OK);
    assert(n == len);
    for (uint32_t seed = 1U; seed < 256U; ++seed) {
        fill(want, len, (uint8_t)seed);
        if (memcmp(got, want, len) == 0) return seed;
    }
    assert(!"a record that is neither version");
    return 0U;
}

static void test_layouts(void)
{
    disk_reset();
    assert(SD_StoreOpen() == SD_STORE_LAYOUT);           /* no signature */
    disk_mbr(63U, 100000U, 0x0BU);
    assert(SD_StoreOpen() == SD_STORE_LAYOUT);           /* old DOS layout over the area */
    disk_mbr(1U, 255U, 0x0BU);
    assert(SD_StoreOpen() == SD_STORE_BLANK);            /* ends right before it */
    disk_mbr(768U, 1000U, 0x0BU);
    assert(SD_StoreOpen() == SD_STORE_BLANK);            /* starts right after it */
    disk_mbr(1U, 1000U, 0xEEU);
    assert(SD_StoreOpen() == SD_STORE_LAYOUT);           /* GPT */
    disk_mbr(2048U, 1000U, 0x0CU);
    disk[0][0] = 0xEBU; disk[0][2] = 0x90U;              /* a FAT volume at LBA 0 */
    disk[0][11] = 0x00U; disk[0][12] = 0x02U; disk[0][21] = 0xF8U;
    assert(SD_StoreOpen() == SD_STORE_LAYOUT);
    disk_mbr(8192U, 1000000U, 0x0CU);                    /* SD Formatter's 4 MiB alignment */
    assert(SD_StoreOpen() == SD_STORE_BLANK);
    faults.no_card = 1U;
    assert(SD_StoreOpen() == SD_STORE_NO_CARD);
}

static void test_round_trip(void)
{
    fresh_card();
    uint8_t rec[1020], cal[288];
    fill(rec, sizeof(rec), 1U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));
    assert(read_seed(0U, sizeof(rec)) == 1U);
    for (uint8_t v = 2U; v < 12U; ++v) {
        fill(rec, sizeof(rec), v);
        assert(SD_StoreWrite(0U, rec, sizeof(rec)));
        assert(read_seed(0U, sizeof(rec)) == v);
    }
    for (uint16_t slot = 1U; slot < SD_STORE_SLOTS; ++slot) {
        fill(cal, sizeof(cal), (uint8_t)(slot & 0xFFU) | 1U);
        assert(SD_StoreWrite(slot, cal, sizeof(cal)));
    }
    for (uint16_t slot = 1U; slot < SD_STORE_SLOTS; ++slot) {
        assert(read_seed(slot, sizeof(cal)) == ((slot & 0xFFU) | 1U));
    }
    /* Too long, empty, out of range */
    static uint8_t big[SD_STORE_SLOT0_MAX + 1U];
    assert(!SD_StoreWrite(0U, big, sizeof(big)));
    assert(!SD_StoreWrite(1U, big, SD_STORE_SLOT_MAX + 1U));
    assert(!SD_StoreWrite(1U, cal, 0U));
    assert(!SD_StoreWrite(SD_STORE_SLOTS, cal, sizeof(cal)));
    /* The version before the newest, for a newest record its reader cannot
     * use: 10 under 11, then 11 under 12; a slot written once has none. */
    uint8_t older[1020], want[1020];
    uint32_t olen = 0U, oseq = 0U, nseq = 0U;
    assert(SD_StoreReadOlder(0U, older, sizeof(older), &olen, &oseq) == SD_STORE_REC_OK && olen == sizeof(rec));
    fill(want, sizeof(want), 10U);
    assert(memcmp(older, want, sizeof(want)) == 0);
    assert(SD_StoreRead(0U, older, sizeof(older), NULL, &nseq) == SD_STORE_REC_OK && nseq == oseq + 1U);
    fill(rec, sizeof(rec), 12U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));
    assert(SD_StoreReadOlder(0U, older, sizeof(older), NULL, NULL) == SD_STORE_REC_OK);
    fill(want, sizeof(want), 11U);
    assert(memcmp(older, want, sizeof(want)) == 0);
    assert(SD_StoreReadOlder(0U, older, 10U, NULL, NULL) == SD_STORE_REC_ERROR); /* no room */
    fill(rec, sizeof(rec), 11U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));                              /* 11 newest again */
    assert(SD_StoreReadOlder(2U, older, sizeof(older), NULL, NULL) == SD_STORE_REC_NONE); /* one copy */
    /* A reopen sees the same */
    power_cycle();
    assert(read_seed(0U, sizeof(rec)) == 11U);
    assert(read_seed(5U, sizeof(cal)) == 5U);
    assert(!faults.wrote_outside);
    assert(disk_hold_depth() == 0);
}

/* A save cut at every sector, at several points inside the sector. */
static void test_power_cuts(void)
{
    static const uint32_t cuts[] = {0U, 1U, 23U, 24U, 100U, 511U};
    for (uint32_t len = 100U; len <= 1100U; len += 1000U) {
        const uint32_t sectors = (24U + len + 511U) / 512U;
        /* writes per save: the record's sectors (no verify writes) */
        for (uint32_t w = 0U; w < sectors; ++w) {
            for (unsigned c = 0U; c < sizeof(cuts) / sizeof(cuts[0]); ++c) {
                fresh_card();
                uint8_t rec[1100];
                for (uint8_t v = 1U; v <= 3U; ++v) {
                    fill(rec, len, v);
                    assert(SD_StoreWrite(0U, rec, len));
                }
                fill(rec, len, 4U);
                faults.cut_after_writes_set = 1U;
                faults.cut_after_writes = w;
                faults.cut_bytes = cuts[c];
                assert(!SD_StoreWrite(0U, rec, len));
                power_cycle();
                /* The last good version -- or the new one, when the cut came
                 * after its last byte (the rest of the sector is padding). */
                const uint32_t seed = read_seed(0U, len);
                assert(seed == 3U || seed == 4U);
                if (w + 1U < sectors) assert(seed == 3U);
                if (seed == 4U) assert(cuts[c] >= (24U + len) - 512U * w);
                /* and the store carries on */
                fill(rec, len, 5U);
                assert(SD_StoreWrite(0U, rec, len));
                assert(read_seed(0U, len) == 5U);
                power_cycle();
                assert(read_seed(0U, len) == 5U);
            }
        }
    }
    /* A cut on the very first save of a slot leaves no record, not garbage. */
    fresh_card();
    uint8_t cal[288];
    fill(cal, sizeof(cal), 9U);
    faults.cut_after_writes_set = 1U;
    faults.cut_after_writes = 0U;
    faults.cut_bytes = 200U;
    assert(!SD_StoreWrite(7U, cal, sizeof(cal)));
    power_cycle();
    assert(read_seed(7U, sizeof(cal)) == 0U);
}

static void test_damage_and_scrub(void)
{
    fresh_card();
    uint8_t rec[600];
    fill(rec, sizeof(rec), 1U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));   /* copy A */
    fill(rec, sizeof(rec), 2U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));   /* copy B */
    /* B (the newest) reads back damaged: A, one version older, is read. */
    faults.flip_lba_set = 1U;
    faults.flip_lba = 512U + 1U + 1U;               /* B, slot 0, second sector */
    faults.flip_byte = 77U;
    assert(read_seed(0U, sizeof(rec)) == 1U);
    faults.flip_lba_set = 0U;
    /* Really damaged on the card: the scrub rewrites it from the good copy. */
    disk[512U + 1U + 1U][77] ^= 0x01U;
    assert(read_seed(0U, sizeof(rec)) == 1U);
    sd_store_info_t before, after;
    SD_StoreGetInfo(&before);
    uint8_t repaired = 0U;
    for (unsigned i = 0U; i < SD_STORE_SLOTS; ++i) repaired |= SD_StoreScrubStep();
    assert(repaired);
    SD_StoreGetInfo(&after);
    assert(after.repairs == before.repairs + 1U);
    /* Both copies now hold version 1, and a further scrub finds nothing. */
    repaired = 0U;
    for (unsigned i = 0U; i < SD_STORE_SLOTS; ++i) repaired |= SD_StoreScrubStep();
    assert(!repaired);
    assert(read_seed(0U, sizeof(rec)) == 1U);
    /* The next save goes to A and keeps B as the fallback. */
    fill(rec, sizeof(rec), 3U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));
    assert(read_seed(0U, sizeof(rec)) == 3U);
    disk[256U + 1U][40] ^= 0x10U;                   /* damage A's newest */
    assert(read_seed(0U, sizeof(rec)) == 1U);

    /* A copy that will not read at all is skipped too. */
    fresh_card();
    fill(rec, sizeof(rec), 1U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));
    fill(rec, sizeof(rec), 2U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));
    faults.read_fail_lba_set = 1U;
    faults.read_fail_lba = 512U + 1U;
    assert(read_seed(0U, sizeof(rec)) == 1U);
    faults.read_fail_lba = 256U + 1U;
    assert(read_seed(0U, sizeof(rec)) == 2U);
    faults.read_fail_lba_set = 0U;

    /* A card that stops taking writes: the save fails, the old version stays. */
    faults.refuse_writes = 1U;
    fill(rec, sizeof(rec), 3U);
    assert(!SD_StoreWrite(0U, rec, sizeof(rec)));
    faults.refuse_writes = 0U;
    assert(read_seed(0U, sizeof(rec)) == 2U);
    SD_StoreGetInfo(&after);
    assert(after.degraded);

    /* Low supply, refused I/O: nothing is written. */
    const uint32_t writes = disk_writes_total;
    faults.low_supply = 1U;
    assert(!SD_StoreWrite(0U, rec, sizeof(rec)));
    faults.low_supply = 0U;
    faults.io_refused = 1U;
    assert(!SD_StoreWrite(0U, rec, sizeof(rec)));
    assert(SD_StoreRead(0U, rec, sizeof(rec), NULL, NULL) == SD_STORE_REC_ERROR);
    faults.io_refused = 0U;
    assert(disk_writes_total == writes);
}

static void test_headers(void)
{
    fresh_card();
    uint8_t rec[300];
    fill(rec, sizeof(rec), 1U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));
    fill(rec, sizeof(rec), 2U);
    assert(SD_StoreWrite(3U, rec, sizeof(rec)));

    /* One header lost: the other serves, and the open repairs it. */
    memset(disk[512], 0, 512U);
    power_cycle();
    assert(disk[512][0] == 'M');
    memset(disk[256], 0xFF, 512U);
    power_cycle();
    assert(read_seed(3U, sizeof(rec)) == 2U);

    /* Another radio: refused until adopted, records kept. */
    disk_uid[0] ^= 1U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_FOREIGN);
    assert(SD_StoreRead(3U, rec, sizeof(rec), NULL, NULL) == SD_STORE_REC_ERROR);
    assert(!SD_StoreWrite(3U, rec, sizeof(rec)));
    assert(SD_StoreAdopt());
    assert(SD_StoreState() == SD_STORE_READY);
    assert(read_seed(3U, sizeof(rec)) == 2U);
    disk_uid[0] ^= 1U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_FOREIGN);         /* now the other radio's */
    assert(SD_StoreAdopt());

    /* Both headers lost: damaged, and a repair keeps the records. */
    memset(disk[256], 0, 512U);
    memset(disk[512], 0, 512U);
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_DAMAGED);
    assert(SD_StoreAdopt());
    assert(read_seed(3U, sizeof(rec)) == 2U);
    assert(read_seed(0U, sizeof(rec)) == 1U);
    power_cycle();
    assert(read_seed(3U, sizeof(rec)) == 2U);

    /* A new store on the same card: the old records are gone for good. */
    assert(SD_StoreCreate(SD_STORE_ORIGIN_NEW));
    assert(read_seed(3U, sizeof(rec)) == 0U);
    assert(read_seed(0U, sizeof(rec)) == 0U);
    power_cycle();
    assert(read_seed(3U, sizeof(rec)) == 0U);

    /* Erase: both copies invalid. */
    fill(rec, sizeof(rec), 4U);
    assert(SD_StoreWrite(9U, rec, sizeof(rec)));
    assert(SD_StoreWrite(9U, rec, sizeof(rec)));
    assert(SD_StoreErase(9U));
    assert(read_seed(9U, sizeof(rec)) == 0U);

    /* Origin marks land in both headers. */
    assert(SD_StoreSetOrigin(SD_STORE_ORIGIN_NVM | SD_STORE_ORIGIN_IMPORTING, 0U));
    power_cycle();
    sd_store_info_t inf;
    SD_StoreGetInfo(&inf);
    assert(inf.origin & SD_STORE_ORIGIN_NVM);
    assert(inf.origin & SD_STORE_ORIGIN_NEW);
    assert(inf.origin & SD_STORE_ORIGIN_INCOMPLETE);
    assert(SD_StoreSetOrigin(0U, SD_STORE_ORIGIN_INCOMPLETE));
    power_cycle();
    SD_StoreGetInfo(&inf);
    assert((inf.origin & SD_STORE_ORIGIN_INCOMPLETE) == 0U && (inf.origin & SD_STORE_ORIGIN_NVM));
    assert(!faults.wrote_outside);
    assert(disk_hold_depth() == 0);
}

/* The scrub's two kinds of work: a record saved once gets its second copy
 * quietly (a mirror), a damaged copy is rewritten and logged (a repair);
 * records an earlier store left are neither damage nor this store's. And
 * what `storage` and `storage list` report of the copies and headers. */
static void test_mirror_repair_and_health(void)
{
    fresh_card();
    sd_store_info_t inf;
    sd_store_copy_t cp[2];
    SD_StoreGetInfo(&inf);
    assert(inf.headers == 3U);
    const uint32_t mirrors0 = inf.mirrors, repairs0 = inf.repairs, bad0 = inf.bad_copies;
    uint8_t rec[200];
    fill(rec, sizeof(rec), 1U);
    assert(SD_StoreWrite(5U, rec, sizeof(rec)));
    SD_StoreSlotCopies(5U, cp);
    assert(cp[0].state == SD_STORE_COPY_OK && cp[0].seq == 1U && cp[1].state == SD_STORE_COPY_EMPTY);
    const uint32_t events = disk_events;
    uint8_t touched = 0U;
    for (unsigned i = 0U; i < SD_STORE_SLOTS; ++i) touched |= SD_StoreScrubStep();
    assert(touched);
    SD_StoreGetInfo(&inf);
    assert(inf.mirrors == mirrors0 + 1U && inf.repairs == repairs0 && inf.bad_copies == bad0);
    assert(disk_events == events);                       /* nothing for the log */
    SD_StoreSlotCopies(5U, cp);
    assert(cp[0].state == SD_STORE_COPY_OK && cp[1].state == SD_STORE_COPY_OK && cp[1].seq == 1U);

    /* Damage the mirror: reported BAD, repaired by the scrub and logged. */
    disk[512U + 1U + 8U + 4U][30] ^= 0x04U;              /* B, slot 5 */
    SD_StoreSlotCopies(5U, cp);
    assert(cp[0].state == SD_STORE_COPY_OK && cp[1].state == SD_STORE_COPY_BAD);
    for (unsigned i = 0U; i < SD_STORE_SLOTS; ++i) (void)SD_StoreScrubStep();
    SD_StoreGetInfo(&inf);
    assert(inf.repairs == repairs0 + 1U && inf.mirrors == mirrors0 + 1U && inf.bad_copies > bad0);
    assert(disk_events == events + 1U && disk_last_event == SD_STORE_EV_REPAIRED);
    SD_StoreSlotCopies(5U, cp);
    assert(cp[1].state == SD_STORE_COPY_OK);

    /* A new store over this one: the old records are OTHER, never read,
     * never repaired; the first save beside one is mirrored, not repaired. */
    assert(SD_StoreCreate(SD_STORE_ORIGIN_NEW));
    SD_StoreSlotCopies(5U, cp);
    assert(cp[0].state == SD_STORE_COPY_OTHER && cp[1].state == SD_STORE_COPY_OTHER);
    assert(read_seed(5U, sizeof(rec)) == 0U);
    SD_StoreGetInfo(&inf);
    const uint32_t repairs = inf.repairs, mirrors = inf.mirrors;
    for (unsigned i = 0U; i < SD_STORE_SLOTS; ++i) (void)SD_StoreScrubStep();
    SD_StoreGetInfo(&inf);
    assert(inf.repairs == repairs && inf.mirrors == mirrors);
    fill(rec, sizeof(rec), 9U);
    assert(SD_StoreWrite(5U, rec, sizeof(rec)));
    SD_StoreSlotCopies(5U, cp);
    assert(cp[0].state == SD_STORE_COPY_OK && cp[0].seq == 1U && cp[1].state == SD_STORE_COPY_OTHER);
    for (unsigned i = 0U; i < SD_STORE_SLOTS; ++i) (void)SD_StoreScrubStep();
    SD_StoreGetInfo(&inf);
    assert(inf.repairs == repairs && inf.mirrors == mirrors + 1U);
    assert(read_seed(5U, sizeof(rec)) == 9U);

    /* A lost header is reported until the open can write it back. */
    memset(disk[512], 0, 512U);
    faults.low_supply = 1U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_READY);
    SD_StoreGetInfo(&inf);
    assert(inf.headers == 1U);
    faults.low_supply = 0U;
    power_cycle();
    SD_StoreGetInfo(&inf);
    assert(inf.headers == 3U);
    SD_StoreClose();
    SD_StoreGetInfo(&inf);
    assert(inf.headers == 0U);
    SD_StoreSlotCopies(5U, cp);
    assert(cp[0].state == SD_STORE_COPY_IO && cp[1].state == SD_STORE_COPY_IO);
    assert(!faults.wrote_outside);
}

/* The card initialised again since the store read it -- a driver retry,
 * FatFs after an error, a card put back, perhaps another radio's: nothing is
 * read or written, nor created or adopted, until the store has looked at it
 * again; then this radio's store is READY again, another's is FOREIGN. */
static void test_reinitialised_card(void)
{
    fresh_card();
    const uint8_t rec[16] = {1, 2, 3, 4};
    uint8_t got[16];
    uint32_t len = 0U;
    assert(SD_StoreWrite(5U, rec, sizeof(rec)));
    const uint32_t writes = disk_writes_total;
    ++disk_session;
    assert(SD_StoreNeedsReopen());
    assert(SD_StoreRead(5U, got, sizeof(got), &len, NULL) == SD_STORE_REC_ERROR);
    assert(!SD_StoreWrite(5U, rec, sizeof(rec)) && !SD_StoreErase(5U));
    assert(!SD_StoreCreate(SD_STORE_ORIGIN_NEW) && !SD_StoreAdopt());
    assert(disk_writes_total == writes);
    assert(SD_StoreOpen() == SD_STORE_READY && !SD_StoreNeedsReopen());
    assert(SD_StoreRead(5U, got, sizeof(got), &len, NULL) == SD_STORE_REC_OK && got[3] == 4U);
    /* another radio's card in its place */
    ++disk_session;
    disk_uid[0] ^= 1U;
    assert(SD_StoreOpen() == SD_STORE_FOREIGN && !SD_StoreNeedsReopen());
    disk_uid[0] ^= 1U;
}

/* A read the I/O gate (or a timeout) cuts short says nothing about that
 * copy, which may hold the newer version: the read is an error, not the
 * older copy; the scrub neither copies over it nor counts it bad; a save
 * neither writes nor logs a failure. Once the card is mounted again (a new
 * initialisation) and the store reopened, the newest version reads. */
static void cut_back(void)
{
    faults.cut_read_lba_set = 0U;
    faults.not_ready = 0U;
    ++disk_session;
    assert(SD_StoreOpen() == SD_STORE_READY);
}

static void test_reads_cut_short(void)
{
    fresh_card();
    uint8_t rec[600];
    fill(rec, sizeof(rec), 1U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));   /* A: version 1 */
    fill(rec, sizeof(rec), 2U);
    assert(SD_StoreWrite(0U, rec, sizeof(rec)));   /* B: version 2, the newest */
    sd_store_info_t before, after;
    SD_StoreGetInfo(&before);
    const uint32_t writes = disk_writes_total, events = disk_events;

    /* B cut at its first sector, then part way through the record. */
    const uint32_t cuts[2] = {512U + 1U, 512U + 1U + 1U};
    for (unsigned i = 0U; i < 2U; ++i) {
        faults.cut_read_lba_set = 1U;
        faults.cut_read_lba = cuts[i];
        uint32_t n = 0U;
        assert(SD_StoreRead(0U, rec, sizeof(rec), &n, NULL) == SD_STORE_REC_ERROR);
        cut_back();
        faults.cut_read_lba_set = 1U;
        faults.cut_read_lba = cuts[i];
        for (unsigned k = 0U; k < SD_STORE_SLOTS; ++k) {
            SD_StoreScrubStep();
            if (faults.not_ready) break;
        }
        cut_back();
        assert(read_seed(0U, sizeof(rec)) == 2U);
    }
    /* A save whose look at the copies is cut short: not written, not degraded. */
    faults.cut_read_lba_set = 1U;
    faults.cut_read_lba = 256U + 1U;
    fill(rec, sizeof(rec), 3U);
    assert(!SD_StoreWrite(0U, rec, sizeof(rec)));
    cut_back();
    SD_StoreGetInfo(&after);
    assert(disk_events == events);  /* no verify failure, no WRITE GAVE UP */
    assert(after.verify_failures == before.verify_failures);
    assert(after.bad_copies == before.bad_copies && after.repairs == before.repairs);
    assert(disk_writes_total == writes);
    assert(read_seed(0U, sizeof(rec)) == 2U);

    /* A copy the card itself will not read (it stays ready): the other wins,
     * and the scrub rewrites the unreadable one as a repair. */
    faults.read_fail_lba_set = 1U;
    faults.read_fail_lba = 256U + 1U;
    assert(read_seed(0U, sizeof(rec)) == 2U);
    uint8_t fixed = 0U;
    for (unsigned k = 0U; k < SD_STORE_SLOTS; ++k) fixed |= SD_StoreScrubStep();
    assert(!faults.read_fail_lba_set);
    SD_StoreGetInfo(&after);
    assert(fixed && after.repairs == before.repairs + 1U);
    assert(read_seed(0U, sizeof(rec)) == 2U);

    /* An empty slot read whole is "none"; one whose read is cut is not. */
    uint8_t small[16];
    assert(SD_StoreRead(9U, small, sizeof(small), NULL, NULL) == SD_STORE_REC_NONE);
    faults.cut_read_lba_set = 1U;
    faults.cut_read_lba = 512U + 1U + SD_STORE_SETTINGS_SECTORS + 8U;
    assert(SD_StoreRead(9U, small, sizeof(small), NULL, NULL) == SD_STORE_REC_ERROR);
    cut_back();
    assert(!faults.wrote_outside);
}

int main(void)
{
    test_layouts();
    test_round_trip();
    test_power_cuts();
    test_damage_and_scrub();
    test_headers();
    test_mirror_repair_and_health();
    test_reinitialised_card();
    test_reads_cut_short();
    printf("sd_store: PASS (%lu reads, %lu writes)\n",
           (unsigned long)disk_reads_total, (unsigned long)disk_writes_total);
    return 0;
}

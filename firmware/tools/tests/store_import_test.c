/* v8.08 -> v8.09: the import of v8.08's flash pages (SYSTEM/NVM808.BIN) and
 * card files into the SD store, by v8.08's own rules; the store's backup and
 * restore; the calibration store's band cache, CAL NOT LOADED hold and its
 * refusal to write over a record it could not read. Runs the production
 * mod_storage_import.c, mod_sd_store.c, mod_cal_storage.c and FatFs on a RAM
 * card (stubs/sd_fat_disk.c). */
#include "stubs/sd_fat_disk.h"
#include "radio_config.h"
#include "mod_crc32.h"
#include "fw_update_format.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NVM_LEN   0xB000U
#define PAGE      2048U

static uint8_t nvm[NVM_LEN];
static uint8_t file[64U + NVM_LEN];

static void blob_make(settings_blob_t *b, uint32_t version, float freq, uint8_t backend_sd)
{
    const uint32_t length = version == 26U ? SETTINGS_V26_LENGTH
                          : version == 27U ? SETTINGS_V27_LENGTH : (uint32_t)sizeof(*b);
    memset(b, 0, sizeof(*b));
    b->magic = SETTINGS_MAGIC;
    b->version = version;
    b->length = length;
    b->freq_mhz = freq;
    b->cw_wpm = 21.0f;
    RADIO_ConfigDefaults(&b->rev1);
    b->outphase_reserved[SETTINGS_BACKEND_IDX] = backend_sd ? SETTINGS_BACKEND_SD : 0U;
    b->crc32 = crc32_simple((const uint8_t *)b + 16U, length - 16U);
}

/* A v8.08 flash settings record at `off` of the settings pages. */
static void nvm_settings(uint32_t off, uint32_t seq, uint32_t version, float freq, uint8_t backend_sd)
{
    settings_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.rec_magic = SETTINGS_REC_MAGIC;
    rec.seq = seq;
    blob_make(&rec.blob, version, freq, backend_sd);
    memcpy(nvm + off, &rec, 8U + rec.blob.length);
}

static void cal_make(ssb_cal_record_t *r, uint16_t band, uint8_t sb, uint8_t backend, uint32_t seq, float mark)
{
    memset(r, 0, sizeof(*r));
    r->rec_magic = CAL_REC_MAGIC;
    r->seq = seq;
    r->band_idx = band;
    r->sideband = sb;
    r->analytic_backend = backend;
    r->branch_ac_offset_ticks = (int16_t)(band + 1);
    for (unsigned i = 0U; i < PHASE_CAL_BINS; ++i) r->theta_linearity_pd[i] = mark * 0.001f * (float)i;
    r->crc32 = crc32_simple((const uint8_t *)&r->seq,
                            sizeof(*r) - offsetof(ssb_cal_record_t, seq) - sizeof(r->crc32));
}

static void nvm_cal(uint32_t page, uint32_t slot, uint16_t band, uint8_t sb, uint8_t backend, uint32_t seq, float mark)
{
    ssb_cal_record_t r;
    cal_make(&r, band, sb, backend, seq, mark);
    memcpy(nvm + 0x1000U + page * PAGE + slot * 288U, &r, sizeof(r));
}

static uint32_t nvm_put(const uint32_t uid[3])
{
    const uint32_t head[16] = {0x384D564EUL, 1U, 0x08075000UL, NVM_LEN, fw_crc32(nvm, NVM_LEN),
                               uid[0], uid[1], uid[2], 808U, 0, 0, 0, 0, 0, 0, 0};
    memcpy(file, head, sizeof(head));
    memcpy(file + 64U, nvm, NVM_LEN);
    (void)fat_mkdir("SYSTEM");
    return fat_put_file("SYSTEM/NVM808.BIN", file, sizeof(file));
}

/* What the store holds for a key: 1 and the record, 0 none. */
static uint8_t store_cal(uint16_t band, uint8_t sb, uint8_t backend, ssb_cal_record_t *out)
{
    uint32_t len = 0U;
    return (uint8_t)(SD_StoreRead(cal_slot_for(band, sb, backend), out, sizeof(*out), &len, NULL) == SD_STORE_REC_OK &&
                     len == sizeof(*out));
}

static void fresh(void)
{
    fat_disk_format();
    fat_card_present = 1U;
    fat_applied_count = 0U;
    memset(&fat_applied, 0, sizeof(fat_applied));
    memset(nvm, 0xFF, sizeof(nvm));
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_BLANK);
    cal_store_forget();
}

/* A v8.08 radio on its flash store (v27 record, backend FLASH): flash
 * settings, every key from flash, the newest of several; a damaged newer
 * record and records after an erased slot do not count. */
static void test_flash_store(void)
{
    fresh();
    nvm_settings(0U, 5U, 27U, 7.074f, 0U);
    nvm_settings(1024U, 6U, 27U, 14.074f, 0U);                 /* the newest */
    nvm_settings(PAGE + 0U, 7U, 27U, 21.074f, 0U);
    nvm[PAGE + 8U + 40U] ^= 0x01U;                             /* ...but damaged */
    nvm_cal(0U, 0U, 3U, 0U, 0U, 1U, 1.0f);
    nvm_cal(0U, 1U, 3U, 0U, 0U, 9U, 2.0f);                     /* newer, same key */
    nvm_cal(0U, 2U, 3U, 1U, 4U, 2U, 3.0f);                     /* H12P */
    nvm_cal(1U, 0U, 7U, 0U, 2U, 4U, 4.0f);                     /* HILBERT_PAIR_24 -> the Hilbert key */
    /* page 2: an erased slot, then a record v8.08's scan never reached */
    nvm_cal(2U, 1U, 9U, 1U, 0U, 5U, 5.0f);
    /* a card file that must not count: the card was not v8.08's store */
    ssb_cal_record_t card;
    cal_make(&card, 3U, 0U, 0U, 50U, 9.0f);
    assert(fat_put_file("M32CAL.BIN", &card, sizeof(card)));
    assert(nvm_put(fat_uid));

    assert(MOD_StorageImportAvailable() == 2U);
    assert(MOD_StorageImport(1U));
    assert(SD_StoreState() == SD_STORE_READY);
    assert(fat_applied_count == 1U && fat_applied.freq_mhz == 14.074f);
    assert(fat_applied.outphase_reserved[SETTINGS_BACKEND_IDX] == SETTINGS_BACKEND_SD);
    ssb_cal_record_t r;
    assert(store_cal(3U, 0U, 0U, &r) && r.theta_linearity_pd[1] == 2.0f * 0.001f);
    assert(store_cal(3U, 1U, 4U, &r) && r.theta_linearity_pd[1] == 3.0f * 0.001f);
    assert(store_cal(7U, 0U, 1U, &r) && r.analytic_backend == (uint8_t)SSB_ANALYTIC_HILBERT);
    assert(!store_cal(9U, 1U, 0U, &r));
    uint8_t got_nvm = 0U, files = 0U, from = 0U;
    uint16_t cal_flash = 0U, cal_card = 0U, failed = 0U;
    MOD_StorageImportSummary(&got_nvm, &files, &from, &cal_flash, &cal_card, &failed);
    assert(got_nvm == 1U && from == 1U && cal_flash == 3U && cal_card == 0U && failed == 0U);
    /* The import log is on the card. */
    char text[200];
    uint32_t n = 0U;
    assert(fat_get_file("SYSTEM/IMPORT.LOG", text, sizeof(text) - 1U, &n) && n > 0U);
    text[n] = 0;
    assert(strstr(text, "settings=flash") && strstr(text, "cal_flash=3"));
}

/* A v8.08 radio on the card store: the card's newer settings file wins over
 * the flash record, a key's card record over its flash one; M32CFG.BIN
 * damaged, its backup M32BAK.BIN is taken. */
static void test_card_store(void)
{
    fresh();
    nvm_settings(0U, 3U, 27U, 7.0f, 1U);
    nvm_cal(0U, 0U, 1U, 0U, 0U, 1U, 1.0f);
    nvm_cal(0U, 1U, 2U, 0U, 0U, 1U, 1.5f);
    assert(nvm_put(fat_uid));
    settings_record_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rec_magic = SETTINGS_REC_MAGIC;
    cfg.seq = 40U;
    blob_make(&cfg.blob, 27U, 18.1f, 1U);
    settings_record_t bak = cfg;
    bak.seq = 39U;
    blob_make(&bak.blob, 27U, 10.1f, 1U);
    cfg.blob.cw_wpm = 30.0f;                                   /* CRC now wrong */
    assert(fat_put_file("M32CFG.BIN", &cfg, sizeof(cfg)));
    assert(fat_put_file("M32BAK.BIN", &bak, sizeof(bak)));
    ssb_cal_record_t cards[2];
    cal_make(&cards[0], 1U, 0U, 0U, 7U, 8.0f);                 /* overrides flash */
    cal_make(&cards[1], 5U, 1U, 0U, 2U, 6.0f);                 /* card only */
    assert(fat_put_file("M32CAL.BIN", cards, sizeof(cards)));

    assert(MOD_StorageImport(1U));
    assert(fat_applied.freq_mhz == 10.1f);
    ssb_cal_record_t r;
    assert(store_cal(1U, 0U, 0U, &r) && r.theta_linearity_pd[1] == 8.0f * 0.001f);
    assert(store_cal(2U, 0U, 0U, &r) && r.theta_linearity_pd[1] == 1.5f * 0.001f);
    assert(store_cal(5U, 1U, 0U, &r));
    uint16_t cal_flash = 0U, cal_card = 0U;
    uint8_t from = 0U;
    MOD_StorageImportSummary(NULL, NULL, &from, &cal_flash, &cal_card, NULL);
    assert(from == 2U && cal_flash == 1U && cal_card == 2U);
}

/* v26 in flash on the flash store (v8.07, moved to the card in RAM): the
 * flash record wins over any card file. */
static void test_v26_from_flash(void)
{
    fresh();
    nvm_settings(0U, 2U, 26U, 3.5f, 0U);
    assert(nvm_put(fat_uid));
    settings_record_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rec_magic = SETTINGS_REC_MAGIC;
    cfg.seq = 99U;
    blob_make(&cfg.blob, 27U, 28.0f, 1U);
    assert(fat_put_file("M32CFG.BIN", &cfg, sizeof(cfg)));
    assert(MOD_StorageImport(1U));
    assert(fat_applied.freq_mhz == 3.5f);
    assert(fat_applied.outphase_reserved[SETTINGS_FLAGS_IDX] == 0U);
}

/* The dump is checked: another radio's, or damaged, is refused (nothing
 * written); without it, the card files alone can still be imported. */
static void test_refusals(void)
{
    fresh();
    nvm_settings(0U, 1U, 27U, 7.0f, 0U);
    uint32_t other[3] = {fat_uid[0], fat_uid[1], fat_uid[2] ^ 1U};
    assert(nvm_put(other));
    assert(!MOD_StorageImport(1U));
    assert(SD_StoreState() == SD_STORE_BLANK);
    assert(nvm_put(fat_uid));
    file[64U + 100U] ^= 0x01U;
    assert(fat_put_file("SYSTEM/NVM808.BIN", file, sizeof(file)));
    assert(!MOD_StorageImport(1U));
    assert(SD_StoreState() == SD_STORE_BLANK);
    ssb_cal_record_t card;
    cal_make(&card, 4U, 0U, 0U, 3U, 2.0f);
    assert(fat_put_file("M32CAL.BIN", &card, sizeof(card)));
    assert(MOD_StorageImport(0U));
    ssb_cal_record_t r;
    assert(store_cal(4U, 0U, 0U, &r));
}

/* Backup, factory erase, restore: the same records; a backup of another
 * radio, or one damaged, is refused before anything is written. */
static void test_backup_restore(void)
{
    fresh();
    for (uint16_t band = 0U; band < 4U; ++band) nvm_cal(0U, band, band, 0U, 0U, 1U, (float)band + 1.0f);
    nvm_settings(PAGE, 1U, 27U, 24.9f, 0U);
    assert(nvm_put(fat_uid));
    assert(MOD_StorageImport(1U));
    assert(MOD_StorageBackup());
    static uint8_t backup[16384];
    uint32_t blen = 0U;
    assert(fat_get_file("SYSTEM/STOREBAK.BIN", backup, sizeof(backup), &blen) && blen > 0U);
    /* erase everything, as a factory reset does */
    assert(SD_StoreErase(0U));
    assert(cal_store_erase_all());
    ssb_cal_record_t r;
    assert(!store_cal(2U, 0U, 0U, &r));
    uint16_t count = 0U;
    assert(MOD_StorageRestore("SYSTEM/STOREBAK.BIN", &count));
    assert(count == 5U);
    assert(store_cal(2U, 0U, 0U, &r) && r.theta_linearity_pd[1] == 3.0f * 0.001f);
    settings_record_t s;
    uint32_t len = 0U;
    assert(SD_StoreRead(0U, &s, sizeof(s), &len, NULL) == SD_STORE_REC_OK && s.blob.freq_mhz == 24.9f);
    /* damaged entry: refused, the store unchanged */
    assert(SD_StoreErase(cal_slot_for(1U, 0U, 0U)));
    backup[blen - 10U] ^= 0x01U;
    assert(fat_put_file("SYSTEM/RESTORE.BIN", backup, blen));
    assert(!MOD_StorageRestore(NULL, &count));
    assert(!store_cal(1U, 0U, 0U, &r));
    /* another radio's backup: refused */
    backup[blen - 10U] ^= 0x01U;
    fat_uid[1] ^= 4U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_FOREIGN);
    assert(SD_StoreAdopt());
    assert(fat_put_file("SYSTEM/RESTORE.BIN", backup, blen));
    assert(!MOD_StorageRestore(NULL, &count));
    fat_uid[1] ^= 4U;
}

/* The calibration store: a band change reads both sidebands once; a band
 * whose records cannot be read locks TX (no unmute gets past it) and is
 * never written over; the lock lifts once they read. */
static void test_cal_store(void)
{
    fresh();
    assert(SD_StoreCreate(SD_STORE_ORIGIN_NEW));
    ssb_cal_record_t r;
    cal_make(&r, 6U, 1U, 0U, 0U, 4.0f);
    assert(cal_store_put(&r));
    fat_muted = 0U;
    active_band_idx = 6U;
    active_sideband_enum = SIDEBAND_LSB;
    cfg_ssb_analytic_backend = SSB_ANALYTIC_WEAVER;
    cal_store_forget();
    load_current_calibration_context();
    assert(theta_linearity_pd[1] == 4.0f * 0.001f && cfg_branch_ac_offset_ticks == 7);
    assert(!MOD_CalNotLoaded() && !RADIO_ConfigStorageTxLock());
    /* the card goes; the band changes: TX locked at once, nothing written */
    fat_card_present = 0U;
    SD_StoreClose();
    active_band_idx = 8U;
    load_current_calibration_context();
    assert(MOD_CalNotLoaded() && RADIO_ConfigStorageTxLock() && !fat_muted);
    assert(theta_linearity_pd[1] == 0.0f);
    assert(save_current_band_calibrations() == 1U);           /* says nothing to do... */
    fat_card_present = 1U;
    assert(SD_StoreOpen() == SD_STORE_READY);
    assert(!store_cal(8U, 1U, 0U, &r));                        /* ...and wrote nothing */
    /* it reads again: the lock lifts */
    load_current_calibration_context();
    assert(!MOD_CalNotLoaded() && !RADIO_ConfigStorageTxLock());
    /* a sideband change is served from the band cache, card or not */
    active_band_idx = 6U;
    load_current_calibration_context();
    fat_card_present = 0U;
    active_sideband_enum = SIDEBAND_USB;                       /* no record: zeros, not held */
    load_current_calibration_context();
    assert(theta_linearity_pd[1] == 0.0f && !MOD_CalNotLoaded() && !RADIO_ConfigStorageTxLock());
    active_sideband_enum = SIDEBAND_LSB;
    load_current_calibration_context();
    assert(theta_linearity_pd[1] == 4.0f * 0.001f && !MOD_CalNotLoaded());
    fat_card_present = 1U;
    /* a calibration changed in RAM is stored; an unchanged one is not rewritten */
    theta_linearity_pd[2] = 0.5f;
    assert(save_current_band_calibrations());
    assert(store_cal(6U, 1U, 0U, &r) && r.theta_linearity_pd[2] == 0.5f);
    sd_store_info_t before, after;
    SD_StoreGetInfo(&before);
    assert(save_current_band_calibrations());
    SD_StoreGetInfo(&after);
    assert(after.writes == before.writes);
    /* a never-calibrated band with nothing calibrated writes no record */
    active_band_idx = 11U;
    load_current_calibration_context();
    assert(save_current_band_calibrations());
    assert(!store_cal(11U, 1U, 0U, &r));
    assert(!fat_wrote_outside);
}

/* A band change reads while its own retune shuts the I/O gate, and a USB
 * audio stream shuts it for as long as it runs: the read waits, TX locked,
 * without a word, and goes past the gate once TX is locked, down and the
 * retune done. One that cannot (the radio transmitting) is said after 3 s,
 * once. A card initialised again is looked at again before it is read; RUN
 * WITHOUT CARD lifts the lock, knowingly. */
static void test_cal_deferred(void)
{
    fresh();
    assert(SD_StoreCreate(SD_STORE_ORIGIN_NEW));
    ssb_cal_record_t r;
    cal_make(&r, 4U, 1U, 0U, 0U, 3.0f);
    assert(cal_store_put(&r));
    active_sideband_enum = SIDEBAND_LSB;
    cfg_ssb_analytic_backend = SSB_ANALYTIC_WEAVER;
    fat_tick_step = 0U;
    fat_tick = 100000U;
    const uint32_t events0 = fat_events;

    /* the retune: nothing read, TX locked, nothing said */
    active_band_idx = 4U;
    cal_store_forget();
    fat_gate_closed = 1U;
    fat_retune_pending = 1U;
    const uint32_t grants0 = fat_grants;
    load_current_calibration_context();
    assert(RADIO_ConfigStorageTxLock() && !MOD_CalNotLoaded() && theta_linearity_pd[1] == 0.0f);
    MOD_CalReloadService(fat_tick);
    assert(RADIO_ConfigStorageTxLock() && fat_grants == grants0 && fat_events == events0);
    /* retune done, the stream still on: read past the gate, the lock lifts */
    fat_retune_pending = 0U;
    fat_tick += 10U;
    MOD_CalReloadService(fat_tick);
    assert(fat_grants == grants0 + 1U && !RADIO_ConfigStorageTxLock() && !MOD_CalNotLoaded());
    assert(theta_linearity_pd[1] == 3.0f * 0.001f && fat_events == events0);

    /* transmitting: no grant; after 3 s it says CAL NOT LOADED, once */
    active_band_idx = 9U;
    fat_transmitting = 1U;
    load_current_calibration_context();
    assert(RADIO_ConfigStorageTxLock() && !MOD_CalNotLoaded());
    fat_tick += 2000U;
    MOD_CalReloadService(fat_tick);
    assert(!MOD_CalNotLoaded() && fat_events == events0);
    fat_tick += 1500U;
    MOD_CalReloadService(fat_tick);
    assert(MOD_CalNotLoaded() && fat_events == events0 + 1U && fat_last_event_code == 12U);
    MOD_CalReloadService(fat_tick);
    assert(fat_events == events0 + 1U);
    fat_transmitting = 0U;
    MOD_CalReloadService(fat_tick);
    assert(!RADIO_ConfigStorageTxLock() && !MOD_CalNotLoaded());

    /* the card initialised again (a driver retry, FatFs after an error): the
     * store reads nothing until it has looked at the card again */
    fat_gate_closed = 0U;
    ++fat_session;
    assert(SD_StoreNeedsReopen());
    uint32_t len = 0U;
    assert(SD_StoreRead(cal_slot_for(4U, 1U, 0U), &r, sizeof(r), &len, NULL) == SD_STORE_REC_ERROR);
    active_band_idx = 4U;
    cal_store_forget();
    load_current_calibration_context();
    assert(!SD_StoreNeedsReopen() && !RADIO_ConfigStorageTxLock() && theta_linearity_pd[1] == 3.0f * 0.001f);

    /* lost with the radio running: TX locked; RUN WITHOUT CARD lifts it */
    fat_card_present = 0U;
    SD_StoreClose();
    active_band_idx = 6U;
    load_current_calibration_context();
    assert(MOD_CalNotLoaded() && RADIO_ConfigStorageTxLock());
    fat_nocard = 1U;
    load_current_calibration_context();
    assert(!MOD_CalNotLoaded() && !RADIO_ConfigStorageTxLock() && theta_linearity_pd[1] == 0.0f);
    fat_nocard = 0U;
    fat_card_present = 1U;
    fat_tick_step = 1000U;
}

static uint8_t backup_running(uint16_t *slot)
{
    uint8_t running = 0U;
    MOD_StorageBackupStatus(NULL, &running, slot, NULL, NULL);
    return running;
}

static uint8_t backup_pending(void)
{
    uint8_t pending = 0U;
    MOD_StorageBackupStatus(&pending, NULL, NULL, NULL, NULL);
    return pending;
}

/* The background backup: due a quiet minute after a calibration change or
 * an import, written a few slots a step, at most every ten minutes; built
 * beside the last backup and renamed over it only once whole, so a job cut
 * short (the card pulled) leaves the previous backup as it was. */
static void test_background_backup(void)
{
    static uint8_t first[16384], now_file[16384];
    uint32_t first_len = 0U, len = 0U, ok0 = 0U, ok = 0U, failed0 = 0U, failed = 0U;
    uint16_t slot = 0U, count = 0U;
    fresh();
    for (uint16_t band = 0U; band < 6U; ++band) nvm_cal(0U, band, band, 0U, 0U, 1U, (float)band + 1.0f);
    nvm_settings(PAGE, 1U, 27U, 24.9f, 0U);
    assert(nvm_put(fat_uid));
    assert(MOD_StorageImport(1U));                         /* asks for a backup */
    MOD_StorageBackupStatus(NULL, NULL, NULL, &ok0, &failed0);
    uint32_t t = 1000000U;
    MOD_StorageBackupService(t, 1U);
    assert(backup_pending() && !backup_running(NULL));
    MOD_StorageBackupService(t + 59000U, 1U);              /* not before a quiet minute */
    assert(backup_pending() && !backup_running(NULL));
    MOD_StorageBackupService(t + 61000U, 0U);              /* nor while card I/O may not block */
    assert(backup_pending() && !backup_running(NULL));
    t += 61000U;
    MOD_StorageBackupService(t, 1U);
    assert(!backup_pending() && backup_running(&slot) && slot == 0U);
    assert(!fat_get_file("SYSTEM/STOREBAK.BIN", first, sizeof(first), &first_len));
    unsigned steps = 0U;
    MOD_StorageBackupService(t + 100U, 1U);                /* one step per 500 ms */
    assert(backup_running(&slot) && slot == 0U);
    do {
        t += 500U;
        MOD_StorageBackupService(t, 1U);
        ++steps;
        if (backup_running(&slot)) assert(slot == 4U * steps);
    } while (backup_running(NULL) && steps < 100U);
    assert(steps == (SD_STORE_SLOTS + 3U) / 4U);
    MOD_StorageBackupStatus(NULL, NULL, NULL, &ok, &failed);
    assert(ok == ok0 + 1U && failed == failed0);
    assert(fat_get_file("SYSTEM/STOREBAK.BIN", first, sizeof(first), &first_len) && first_len > 0U);
    assert(!fat_get_file("SYSTEM/STOREBAK.TMP", now_file, sizeof(now_file), &len));
    assert(MOD_StorageRestore("SYSTEM/STOREBAK.BIN", &count) && count == 7U);

    /* A calibration change: due again, but not within ten minutes of the last. */
    ssb_cal_record_t r;
    active_band_idx = 2U;
    active_sideband_enum = SIDEBAND_USB;
    cfg_ssb_analytic_backend = SSB_ANALYTIC_WEAVER;
    cal_store_forget();
    load_current_calibration_context();
    theta_linearity_pd[3] = 0.25f;
    assert(save_current_band_calibrations());              /* asks for a backup */
    MOD_StorageBackupService(t, 1U);
    MOD_StorageBackupService(t + 120000U, 1U);
    assert(backup_pending() && !backup_running(NULL));
    t += 600000U;
    MOD_StorageBackupService(t, 1U);
    assert(backup_running(NULL));

    /* The card goes part way: the job fails, the last backup is untouched. */
    for (unsigned k = 0U; k < 10U; ++k) {
        t += 500U;
        MOD_StorageBackupService(t, 1U);
    }
    assert(backup_running(NULL));
    fat_card_present = 0U;
    SD_StoreClose();
    t += 500U;
    MOD_StorageBackupService(t, 1U);
    assert(!backup_running(NULL) && backup_pending());
    MOD_StorageBackupStatus(NULL, NULL, NULL, NULL, &failed);
    assert(failed == failed0 + 1U);
    fat_card_present = 1U;
    assert(SD_StoreOpen() == SD_STORE_READY);
    assert(fat_get_file("SYSTEM/STOREBAK.BIN", now_file, sizeof(now_file), &len));
    assert(len == first_len && memcmp(now_file, first, len) == 0);

    /* ...and it is made again a quiet minute later, the change in it. */
    t += 61000U;
    MOD_StorageBackupService(t, 1U);
    steps = 0U;
    while (backup_running(NULL) && steps++ < 100U) {
        t += 500U;
        MOD_StorageBackupService(t, 1U);
    }
    assert(!backup_running(NULL) && !backup_pending());
    MOD_StorageBackupStatus(NULL, NULL, NULL, &ok, NULL);
    assert(ok == ok0 + 2U);
    assert(SD_StoreErase(cal_slot_for(2U, 0U, 0U)));
    assert(MOD_StorageRestore("SYSTEM/STOREBAK.BIN", &count) && count == 7U);
    assert(store_cal(2U, 0U, 0U, &r) && r.theta_linearity_pd[3] == 0.25f);
    assert(!fat_wrote_outside);
}

/* An import cut short (the card gone part way): the store is left marked
 * IMPORTING, the boot holds on it, and resuming imports again and clears the
 * mark. A restore cut short is marked RESTORING and resumed from
 * RESTORE.BIN; without its source, resuming keeps the store as it is. */
static void test_cut_short(void)
{
    static uint8_t backup[16384];
    uint32_t blen = 0U;
    uint16_t count = 0U;
    ssb_cal_record_t r;
    fresh();
    for (uint16_t band = 0U; band < 10U; ++band) nvm_cal(band / 7U, band % 7U, band, 1U, 0U, 1U, (float)band + 1.0f);
    nvm_settings(PAGE, 1U, 27U, 14.2f, 0U);
    assert(nvm_put(fat_uid));
    fat_cut_writes_set = 1U;
    fat_cut_writes = 2U + 3U + 4U;                      /* headers, settings, four records */
    assert(!MOD_StorageImport(1U));
    fat_cut_writes_set = 0U;
    fat_card_present = 1U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_READY);
    assert(MOD_StorageIncomplete() == 1U);
    assert(store_cal(3U, 1U, 0U, &r) && !store_cal(4U, 1U, 0U, &r));
    assert(strcmp(MOD_StorageResumeAction(), "IMPORT V8.08 AGAIN") == 0);
    assert(MOD_StorageResume());
    assert(MOD_StorageIncomplete() == 0U);
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_READY && MOD_StorageIncomplete() == 0U);
    for (uint16_t band = 0U; band < 10U; ++band) {
        assert(store_cal(band, 1U, 0U, &r) && r.theta_linearity_pd[1] == ((float)band + 1.0f) * 0.001f);
    }

    /* A restore cut short, resumed from RESTORE.BIN. */
    assert(MOD_StorageBackup());
    assert(fat_get_file("SYSTEM/STOREBAK.BIN", backup, sizeof(backup), &blen) && blen > 0U);
    assert(fat_put_file("SYSTEM/RESTORE.BIN", backup, blen));
    assert(cal_store_erase_all());
    fat_cut_writes_set = 1U;
    fat_cut_writes = 2U + 3U + 2U;
    assert(!MOD_StorageRestore(NULL, &count));
    fat_cut_writes_set = 0U;
    fat_card_present = 1U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_READY && MOD_StorageIncomplete() == 2U);
    assert(strcmp(MOD_StorageResumeAction(), "RESTORE AGAIN") == 0);
    assert(MOD_StorageResume() && MOD_StorageIncomplete() == 0U);
    assert(store_cal(9U, 1U, 0U, &r));

    /* Cut short again, its source gone: resuming keeps the store. */
    assert(cal_store_erase_all());
    fat_cut_writes_set = 1U;
    fat_cut_writes = 2U + 1U;
    assert(!MOD_StorageRestore(NULL, &count));
    fat_cut_writes_set = 0U;
    fat_card_present = 1U;
    SD_StoreClose();
    assert(SD_StoreOpen() == SD_STORE_READY && MOD_StorageIncomplete() == 2U);
    assert(fat_rm("SYSTEM/RESTORE.BIN"));
    assert(strcmp(MOD_StorageResumeAction(), "KEEP THE STORE AS IT IS") == 0);
    assert(MOD_StorageResume() && MOD_StorageIncomplete() == 0U);
    assert(!store_cal(9U, 1U, 0U, &r));                 /* kept as it was: erased */
    assert(!fat_wrote_outside);
}

int main(void)
{
    test_flash_store();
    test_card_store();
    test_v26_from_flash();
    test_refusals();
    test_backup_restore();
    test_cal_store();
    test_cal_deferred();
    test_background_backup();
    test_cut_short();
    printf("store_import: PASS\n");
    return 0;
}

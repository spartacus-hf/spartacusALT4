#pragma GCC optimize ("Os") /* main loop, rare: built for size */
/**
 * @file    mod_storage_import.c
 * @brief   The settings store's import (from v8.08), backup and restore.
 *
 * Import: v8.08 kept settings and calibration in the MCU's flash (0x08075000,
 * 44 KiB) and, with the card as its store, in M32CFG.BIN / M32BAK.BIN /
 * M32CAL.BIN on the card. v8.09's application covers that flash, so
 * tools/flash_rev1.py --factory reads it over SWD first and puts it on the
 * card as SYSTEM/NVM808.BIN (a 64-byte header, then the 44 KiB). The import
 * reads both with v8.08's own rules (MOD_LoadSettings, cal_find_record of
 * v8.08): the flash record says where the settings lived; a key's card
 * record, where the card was the store, wins over its flash record; within
 * one source the highest sequence number wins. Every record is checked as
 * v8.08 checked it before it is written.
 *
 * Backup and restore: SYSTEM/STOREBAK.BIN holds every valid record of the
 * store, each with its CRC, after a header naming the radio (MCU UID). It is
 * built as SYSTEM/STOREBAK.TMP and renamed over the last backup only once
 * whole, so a backup cut short leaves the previous one. It is written on
 * request (SD menu, console, before a factory reset) and, a minute after a
 * calibration change or an import, by the main loop in the background, a few
 * slots a step, at most every ten minutes. A restore reads SYSTEM/RESTORE.BIN
 * (or the backup) the same way, checks all of it first and writes only then.
 */

#include "mod_storage_format_priv.h"
#include "mod_cal_storage_priv.h"
#include "mod_storage_store_priv.h"
#include "mod_sd_store.h"
#include "mod_sdcard.h"
#include "mod_sdcard_service.h"
#include "fw_update_format.h"
#include "mod_crc32.h"
#include "../../Middlewares/Third_Party/FatFs/src/ff.h"
#include <stdio.h>

#define SYSTEM_DIR    "SYSTEM"
#define BACKUP_FILE   "SYSTEM/STOREBAK.BIN"
#define BACKUP_TMP    "SYSTEM/STOREBAK.TMP"
#define RESTORE_FILE  "SYSTEM/RESTORE.BIN"
#define NVM_FILE      "SYSTEM/NVM808.BIN"
#define IMPORT_LOG    "SYSTEM/IMPORT.LOG"
#define V808_CFG      "M32CFG.BIN"
#define V808_BAK      "M32BAK.BIN"
#define V808_CAL      "M32CAL.BIN"

/* v8.08's flash store, as the dump carries it. */
#define NVM_MAGIC        0x384D564EUL /* "NVM8" */
#define NVM_BASE         0x08075000UL
#define NVM_LENGTH       0xB000UL     /* 4 KiB settings + 40 KiB calibration */
#define NVM_HEADER       64U
#define NVM_PAGE         2048U
#define NVM_CAL_OFFSET   0x1000UL
#define NVM_CAL_PAGES    20U
#define NVM_CAL_STRIDE   288U         /* CAL_RECORD_SIZE: (288 + 7) & ~7 */
#define NVM_CAL_PER_PAGE (NVM_PAGE / NVM_CAL_STRIDE)
#define V808_CAL_RECORDS (NVM_CAL_PER_PAGE * NVM_CAL_PAGES)

typedef struct {
    uint32_t magic, version, base, length, crc32;
    uint32_t uid[3];
    uint32_t source_fw;
    uint32_t reserved[7];
} nvm_header_t;

#define BACKUP_MAGIC 0x4232334DUL /* "M32B" */
typedef struct {
    uint32_t magic, version, entries, instance;
    uint32_t uid[3];
    uint32_t fw;
    uint32_t body_crc32;  /* every entry, header and payload */
    uint32_t header_crc32;
} backup_header_t;

typedef struct {
    uint16_t slot, reserved;
    uint32_t length;
    uint32_t crc32;
} backup_entry_t;

_Static_assert(sizeof(nvm_header_t) == NVM_HEADER, "flash dump header");
_Static_assert(sizeof(ssb_cal_record_t) == NVM_CAL_STRIDE, "v8.08 calibration record");
_Static_assert(BAND_COUNT * 6U <= V808_CAL_RECORDS, "keys");

static uint8_t files_ready(void)
{
    return (uint8_t)(SD_IsMounted() && SD_StoreDevIoAllowed());
}

static uint8_t read_at(FIL *f, uint32_t off, void *buf, uint32_t len)
{
    UINT got = 0U;
    return (uint8_t)(f_lseek(f, off) == FR_OK && f_read(f, buf, len, &got) == FR_OK && got == len);
}

/* ---- Import ---------------------------------------------------------------- */

typedef struct {
    uint8_t nvm, files;           /* sources found */
    uint8_t settings_from;        /* 0 none, 1 flash, 2 card file */
    uint8_t backend_sd;           /* v8.08's settings said the card was the store */
    uint16_t cal_flash, cal_card; /* keys taken from each */
    uint16_t written, failed;
} import_result_t;

static import_result_t last_import;

/* The first bytes of a settings record: rec_magic, seq, and the blob's
 * magic, version, length and CRC. */
typedef struct { uint32_t rec_magic, seq, magic, version, length, crc32; } settings_head_t;

/* A settings record at `at` of an open file checked without holding it: its
 * head plausible, and the CRC of its blob streamed off the file. */
static uint8_t settings_at_valid(FIL *f, uint32_t at, uint32_t room, settings_head_t *h)
{
    uint8_t chunk[64];
    if (!read_at(f, at, h, sizeof(*h)) || h->rec_magic != SETTINGS_REC_MAGIC ||
        h->magic != SETTINGS_MAGIC) return 0U;
    const uint32_t length = storage_blob_length(h->version);
    if (length == 0U || h->length != length || 8U + length > room) return 0U;
    uint32_t crc = 0xFFFFFFFFUL, off = at + 8U + 16U, left = length - 16U;
    while (left) {
        const uint32_t n = left > sizeof(chunk) ? (uint32_t)sizeof(chunk) : left;
        if (!read_at(f, off, chunk, n)) return 0U;
        crc = fw_crc32_continue(crc, chunk, n);
        off += n;
        left -= n;
    }
    return (uint8_t)(~crc == h->crc32);
}

/* Read a checked record into `out` and load its blob as v8.08 would. */
static uint8_t settings_take(FIL *f, uint32_t at, const settings_head_t *h, settings_record_t *out)
{
    memset(out, 0, sizeof(*out));
    return (uint8_t)(read_at(f, at, out, 8U + h->length) && storage_blob_load_in_place(&out->blob));
}

/* The newest valid settings record of v8.08's two flash pages: records at
 * any doubleword, as settings_scan_latest scanned them (a later one wins a
 * tie). */
static uint8_t nvm_settings(FIL *f, settings_record_t *best)
{
    settings_head_t h;
    uint8_t found = 0U;
    for (uint32_t page = 0U; page < 2U; ++page) {
        for (uint32_t off = 0U; off + 24U <= NVM_PAGE; off += 8U) {
            const uint32_t at = NVM_HEADER + page * NVM_PAGE + off;
            if (!settings_at_valid(f, at, NVM_PAGE - off, &h)) continue;
            if (found && h.seq < best->seq) continue;
            found = settings_take(f, at, &h, best);
        }
    }
    return found;
}

/* v8.08's settings file on the card into `out`: M32CFG.BIN or its backup
 * M32BAK.BIN, whichever valid one is newer (v8.08 itself took the first
 * readable one, valid or not). `out` is left alone when neither is. */
static uint8_t card_settings(settings_record_t *out)
{
    static const char *const names[2] = {V808_CFG, V808_BAK};
    settings_head_t h;
    uint32_t card_seq = 0U, pick_seq = 0U;
    uint8_t found = 0U, pick = 0xFFU;
    for (uint8_t i = 0U; i < 2U; ++i) {
        FIL f;
        if (f_open(&f, names[i], FA_READ) != FR_OK) continue;
        if (settings_at_valid(&f, 0U, (uint32_t)f_size(&f), &h) && (!found || h.seq > card_seq)) {
            found = 1U;
            card_seq = h.seq;
            pick = i;
            pick_seq = h.seq;
        }
        (void)f_close(&f);
    }
    if (!found) return 0U;
    FIL f;
    if (f_open(&f, names[pick], FA_READ) != FR_OK) return 0U;
    const uint8_t ok = (uint8_t)(settings_at_valid(&f, 0U, (uint32_t)f_size(&f), &h) && h.seq == pick_seq &&
                                 settings_take(&f, 0U, &h, out));
    (void)f_close(&f);
    return ok;
}

/* Per key, where its newest record is: the file offset (bit 31: the card
 * file, else the flash dump) and its sequence number. */
typedef struct { uint32_t at, seq; } cal_pick_t;
#define PICK_CARD 0x80000000UL

static __attribute__((noinline)) void cal_scan(FIL *f, uint32_t base, uint32_t count, uint8_t page_breaks,
                                               uint32_t mark, cal_pick_t *pick)
{
    ssb_cal_record_t raw, rec;
    for (uint32_t i = 0U; i < count; ++i) {
        const uint32_t at = base + (page_breaks ? (i / NVM_CAL_PER_PAGE) * NVM_PAGE +
                                                  (i % NVM_CAL_PER_PAGE) * NVM_CAL_STRIDE
                                                : i * NVM_CAL_STRIDE);
        if (!read_at(f, at, &raw, sizeof(raw))) {
            if (page_breaks) continue;
            return; /* the card file ends */
        }
        /* An erased slot ends its flash page, as v8.08's scan stopped there. */
        if (page_breaks && raw.rec_magic == 0xFFFFFFFFUL) {
            i = (i / NVM_CAL_PER_PAGE + 1U) * NVM_CAL_PER_PAGE - 1U;
            continue;
        }
        if (!cal_record_accept(&raw, &rec)) continue;
        const uint16_t slot = cal_slot_for(rec.band_idx, rec.sideband, rec.analytic_backend);
        if (slot == 0U) continue;
        cal_pick_t *p = &pick[slot - 1U];
        const uint8_t same_source = (uint8_t)((p->at & PICK_CARD) == mark);
        if (p->at == 0U || !same_source || rec.seq >= p->seq) {
            if (p->at != 0U && !same_source && mark == 0U) continue; /* flash never over card */
            p->at = at | mark;
            p->seq = rec.seq;
        }
    }
}

static __attribute__((noinline)) void cal_write(FIL *nvm, FIL *card, const cal_pick_t *pick, import_result_t *r)
{
    ssb_cal_record_t raw, rec;
    for (uint32_t k = 0U; k < BAND_COUNT * 6U; ++k) {
        if (pick[k].at == 0U) continue;
        FIL *f = (pick[k].at & PICK_CARD) ? card : nvm;
        if (!read_at(f, pick[k].at & ~PICK_CARD, &raw, sizeof(raw)) || !cal_record_accept(&raw, &rec) ||
            !cal_store_put(&rec)) {
            ++r->failed;
            continue;
        }
        ++r->written;
        if (pick[k].at & PICK_CARD) ++r->cal_card;
        else ++r->cal_flash;
        APP_WatchdogProgress();
    }
}

static uint8_t nvm_open(FIL *f)
{
    nvm_header_t h;
    if (f_open(f, NVM_FILE, FA_READ) != FR_OK) return 0U;
    uint32_t uid[3];
    SD_StoreDevUid(uid);
    uint8_t ok = (uint8_t)(read_at(f, 0U, &h, sizeof(h)) && h.magic == NVM_MAGIC && h.version == 1U &&
                           h.base == NVM_BASE && h.length == NVM_LENGTH &&
                           f_size(f) == NVM_HEADER + NVM_LENGTH && memcmp(h.uid, uid, sizeof(uid)) == 0);
    if (ok) {
        /* The whole dump against its CRC, in sector-sized pieces. */
        uint8_t *b = SD_StoreDevBuffer();
        uint32_t crc = 0xFFFFFFFFUL;
        for (uint32_t off = 0U; ok && off < NVM_LENGTH; off += 512U) {
            ok = read_at(f, NVM_HEADER + off, b, 512U);
            crc = fw_crc32_continue(crc, b, 512U);
        }
        ok = (uint8_t)(ok && ~crc == h.crc32);
    }
    if (!ok) (void)f_close(f);
    return ok;
}

static void import_log(const import_result_t *r)
{
    char line[128];
    const int n = snprintf(line, sizeof(line),
                           "IMPORT v%u.%02u nvm=%u files=%u settings=%s backend=%s cal_flash=%u cal_card=%u written=%u failed=%u\r\n",
                           (unsigned)FW_VERSION_MAJOR, (unsigned)FW_VERSION_MINOR, (unsigned)r->nvm,
                           (unsigned)r->files,
                           r->settings_from == 1U ? "flash" : r->settings_from == 2U ? "card" : "none",
                           r->backend_sd ? "sd" : "flash", (unsigned)r->cal_flash, (unsigned)r->cal_card,
                           (unsigned)r->written, (unsigned)r->failed);
    if (n > 0) (void)SD_DiagWriteFile(IMPORT_LOG, 0U, line, (uint32_t)n, 1U);
}

/* The settings, picked as v8.08's MOD_LoadSettings picked them: the flash
 * record says where they lived -- in flash (or just moved from it: FROM_FLASH),
 * the flash record; on the card, the card's file, the flash record standing
 * in when the card has none. Then run on them and store them. */
static __attribute__((noinline)) void import_settings(FIL *nvm, uint8_t have_nvm, import_result_t *r)
{
    settings_record_t rec;
    memset(&rec, 0, sizeof(rec));
    const uint8_t have_flash = (uint8_t)(have_nvm && nvm_settings(nvm, &rec));
    r->backend_sd = (uint8_t)(!have_flash ||
                              rec.blob.outphase_reserved[SETTINGS_BACKEND_IDX] == SETTINGS_BACKEND_SD);
    const uint8_t from_flash = (uint8_t)(have_flash &&
                                         (rec.blob.outphase_reserved[SETTINGS_FLAGS_IDX] & SETTINGS_FLAG_FROM_FLASH));
    if (have_flash) r->settings_from = 1U;
    if (r->backend_sd && !from_flash && card_settings(&rec)) {
        r->settings_from = 2U;
        r->files = 1U;
    }
    if (!r->settings_from) return;
    rec.blob.outphase_reserved[SETTINGS_FLAGS_IDX] = 0U;
    rec.blob.outphase_reserved[SETTINGS_BACKEND_IDX] = SETTINGS_BACKEND_SD;
    rec.blob.crc32 = crc32_simple((const uint8_t *)&rec.blob + 16U, sizeof(rec.blob) - 16U);
    if (storage_settings_apply_save(&rec.blob)) ++r->written;
    else ++r->failed;
}

/* Every key's newest record (where the card was v8.08's store, a card
 * record over a flash one), into the store. */
static __attribute__((noinline)) void import_cal(FIL *nvm, FIL *card, uint8_t have_card, import_result_t *r)
{
    cal_pick_t pick[BAND_COUNT * 6U];
    memset(pick, 0, sizeof(pick));
    if (r->nvm) cal_scan(nvm, NVM_HEADER + NVM_CAL_OFFSET, V808_CAL_RECORDS, 1U, 0U, pick);
    if (have_card && r->backend_sd) cal_scan(card, 0U, V808_CAL_RECORDS, 0U, PICK_CARD, pick);
    cal_write(nvm, card, pick, r);
}

uint8_t MOD_StorageImport(uint8_t require_nvm)
{
    import_result_t r;
    FIL nvm, card;
    memset(&r, 0, sizeof(r));
    if (MOD_StorageRunsWithoutCard() || !files_ready()) return 0U;
    const sd_store_state_t state = SD_StoreState();
    if (state != SD_STORE_READY && state != SD_STORE_BLANK) return 0U;

    SD_DriverHold(1U);
    r.nvm = nvm_open(&nvm);
    if (require_nvm && !r.nvm) {
        SD_DriverHold(0U);
        return 0U;
    }
    const uint8_t have_card_cal = (uint8_t)(f_open(&card, V808_CAL, FA_READ) == FR_OK);
    if (have_card_cal) r.files = 1U;
    /* INCOMPLETE until the last record is in: a power cut part way leaves a
     * store the boot holds on (MOD_StorageCardRequired), not one that runs
     * on half its calibration. */
    const uint32_t origin = (r.nvm ? SD_STORE_ORIGIN_NVM : 0U) |
                            ((have_card_cal || MOD_StorageImportAvailable()) ? SD_STORE_ORIGIN_FILES : 0U) |
                            SD_STORE_ORIGIN_IMPORTING;
    uint8_t ok = (state == SD_STORE_BLANK) ? SD_StoreCreate(origin) : SD_StoreSetOrigin(origin, 0U);
    if (ok) {
        import_settings(&nvm, r.nvm, &r);
        import_cal(&nvm, &card, have_card_cal, &r);
        ok = SD_StoreSetOrigin(0U, SD_STORE_ORIGIN_IMPORTING);
    }
    if (r.nvm) (void)f_close(&nvm);
    if (have_card_cal) (void)f_close(&card);
    SD_DriverHold(0U);
    if (!ok) return 0U;

    cal_store_forget();
    load_current_calibration_context();
    last_import = r;
    import_log(&r);
    MOD_StorageBackupRequest(); /* the imported store gets its backup soon */
    MOD_EventLog(MOD_EVENT_STORAGE, 16U, (uint16_t)r.written, ((uint32_t)r.cal_flash << 16) | r.cal_card,
                 ((uint32_t)r.nvm << 8) | r.settings_from);
    return (uint8_t)(r.failed == 0U);
}

/* An import (1) or a restore (2) was cut short: the store is this radio's,
 * but its records are part old, part new, or missing. 0: neither. */
uint8_t MOD_StorageIncomplete(void)
{
    sd_store_info_t info;
    SD_StoreGetInfo(&info);
    if (info.state != SD_STORE_READY) return 0U;
    return (info.origin & SD_STORE_ORIGIN_RESTORING) ? 2U : (info.origin & SD_STORE_ORIGIN_IMPORTING) ? 1U : 0U;
}

/* The operator takes the store as it is (`storage keep`, the boot hold). */
uint8_t MOD_StorageKeep(void)
{
    if (!MOD_StorageIncomplete()) return 1U;
    if (!files_ready() || !SD_StoreSetOrigin(0U, SD_STORE_ORIGIN_INCOMPLETE)) return 0U;
    MOD_EventLog(MOD_EVENT_STORAGE, 18U, 0U, 0U, 0U);
    return 1U;
}

static uint8_t restore_file_present(void)
{
    FILINFO fi;
    return (uint8_t)(files_ready() && f_stat(RESTORE_FILE, &fi) == FR_OK);
}

/* What finishing a cut-short import or restore does: the same operation
 * again when its source is still on the card, else keep the store. */
const char *MOD_StorageResumeAction(void)
{
    const uint8_t cut = MOD_StorageIncomplete();
    if (cut == 1U && MOD_StorageImportAvailable()) return "IMPORT V8.08 AGAIN";
    if (cut == 2U && restore_file_present()) return "RESTORE AGAIN";
    return "KEEP THE STORE AS IT IS";
}

uint8_t MOD_StorageResume(void)
{
    const uint8_t cut = MOD_StorageIncomplete();
    if (cut == 1U && MOD_StorageImportAvailable()) return MOD_StorageImport(0U);
    if (cut == 2U && restore_file_present()) return MOD_StorageRestore(NULL, NULL);
    return MOD_StorageKeep();
}

/* What the last import did, for the console. */
void MOD_StorageImportSummary(uint8_t *nvm, uint8_t *files, uint8_t *settings_from,
                              uint16_t *cal_flash, uint16_t *cal_card, uint16_t *failed)
{
    if (nvm) *nvm = last_import.nvm;
    if (files) *files = last_import.files;
    if (settings_from) *settings_from = last_import.settings_from;
    if (cal_flash) *cal_flash = last_import.cal_flash;
    if (cal_card) *cal_card = last_import.cal_card;
    if (failed) *failed = last_import.failed;
}

/* Is there something to import on this card (the boot hold's question)? */
uint8_t MOD_StorageImportAvailable(void)
{
    FILINFO info;
    if (!files_ready()) return 0U;
    if (f_stat(NVM_FILE, &info) == FR_OK) return 2U;
    if (f_stat(V808_CFG, &info) == FR_OK || f_stat(V808_CAL, &info) == FR_OK) return 1U;
    return 0U;
}

/* ---- Backup and restore ---------------------------------------------------- */

static uint8_t write_all(FIL *f, const void *data, uint32_t len)
{
    UINT put = 0U;
    return (uint8_t)(f_write(f, data, len, &put) == FR_OK && put == len);
}

/* A backup under construction: the next slot, and what the header will say. */
typedef struct {
    uint8_t  active;
    uint16_t slot;
    uint32_t entries, body, instance;
} backup_job_t;

static backup_job_t job;

/* Background backups (MOD_StorageBackupService). */
#define BACKUP_QUIET_MS    60000U   /* after the last calibration change */
#define BACKUP_MIN_GAP_MS  600000U  /* between two background backups */
#define BACKUP_STEP_MS     500U
#define BACKUP_STEP_SLOTS  4U
static uint32_t backup_requests, backup_seen, backup_changed_at, backup_done_at;
static uint32_t backup_ok, backup_failed;
static uint8_t backup_pending;

void MOD_StorageBackupRequest(void)
{
    ++backup_requests;
}

static __attribute__((noinline)) uint8_t backup_begin(void)
{
    sd_store_info_t info;
    FIL f;
    job.active = 0U;
    if (!files_ready() || SD_StoreState() != SD_STORE_READY) return 0U;
    SD_StoreGetInfo(&info);
    backup_header_t h;
    memset(&h, 0, sizeof(h)); /* a placeholder until the job finishes */
    SD_DriverHold(1U);
    FRESULT res = f_mkdir(SYSTEM_DIR);
    uint8_t ok = (uint8_t)((res == FR_OK || res == FR_EXIST) &&
                           f_open(&f, BACKUP_TMP, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    if (ok) {
        ok = write_all(&f, &h, sizeof(h));
        if (f_close(&f) != FR_OK) ok = 0U;
    }
    SD_DriverHold(0U);
    if (!ok) return 0U;
    job.active = 1U;
    job.slot = 0U;
    job.entries = 0U;
    job.body = 0xFFFFFFFFUL;
    job.instance = info.instance;
    return 1U;
}

/* The header, then the finished file in place of the last backup. */
static __attribute__((noinline)) uint8_t backup_finish(void)
{
    FIL f;
    backup_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = BACKUP_MAGIC;
    h.version = 1U;
    h.entries = job.entries;
    h.instance = job.instance;
    SD_StoreDevUid(h.uid);
    h.fw = (uint32_t)FW_VERSION_MAJOR * 100UL + (uint32_t)FW_VERSION_MINOR;
    h.body_crc32 = ~job.body;
    h.header_crc32 = fw_crc32(&h, offsetof(backup_header_t, header_crc32));
    SD_DriverHold(1U);
    uint8_t ok = (uint8_t)(f_open(&f, BACKUP_TMP, FA_OPEN_EXISTING | FA_WRITE) == FR_OK);
    if (ok) {
        ok = (uint8_t)(write_all(&f, &h, sizeof(h)) && f_sync(&f) == FR_OK);
        if (f_close(&f) != FR_OK) ok = 0U;
    }
    if (ok) {
        const FRESULT gone = f_unlink(BACKUP_FILE);
        ok = (uint8_t)((gone == FR_OK || gone == FR_NO_FILE) && f_rename(BACKUP_TMP, BACKUP_FILE) == FR_OK);
    }
    SD_DriverHold(0U);
    return ok;
}

/* Up to `slots` slots of the store into the file. 1 while there is more,
 * 2 once the backup is in place, 0 when it failed (the job is dropped). */
static __attribute__((noinline)) uint8_t backup_steps(uint16_t slots)
{
    settings_record_t buf; /* the largest record */
    sd_store_info_t info;
    FIL f;
    if (!job.active) return 0U;
    SD_StoreGetInfo(&info);
    if (!files_ready() || info.state != SD_STORE_READY || info.instance != job.instance) {
        job.active = 0U;
        return 0U;
    }
    SD_DriverHold(1U);
    uint8_t ok = (uint8_t)(f_open(&f, BACKUP_TMP, FA_OPEN_APPEND | FA_WRITE) == FR_OK);
    for (uint16_t n = 0U; ok && n < slots && job.slot < SD_STORE_SLOTS; ++n, ++job.slot) {
        uint32_t len = 0U;
        const sd_store_rec_t rr = SD_StoreRead(job.slot, &buf, sizeof(buf), &len, NULL);
        if (rr == SD_STORE_REC_NONE) continue;
        if (rr != SD_STORE_REC_OK) { ok = 0U; break; }
        const backup_entry_t e = {job.slot, 0U, len, fw_crc32(&buf, len)};
        job.body = fw_crc32_continue(job.body, &e, sizeof(e));
        job.body = fw_crc32_continue(job.body, &buf, len);
        ok = (uint8_t)(write_all(&f, &e, sizeof(e)) && write_all(&f, &buf, len));
        ++job.entries;
        APP_WatchdogProgress();
    }
    if (ok && f_close(&f) != FR_OK) ok = 0U;
    else if (!ok) (void)f_close(&f);
    SD_DriverHold(0U);
    if (ok && job.slot >= SD_STORE_SLOTS) {
        job.active = 0U;
        return backup_finish() ? 2U : 0U;
    }
    if (!ok) job.active = 0U;
    return ok;
}

uint8_t MOD_StorageBackup(void)
{
    uint8_t r = backup_begin();
    while (r == 1U) r = backup_steps(SD_STORE_SLOTS);
    if (r == 2U) ++backup_ok;
    else ++backup_failed;
    return (uint8_t)(r == 2U);
}

/* Main loop (MOD_StorageAutosaveService): the background backup, when one
 * is due, a step at a time and only where card I/O may block. */
void MOD_StorageBackupService(uint32_t now, uint8_t io_allowed)
{
    static uint32_t step_at;
    if (backup_requests != backup_seen) {
        backup_seen = backup_requests;
        backup_changed_at = now;
        backup_pending = 1U;
    }
    if (!io_allowed) return;
    if (job.active) {
        if ((uint32_t)(now - step_at) < BACKUP_STEP_MS) return;
        step_at = now;
        const uint8_t r = backup_steps(BACKUP_STEP_SLOTS);
        if (r == 2U) {
            ++backup_ok;
            backup_done_at = now ? now : 1U;
        } else if (r == 0U) {
            ++backup_failed;
            backup_pending = 1U; /* try again after another quiet minute */
            backup_changed_at = now;
        }
        return;
    }
    if (!backup_pending || (uint32_t)(now - backup_changed_at) < BACKUP_QUIET_MS) return;
    if (backup_done_at && (uint32_t)(now - backup_done_at) < BACKUP_MIN_GAP_MS) return;
    backup_pending = 0U;
    step_at = now;
    if (!backup_begin()) {
        ++backup_failed;
        backup_pending = 1U;
        backup_changed_at = now;
    }
}

void MOD_StorageBackupStatus(uint8_t *pending, uint8_t *running, uint16_t *slot, uint32_t *ok, uint32_t *failed)
{
    if (pending) *pending = backup_pending;
    if (running) *running = job.active;
    if (slot) *slot = job.slot;
    if (ok) *ok = backup_ok;
    if (failed) *failed = backup_failed;
}

/* Every entry of a backup file checked (pass 0), then written (pass 1). */
static __attribute__((noinline)) uint8_t restore_pass(FIL *f, const backup_header_t *h, uint8_t write,
                                                      uint16_t *count)
{
    settings_record_t buf;
    ssb_cal_record_t rec;
    uint32_t at = sizeof(*h), body = 0xFFFFFFFFUL;
    *count = 0U;
    for (uint32_t n = 0U; n < h->entries; ++n) {
        backup_entry_t e;
        if (!read_at(f, at, &e, sizeof(e)) || e.slot >= SD_STORE_SLOTS || e.length == 0U ||
            e.length > sizeof(buf) || !read_at(f, at + sizeof(e), &buf, e.length) ||
            fw_crc32(&buf, e.length) != e.crc32) return 0U;
        body = fw_crc32_continue(body, &e, sizeof(e));
        body = fw_crc32_continue(body, &buf, e.length);
        at += sizeof(e) + e.length;
        if (e.slot == 0U) {
            if (buf.rec_magic != SETTINGS_REC_MAGIC || !storage_blob_valid(&buf.blob)) return 0U;
            if (write && !SD_StoreWrite(0U, &buf, e.length)) return 0U;
        } else if (e.slot == SD_STORE_SLOT_TRAINER) {
            if (!MOD_TrainerRecordAccept(&buf, e.length)) return 0U;
            if (write && !SD_StoreWrite(SD_STORE_SLOT_TRAINER, &buf, e.length)) return 0U;
        } else {
            if (e.length != sizeof(rec) || !cal_record_accept(&buf, &rec) ||
                cal_slot_for(rec.band_idx, rec.sideband, rec.analytic_backend) != e.slot) return 0U;
            if (write && !cal_store_put(&rec)) return 0U;
        }
        ++*count;
        APP_WatchdogProgress();
    }
    return (uint8_t)(write || ~body == h->body_crc32);
}

uint8_t MOD_StorageRestore(const char *path, uint16_t *restored)
{
    FIL f;
    backup_header_t h;
    uint16_t count = 0U;
    if (restored) *restored = 0U;
    if (MOD_StorageRunsWithoutCard() || !files_ready()) return 0U;
    const sd_store_state_t state = SD_StoreState();
    if (state != SD_STORE_READY && state != SD_STORE_BLANK) return 0U;
    SD_DriverHold(1U);
    uint8_t ok = (uint8_t)(f_open(&f, path ? path : RESTORE_FILE, FA_READ) == FR_OK);
    if (!ok) {
        SD_DriverHold(0U);
        return 0U;
    }
    uint32_t uid[3];
    SD_StoreDevUid(uid);
    ok = (uint8_t)(read_at(&f, 0U, &h, sizeof(h)) && h.magic == BACKUP_MAGIC && h.version == 1U &&
                   h.header_crc32 == fw_crc32(&h, offsetof(backup_header_t, header_crc32)) &&
                   memcmp(h.uid, uid, sizeof(uid)) == 0 && h.entries <= SD_STORE_SLOTS &&
                   restore_pass(&f, &h, 0U, &count));
    const uint32_t origin = SD_STORE_ORIGIN_RESTORED | SD_STORE_ORIGIN_RESTORING;
    if (ok && state == SD_STORE_BLANK) ok = SD_StoreCreate(origin);
    else if (ok) ok = SD_StoreSetOrigin(origin, 0U);
    if (ok) ok = restore_pass(&f, &h, 1U, &count);
    if (ok) ok = SD_StoreSetOrigin(0U, SD_STORE_ORIGIN_RESTORING);
    (void)f_close(&f);
    SD_DriverHold(0U);
    if (!ok) return 0U;
    if (restored) *restored = count;
    cal_store_forget();
    storage_trainer_forget();
    (void)MOD_LoadSettings();
    load_current_calibration_context();
    MOD_EventLog(MOD_EVENT_STORAGE, 17U, count, 0U, 0U);
    return 1U;
}

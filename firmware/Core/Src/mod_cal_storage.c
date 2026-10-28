#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_cal_storage_priv.h"
#include "mod_calibration_logic.h"
#include "mod_crc32.h"
#include "mod_sd_store.h"
#include "radio.h"
#include "radio_config.h"
#include "board_rev1.h"

#if MOD_ENABLE_CAL_STORAGE

/* Calibration per band, sideband and analytic backend: one record each in
 * the card's settings store (mod_sd_store.c), slot 1 + band * 6 + sideband *
 * 3 + backend. The active band's two sideband records are kept here once
 * read, so a sideband change reads nothing and a band change reads two
 * sectors (v8.08 scanned up to 40 KB of M32CAL.BIN, 134 ms, for each).
 *
 * A band's record that could not be read is never written over: the values
 * in RAM are then zeros, not that band's, and an autosave would have put
 * them in its place. Until it is read, TX is locked (CAL NOT LOADED, a
 * RADIO_ConfigTxBlock reason: no unmute gets past it). A card lost while the
 * radio runs therefore never makes it transmit on values it did not mean to;
 * RUN WITHOUT CARD (SD menu, console `storage nocard`) is the operator's way
 * to transmit uncalibrated, knowingly, as on a band never calibrated.
 *
 * A band change reads while its own retune holds the I/O gate shut, and a
 * USB audio stream holds it shut for as long as it runs: those reads wait,
 * quietly, and go as soon as they may (cal_read_band_now) -- TX is locked
 * meanwhile, and CAL NOT LOADED is said only for a read that failed or one
 * still waiting after CAL_QUIET_MS. */

_Static_assert(1U + (uint32_t)BAND_COUNT * 6U <= SD_STORE_SLOTS, "every calibration key has a store slot");
_Static_assert(sizeof(ssb_cal_record_t) <= SD_STORE_SLOT_MAX, "a calibration record fits a store slot");

typedef struct {
    uint16_t band;
    uint8_t  backend;   /* storage key */
    uint8_t  valid;     /* both sidebands read from the store */
    uint8_t  found[2];  /* a record exists for the sideband */
    ssb_cal_record_t rec[2];
} cal_cache_t;

static cal_cache_t cal_cache;
static uint8_t cal_loaded;         /* the active band's calibration came from the store */
static uint8_t cal_reload_pending; /* not read yet: TX locked */
static uint8_t cal_read_failed;    /* ...and a read failed, not only waited for the gate */
static uint8_t cal_logged;         /* CAL NOT LOADED logged for this wait */
static uint32_t cal_pending_since;

#define CAL_QUIET_MS 3000U

static uint8_t cal_record_payload_equal(const ssb_cal_record_t *a, const ssb_cal_record_t *b)
{
    if (a->band_idx != b->band_idx) return 0U;
    if (cal_backend_storage_key(a->analytic_backend) != cal_backend_storage_key(b->analytic_backend)) return 0U;
    if (a->sideband != b->sideband) return 0U;
    if (a->branch_ac_offset_ticks != b->branch_ac_offset_ticks) return 0U;
    if ((a->flags & SSB_CAL_FLAGS_MASK) != (b->flags & SSB_CAL_FLAGS_MASK)) return 0U;
    if (memcmp(&a->phase_cal, &b->phase_cal, sizeof(a->phase_cal)) != 0) return 0U;
    if (memcmp(a->theta_linearity_pd, b->theta_linearity_pd,
               sizeof(a->theta_linearity_pd)) != 0) return 0U;
    return 1U;
}

static uint32_t cal_crc(const ssb_cal_record_t *rec)
{
    return crc32_simple((const uint8_t *)&rec->seq,
                        sizeof(*rec) - offsetof(ssb_cal_record_t, seq) - sizeof(rec->crc32));
}

static uint8_t cal_record_is_valid(const ssb_cal_record_t *rec)
{
    if (rec->rec_magic != CAL_REC_MAGIC) return 0U;
    if (rec->band_idx >= BAND_COUNT) return 0U;
    if (rec->sideband > (uint8_t)SIDEBAND_LSB) return 0U;
    if (rec->analytic_backend >= (uint8_t)SSB_ANALYTIC_COUNT) return 0U;
    return (cal_crc(rec) == rec->crc32) ? 1U : 0U;
}

static void cal_record_normalize(const ssb_cal_record_t *stored, ssb_cal_record_t *current)
{
    /* Only CAL4 records get here (cal_record_is_valid): the prototype's CAL2
     * conversion went with the old settings migrations. */
    *current = *stored;
    current->flags &= SSB_CAL_FLAGS_MASK;
    phase_cal_sanitize(&current->phase_cal);
    for (uint32_t i = 0U; i < PHASE_CAL_BINS; i++) {
        float value = current->theta_linearity_pd[i];
        if (!isfinite(value)) value = 0.0f;
        current->theta_linearity_pd[i] = clampf(value,
                                                 -PHASE_CAL_THETA_LIMIT,
                                                 PHASE_CAL_THETA_LIMIT);
    }
}

uint8_t cal_record_accept(const void *raw, ssb_cal_record_t *out)
{
    ssb_cal_record_t rec;
    memcpy(&rec, raw, sizeof(rec));
    if (!cal_record_is_valid(&rec)) return 0U;
    cal_record_normalize(&rec, out);
    return 1U;
}

static uint8_t cal_backend_index(uint8_t storage_key)
{
    if (storage_key == (uint8_t)SSB_ANALYTIC_WEAVER) return 0U;
    if (storage_key == (uint8_t)SSB_ANALYTIC_H12P) return 2U;
    return 1U; /* the Hilbert family shares one record */
}

uint16_t cal_slot_for(uint16_t band_idx, uint8_t sideband, uint8_t backend)
{
    if (band_idx >= BAND_COUNT || sideband > (uint8_t)SIDEBAND_LSB) return 0U;
    return (uint16_t)(1U + (uint32_t)band_idx * 6U + (uint32_t)sideband * 3U +
                      cal_backend_index(cal_backend_storage_key(backend)));
}

/* The record as it is stored: the store's own sequence number orders
 * versions, so the record's seq is 0 and its CRC covers that. */
uint8_t cal_store_put(const ssb_cal_record_t *rec)
{
    const uint16_t slot = cal_slot_for(rec->band_idx, rec->sideband, rec->analytic_backend);
    if (slot == 0U) return 0U;
    ssb_cal_record_t out = *rec;
    out.rec_magic = CAL_REC_MAGIC;
    out.analytic_backend = cal_backend_storage_key(rec->analytic_backend);
    out.seq = 0U;
    out.crc32 = cal_crc(&out);
    if (!SD_StoreWrite(slot, &out, sizeof(out))) return 0U;
    if (cal_cache.valid && cal_cache.band == out.band_idx && cal_cache.backend == out.analytic_backend) {
        cal_cache.rec[out.sideband] = out;
        cal_cache.found[out.sideband] = 1U;
    }
    return 1U;
}

/* Both sidebands of a band from the store; 0 when the store could not be
 * read (no card, I/O refused or failed) -- an absent record is not that. */
static uint8_t cal_read_band(uint16_t band_idx, uint8_t key)
{
    cal_cache.valid = 0U;
    for (uint8_t sb = 0U; sb < 2U; ++sb) {
        ssb_cal_record_t raw;
        uint32_t len = 0U;
        const sd_store_rec_t r = SD_StoreRead(cal_slot_for(band_idx, sb, key), &raw, sizeof(raw), &len, NULL);
        if (r == SD_STORE_REC_ERROR) return 0U;
        cal_cache.found[sb] = (uint8_t)(r == SD_STORE_REC_OK && len == sizeof(raw) &&
                                        cal_record_is_valid(&raw) && raw.band_idx == band_idx &&
                                        raw.sideband == sb &&
                                        cal_backend_storage_key(raw.analytic_backend) == key);
        if (cal_cache.found[sb]) cal_record_normalize(&raw, &cal_cache.rec[sb]);
    }
    cal_cache.band = band_idx;
    cal_cache.backend = key;
    cal_cache.valid = 1U;
    return 1U;
}

uint8_t MOD_CalNotLoaded(void)
{
    if (!cal_reload_pending || MOD_StorageRunsWithoutCard()) return 0U;
    return (uint8_t)(cal_read_failed || (uint32_t)(HAL_GetTick() - cal_pending_since) >= CAL_QUIET_MS);
}

static void cal_pending_set(uint8_t on, uint8_t waited)
{
    if (on && !cal_reload_pending) {
        cal_pending_since = HAL_GetTick();
        cal_logged = 0U;
        cal_read_failed = 0U;
    }
    cal_reload_pending = on;
    if (!on) cal_read_failed = 0U;
    else if (!waited) cal_read_failed = 1U;
    RADIO_ConfigSetStorageTxLock(on ? "CAL NOT LOADED" : NULL);
    if (MOD_CalNotLoaded() && !cal_logged) {
        cal_logged = 1U;
        MOD_EventLog(MOD_EVENT_STORAGE, 12U, active_band_idx, (uint32_t)SD_StoreState(), cal_read_failed);
    }
}

/* Past the I/O gate while TX is locked for these records: no TX can start,
 * the transmitter is down and no retune is under way, so a read holds up
 * nothing realtime (receive runs in interrupts). The gate's own refusals are
 * about TX audio, and a USB audio stream keeps it shut for as long as it runs
 * -- a digital-mode program's, for hours. v8.08 read flash here, no wait. */
static uint8_t cal_grant_ok(void)
{
    const radio_state_t st = RADIO_State();
    return (uint8_t)(cal_reload_pending && st != RADIO_TX_PREPARE && st != RADIO_TRANSMIT &&
                     !BOARD_RfActive() && !MOD_CarrierFrequencyApplyPending() &&
                     !carrier_clock_retune_in_progress);
}

/* The band's records from the store now, or 0 (`waited` set when only the
 * gate stood in the way). A card initialised again since the store read it
 * is looked at again first (MOD_StorageReconnect). */
static uint8_t cal_read_band_now(uint16_t band, uint8_t key, uint8_t *waited)
{
    *waited = 0U;
    if (MOD_BlockingIoAllowed(HAL_GetTick())) return (uint8_t)(MOD_StorageReconnect() && cal_read_band(band, key));
    if (!cal_grant_ok()) {
        *waited = 1U;
        return 0U;
    }
    SD_StoreDevGrantIo(1U);
    const uint8_t ok = (uint8_t)(MOD_StorageReconnect() && cal_read_band(band, key));
    SD_StoreDevGrantIo(0U);
    return ok;
}

/* Until the band's records read: a read that failed again once a second, one
 * that waited for the gate as soon as it may. */
void MOD_CalReloadService(uint32_t now)
{
    static uint32_t tried_at;
    if (!cal_reload_pending) return;
    if (MOD_CalNotLoaded() && !cal_logged) cal_pending_set(1U, 1U); /* the quiet wait is over: say so */
    if (cal_read_failed && (uint32_t)(now - tried_at) < 1000U) return;
    if (!MOD_BlockingIoAllowed(now) && !cal_grant_ok()) return;
    tried_at = now;
    load_current_calibration_context();
}

void load_current_calibration_context(void)
{
    refresh_active_cal_ptr();
    const uint16_t band = active_band_idx;
    const uint8_t sb = (uint8_t)active_sideband_enum;
    const uint8_t key = cal_backend_storage_key((uint8_t)cfg_ssb_analytic_backend);
    const uint8_t cached = (uint8_t)(cal_cache.valid && cal_cache.band == band && cal_cache.backend == key);
    uint8_t waited = 0U;
    const uint8_t have = (uint8_t)(cached || (!MOD_StorageRunsWithoutCard() && cal_read_band_now(band, key, &waited)));

    if (have && sb <= (uint8_t)SIDEBAND_LSB && cal_cache.found[sb]) {
        const ssb_cal_record_t *rec = &cal_cache.rec[sb];
        *active_cal = rec->phase_cal;
        phase_cal_sanitize(active_cal);
        cfg_branch_ac_offset_ticks = rec->branch_ac_offset_ticks;
        memcpy(theta_linearity_pd, rec->theta_linearity_pd,
               sizeof(theta_linearity_pd));
        active_cal_flags = rec->flags & SSB_CAL_FLAGS_MASK;
        {
            int16_t ac_lim = mod_branch_ac_limit_ticks();
            if (cfg_branch_ac_offset_ticks > ac_lim) {
                cfg_branch_ac_offset_ticks = ac_lim;
                carrier_values_capped = 1U;
            }
            if (cfg_branch_ac_offset_ticks < -ac_lim) {
                cfg_branch_ac_offset_ticks = -ac_lim;
                carrier_values_capped = 1U;
            }
        }
    } else {
        memset(active_cal, 0, sizeof(*active_cal));
        memset(theta_linearity_pd, 0, sizeof(theta_linearity_pd));
        active_cal_flags = 0U;
        cfg_branch_ac_offset_ticks = 0;
        /* CEU6 calibration remains in the backup; never apply it to REV1. */
    }
    cal_loaded = have;
    cal_pending_set((uint8_t)(!have && !MOD_StorageRunsWithoutCard()), waited);
}

/* The record the active values make, seq 0. */
static void cal_record_from_active(ssb_cal_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->rec_magic = CAL_REC_MAGIC;
    rec->band_idx = active_band_idx;
    rec->sideband = (uint8_t)active_sideband_enum;
    rec->analytic_backend = cal_backend_storage_key((uint8_t)cfg_ssb_analytic_backend);
    rec->phase_cal = *active_cal;
    rec->branch_ac_offset_ticks = cfg_branch_ac_offset_ticks;
    rec->flags = active_cal_flags & SSB_CAL_FLAGS_MASK;
    memcpy(rec->theta_linearity_pd, theta_linearity_pd,
           sizeof(theta_linearity_pd));
}

void cal_store_forget(void)
{
    cal_cache.valid = 0U;
}

uint8_t save_current_band_calibrations(void)
{
    if (MOD_StorageRunsWithoutCard()) return 1U; /* nothing is kept; the settings save says so */
    if (!cal_loaded) return 1U;                  /* never over a record that was not read */
    ssb_cal_record_t rec;
    cal_record_from_active(&rec);
    const uint8_t sb = rec.sideband;
    if (sb > (uint8_t)SIDEBAND_LSB) return 0U;
    if (cal_cache.valid && cal_cache.band == rec.band_idx && cal_cache.backend == rec.analytic_backend) {
        if (cal_cache.found[sb] && cal_record_payload_equal(&cal_cache.rec[sb], &rec)) return 1U;
        if (!cal_cache.found[sb]) {
            /* No record and nothing calibrated: nothing to keep. */
            ssb_cal_record_t blank;
            memset(&blank, 0, sizeof(blank));
            blank.band_idx = rec.band_idx;
            blank.sideband = rec.sideband;
            blank.analytic_backend = rec.analytic_backend;
            if (cal_record_payload_equal(&blank, &rec)) return 1U;
        }
    }
    if (!cal_store_put(&rec)) {
        MOD_EventLog(MOD_EVENT_STORAGE, 3U, 0U, rec.band_idx, cal_slot_for(rec.band_idx, rec.sideband, rec.analytic_backend));
        return 0U;
    }
    MOD_StorageBackupRequest(); /* a calibration changed: STOREBAK.BIN follows */
    return 1U;
}

/* Factory reset: every calibration record invalidated. */
uint8_t cal_store_erase_all(void)
{
    uint8_t ok = 1U;
    for (uint16_t band = 0U; band < BAND_COUNT; ++band) {
        for (uint16_t k = 0U; k < 6U; ++k) {
            if (!SD_StoreErase((uint16_t)(1U + band * 6U + k))) ok = 0U;
        }
        APP_WatchdogProgress();
    }
    cal_cache.valid = 0U;
    return ok;
}

#else /* MOD_ENABLE_CAL_STORAGE */

void load_current_calibration_context(void)
{
    refresh_active_cal_ptr();
    memset(active_cal, 0, sizeof(*active_cal));
    phase_cal_sanitize(active_cal);
    active_cal_flags = 0U;
    cfg_branch_ac_offset_ticks = 0;
    memset(theta_linearity_pd, 0, sizeof(theta_linearity_pd));
}

uint8_t save_current_band_calibrations(void) { return 1U; }
void cal_store_forget(void) {}
uint8_t cal_store_erase_all(void) { return 1U; }
uint8_t MOD_CalNotLoaded(void) { return 0U; }
void MOD_CalReloadService(uint32_t now) { (void)now; }

#endif

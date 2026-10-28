/* Size over speed: nothing here runs in the RF or audio path. */
#pragma GCC optimize ("Os")

#include "mod_storage_format_priv.h"
#include "mod_storage_store_priv.h"
#include "mod_crc32.h"

/* Settings blob format helpers and the one migration still carried.
 * This file is deliberately cold-path only; it must not alter the persisted
 * settings layout without an explicit SETTINGS_VERSION migration.
 */

uint8_t settings_pack_aux_modes(gain_telemetry_mode_t gain_mode, comp_math_mode_t comp_mode)
{
    uint8_t packed = 0U;
    if ((uint32_t)gain_mode >= (uint32_t)GAIN_TELEMETRY_COUNT) gain_mode = GAIN_TELEMETRY_OFF;
    if ((uint32_t)comp_mode >= (uint32_t)COMP_MATH_COUNT) comp_mode = COMP_MATH_LUT;
    packed |= (uint8_t)gain_mode & SETTINGS_RESERVED2_GAIN_TELEM_MASK;
    packed |= (uint8_t)(((uint8_t)comp_mode << SETTINGS_RESERVED2_COMP_MATH_SHIFT) & SETTINGS_RESERVED2_COMP_MATH_MASK);
    return packed;
}


gain_telemetry_mode_t settings_unpack_gain_telem(uint8_t packed)
{
    uint8_t mode = packed & SETTINGS_RESERVED2_GAIN_TELEM_MASK;
    return (mode < (uint8_t)GAIN_TELEMETRY_COUNT) ? (gain_telemetry_mode_t)mode : GAIN_TELEMETRY_OFF;
}


comp_math_mode_t settings_unpack_comp_math(uint8_t packed)
{
    uint8_t mode = (packed & SETTINGS_RESERVED2_COMP_MATH_MASK) >> SETTINGS_RESERVED2_COMP_MATH_SHIFT;
    return (mode < (uint8_t)COMP_MATH_COUNT) ? (comp_math_mode_t)mode : COMP_MATH_LUT;
}


uint8_t settings_blob_equal(const settings_blob_t *a, const settings_blob_t *b)
{
    return (memcmp(a, b, sizeof(*a)) == 0) ? 1U : 0U;
}

static float phase_cal_clamp_value(float v, float limit)
{
    if (!isfinite(v)) return 0.0f;
    return clampf(v, -limit, limit);
}

void phase_cal_sanitize(phase_cal_t *cal)
{
    cal->differential_skew = phase_cal_clamp_value(cal->differential_skew,
                                                    PHASE_CAL_DIFFERENTIAL_LIMIT);
    for (uint32_t i = 0U; i < PHASE_CAL_BINS; i++) {
        cal->common_phase_pd[i] =
            phase_cal_clamp_value(cal->common_phase_pd[i], PHASE_CAL_COMMON_LIMIT);
    }
    cal->reserved[0] = 0.0f;
    cal->reserved[1] = 0.0f;
}

void phase_cal_legacy_sanitize(phase_cal_legacy_t *cal)
{
    cal->static_trim = phase_cal_clamp_value(cal->static_trim,
                                              PHASE_CAL_LEGACY_STATIC_LIMIT);
    cal->amp_trim_a = phase_cal_clamp_value(cal->amp_trim_a,
                                             PHASE_CAL_LEGACY_AMP_LIMIT);
    cal->amp_trim_c = phase_cal_clamp_value(cal->amp_trim_c,
                                             PHASE_CAL_LEGACY_AMP_LIMIT);
    for (uint32_t i = 0U; i < PHASE_CAL_BINS; i++) {
        cal->envelope_table[i] =
            phase_cal_clamp_value(cal->envelope_table[i],
                                  PHASE_CAL_LEGACY_ENVELOPE_LIMIT);
    }
}

int16_t settings_clamp_permille(int32_t v)
{
    if (v > (int32_t)PA_DUTY_PERMILLE_SCALE) v = (int32_t)PA_DUTY_PERMILLE_SCALE;
    if (v < -(int32_t)PA_DUTY_PERMILLE_SCALE) v = -(int32_t)PA_DUTY_PERMILLE_SCALE;
    return (int16_t)v;
}

pa_geometry_t settings_pa_geometry(const settings_blob_t *b)
{
    switch (b->reserved6[2]) {
        case 0U: /* A SWAP, and A INVERTED since */
        case 2U: /* C SWAP: the same output, turned */
            return PA_GEOMETRY_A_INVERTED;
        case 4U:                     /* EXPERT */
        case PA_WIRING_LEGACY_V633:  /* an RF180 flag on one branch inverted it */
            return (((b->reserved6[0] >> 2U) ^ (b->reserved6[0] >> 3U)) & 1U)
                 ? PA_GEOMETRY_A_INVERTED : PA_GEOMETRY_NORMAL;
        default: /* NORMAL, both swapped, and the mirrored maps */
            return PA_GEOMETRY_NORMAL;
    }
}

/* The records this image loads: its own (v28), v27 (v8.08, v8.09) and v26,
 * what v8.07 wrote. REV1 is a single board that has run v8.07, so the
 * converters for v1-v25 records went to make room (the owner's call,
 * 2026-10-01); an older record fails to load and the board starts from
 * defaults. v27 appended the four voice-receive bytes, which a v26 record
 * leaves zero -- each one's default (2700 Hz SSB, 10 kHz AM, MED AGC, squelch
 * off) -- and made the SD card the settings store, so a v26 record moves
 * there. The move happens in RAM; the card gets the settings at boot and the
 * flash record is not rewritten for it (MOD_LoadSettings). v28 appended the
 * power and SWR meter, whose zeros are its defaults likewise. v29 appended
 * the Morse trainer's preferences (zeros: none, its defaults) and moves the
 * keyer to its new defaults once -- Iambic B with Curtis 75/45, weight 50,
 * no auto space, no latency, 3 ms debounce -- keeping the operator's speed,
 * SWAP, pitch, filter, volume and protection. */
uint32_t storage_blob_length(uint32_t version)
{
    return version == 26UL ? SETTINGS_V26_LENGTH
         : version == 27UL ? SETTINGS_V27_LENGTH
         : version == 28UL ? SETTINGS_V28_LENGTH
         : (version == SETTINGS_VERSION ? (uint32_t)sizeof(settings_blob_t) : 0UL);
}

static uint32_t settings_blob_length(const settings_blob_t *b)
{
    return storage_blob_length(b->version);
}

uint8_t storage_blob_valid(const void *raw_blob)
{
    const settings_blob_t *b = (const settings_blob_t *)raw_blob;
    const uint32_t length = settings_blob_length(b);
    if (b->magic != SETTINGS_MAGIC || length == 0UL || b->length != length) return 0U;
    return (uint8_t)(b->crc32 == crc32_simple((const uint8_t *)raw_blob + 16U, length - 16U));
}

/* The conversions of a loaded blob, its raw bytes already in `out` and
 * everything past them zero. */
static void settings_blob_finish(settings_blob_t *out, uint32_t version)
{
    if (version < 29UL) {
        out->cw_keyer_mode = (uint8_t)CW_KEYER_IAMBIC_B;
        out->cw_curtis_b_dit_pct = 75.0f;
        out->cw_curtis_b_dah_pct = 45.0f;
        out->cw_weight = 50.0f;
        out->cw_autospace = 0U;
        out->cw_latency_eighths = 0U;
        out->cw_debounce_ms = 3.0f;
        memset(out->reserved7, 0, sizeof(out->reserved7));
        out->cw_mem_repeat_s = 0U;
        memset(out->reserved8, 0, sizeof(out->reserved8));
    }
    if (version < 27UL) {
        if (out->outphase_reserved[SETTINGS_BACKEND_IDX] != SETTINGS_BACKEND_SD)
            out->outphase_reserved[SETTINGS_FLAGS_IDX] |= SETTINGS_FLAG_FROM_FLASH;
        out->outphase_reserved[SETTINGS_BACKEND_IDX] = SETTINGS_BACKEND_SD;
    }
    out->version = SETTINGS_VERSION;
    out->length = sizeof(*out);
    phase_cal_legacy_sanitize(&out->cal_usb);
    phase_cal_legacy_sanitize(&out->cal_lsb);
    out->crc32 = crc32_simple((const uint8_t *)out + 16U, sizeof(*out) - 16U);
}

uint8_t settings_blob_load_current(const void *raw_blob, settings_blob_t *out)
{
    const settings_blob_t *b = (const settings_blob_t *)raw_blob;
    const uint32_t length = settings_blob_length(b);
    if (!storage_blob_valid(raw_blob)) return 0U;
    const uint32_t version = b->version;
    memset(out, 0, sizeof(*out));
    memcpy(out, raw_blob, length);
    settings_blob_finish(out, version);
    return 1U;
}

uint8_t storage_blob_load_in_place(settings_blob_t *b)
{
    if (!storage_blob_valid(b)) return 0U;
    const uint32_t length = settings_blob_length(b), version = b->version;
    memset((uint8_t *)b + length, 0, sizeof(*b) - length);
    settings_blob_finish(b, version);
    return 1U;
}

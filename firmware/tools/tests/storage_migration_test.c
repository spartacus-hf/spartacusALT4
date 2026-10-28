#include "mod_storage_format_priv.h"
#include "mod_crc32.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

static uint32_t blob_crc(const settings_blob_t *b, uint32_t length)
{
    return crc32_simple((const uint8_t *)&b->active_config, length - (uint32_t)offsetof(settings_blob_t, active_config));
}

/* A record as `version` wrote it: the current layout cut to that version's
 * length, every byte past it garbage (so a load that reads past the end, or
 * keeps what it should clear, shows). */
static void make_record(settings_blob_t *b, uint32_t version, uint32_t length)
{
    memset(b, 0xA5, sizeof(*b));
    memset(b, 0, length);
    b->magic = SETTINGS_MAGIC;
    b->version = version;
    b->length = length;
    b->freq_mhz = 14.0182f;
    b->cw_wpm = 22.0f;
    b->cw_semi_qsk_tail_dits = 7.0f;
    b->cw_paddle_reserved = (uint8_t)(2U << SETTINGS_CLOCK_POLICY_SHIFT);
    b->reserved6[2] = (uint8_t)PA_GEOMETRY_A_INVERTED;
    RADIO_ConfigDefaults(&b->rev1);
    b->rev1.ref_ppb = 7190;
    b->rev1.cw_pitch_10hz = 55U;
    b->rev1.cw_bw_10hz = 30U;
    b->rev1.cw_flags = RADIO_CW_AGC_FAST;
    if (length > SETTINGS_V26_LENGTH) {
        b->rev1.ssb_bw_100hz = 18U;
        b->rev1.am_bw_khz = 6U;
        b->rev1.rx_agc = RADIO_RX_AGC_FAST;
        b->rev1.fm_squelch = 3U;
    } else {
        memset((uint8_t *)b + length, 0xA5, sizeof(*b) - length);
    }
    b->crc32 = blob_crc(b, length);
}

static void check_common(const settings_blob_t *out)
{
    CHECK(out->magic == SETTINGS_MAGIC);
    CHECK(out->version == SETTINGS_VERSION);
    CHECK(out->length == sizeof(settings_blob_t));
    CHECK(out->crc32 == blob_crc(out, sizeof(*out)));
    CHECK(out->freq_mhz == 14.0182f && out->cw_wpm == 22.0f && out->cw_semi_qsk_tail_dits == 7.0f);
    CHECK(((out->cw_paddle_reserved & SETTINGS_CLOCK_POLICY_MASK) >> SETTINGS_CLOCK_POLICY_SHIFT) == 2U);
    CHECK(settings_pa_geometry(out) == PA_GEOMETRY_A_INVERTED);
    CHECK(RADIO_ConfigValid(&out->rev1));
    CHECK(out->rev1.ref_ppb == 7190);
    CHECK(RADIO_ConfigCwPitchHz(&out->rev1) == 550U);
    CHECK(RADIO_ConfigCwBandwidthHz(&out->rev1) == 300U);
    CHECK(RADIO_ConfigCwAgc(&out->rev1) == RADIO_CW_AGC_FAST);
}

/* v8.07's record (v26): kept whole, the voice-receive bytes it lacks read
 * as their defaults, and it moves to the SD card. */
static void test_v26(void)
{
    settings_blob_t old, out;
    make_record(&old, 26UL, SETTINGS_V26_LENGTH);
    CHECK(settings_blob_load_current(&old, &out) == 1U);
    check_common(&out);
    CHECK(out.rev1.ssb_bw_100hz == 0U && out.rev1.am_bw_khz == 0U);
    CHECK(out.rev1.rx_agc == 0U && out.rev1.fm_squelch == 0U);
    CHECK(RADIO_ConfigSsbBandwidthHz(&out.rev1) == RADIO_SSB_BW_DEFAULT_HZ);
    CHECK(RADIO_ConfigAmBandwidthHz(&out.rev1) == RADIO_AM_BW_DEFAULT_HZ);
    CHECK(RADIO_ConfigRxAgc(&out.rev1) == RADIO_RX_AGC_DEFAULT);
    CHECK(out.outphase_reserved[SETTINGS_BACKEND_IDX] == SETTINGS_BACKEND_SD);
    /* It was in the flash store: the card is written from it, not read. */
    CHECK(out.outphase_reserved[SETTINGS_FLAGS_IDX] & SETTINGS_FLAG_FROM_FLASH);
    old.crc32 ^= 1U;
    CHECK(settings_blob_load_current(&old, &out) == 0U);
    /* One that v8.07 kept on the card already: the card's copy is the newer. */
    make_record(&old, 26UL, SETTINGS_V26_LENGTH);
    old.outphase_reserved[SETTINGS_BACKEND_IDX] = SETTINGS_BACKEND_SD;
    old.crc32 = blob_crc(&old, SETTINGS_V26_LENGTH);
    CHECK(settings_blob_load_current(&old, &out) == 1U);
    CHECK(!(out.outphase_reserved[SETTINGS_FLAGS_IDX] & SETTINGS_FLAG_FROM_FLASH));
    /* A v26 record with the current length is not one v8.07 wrote. */
    make_record(&old, 26UL, sizeof(old));
    CHECK(settings_blob_load_current(&old, &out) == 0U);
}

/* v8.08 and v8.09's record (v27): kept whole; the power and SWR meter's
 * fields it lacks (v28) read as zero, each one's default, whatever lies past
 * its end. */
static void test_v27(void)
{
    settings_blob_t old, out;
    make_record(&old, 27UL, SETTINGS_V27_LENGTH);
    memset((uint8_t *)&old + SETTINGS_V27_LENGTH, 0xA5, sizeof(old) - SETTINGS_V27_LENGTH);
    CHECK(settings_blob_load_current(&old, &out) == 1U);
    check_common(&out);
    CHECK(RADIO_ConfigSsbBandwidthHz(&out.rev1) == 1800U && out.rev1.fm_squelch == 3U);
    CHECK(out.rev1.meter_flags == 0U && out.rev1.meter_update_10ms == 0U &&
          out.rev1.meter_hang_100ms == 0U && out.rev1.tune_pct == 0U);
    for (unsigned i = 0U; i < 12U; ++i) CHECK(out.rev1.meter_port_mv[i] == 0U);
    for (unsigned i = 0U; i < 10U; ++i)
        CHECK(out.rev1.meter_gain_x1000[i] == 0U && out.rev1.meter_leak_x10000[i] == 0U && out.rev1.meter_floor_100uv[i] == 0U);
    CHECK(RADIO_ConfigTxPanel(&out.rev1) && RADIO_ConfigMeterUpdateMs(&out.rev1) == RADIO_METER_UPDATE_DEF_MS &&
          RADIO_ConfigMeterHangMs(&out.rev1) == RADIO_METER_HANG_DEF_MS &&
          RADIO_ConfigTunePct(&out.rev1) == RADIO_TUNE_PCT_DEF);
    /* A v27 record with the current length is not one v8.08 wrote. */
    make_record(&old, 27UL, sizeof(old));
    CHECK(settings_blob_load_current(&old, &out) == 0U);
}

/* main's record through v8.10-dev (v28): kept whole, except the keyer,
 * which moves once to its new defaults -- Iambic B, Curtis 75/45, weight
 * 50, no auto space, no latency, 3 ms debounce -- and the Morse trainer's
 * preferences it lacks, which read as none (the trainer's defaults). The
 * operator's speed, SWAP, protection and receive settings stay. */
static void test_v28(void)
{
    settings_blob_t old, out;
    make_record(&old, 28UL, SETTINGS_V28_LENGTH);
    old.cw_keyer_mode = (uint8_t)CW_KEYER_IAMBIC_A;
    old.cw_curtis_b_dit_pct = 10.0f;
    old.cw_curtis_b_dah_pct = 20.0f;
    old.cw_weight = 60.0f;
    old.cw_autospace = 2U;
    old.cw_latency_eighths = 4U;
    old.cw_debounce_ms = 8.0f;
    old.cw_swap = 1U;
    old.cw_paddle_protect_mode = 2U;
    old.cw_both_policy = 1U;
    old.reserved7[0] = 3U;  /* the old trainer's mode */
    old.cw_mem_repeat_s = 3U; /* the old trainer's adapt */
    old.reserved8[2] = 2U;  /* and Koch order */
    old.rev1.tune_pct = 30U;
    memset((uint8_t *)&old + SETTINGS_V28_LENGTH, 0xA5, sizeof(old) - SETTINGS_V28_LENGTH);
    old.crc32 = blob_crc(&old, SETTINGS_V28_LENGTH);
    CHECK(settings_blob_load_current(&old, &out) == 1U);
    check_common(&out);
    CHECK(out.cw_keyer_mode == (uint8_t)CW_KEYER_IAMBIC_B);
    CHECK(out.cw_curtis_b_dit_pct == 75.0f && out.cw_curtis_b_dah_pct == 45.0f);
    CHECK(out.cw_weight == 50.0f && out.cw_autospace == 0U && out.cw_latency_eighths == 0U);
    CHECK(out.cw_debounce_ms == 3.0f);
    CHECK(out.cw_swap == 1U && out.cw_paddle_protect_mode == 2U && out.cw_both_policy == 1U);
    CHECK(RADIO_ConfigTunePct(&out.rev1) == 30U);
    CHECK(out.reserved7[0] == 0U && out.cw_mem_repeat_s == 0U && out.reserved8[2] == 0U);
    for (unsigned i = 0U; i < sizeof(out.cw_trainer); ++i) CHECK(out.cw_trainer[i] == 0U);
    /* A v28 record with the current length is not one v28 wrote. */
    make_record(&old, 28UL, sizeof(old));
    CHECK(settings_blob_load_current(&old, &out) == 0U);
}

/* The current record reads back as written, store marker included. */
static void test_current(void)
{
    settings_blob_t blob, out;
    make_record(&blob, SETTINGS_VERSION, sizeof(blob));
    blob.outphase_reserved[SETTINGS_BACKEND_IDX] = 0U;
    blob.cal_usb.static_trim = __builtin_nanf("");
    blob.rev1.tune_pct = 30U;
    blob.rev1.meter_leak_x10000[5] = 800U;
    /* The keyer as the operator set it, and the trainer's preferences: kept. */
    blob.cw_keyer_mode = (uint8_t)CW_KEYER_IAMBIC_A;
    blob.cw_curtis_b_dit_pct = 0.0f;
    blob.cw_latency_eighths = 4U;
    blob.cw_trainer[0] = SETTINGS_TRAINER_LAYOUT;
    blob.cw_trainer[5] = 21U;
    blob.crc32 = blob_crc(&blob, sizeof(blob));
    CHECK(settings_blob_load_current(&blob, &out) == 1U);
    check_common(&out);
    CHECK(RADIO_ConfigTunePct(&out.rev1) == 30U && out.rev1.meter_leak_x10000[5] == 800U);
    CHECK(out.cw_keyer_mode == (uint8_t)CW_KEYER_IAMBIC_A && out.cw_curtis_b_dit_pct == 0.0f &&
          out.cw_latency_eighths == 4U);
    CHECK(out.cw_trainer[0] == SETTINGS_TRAINER_LAYOUT && out.cw_trainer[5] == 21U);
    CHECK(out.cal_usb.static_trim == 0.0f);
    CHECK(RADIO_ConfigSsbBandwidthHz(&out.rev1) == 1800U);
    CHECK(RADIO_ConfigAmBandwidthHz(&out.rev1) == 6000U);
    CHECK(RADIO_ConfigRxAgc(&out.rev1) == RADIO_RX_AGC_FAST);
    CHECK(out.rev1.fm_squelch == 3U);
    CHECK(out.outphase_reserved[SETTINGS_BACKEND_IDX] == 0U);
    CHECK(!(out.outphase_reserved[SETTINGS_FLAGS_IDX] & SETTINGS_FLAG_FROM_FLASH));
    blob.crc32 ^= 1U;
    CHECK(settings_blob_load_current(&blob, &out) == 0U);
    blob.crc32 ^= 1U;
    blob.magic ^= 1U;
    CHECK(settings_blob_load_current(&blob, &out) == 0U);
    blob.magic = SETTINGS_MAGIC;
    blob.length -= 4U;
    CHECK(settings_blob_load_current(&blob, &out) == 0U);
}

/* Records from before v8.07 are not converted any more (REV1 is a single
 * board that has run v8.07): they fail to load and the board starts from
 * defaults rather than misread them. */
static void test_older_rejected(void)
{
    settings_blob_t blob, out;
    for (uint32_t version = 1UL; version <= SETTINGS_VERSION + 1UL; version++) {
        if (version == 26UL || version == 27UL || version == 28UL || version == SETTINGS_VERSION) continue;
        make_record(&blob, version, SETTINGS_V26_LENGTH);
        CHECK(settings_blob_load_current(&blob, &out) == 0U);
        make_record(&blob, version, sizeof(blob));
        CHECK(settings_blob_load_current(&blob, &out) == 0U);
    }
}

/* reserved6 through v8.05: [0] A/C sign (0x01/0x02) and RF180 (0x04/0x08)
 * flags, [2] the nine-entry PA WIRING MAP. Only a single inverted branch
 * survives as A INVERTED; mirrors, sign flags and double swaps are NORMAL. */
static void test_pa_geometry_from_wiring_map(void)
{
    static const struct { uint8_t map, flags; pa_geometry_t want; } cases[] = {
        {0U, 0x00U, PA_GEOMETRY_A_INVERTED}, /* A SWAP */
        {1U, 0x0FU, PA_GEOMETRY_NORMAL},     /* NORMAL ignores the flags */
        {2U, 0x00U, PA_GEOMETRY_A_INVERTED}, /* C SWAP */
        {3U, 0x00U, PA_GEOMETRY_NORMAL},     /* A+C SWAP */
        {4U, 0x04U, PA_GEOMETRY_A_INVERTED}, /* EXPERT, A RF180 */
        {4U, 0x08U, PA_GEOMETRY_A_INVERTED}, /* EXPERT, C RF180 */
        {4U, 0x0CU, PA_GEOMETRY_NORMAL},     /* EXPERT, both RF180 */
        {4U, 0x03U, PA_GEOMETRY_NORMAL},     /* EXPERT, sign flags only */
        {5U, 0x00U, PA_GEOMETRY_NORMAL},     /* A MIR, the old default */
        {6U, 0x00U, PA_GEOMETRY_NORMAL},     /* C MIR */
        {7U, 0x00U, PA_GEOMETRY_NORMAL},     /* A+C MIR */
        {PA_WIRING_LEGACY_V633, 0x05U, PA_GEOMETRY_A_INVERTED},
        {PA_WIRING_LEGACY_V633, 0x00U, PA_GEOMETRY_NORMAL},
        {200U, 0x04U, PA_GEOMETRY_NORMAL},
    };
    settings_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    for (uint32_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        blob.reserved6[0] = cases[i].flags;
        blob.reserved6[2] = cases[i].map;
        CHECK(settings_pa_geometry(&blob) == cases[i].want);
    }
    /* What this firmware writes reads back as itself. */
    blob.reserved6[0] = 0U;
    blob.reserved6[2] = (uint8_t)PA_GEOMETRY_A_INVERTED;
    CHECK(settings_pa_geometry(&blob) == PA_GEOMETRY_A_INVERTED);
    blob.reserved6[2] = (uint8_t)PA_GEOMETRY_NORMAL;
    CHECK(settings_pa_geometry(&blob) == PA_GEOMETRY_NORMAL);
    blob.reserved6[2] = (uint8_t)PA_GEOMETRY_DEFAULT;
    CHECK(settings_pa_geometry(&blob) == PA_GEOMETRY_NORMAL);
}

int main(void)
{
    static const uint8_t crc_vector[] = "123456789";
    CHECK(crc32_simple(crc_vector, 9U) == 0xCBF43926UL);
    CHECK(settings_unpack_gain_telem(settings_pack_aux_modes(
              GAIN_TELEMETRY_LIVE, COMP_MATH_LUT)) == GAIN_TELEMETRY_LIVE);
    CHECK(settings_unpack_comp_math(settings_pack_aux_modes(
              GAIN_TELEMETRY_LIVE, COMP_MATH_LUT)) == COMP_MATH_LUT);
    test_v26();
    test_v27();
    test_v28();
    test_current();
    test_older_rejected();
    test_pa_geometry_from_wiring_map();
    if (failures != 0U) return 1;
    puts("storage migrations v26-current: PASS");
    return 0;
}

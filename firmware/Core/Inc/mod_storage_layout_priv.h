#ifndef MOD_STORAGE_LAYOUT_PRIV_H
#define MOD_STORAGE_LAYOUT_PRIV_H

#include "mod_priv.h"
#include <stddef.h>
#include "radio_config.h"

/* Current flash settings and calibration record layout.
 * Do not reorder persisted fields or change these constants without an
 * explicit storage migration.
 */

#define SETTINGS_MAGIC      0xC31E1E01UL
/* v26: radio_config_t's three bytes after clock_flags became the CW receive
 * settings (cw_pitch_10hz, cw_bw_10hz, cw_flags); same length as v25.
 * v27: the voice-receive bytes. v28: the power and SWR meter (88 bytes).
 * v29: the Morse trainer's preferences (24 bytes); the keyer's new
 * defaults applied once to a v28 record (settings_blob_finish). */
#define SETTINGS_VERSION    29UL
#define SETTINGS_REC_MAGIC  0x53455432UL

/* V633, a v8.05 PA WIRING MAP value a stored reserved6[2] may still hold;
 * the load turns it into a pa_geometry_t (settings_pa_geometry). */
#define PA_WIRING_LEGACY_V633 8U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
    uint32_t crc32;
    /* --- payload starts here --- */
    mod_config_t  active_config;
    sideband_t    sideband;
    audio_profile_t profile;
    float    drive;
    float    gate;
    float    knee;
    float    freq_mhz;
    float    fm_dev;
    float    am_carrier;
    float    am_depth;
    uint8_t  mic_agc_enable;
    float    mic_agc_target;
    float    mic_agc_max_gain;
    uint8_t  noise_gate_enable;
    float    noise_gate_thresh;
    uint8_t  alc_enable;
    float    alc_ceiling;
    uint8_t  cessb_enable;
    float    cessb_clip;
    uint8_t  cessb_iter;
    uint8_t  comp_enable;
    float    comp_target;
    float    comp_ratio;
    uint8_t  eq_enable;
    float    eq_bass;
    float    eq_treble;
    uint16_t deadtime_ticks;
    uint16_t deadtime_rise_ticks;
    uint16_t deadtime_fall_ticks;
    int16_t  duty_offset_ticks;
    int16_t  branch_ac_offset_ticks;
    uint8_t  cordic_enable;
    uint8_t  reserved0;
    uint16_t reserved1;
    /* Archived v19 calibration payloads. They are converted into the typed
     * per-band CAL3 record and never applied directly. */
    phase_cal_legacy_t cal_usb;
    phase_cal_legacy_t cal_lsb;
    float    ssb_phase_bias;
    uint8_t  ssb_analytic_backend;
    uint8_t  ssb_ce_enable;
    uint8_t  theta_pd_enable;
    uint8_t  tx_bw_preset;
    float    ssb_ce_mix;
    float    ssb_ce_floor;
    float    ssb_ce_knee;
    float    ssb_low_cut;
    uint8_t  outphase_law;
    uint8_t  reserved2[3];
    float    rf_en_assert_metric;
    float    rf_en_release_metric;
    uint16_t rf_en_hold_frames;
    uint8_t  usb_clean_mode;
    uint8_t  usb_channel_mode;
    uint8_t  dsp_math_mode;
    uint8_t  rf_metric_mode;
    uint8_t  cw_keyer_mode;
    uint8_t  cw_breakin_mode;
    uint8_t  cw_swap;
    uint8_t  cw_autospace;
    float    cw_wpm;
    float    cw_weight;
    float    cw_semi_qsk_tail_dits;
    float    cw_ramp_ms;
    float    cw_drive;
    uint8_t  cw_practice;
    uint8_t  reserved4[3];
    float    cw_debounce_ms;
    uint8_t  cw_paddle_protect_mode;
    uint8_t  cw_both_policy;
    uint8_t  tx_test_both_policy;
    /* bit0: CPU diagnostics; bits1-2: carrier clock policy;
     * bits3-5: GPIO CPU scope mode; bit6: SSB strict-QMX12 pipeline. */
    uint8_t  cw_paddle_reserved;
    float    cw_paddle_arm_open_ms;
    float    cw_paddle_arm_press_ms;
    float    cw_paddle_arm_release_ms;
    float    cw_paddle_stuck_ms;
    float    tx_test_paddle_stuck_ms;
    pa_band_profile_t pa_legacy_profile;
    pa_band_profile_t pa_band_profiles[BAND_COUNT];
    /* Through v28 the old trainer's mode, source, group length, Koch lesson
     * and repeat limit; written 0 since v29 (cw_trainer). */
    uint8_t  reserved7[5];
    /* reserved5[0]: cw_theta_model_t; reserved5[1]: bit0 USB VOX, bit1 DIGI
     * TONE off, bit2 REF SOURCE LOCAL; reserved5[2]: unused. */
    uint8_t  reserved5[3];
    float    cw_curtis_b_dit_pct;
    float    cw_curtis_b_dah_pct;
    uint8_t  cw_latency_eighths;
    /* Through v28 the old trainer's adapt, done, stop-after and Koch order.
     * v29: the radio memories' repeat seconds (0 once), the rest written 0. */
    uint8_t  cw_mem_repeat_s;
    uint8_t  reserved8[3];
    /* reserved6[2]: pa_geometry_t. Through v8.05 it was the PA WIRING MAP, with
     * the A/C sign and RF180 flags in [0] and a sign scope in [1]; those are
     * written 0 now, and a load converts old maps (settings_pa_geometry). */
    uint8_t  reserved6[3];
    float    outphase_ssb_max_theta;
    float    outphase_am_max_theta;
    float    outphase_fm_theta;
    float    outphase_cw_theta;
    float    outphase_test_theta;
    /* outphase_reserved[0]: mod_outphase_fallback_t;
     * [1]: mod_storage_backend_t boot marker; [2]: storage flags. */
    uint8_t  outphase_reserved[4];
    float    usb_input_gain;
    float    fm_input_gain;
    uint8_t  usb_audio_policy;
    uint8_t  fm_deviation_preset;
    uint8_t  fm_test_tone;
    uint8_t  am_profile;
    uint8_t  sd_browser_view;
    uint8_t  sd_browser_sort;
    uint8_t  sd_browser_filter;
    /* bit0 descending, bit1 folders first, bit2 show hidden. */
    uint8_t  sd_browser_flags;
    /* bit0 ask, bit1 advance, bit2 repeat, bit3 shuffle, bit4 clean. */
    uint8_t  sd_player_flags;
    uint8_t  sd_player_gain_percent;
    uint8_t  sd_player_seek_seconds;
    uint8_t  sd_player_downmix;
    uint8_t  sd_player_rf_mode;
    uint8_t  sd_reserved[3];
    radio_config_t rev1;
    /* v29: the Morse trainer's preferences, mtrain_prefs_t from byte 1;
     * cw_trainer[0] its layout, 0 for none (a v28 record): the defaults. */
    uint8_t  cw_trainer[24];
} settings_blob_t;

#define SETTINGS_V20_LENGTH ((uint32_t)offsetof(settings_blob_t, rev1))
_Static_assert(SETTINGS_V20_LENGTH == 864U, "prototype settings prefix must not change");

/* v26 (v8.07) ends after cw_flags; v27 (v8.08, v8.09) appended the four
 * voice-receive bytes, v28 the meter. Older records are no longer loaded
 * (settings_blob_load_current). */
#define SETTINGS_V26_LENGTH (SETTINGS_V20_LENGTH + (uint32_t)offsetof(radio_config_t, ssb_bw_100hz))
_Static_assert(SETTINGS_V26_LENGTH == 1008U, "v26 settings length must not change");
#define SETTINGS_V27_LENGTH (SETTINGS_V20_LENGTH + (uint32_t)offsetof(radio_config_t, meter_flags))
_Static_assert(SETTINGS_V27_LENGTH == 1012U, "v27 settings length must not change");
#define SETTINGS_V28_LENGTH ((uint32_t)offsetof(settings_blob_t, cw_trainer))
_Static_assert(SETTINGS_V28_LENGTH == 1100U, "v28 settings length must not change");
#define SETTINGS_TRAINER_LAYOUT 1U


typedef struct {
    uint32_t rec_magic;
    uint32_t seq;
    settings_blob_t blob;
} settings_record_t;

/* cw_paddle_reserved bits 1..2: the carrier clock policy (mod_clock_policy_t). */
#define SETTINGS_CLOCK_POLICY_SHIFT  1U
/* outphase_reserved[SETTINGS_BACKEND_IDX]: mod_storage_backend_t, the store
 * the settings live in (MOD_STORAGE_FLASH 0, MOD_STORAGE_SD 1). */
#define SETTINGS_BACKEND_IDX         1U
#define SETTINGS_BACKEND_SD          1U
/* outphase_reserved[SETTINGS_FLAGS_IDX], set by the load only, never stored:
 * this record lived in the flash store and was moved to the card in RAM
 * (settings_blob_load_current), so whatever settings file the card holds is
 * older than it -- the card is written from it, not read over it. */
#define SETTINGS_FLAGS_IDX           2U
#define SETTINGS_FLAG_FROM_FLASH     0x80U
#define SETTINGS_CLOCK_POLICY_MASK   0x06U

#define SETTINGS_RESERVED2_GAIN_TELEM_MASK   0x03U
#define SETTINGS_RESERVED2_COMP_MATH_MASK    0x0CU
#define SETTINGS_RESERVED2_COMP_MATH_SHIFT   2U


#define CAL_REC_MAGIC               0x43414C34UL  /* "CAL4": REV1 hardware only */

typedef struct {
    uint32_t rec_magic;
    uint32_t seq;
    uint16_t band_idx;
    uint8_t  sideband;
    uint8_t  analytic_backend;
    phase_cal_t phase_cal;
    int16_t  branch_ac_offset_ticks;
    uint16_t flags;
    float    theta_linearity_pd[PHASE_CAL_BINS];
    uint32_t crc32;
} ssb_cal_record_t;

_Static_assert(sizeof(ssb_cal_record_t) == 288U,
               "v8.08's flash and card records are 288 bytes: the import reads them so");


#endif /* MOD_STORAGE_LAYOUT_PRIV_H */

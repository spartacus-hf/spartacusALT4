#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_storage_format_priv.h"
#include "mod_crc32.h"
#include "mod_sd_store.h"
#include "mod_cal_storage_priv.h"
#include "mod_storage_store_priv.h"
#include "mod_morse_trainer.h"
#include "mod_sdcard_service.h"
#include "sd_audio_player.h"
#include "sd_file_browser.h"
#include "mod_cessb_policy.h"
#include "radio_clocks.h"
#include "mod_ssb_envelope_logic.h"
#include "radio.h"
#include "radio_audio.h"
#include "radio_pd.h"

#define STORAGE_BACKEND_RESERVED_IDX SETTINGS_BACKEND_IDX
#define STORAGE_FLAGS_RESERVED_IDX   2U
#define SETTINGS_CPU_DIAG_BIT        0x01U
#define SETTINGS_CPU_SCOPE_SHIFT     3U
#define SETTINGS_CPU_SCOPE_MASK      0x38U
#define SETTINGS_SSB_PIPELINE_STRICT 0x40U

static uint8_t storage_blocking_io_allowed(void)
{
    return MOD_BlockingIoAllowed(HAL_GetTick());
}

/* ═══════════════ Settings in the SD card's store ═══════════════
 *
 * Since v8.09 the settings are slot 0 of the store in the card's system
 * area (mod_sd_store.c): a settings_record_t whose blob is checked by
 * settings_blob_load_current as the flash records were. The store keeps the
 * previous version until the new one has read back, so a save cut short by
 * a power loss loses only itself. The MCU's flash holds no settings or
 * calibration any more: its 44 KiB went to the application.
 */

static void settings_snapshot(settings_blob_t *b)
{
    sd_browser_config_t browser;
    sd_audio_player_config_t player;
    memset(b, 0, sizeof(*b));
    b->magic   = SETTINGS_MAGIC;
    b->version = SETTINGS_VERSION;
    b->length  = sizeof(*b);
    b->active_config     = ((mod_config_t)requested_config == MOD_CFG_TX_CARRIER_TEST)
                       ? MOD_CFG_PROD_SSB_14MHZ
                       : (mod_config_t)requested_config;
    b->sideband          = active_sideband_enum;
    b->profile           = active_audio_profile;
    b->drive             = cfg_drive;
    b->gate              = cfg_gate;
    b->knee              = cfg_knee; /* DEPRECATED: retained for settings compatibility */
    b->freq_mhz          = MOD_GetCarrierFreqMHz();
    b->fm_dev            = cfg_fm_dev;
    b->am_carrier        = cfg_am_carrier;
    b->am_depth          = cfg_am_depth;
    b->mic_agc_enable    = cfg_mic_agc_enable;
    b->mic_agc_target    = cfg_mic_agc_target;
    b->mic_agc_max_gain  = cfg_mic_agc_max_gain;
    b->noise_gate_enable = cfg_ng_enable;
    b->noise_gate_thresh = cfg_ng_thresh;
    b->alc_enable        = cfg_alc_enable;
    b->alc_ceiling       = cfg_alc_ceiling;
    b->cessb_enable      = cfg_cessb_enable;
    b->cessb_clip        = cfg_cessb_clip;
    b->cessb_iter        = cfg_cessb_iter;
    b->comp_enable       = cfg_comp_enable;
    b->comp_target       = cfg_comp_target;
    b->comp_ratio        = cfg_comp_ratio;
    b->eq_enable         = cfg_eq_enable;
    b->eq_bass           = cfg_eq_bass;
    b->eq_treble         = cfg_eq_treble;
    b->deadtime_ticks    = MOD_GetDeadTimeTicks();
    b->deadtime_rise_ticks = cfg_deadtime_rise_ticks;
    b->deadtime_fall_ticks = cfg_deadtime_fall_ticks;
    b->duty_offset_ticks = cfg_duty_offset_ticks;
    b->cordic_enable     = cfg_cordic_enable;
    b->reserved0         = (uint8_t)cfg_ui_home_layout;
    b->reserved1         = (uint16_t)(((uint16_t)((uint8_t)cfg_ui_shortcut_up) << 8) | (uint16_t)(cfg_ui_home_flags & UI_HOME_FLAGS_DEFAULT));
    b->ssb_phase_bias    = cfg_ssb_phase_bias;
    b->ssb_analytic_backend = (uint8_t)cfg_ssb_analytic_backend;
    b->ssb_ce_enable     = cfg_ssb_ce_enable;
    b->theta_pd_enable   = 0U; /* retired v19 calibration-enable byte */
    b->tx_bw_preset      = (uint8_t)cfg_tx_bw_preset;
    b->ssb_ce_mix        = cfg_ssb_ce_mix;
    b->ssb_ce_floor      = cfg_ssb_ce_floor;
    b->ssb_ce_knee       = cfg_ssb_ce_knee;
    b->ssb_low_cut       = cfg_ssb_low_cut;
    b->outphase_law      = (uint8_t)cfg_outphase_law;
    b->reserved2[0] = (uint8_t)cfg_ui_shortcut_down;
    b->reserved2[1] = cfg_runtime_diag_enable ? 1U : 0U;
    b->reserved2[2] = settings_pack_aux_modes(cfg_gain_telemetry_mode, cfg_comp_math_mode);
    b->rf_en_assert_metric = cfg_rf_en_assert_metric;
    b->rf_en_release_metric = cfg_rf_en_release_metric;
    b->rf_en_hold_frames = cfg_rf_en_hold_frames;
    b->usb_clean_mode = cfg_usb_clean_mode ? 1U : 0U;
    b->usb_channel_mode = (uint8_t)cfg_usb_channel_mode;
    b->dsp_math_mode = (uint8_t)cfg_dsp_math_mode;
    b->rf_metric_mode = (uint8_t)cfg_rf_metric_mode;
    b->cw_keyer_mode = (uint8_t)cfg_cw_keyer_mode;
    b->cw_breakin_mode = (uint8_t)cfg_cw_breakin_mode;
    b->cw_swap = cfg_cw_swap ? 1U : 0U;
    b->cw_autospace = MOD_GetCwAutoSpaceDits();
    b->cw_wpm = cfg_cw_wpm;
    b->cw_weight = cfg_cw_weight;
    b->cw_semi_qsk_tail_dits = cfg_cw_semi_qsk_tail_dits;
    b->cw_ramp_ms = cfg_cw_ramp_ms;
    b->cw_drive = cfg_cw_drive;
    b->cw_practice = cfg_cw_practice ? 1U : 0U;
    memset(b->reserved4, 0, sizeof(b->reserved4));
    b->reserved4[0] = 0U; /* was the CEU6 SEL pull-up; buttons_high owns polarity */
    {
        int16_t theta_milli = (int16_t)(cfg_cw_theta_trim * 1000.0f + (cfg_cw_theta_trim >= 0.0f ? 0.5f : -0.5f));
        b->reserved4[1] = (uint8_t)(theta_milli & 0xFF);
        b->reserved4[2] = (uint8_t)(((uint16_t)theta_milli >> 8) & 0xFF);
    }
    b->cw_debounce_ms = cfg_cw_debounce_ms;
    b->cw_paddle_protect_mode = (uint8_t)cfg_cw_paddle_protect_mode;
    b->cw_both_policy = (uint8_t)cfg_cw_both_policy;
    b->tx_test_both_policy = (uint8_t)cfg_tx_test_both_policy;
    b->cw_paddle_reserved = (uint8_t)((cfg_cpu_diag_enable ? SETTINGS_CPU_DIAG_BIT : 0U)
                         | ((((uint8_t)MOD_GetCarrierClockPolicy()) & 0x03U) << SETTINGS_CLOCK_POLICY_SHIFT)
                         | ((((uint8_t)MOD_GetCpuScopeMode()) & 0x07U) << SETTINGS_CPU_SCOPE_SHIFT)
                         | ((MOD_GetSsbPipelineMode() == SSB_PIPELINE_STRICT_QMX12_EXP)
                            ? SETTINGS_SSB_PIPELINE_STRICT : 0U));
    b->cw_paddle_arm_open_ms = cfg_cw_paddle_arm_open_ms;
    b->cw_paddle_arm_press_ms = cfg_cw_paddle_arm_press_ms;
    b->cw_paddle_arm_release_ms = cfg_cw_paddle_arm_release_ms;
    b->cw_paddle_stuck_ms = cfg_cw_paddle_stuck_ms;
    b->tx_test_paddle_stuck_ms = cfg_tx_test_paddle_stuck_ms;
    memset(b->reserved7, 0, sizeof(b->reserved7));
    memset(b->reserved5, 0, sizeof(b->reserved5));
    b->reserved5[0] = (uint8_t)MOD_GetCwThetaModel();
    /* bit0 USB VOX, bit1 DIGI TONE off: zero is QMX's way, VOX OFF, TONE ON.
     * bit2 REF SOURCE LOCAL: zero follows the USB host, as before v8.10. */
    b->reserved5[1] = (uint8_t)((MOD_GetUsbVox() ? 1U : 0U) | (MOD_GetDigiTone() ? 0U : 2U) |
                                (RADIO_RefFollowEnabled() ? 0U : 4U));
    b->cw_curtis_b_dit_pct = cfg_cw_curtis_b_dit_pct;
    b->cw_curtis_b_dah_pct = cfg_cw_curtis_b_dah_pct;
    b->cw_latency_eighths = cfg_cw_latency_eighths;
    b->cw_mem_repeat_s = cfg_cw_mem_repeat_s;
    memset(b->reserved8, 0, sizeof(b->reserved8));
    memset(b->cw_trainer, 0, sizeof(b->cw_trainer));
    b->cw_trainer[0] = SETTINGS_TRAINER_LAYOUT;
    memcpy(&b->cw_trainer[1], MTRAIN_Prefs(), sizeof(mtrain_prefs_t));
    memset(b->reserved6, 0, sizeof(b->reserved6));
    b->reserved6[2] = (uint8_t)MOD_GetPaGeometry();
    b->outphase_ssb_max_theta = cfg_outphase_ssb_max_theta;
    b->outphase_am_max_theta = cfg_outphase_am_max_theta;
    b->outphase_fm_theta = cfg_outphase_fm_theta;
    b->outphase_cw_theta = cfg_outphase_cw_theta;
    b->outphase_test_theta = cfg_outphase_test_theta;
    memset(b->outphase_reserved, 0, sizeof(b->outphase_reserved));
    b->outphase_reserved[0] = (uint8_t)MOD_GetOutphaseFallbackPolicy();
    b->outphase_reserved[STORAGE_BACKEND_RESERVED_IDX] = (uint8_t)cfg_storage_backend;
    b->outphase_reserved[STORAGE_FLAGS_RESERVED_IDX] = 0U; /* card presence and v8.08's store flags are not settings */
    b->usb_input_gain = cfg_usb_input_gain;
    b->fm_input_gain = cfg_fm_input_gain;
    b->usb_audio_policy = (uint8_t)MOD_GetUsbAudioPolicy();
    b->fm_deviation_preset = (uint8_t)MOD_GetFmDeviationPreset();
    b->fm_test_tone = cfg_fm_test_tone ? 1U : 0U;
    b->am_profile = (uint8_t)MOD_GetAmProfile();
    SD_BrowserGetConfig(&browser);
    SD_AudioPlayerGetConfig(&player);
    b->sd_browser_view = (uint8_t)browser.view;
    b->sd_browser_sort = (uint8_t)browser.sort;
    b->sd_browser_filter = (uint8_t)browser.filter;
    b->sd_browser_flags = (uint8_t)((browser.descending ? 0x01U : 0U) |
                                    (browser.folders_first ? 0x02U : 0U) |
                                    (browser.show_hidden ? 0x04U : 0U));
    b->sd_player_flags = (uint8_t)((player.ask_each_track ? 0x01U : 0U) |
                                   (player.advance ? 0x02U : 0U) |
                                   (player.repeat ? 0x04U : 0U) |
                                   (player.shuffle ? 0x08U : 0U) |
                                   (player.clean ? 0x10U : 0U));
    b->sd_player_gain_percent = player.gain_percent;
    b->sd_player_seek_seconds = player.seek_seconds;
    b->sd_player_downmix = (uint8_t)player.downmix;
    b->sd_player_rf_mode = (uint8_t)SD_AudioPlayerGetRfMode();
    /* cal_usb, cal_lsb and branch_ac_offset_ticks are the pre-typed
     * calibration's fields: nothing has read them since the typed
     * calibration domains, and a record now carries them as zeros (the
     * memset above) instead of round-tripping them through RAM. */
    b->pa_legacy_profile = pa_legacy_profile;
    memcpy(b->pa_band_profiles, pa_band_profiles, sizeof(b->pa_band_profiles));

    RADIO_GetConfig(&b->rev1);
    b->crc32 = crc32_simple((const uint8_t *)&b->active_config,
                             sizeof(*b) - offsetof(settings_blob_t, active_config));
}

static void settings_apply(const settings_blob_t *b)
{
    RADIO_SetConfig(&b->rev1);
    sd_browser_config_t browser;
    sd_audio_player_config_t player;
    /* Start from the stored profile defaults, then apply every persisted
     * runtime field on top so the restored values exactly match what was
     * saved, including CUSTOM combinations. */
    apply_audio_profile(b->profile);

    active_sideband_enum = (b->sideband == SIDEBAND_LSB) ? SIDEBAND_LSB : SIDEBAND_USB;
    active_sideband_sign = (active_sideband_enum == SIDEBAND_LSB) ? 1.0f : -1.0f;

    cfg_drive             = b->drive;
    cfg_gate              = b->gate;
    cfg_knee              = b->knee; /* DEPRECATED: retained for settings compatibility */
    cfg_fm_dev            = b->fm_dev;
    cfg_am_carrier        = b->am_carrier;
    cfg_am_depth          = b->am_depth;
    cfg_mic_agc_enable    = b->mic_agc_enable ? 1U : 0U;
    cfg_mic_agc_target    = b->mic_agc_target;
    cfg_mic_agc_max_gain  = b->mic_agc_max_gain;
    cfg_ng_enable         = b->noise_gate_enable ? 1U : 0U;
    cfg_ng_thresh         = b->noise_gate_thresh;
    cfg_alc_enable        = b->alc_enable ? 1U : 0U;
    cfg_alc_ceiling       = b->alc_ceiling;
    cfg_cessb_enable      = mod_cessb_enable_for_build(b->cessb_enable);
    cfg_cessb_clip        = b->cessb_clip;
    cfg_cessb_iter        = (b->cessb_iter > CESSB_ITER_MAX) ? CESSB_ITER_MAX : b->cessb_iter;
    cfg_comp_enable       = b->comp_enable ? 1U : 0U;
    cfg_comp_target       = b->comp_target;
    cfg_comp_ratio        = b->comp_ratio;
    cfg_eq_enable         = b->eq_enable ? 1U : 0U;
    cfg_eq_bass           = b->eq_bass;
    cfg_eq_treble         = b->eq_treble;
#if MOD_DEBUG_SWITCHES
    cfg_cordic_enable     = b->cordic_enable ? 1U : 0U;
#endif
    cfg_ssb_phase_bias    = mod_ssb_phase_bias_for_build(b->ssb_phase_bias);
    cfg_ssb_analytic_backend = ((uint32_t)b->ssb_analytic_backend < (uint32_t)SSB_ANALYTIC_COUNT)
                             ? (ssb_analytic_backend_t)b->ssb_analytic_backend
                             : SSB_ANALYTIC_WEAVER;
    cfg_ssb_ce_enable     = mod_ssb_ce_enable_for_build(b->ssb_ce_enable);
    cfg_tx_bw_preset      = ((uint32_t)b->tx_bw_preset < (uint32_t)TX_BW_COUNT)
                          ? (tx_bw_preset_t)b->tx_bw_preset
                          : TX_BW_200_2900;
    cfg_ssb_ce_mix        = b->ssb_ce_mix;
    cfg_ssb_ce_floor      = b->ssb_ce_floor;
    cfg_ssb_ce_knee       = b->ssb_ce_knee;
    cfg_ssb_low_cut       = b->ssb_low_cut;
    cfg_outphase_law      = ((uint32_t)b->outphase_law < (uint32_t)OUTPHASE_LAW_COUNT)
                          ? (outphase_law_t)b->outphase_law
                          : OUTPHASE_LAW_CLASSIC;
    cfg_rf_en_assert_metric = b->rf_en_assert_metric;
    cfg_rf_en_release_metric = b->rf_en_release_metric;
    cfg_rf_en_hold_frames = (b->rf_en_hold_frames > 255U) ? 255U : (uint8_t)b->rf_en_hold_frames;
    if ((cfg_rf_en_hold_frames == 2U) &&
        (cfg_rf_en_assert_metric >= (RF_EN_GPIO_ASSERT_METRIC_LEGACY - 0.0005f)) &&
        (cfg_rf_en_assert_metric <= (RF_EN_GPIO_ASSERT_METRIC_LEGACY + 0.0005f)) &&
        (cfg_rf_en_release_metric >= (RF_EN_GPIO_RELEASE_METRIC_LEGACY - 0.0005f)) &&
        (cfg_rf_en_release_metric <= (RF_EN_GPIO_RELEASE_METRIC_LEGACY + 0.0005f))) {
        cfg_rf_en_assert_metric = RF_EN_GPIO_ASSERT_METRIC_DEFAULT;
        cfg_rf_en_release_metric = RF_EN_GPIO_RELEASE_METRIC_DEFAULT;
    }
    cfg_usb_clean_mode = ((uint32_t)b->usb_audio_policy < (uint32_t)USB_AUDIO_POLICY_COUNT)
                       ? b->usb_audio_policy
                       : (b->usb_clean_mode ? (uint8_t)USB_AUDIO_POLICY_DIGI_CLEAN
                                            : (uint8_t)USB_AUDIO_POLICY_VOICE_PROC);
    cfg_usb_channel_mode = ((uint32_t)b->usb_channel_mode < (uint32_t)USB_AUDIO_CHAN_COUNT)
                         ? (usb_audio_channel_mode_t)b->usb_channel_mode
                         : USB_AUDIO_CHAN_AVERAGE;
#if MOD_DEBUG_SWITCHES
    cfg_dsp_math_mode = ((uint32_t)b->dsp_math_mode < (uint32_t)DSP_MATH_COUNT)
                      ? (dsp_math_mode_t)b->dsp_math_mode
                      : DSP_MATH_SAFE;
    cfg_gain_telemetry_mode = settings_unpack_gain_telem(b->reserved2[2]);
    cfg_comp_math_mode = settings_unpack_comp_math(b->reserved2[2]);
    cfg_rf_metric_mode = ((uint32_t)b->rf_metric_mode < (uint32_t)RF_METRIC_COUNT)
                       ? (rf_metric_mode_t)b->rf_metric_mode
                       : RF_METRIC_EXACT;
#endif
    cfg_cw_keyer_mode = ((uint32_t)b->cw_keyer_mode < (uint32_t)CW_KEYER_COUNT)
                      ? (cw_keyer_mode_t)b->cw_keyer_mode
                      : CW_DEFAULT_KEYER_MODE;
    cfg_cw_breakin_mode = ((uint32_t)b->cw_breakin_mode < (uint32_t)CW_BREAKIN_COUNT)
                        ? (cw_breakin_mode_t)b->cw_breakin_mode
                        : CW_DEFAULT_BREAKIN;
    cfg_cw_swap = b->cw_swap ? 1U : 0U;
    cfg_cw_autospace = ((b->cw_autospace == 0U) || (b->cw_autospace == 1U) || (b->cw_autospace == 2U) || (b->cw_autospace == 3U) || (b->cw_autospace == 4U)) ? b->cw_autospace : 0U;
    cfg_cw_wpm = b->cw_wpm;
    cfg_cw_weight = b->cw_weight;
    cfg_cw_semi_qsk_tail_dits = b->cw_semi_qsk_tail_dits;
    cfg_cw_ramp_ms = b->cw_ramp_ms;
    cfg_cw_drive = b->cw_drive;
    cfg_cw_practice = b->cw_practice ? 1U : 0U;
    cfg_cw_theta_trim = (float)(int16_t)(((uint16_t)b->reserved4[1]) | ((uint16_t)b->reserved4[2] << 8)) / 1000.0f;
    cfg_cw_debounce_ms = b->cw_debounce_ms;
    cfg_cw_curtis_b_dit_pct = b->cw_curtis_b_dit_pct;
    cfg_cw_curtis_b_dah_pct = b->cw_curtis_b_dah_pct;
    cfg_cw_latency_eighths = b->cw_latency_eighths;
    cfg_cw_mem_repeat_s = (b->cw_mem_repeat_s <= 60U) ? b->cw_mem_repeat_s : 0U;
    cfg_cw_paddle_protect_mode = ((uint32_t)b->cw_paddle_protect_mode < (uint32_t)CW_PADDLE_PROTECT_COUNT)
                               ? (cw_paddle_protect_mode_t)b->cw_paddle_protect_mode
                               : CW_DEFAULT_PADDLE_PROTECT_MODE;
    cfg_cw_both_policy = ((uint32_t)b->cw_both_policy < (uint32_t)CW_BOTH_COUNT)
                       ? (cw_both_policy_t)b->cw_both_policy
                       : CW_DEFAULT_BOTH_POLICY;
    cfg_tx_test_both_policy = ((uint32_t)b->tx_test_both_policy < (uint32_t)TXTEST_BOTH_COUNT)
                            ? (tx_test_both_policy_t)b->tx_test_both_policy
                            : TXTEST_DEFAULT_BOTH_POLICY;
    cfg_cw_theta_model = ((uint32_t)b->reserved5[0] < (uint32_t)CW_THETA_MODEL_COUNT)
                       ? (cw_theta_model_t)b->reserved5[0]
                       : CW_THETA_MODEL_DEFAULT;
    MOD_SetUsbVox((uint8_t)(b->reserved5[1] & 1U));
    MOD_SetDigiTone((uint8_t)((b->reserved5[1] & 2U) == 0U));
    RADIO_RefFollowEnable((uint8_t)((b->reserved5[1] & 4U) == 0U));
    cfg_cw_paddle_arm_open_ms = b->cw_paddle_arm_open_ms;
    cfg_cw_paddle_arm_press_ms = b->cw_paddle_arm_press_ms;
    cfg_cw_paddle_arm_release_ms = b->cw_paddle_arm_release_ms;
    cfg_cw_paddle_stuck_ms = b->cw_paddle_stuck_ms;
    cfg_tx_test_paddle_stuck_ms = b->tx_test_paddle_stuck_ms;
    /* The trainer's preferences, or its defaults (a v28 record, or a layout
     * this image does not know). MTRAIN_PrefsChanged puts each in range. */
    if (b->cw_trainer[0] == SETTINGS_TRAINER_LAYOUT) memcpy(MTRAIN_Prefs(), &b->cw_trainer[1], sizeof(mtrain_prefs_t));
    else MTRAIN_DefaultPrefs(MTRAIN_Prefs());
    MTRAIN_PrefsChanged();
    MOD_SetPaGeometry(settings_pa_geometry(b));
    cfg_outphase_ssb_max_theta = b->outphase_ssb_max_theta;
    cfg_outphase_am_max_theta = b->outphase_am_max_theta;
    cfg_outphase_fm_theta = b->outphase_fm_theta;
    cfg_outphase_cw_theta = b->outphase_cw_theta;
    cfg_outphase_test_theta = b->outphase_test_theta;
    cfg_outphase_fallback_policy =
        ((uint32_t)b->outphase_reserved[0] < (uint32_t)MOD_OUTPHASE_FALLBACK_COUNT)
        ? (mod_outphase_fallback_t)b->outphase_reserved[0]
        : MOD_OUTPHASE_FALLBACK_COMPRESS;
    /* The store byte said SD or flash before v8.09; the card is the only
     * store now, whatever an older record says. */
    cfg_storage_backend = MOD_STORAGE_SD;
    cfg_usb_input_gain = b->usb_input_gain;
    if (!(cfg_usb_input_gain == cfg_usb_input_gain) || (cfg_usb_input_gain < 0.05f) || (cfg_usb_input_gain > 8.0f)) {
        cfg_usb_input_gain = 1.0f;
    }
    cfg_fm_input_gain = b->fm_input_gain;
    if (!(cfg_fm_input_gain == cfg_fm_input_gain) || (cfg_fm_input_gain < 0.05f) || (cfg_fm_input_gain > 8.0f)) {
        cfg_fm_input_gain = 1.0f;
    }
    cfg_fm_deviation_preset = ((uint32_t)b->fm_deviation_preset < (uint32_t)FM_DEVIATION_PRESET_COUNT)
                            ? (fm_deviation_preset_t)b->fm_deviation_preset
                            : FM_DEVIATION_PRESET_USER;
    cfg_fm_test_tone = b->fm_test_tone ? 1U : 0U;
    cfg_am_profile = ((uint32_t)b->am_profile < (uint32_t)AM_PROFILE_COUNT)
                   ? (am_profile_t)b->am_profile
                   : AM_PROFILE_EFFICIENT;
    browser.view = (sd_browser_view_t)b->sd_browser_view;
    browser.sort = (sd_browser_sort_t)b->sd_browser_sort;
    browser.filter = (sd_browser_filter_t)b->sd_browser_filter;
    browser.descending = (b->sd_browser_flags & 0x01U) ? 1U : 0U;
    browser.folders_first = (b->sd_browser_flags & 0x02U) ? 1U : 0U;
    browser.show_hidden = (b->sd_browser_flags & 0x04U) ? 1U : 0U;
    SD_BrowserSetConfig(&browser);
    player.ask_each_track = (b->sd_player_flags & 0x01U) ? 1U : 0U;
    player.advance = (b->sd_player_flags & 0x02U) ? 1U : 0U;
    player.repeat = (b->sd_player_flags & 0x04U) ? 1U : 0U;
    player.shuffle = (b->sd_player_flags & 0x08U) ? 1U : 0U;
    player.clean = (b->sd_player_flags & 0x10U) ? 1U : 0U;
    player.gain_percent = b->sd_player_gain_percent;
    player.seek_seconds = b->sd_player_seek_seconds;
    player.downmix = (sd_player_downmix_t)b->sd_player_downmix;
    SD_AudioPlayerSetConfig(&player);
    SD_AudioPlayerSetRfMode((sd_player_rf_mode_t)b->sd_player_rf_mode);
    if ((b->reserved0 == 0U) && (b->reserved1 == 0U) &&
        (b->reserved2[0] == 0U) && (b->reserved2[1] == 0U) && (b->reserved2[2] == 0U)) {
        cfg_ui_home_layout = UI_HOME_LAYOUT_DETAILED;
        cfg_ui_home_flags = UI_HOME_FLAGS_DEFAULT;
        cfg_ui_shortcut_up = UI_SHORTCUT_AB_SWAP;
        cfg_ui_shortcut_down = UI_SHORTCUT_BAND;
        cfg_runtime_diag_enable = 0U;
        cfg_cpu_diag_enable = 0U;
        MOD_SetCpuScopeMode(MOD_CPU_SCOPE_OFF);
        MOD_SetSsbPipelineMode(SSB_PIPELINE_LEGACY);
    } else {
        cfg_ui_home_layout = ((uint32_t)b->reserved0 < (uint32_t)UI_HOME_LAYOUT_COUNT)
                           ? (ui_home_layout_t)b->reserved0
                           : UI_HOME_LAYOUT_DETAILED;
        cfg_ui_home_flags = (uint8_t)(b->reserved1 & UI_HOME_FLAGS_DEFAULT);
        if ((cfg_ui_home_flags & UI_HOME_FLAGS_DEFAULT) == 0U) cfg_ui_home_flags = UI_HOME_FLAGS_DEFAULT;
        cfg_ui_shortcut_up = (((uint32_t)(b->reserved1 >> 8)) < (uint32_t)UI_SHORTCUT_COUNT)
                           ? (ui_shortcut_action_t)(b->reserved1 >> 8)
                           : UI_SHORTCUT_AB_SWAP;
        cfg_ui_shortcut_down = ((uint32_t)b->reserved2[0] < (uint32_t)UI_SHORTCUT_COUNT)
                             ? (ui_shortcut_action_t)b->reserved2[0]
                             : UI_SHORTCUT_BAND;
        cfg_runtime_diag_enable = b->reserved2[1] ? 1U : 0U;
        cfg_cpu_diag_enable = (b->cw_paddle_reserved & SETTINGS_CPU_DIAG_BIT) ? 1U : 0U;
        MOD_SetCpuScopeMode((mod_cpu_scope_mode_t)((b->cw_paddle_reserved & SETTINGS_CPU_SCOPE_MASK) >> SETTINGS_CPU_SCOPE_SHIFT));
        MOD_SetSsbPipelineMode((b->cw_paddle_reserved & SETTINGS_SSB_PIPELINE_STRICT)
                             ? SSB_PIPELINE_STRICT_QMX12_EXP
                             : SSB_PIPELINE_LEGACY);
        cfg_carrier_clock_policy = (mod_clock_policy_t)((b->cw_paddle_reserved & SETTINGS_CLOCK_POLICY_MASK) >> SETTINGS_CLOCK_POLICY_SHIFT);
        if ((uint32_t)cfg_carrier_clock_policy >= (uint32_t)MOD_CLOCK_POLICY_COUNT) {
            cfg_carrier_clock_policy = MOD_CLOCK_POLICY_AUTO;
        }
        /* Persist the selected carrier clock policy, including PREC. PREC is
         * still explicitly experimental, but a user-saved precision policy is
         * honored on the next boot. Boot remains safe because dynamic policies
         * schedule an idle-only retune below, and TX is inhibited while the
         * retune is pending/applying. */
        cfg_precision_clock_arm = (cfg_carrier_clock_policy == MOD_CLOCK_POLICY_PRECISION_DYNAMIC) ? 1U : 0U;
    }
    sanitize_audio_runtime_settings();
    sanitize_ssb_runtime_settings();
    sanitize_outphase_policy_settings();
    sanitize_rf_en_settings();
    sanitize_cw_settings();
    cw_paddle_guard_reset();
    cw_reset_state();
    MOD_TxCarrierTestStop();
    build_outphase_lut();

    memset(&phase_cal_active, 0, sizeof(phase_cal_active));
    memset(theta_linearity_pd, 0, sizeof(theta_linearity_pd));
    active_cal_flags = 0U;
    refresh_active_cal_ptr();

    if (b->freq_mhz >= 1.0f && b->freq_mhz <= 60.0f) {
        update_custom_carrier_geometry(b->freq_mhz);
    }
    if (MOD_GetCarrierClockPolicy() != MOD_CLOCK_POLICY_FIXED_170) {
        /* Persisted dynamic policies, including PREC, are honored after boot.
         * Keep startup deterministic at the validated clock first, then let the
         * background retune service perform the idle-only SYSCLK/HRTIM change.
         * The pending retune state inhibits TX until the carrier is applied. */
        MOD_ScheduleCarrierClockRetune(MOD_GetCarrierRequestedHz(), MOD_BOOT_DYNAMIC_RETUNE_DELAY_MS);
    }

    pa_legacy_profile = b->pa_legacy_profile;
    pa_legacy_profile.duty_offset_half_permille = settings_clamp_permille((int32_t)pa_legacy_profile.duty_offset_half_permille);
    memcpy(pa_band_profiles, b->pa_band_profiles, sizeof(pa_band_profiles));
    for (uint32_t i = 0U; i < BAND_COUNT; i++) {
        pa_band_profiles[i].duty_offset_half_permille =
            settings_clamp_permille((int32_t)pa_band_profiles[i].duty_offset_half_permille);
    }

    pa_resolve_active_band_profile(1U, 0U, 0U);

    recompute_eq_coeffs();

    /* A record saved by the deep-diagnostic image can hold a mode this image
     * does not have (its renderer is not linked): PROD SSB instead, noted as
     * at boot. The menu's LOAD comes here too, not only the boot. */
    if (((uint32_t)b->active_config < MOD_CFG_COUNT) &&
        (b->active_config != MOD_CFG_TX_CARRIER_TEST) &&
        MOD_ModeAvailableInBuild((mod_config_t)b->active_config)) {
        requested_config = (uint32_t)b->active_config;
    } else {
        if ((uint32_t)b->active_config < MOD_CFG_COUNT &&
            b->active_config != MOD_CFG_TX_CARRIER_TEST) {
            mod_pipeline_note_severity(MOD_PIPELINE_STAGE_CONTROL,
                                       MOD_PIPELINE_REASON_MODE_NOT_AVAILABLE,
                                       MOD_PIPELINE_SEVERITY_WARNING,
                                       (uint32_t)b->active_config);
        }
        requested_config = (uint32_t)MOD_CFG_PROD_SSB_14MHZ;
    }

    MOD_SetRuntimeDiagEnable(cfg_runtime_diag_enable);
    MOD_SetCpuDiagEnable(cfg_cpu_diag_enable);

    /* Single-threaded main-loop architecture: settings_apply() only queues the
     * reset here. MOD_ServiceBackground() consumes requested_reset later, so
     * nested apply_config() re-entry is not expected. */
    requested_reset = 1U;
}

#define SETTINGS_SLOT 0U

/* Run without a card: the operator's choice at the NO SD CARD hold. Nothing
 * is read from or written to any card until the next boot -- a card put in
 * later may be this radio's, and the defaults running now must not land on
 * it. */
static uint8_t storage_nocard;

uint8_t MOD_StorageRunsWithoutCard(void) { return storage_nocard; }

/* The store's settings did not read (an I/O error, a read cut short) before
 * any were loaded from it or saved to it: the running ones are the defaults,
 * not the operator's. The boot holds as for an unreadable card and reads
 * them again every second; no save writes the defaults over the record that
 * is there. Before, the failed read seeded the store with them at the first
 * autosave (bench review, 2026-10-02). */
static uint8_t settings_synced, settings_unread;

uint8_t MOD_StorageSettingsUnread(void) { return settings_unread; }

/* ---- The Morse trainer's learner record ---------------------------------- */

#define TRAINER_REC_MAGIC 0x5052544DUL /* "MTRP" */
typedef struct {
    uint32_t magic;
    mtrain_progress_t p;
} trainer_record_t;
_Static_assert(sizeof(trainer_record_t) <= SD_STORE_SLOT_MAX, "the learner record fits a store sector");
_Static_assert(sizeof(mtrain_prefs_t) < sizeof(((settings_blob_t *)0)->cw_trainer), "the trainer's preferences fit");

/* 0 not read, 1 read (or none there: a fresh one), 2 unreadable: kept;
 * and the card it was read from (SD_StoreDevSession). */
static uint8_t trainer_rec_state;
static uint32_t trainer_rec_session;

uint8_t MOD_TrainerRecordAccept(const void *data, uint32_t len)
{
    const trainer_record_t *r = (const trainer_record_t *)data;
    return (uint8_t)(len == sizeof(*r) && r->magic == TRAINER_REC_MAGIC && MTRAIN_ProgressValid(&r->p));
}

static uint8_t trainer_store_ready(void)
{
    return (uint8_t)(!storage_nocard && SD_StoreState() == SD_STORE_READY && storage_blocking_io_allowed());
}

uint8_t MOD_TrainerRecordLoad(void)
{
    trainer_record_t rec;
    uint32_t len = 0U;
    /* Unsaved progress (the card was away) goes to the card first. */
    if (trainer_rec_state == 1U && MTRAIN_ProgressDirty()) return MOD_TrainerRecordSave();
    if (trainer_rec_state && trainer_rec_session == SD_StoreDevSession()) return 1U; /* this card's, read */
    if (!trainer_store_ready()) return 0U;
    memset(&rec, 0, sizeof(rec));
    const sd_store_rec_t r = SD_StoreRead(SD_STORE_SLOT_TRAINER, &rec, sizeof(rec), &len, NULL);
    if (r == SD_STORE_REC_ERROR) return 0U;
    MTRAIN_ResetProgress();
    trainer_rec_session = SD_StoreDevSession();
    if (r == SD_STORE_REC_NONE) {
        trainer_rec_state = 1U;
    } else if (MOD_TrainerRecordAccept(&rec, len)) {
        MTRAIN_ProgressSet(&rec.p);
        trainer_rec_state = 1U;
    } else {
        trainer_rec_state = 2U;
    }
    MTRAIN_ProgressSaved();
    return 1U;
}

static uint8_t trainer_write(void)
{
    trainer_record_t rec;
    if (!trainer_store_ready()) return 0U;
    memset(&rec, 0, sizeof(rec));
    rec.magic = TRAINER_REC_MAGIC;
    rec.p = *MTRAIN_Progress();
    if (!SD_StoreWrite(SD_STORE_SLOT_TRAINER, &rec, sizeof(rec))) return 0U;
    MTRAIN_ProgressSaved();
    return 1U;
}

uint8_t MOD_TrainerRecordSave(void)
{
    if (trainer_rec_state != 1U) return 0U;
    if (!MTRAIN_ProgressDirty()) return 1U;
    if (MTRAIN_Prefs()->nosave) {
        /* SAVE PROGRESS off: nothing written, and the card's record is
         * read again before the next session. */
        storage_trainer_forget();
        return 1U;
    }
    return trainer_write();
}

uint8_t MOD_TrainerRecordDelete(void)
{
    MTRAIN_ResetProgress();
    trainer_rec_state = 1U;
    trainer_rec_session = SD_StoreDevSession();
    if (trainer_write()) return 1U;
    storage_trainer_forget(); /* the card keeps what it has: read it again */
    return 0U;
}

void storage_trainer_forget(void)
{
    trainer_rec_state = 0U;
    MTRAIN_ProgressSaved();
}

uint8_t MOD_TrainerRecordUnsaved(void)
{
    return (uint8_t)(trainer_rec_state != 1U || (MTRAIN_ProgressDirty() && SD_StoreState() != SD_STORE_READY));
}

/* The running settings into the store, in a frame of their own. */
static __attribute__((noinline)) uint8_t settings_save_store(void)
{
    settings_record_t rec;
    if (storage_nocard) return 0U;
    if (SD_StoreState() != SD_STORE_READY) {
        storage_sd_missing = 1U;
        MOD_EventLog(MOD_EVENT_STORAGE, 1U, 0U, (uint32_t)SD_StoreState(), 0U); /* save: no store */
        return 0U;
    }
    memset(&rec, 0, sizeof(rec));
    rec.rec_magic = SETTINGS_REC_MAGIC;
    settings_snapshot(&rec.blob);
    if (!SD_StoreWrite(SETTINGS_SLOT, &rec, sizeof(rec))) {
        storage_sd_missing = 1U;
        MOD_EventLog(MOD_EVENT_STORAGE, 2U, 0U, 0U, 0U); /* save: write failed */
        return 0U;
    }
    storage_sd_missing = 0U;
    settings_synced = 1U;
    return 1U;
}

uint8_t MOD_SaveSettings(void)
{
    if (storage_blocking_io_allowed() == 0U || settings_unread) return 0U;
    /* The band's calibration record goes with the settings. */
    return (uint8_t)(settings_save_store() && save_current_band_calibrations());
}

static uint8_t settings_record_load(const settings_record_t *raw, uint32_t len, settings_record_t *rec)
{
    if (len < 8U + SETTINGS_V20_LENGTH || raw->rec_magic != SETTINGS_REC_MAGIC) return 0U;
    return settings_blob_load_current(&raw->blob, &rec->blob);
}

/* The store's settings record, if it holds one this image can read: v26
 * (v8.07), v27 (v8.08, v8.09) and v28 blobs. NONE for no record, ERROR for a
 * read that failed -- and for a record this image cannot use. That one is
 * not answered with defaults: the autosave would write them over the copy
 * before it, the last settings this image could have read. The copy before
 * it is tried instead (bench, 2026-10-02: a v28 layout changed under
 * development read as nothing, and the defaults went over that copy); with
 * neither usable the radio holds (SD SETTINGS UNREAD) for the operator. */
static __attribute__((noinline)) sd_store_rec_t settings_load_store(settings_record_t *rec)
{
    settings_record_t raw;
    uint32_t len = 0U;
    memset(&raw, 0, sizeof(raw));
    sd_store_rec_t r = SD_StoreRead(SETTINGS_SLOT, &raw, sizeof(raw), &len, NULL);
    if (r != SD_STORE_REC_OK) return r;
    if (settings_record_load(&raw, len, rec)) return SD_STORE_REC_OK;
    memset(&raw, 0, sizeof(raw));
    r = SD_StoreReadOlder(SETTINGS_SLOT, &raw, sizeof(raw), &len, NULL);
    if (r == SD_STORE_REC_OK && settings_record_load(&raw, len, rec)) return SD_STORE_REC_OK;
    return SD_STORE_REC_ERROR;
}

/* The store holds no settings yet (a new store, or calibration only): the
 * running ones go there at the first autosave. */
static uint8_t sd_seed_pending;

uint8_t storage_settings_apply_save(const settings_blob_t *b)
{
    cal_store_forget();
    settings_apply(b);
    load_current_calibration_context();
    return MOD_SaveSettings();
}

uint8_t MOD_LoadSettings(void)
{
    settings_record_t rec;
    if (storage_blocking_io_allowed() == 0U) return 0U;
    cfg_storage_backend = MOD_STORAGE_SD;
    if (storage_nocard) return 0U;
    if (SD_StoreState() != SD_STORE_READY) {
        storage_sd_missing = 1U;
        return 0U;
    }
    storage_sd_missing = 0U;
    const sd_store_rec_t r = settings_load_store(&rec);
    if (r == SD_STORE_REC_ERROR) {
        if (!settings_synced) settings_unread = 1U;
        return 0U;
    }
    settings_unread = 0U;
    settings_synced = 1U;
    if (r != SD_STORE_REC_OK) {
        sd_seed_pending = 1U;
        return 0U;
    }
    settings_apply(&rec.blob);
    return 1U;
}

/* The boot hold's next look at settings that did not read: the card mounted
 * again when the read left it not ready, then read. */
uint8_t MOD_StorageRereadSettings(void)
{
    if (!settings_unread) return 1U;
    if (!SD_IsReady() || SD_StoreNeedsReopen()) SD_ServiceRefresh();
    return MOD_LoadSettings();
}

/* ---- Event log on the card ----------------------------------------------------
 * What went wrong, with more detail than any console counter keeps and
 * surviving a power cycle: LOGS/EVENTS.LOG, one text line per event, kept to
 * LOG_MAX_BYTES and rotated once to EVENTS.1. Events are queued from any
 * context (a protection trip runs in an interrupt) into a small RAM ring and
 * written by the main loop, never while transmitting or streaming. A ring
 * that fills before the card catches up counts what it dropped. */
#define EVENT_RING    16U
#define LOG_FILE      "LOGS/EVENTS.LOG"
#define LOG_OLD       "LOGS/EVENTS.1"
#define LOG_MAX_BYTES (256UL * 1024UL)
typedef struct { uint32_t tick; uint8_t kind, code; uint16_t aux; uint32_t v1, v2; } mod_event_t;
static mod_event_t event_ring[EVENT_RING];
static volatile uint32_t event_wr, event_rd, event_lost;

void MOD_EventLog(uint8_t kind, uint8_t code, uint16_t aux, uint32_t v1, uint32_t v2)
{
    const uint32_t pm = enter_critical();
    const uint32_t wr = event_wr;
    if (wr - event_rd < EVENT_RING) {
        mod_event_t *e = &event_ring[wr % EVENT_RING];
        e->tick = HAL_GetTick(); e->kind = kind; e->code = code; e->aux = aux; e->v1 = v1; e->v2 = v2;
        event_wr = wr + 1U;
    } else {
        ++event_lost;
    }
    exit_critical(pm);
}

void MOD_EventLogCounts(uint32_t *queued, uint32_t *lost)
{
    if (queued) *queued = event_wr - event_rd;
    if (lost) *lost = event_lost;
}

/* A line under construction; no stdio in this image. */
typedef struct { char t[160]; uint16_t n; } log_line_t;
static void ll_s(log_line_t *l, const char *s) { while (*s && l->n < sizeof(l->t) - 2U) l->t[l->n++] = *s++; }
static void ll_u(log_line_t *l, uint32_t v)
{
    char d[10]; unsigned k = 0U;
    do { d[k++] = (char)('0' + v % 10U); v /= 10U; } while (v && k < sizeof(d));
    while (k && l->n < sizeof(l->t) - 2U) l->t[l->n++] = d[--k];
}
static void ll_x(log_line_t *l, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    ll_s(l, "0x");
    for (int sh = 28; sh >= 0; sh -= 4) if (l->n < sizeof(l->t) - 2U) l->t[l->n++] = hex[(v >> sh) & 15U];
}
static void ll_kv(log_line_t *l, const char *k, uint32_t v) { ll_s(l, " "); ll_s(l, k); ll_s(l, "="); ll_u(l, v); }
static void ll_kx(log_line_t *l, const char *k, uint32_t v) { ll_s(l, " "); ll_s(l, k); ll_s(l, "="); ll_x(l, v); }
static void ll_start(log_line_t *l, uint32_t tick, const char *what)
{
    l->n = 0U;
    ll_s(l, "t="); ll_u(l, tick); ll_s(l, " "); ll_s(l, what);
}

/* Append one finished line, rotating first when the file is full. */
static uint8_t log_append(log_line_t *l)
{
    l->t[l->n++] = '\n';
    uint32_t size = 0U;
    uint8_t attr = 0U;
    if (!SD_DiagStat(LOG_FILE, &size, &attr)) {
        (void)SD_DiagMkdir("LOGS");
        size = 0U;
    } else if (size > LOG_MAX_BYTES) {
        (void)SD_DiagDelete(LOG_OLD);
        (void)SD_DiagRename(LOG_FILE, LOG_OLD);
        size = 0U;
    }
    return SD_DiagWriteFile(LOG_FILE, size, l->t, l->n, 0U);
}

static void log_event(log_line_t *l, const mod_event_t *e)
{
    switch (e->kind) {
        case MOD_EVENT_FAULT:
            ll_start(l, e->tick, "FAULT");
            for (uint8_t bit = 0U; bit < 8U; ++bit)
                if (e->code & (1U << bit)) { ll_s(l, " "); ll_s(l, MOD_RadioFaultName(bit)); }
            ll_kv(l, "state", e->aux);
            ll_kv(l, "vin_mv", e->v1 >> 16); ll_kv(l, "rail5_mv", e->v1 & 0xFFFFU);
            ll_kv(l, "fwd_mv", e->v2 >> 16); ll_kv(l, "rev_mv", e->v2 & 0xFFFFU);
            break;
        case MOD_EVENT_PIPELINE:
            ll_start(l, e->tick, "PIPELINE");
            ll_s(l, " "); ll_s(l, MOD_PipelineStageName((uint8_t)(e->aux >> 8)));
            ll_s(l, " "); ll_s(l, MOD_PipelineReasonName(e->code));
            ll_kv(l, "severity", e->aux & 0xFFU); ll_kx(l, "value", e->v1); ll_kv(l, "config", e->v2);
            break;
        case MOD_EVENT_STORAGE:
            ll_start(l, e->tick, "STORAGE");
            {
                static const char *const what[] = {
                    " ?", " SAVE NO STORE", " SAVE WRITE FAILED", " CAL WRITE FAILED", " CARD LOST",
                    " CARD BACK", " COPY DID NOT VERIFY", " WRITE GAVE UP", " COPY REPAIRED",
                    " BAD COPY", " STORE CREATED", " STORE STATE", " CAL NOT LOADED", " RUN WITHOUT CARD",
                    " STORE ADOPTED", " FACTORY RESET", " IMPORTED", " RESTORED", " INCOMPLETE STORE KEPT"};
                ll_s(l, what[(e->code < sizeof(what) / sizeof(what[0])) ? e->code : 0U]);
            }
            ll_kv(l, "aux", e->aux); ll_kx(l, "v1", e->v1); ll_kx(l, "v2", e->v2);
            break;
        case MOD_EVENT_SD:
            ll_start(l, e->tick, "SD");
            ll_s(l, e->code == 1U ? " REINIT" : e->code == 2U ? " READ FAILED" :
                    e->code == 3U ? " WRITE FAILED" : " ?");
            ll_s(l, " "); ll_s(l, SD_ErrorString((sd_error_t)e->aux));
            ll_kv(l, "lba", e->v1); ll_kv(l, e->code == 1U ? "write" : "retries", e->v2);
            break;
        default:
            ll_start(l, e->tick, "EVENT");
            ll_kv(l, "kind", e->kind); ll_kv(l, "code", e->code); ll_kv(l, "aux", e->aux);
            ll_kx(l, "v1", e->v1); ll_kx(l, "v2", e->v2);
            break;
    }
}

extern volatile uint32_t g_reset_csr;
extern volatile uint32_t g_fault_magic, g_fault_reason, g_fault_cfsr, g_fault_hfsr, g_fault_mmfar,
                         g_fault_bfar, g_fault_exc_lr, g_fault_stacked_lr, g_fault_stacked_pc, g_fault_stacked_xpsr;
#define FAULT_MARKER_MAGIC 0xF640FA17UL

/* Once per boot: the firmware, why the MCU last reset, and a HardFault or
 * other fatal exception's registers if one caused it. */
static uint8_t log_boot(log_line_t *l)
{
    mod_clock_snapshot_t c;
    MOD_GetClockSnapshot(&c);
    ll_start(l, HAL_GetTick(), "BOOT");
    ll_s(l, " firmware="); ll_s(l, FW_VERSION_STRING); /* whole: version_tool finds it in the image */
    ll_kx(l, "reset_csr", g_reset_csr);
    ll_kv(l, "sysclk_hz", SystemCoreClock);
    ll_s(l, " supply="); ll_s(l, RADIO_PdSupplyName());
    ll_kv(l, "sd_mib", SD_ServiceCapacityMiB());
    ll_kv(l, "lost", event_lost);
    if (!log_append(l)) return 0U;
    if (g_fault_magic == FAULT_MARKER_MAGIC) {
        ll_start(l, HAL_GetTick(), "CRASH");
        ll_kv(l, "reason", g_fault_reason);
        ll_kx(l, "pc", g_fault_stacked_pc); ll_kx(l, "lr", g_fault_stacked_lr);
        ll_kx(l, "xpsr", g_fault_stacked_xpsr); ll_kx(l, "exc_lr", g_fault_exc_lr);
        ll_kx(l, "cfsr", g_fault_cfsr); ll_kx(l, "hfsr", g_fault_hfsr);
        ll_kx(l, "mmfar", g_fault_mmfar); ll_kx(l, "bfar", g_fault_bfar);
        if (!log_append(l)) return 0U;
        g_fault_magic = 0U;
    }
    return 1U;
}

/* Counters that are not events: receive deadline misses, an RF DMA stall's
 * registers, failed clock retunes. Logged when they move. */
#define RX_LOG_MS 10000U

static void log_counters(log_line_t *l, uint32_t now)
{
    static uint32_t rx_pending, rx_working, rx_dma, i2s_fb, stall_count, retune_failed, rx_logged_at;
    uint32_t pending = 0U, working = 0U, overrun = 0U, dma = 0U, workers = 0U;
    RADIO_AudioFailCounts(&pending, &working, &overrun, &dma, &workers);
    const uint32_t fb = RADIO_AudioI2sFallbacks();
    /* At most one line every RX_LOG_MS, the deltas summed: a receiver
     * missing deadlines for minutes wrote the card four times a second. */
    if ((pending != rx_pending || working != rx_working || dma != rx_dma || fb != i2s_fb) &&
        (!rx_logged_at || (uint32_t)(now - rx_logged_at) >= RX_LOG_MS)) {
        rx_logged_at = now ? now : 1U;
        ll_start(l, now, "RX MISSED");
        ll_kv(l, "pending", pending - rx_pending); ll_kv(l, "working", working - rx_working);
        ll_kv(l, "dma", dma - rx_dma); ll_kv(l, "i2s_fallbacks", fb - i2s_fb);
        ll_kv(l, "state", (uint32_t)RADIO_State());
        if (log_append(l)) { rx_pending = pending; rx_working = working; rx_dma = dma; i2s_fb = fb; }
    }
    mod_rf_stall_t st;
    MOD_GetRfStall(&st);
    if (st.count != stall_count) {
        ll_start(l, st.tick_ms, "RF STALL");
        ll_kv(l, "ticks", st.ticks); ll_kv(l, "state", st.radio_state); ll_kv(l, "config", st.config);
        ll_kx(l, "dma_ccr", st.dma_ccr); ll_kv(l, "dma_cndtr", st.dma_cndtr); ll_kx(l, "tim6_cr1", st.tim6_cr1);
        ll_kx(l, "hrtim_mcr", st.hrtim_mcr); ll_kx(l, "timf_dier", st.timf_dier); ll_kv(l, "queued", st.render_count);
        if (log_append(l)) stall_count = st.count;
    }
    mod_clock_snapshot_t c;
    MOD_GetClockSnapshot(&c);
    if (c.failed != retune_failed) {
        ll_start(l, now, "CLOCK RETUNE FAILED");
        ll_kv(l, "count", c.failed - retune_failed); ll_kv(l, "reason", c.last_reason);
        if (log_append(l)) retune_failed = c.failed;
    }
}

/* A log line that does not land is tried again later and later (2 s up to a
 * minute): each failed write is an event of its own, and retrying every poll
 * kept a card that refuses writes busy with nothing else (bench, 2026-10-01:
 * one write tried every 0.7 s, the main loop held most of the time). */
#define LOG_POLL_MS       250U
#define LOG_RETRY_MS      2000U
#define LOG_RETRY_MAX_MS  60000U

static void log_failed(uint32_t *wait_ms)
{
    *wait_ms = (*wait_ms < LOG_RETRY_MS) ? LOG_RETRY_MS
             : (*wait_ms * 2U > LOG_RETRY_MAX_MS) ? LOG_RETRY_MAX_MS : *wait_ms * 2U;
    /* A card that takes no writes: FatFs keeps the sector it could not write
     * and tries it before every other one, so the card's files stopped
     * reading too. Mounted again, the files read; the line is lost, as the
     * card would lose it anyway. */
    if (SD_WritesFailing()) SD_ServiceRefresh();
}

void MOD_StorageLogService(uint32_t now)
{
    static uint8_t booted;
    static uint32_t polled_at;
    static uint32_t wait_ms = LOG_POLL_MS;
    static uint8_t had_card = 1U;
    static uint32_t steady_at;
    const uint8_t card = SD_IsMounted();
    /* A remount takes the volume down for milliseconds (more when the gate
     * cuts its initialisation short and the next pass finishes it): an edge
     * is logged once it has held a second. */
    if (card == had_card) {
        steady_at = now;
    } else if ((uint32_t)(now - steady_at) >= 1000U) {
        had_card = card;
        MOD_EventLog(MOD_EVENT_STORAGE, card ? 5U : 4U, 0U, 0U, 0U);
    }
    if (!card || (uint32_t)(now - polled_at) < wait_ms) return;
    polled_at = now;
    if (MOD_BackgroundIoAllowed(now) == 0U) return;
    log_line_t l;
    if (!booted) {
        booted = log_boot(&l);
        if (!booted) log_failed(&wait_ms);
        return;
    }
    for (unsigned n = 0U; n < 4U && event_rd != event_wr; ++n) {
        mod_event_t e = event_ring[event_rd % EVENT_RING];
        log_event(&l, &e);
        if (!log_append(&l)) {
            log_failed(&wait_ms);
            return;
        }
        ++event_rd;
        wait_ms = LOG_POLL_MS;
    }
    log_counters(&l, now);
}

uint8_t MOD_StorageCardRequired(void)
{
    return (uint8_t)(!storage_nocard &&
                     (SD_StoreState() != SD_STORE_READY || MOD_StorageIncomplete() || settings_unread));
}

/* ---- Autosave --------------------------------------------------------------
 * With the settings on the card, what an operator changes is kept without a
 * SAVE: volume, frequency, band, mode, filters, keyer, everything the
 * settings blob holds (it holds no screen or menu state). A change is written
 * once the settings have been still for AUTOSAVE_QUIET_MS, at most once per
 * AUTOSAVE_MIN_GAP_MS, never while transmitting or streaming, and the card
 * write takes the main loop, not the real-time paths. */
#define AUTOSAVE_POLL_MS    500U
#define AUTOSAVE_QUIET_MS   2000U
#define AUTOSAVE_MIN_GAP_MS 5000U
#define SCRUB_STEP_MS       500U

/* CRC of the settings as they are now; its own frame so the ~1 KB snapshot
 * is off the stack before the save builds its record. */
static __attribute__((noinline)) uint32_t settings_now_crc(void)
{
    settings_blob_t b;
    settings_snapshot(&b);
    b.outphase_reserved[STORAGE_FLAGS_RESERVED_IDX] = 0U; /* card presence is not a setting */
    return crc32_simple((const uint8_t *)&b + 16U, sizeof(b) - 16U);
}

/* The card answers but was initialised again since the store was read, or
 * went not ready (an error, a transfer the I/O gate cut short): its headers
 * are looked at again before anything is read or written. It may be another
 * card, and then FatFs is mounted afresh as well, not left writing to it with
 * the last card's tables. 1 when this radio's store is usable again. */
uint8_t MOD_StorageReconnect(void)
{
    if (storage_nocard || storage_sd_missing || SD_StoreState() != SD_STORE_READY) return 0U;
    if (SD_IsReady() && !SD_StoreNeedsReopen()) return 1U;
    sd_store_info_t was, now;
    SD_StoreGetInfo(&was);
    if (!SD_IsReady() || SD_StoreOpen() != SD_STORE_READY) SD_ServiceRefresh();
    /* Not ready: the refresh was refused or its initialisation cut short by
     * the gate; the store, unchanged, waits for the next pass. */
    if (SD_StoreState() != SD_STORE_READY || !SD_IsReady() || SD_StoreNeedsReopen()) return 0U;
    /* The same store: the cached records are still its own, and dropping
     * them made the next save rewrite an unchanged record (and a backup). */
    SD_StoreGetInfo(&now);
    if (now.instance != was.instance) cal_store_forget();
    return 1U;
}

/* The store lost its card (a save failed, or none is there): mount again
 * every few seconds, off TX. Only this radio's store is written to: a card
 * that comes back blank or from another radio waits for the operator (SD
 * menu, console `storage`). */
#define CARD_RETRY_MS     3000U
#define CARD_RETRY_MAX_MS 30000U
#define CARD_SLOW_MS      250U
static uint8_t storage_card_recover(uint32_t now)
{
    static uint32_t tried_at;
    static uint32_t wait_ms = CARD_RETRY_MS;
    const sd_store_state_t st = SD_StoreState();
    if (st == SD_STORE_READY && !storage_sd_missing && SD_IsReady() && !SD_StoreNeedsReopen()) return 1U;
    /* A card that is there and read -- blank, another radio's, damaged, the
     * wrong layout -- waits for the operator; only a missing or unreadable
     * one, or a save that failed, is mounted again. */
    if (st != SD_STORE_NO_CARD && st != SD_STORE_IO && st != SD_STORE_READY) return 0U;
    if (MOD_BackgroundIoAllowed(now) == 0U) return 0U;
    /* Not lost, only initialised again (or gone just now: then the state
     * is NO_CARD after this, and the next passes try at their pace). */
    if (st == SD_STORE_READY && !storage_sd_missing) return MOD_StorageReconnect();
    /* A save that failed on a card that takes no writes is not a lost card:
     * mounting it again every 3 s would only fail the next save again. */
    if ((uint32_t)(now - tried_at) < (SD_WritesFailing() ? CARD_RETRY_MAX_MS : wait_ms)) return 0U;
    SD_ServiceRefresh();
    tried_at = HAL_GetTick();
    if (SD_StoreState() != SD_STORE_READY) {
        /* A missing card fails at once. One that is there but does not answer
         * (half inserted, wedged) costs the main loop up to an initialisation
         * every time: it is tried less and less often, up to 30 s apart. */
        const uint8_t slow = (uint8_t)((uint32_t)(tried_at - now) > CARD_SLOW_MS);
        wait_ms = slow ? ((wait_ms * 2U > CARD_RETRY_MAX_MS) ? CARD_RETRY_MAX_MS : wait_ms * 2U) : CARD_RETRY_MS;
        return 0U;
    }
    wait_ms = CARD_RETRY_MS;
    storage_sd_missing = 0U;
    sd_seed_pending = 1U; /* what changed while it was away */
    cal_store_forget();
    MOD_EventLog(MOD_EVENT_STORAGE, 5U, 0U, 0U, 0U);
    return 1U;
}

/* One slot of the store's background check (mod_sd_store.c) per step: both
 * copies of every record are read in turn, and a copy that does not read
 * back valid is rewritten from the other -- a full pass about every 100 s. */
static void storage_scrub(uint32_t now)
{
    static uint32_t at;
    if ((uint32_t)(now - at) < SCRUB_STEP_MS || MOD_BackgroundIoAllowed(now) == 0U) return;
    at = now;
    (void)SD_StoreScrubStep();
}

void MOD_StorageAutosaveService(uint32_t now)
{
    static uint32_t polled_at, changed_at, saved_at, seen_crc, saved_crc;
    static uint8_t primed;
    if (storage_nocard) return;
    if (!storage_card_recover(now)) return;
    storage_scrub(now);
    MOD_StorageBackupService(now, (uint8_t)(MOD_BackgroundIoAllowed(now) != 0U));
    if (sd_seed_pending && primed) saved_crc = ~seen_crc; /* a store came back: write it */
    if ((uint32_t)(now - polled_at) < AUTOSAVE_POLL_MS) return;
    polled_at = now;
    if (MOD_BackgroundIoAllowed(now) == 0U) return;
    const uint32_t crc = settings_now_crc();
    if (!primed) {
        /* What was loaded is what the store holds, unless it has to be seeded. */
        primed = 1U;
        seen_crc = crc;
        saved_crc = sd_seed_pending ? ~crc : crc;
        changed_at = now;
        return;
    }
    if (crc != seen_crc) { seen_crc = crc; changed_at = now; return; }
    if (crc == saved_crc) return;
    if ((uint32_t)(now - changed_at) < AUTOSAVE_QUIET_MS) return;
    /* A card that takes no writes is asked once a minute, not every 5 s. */
    if (saved_at && (uint32_t)(now - saved_at) < (SD_WritesFailing() ? 60000U : AUTOSAVE_MIN_GAP_MS)) return;
    saved_at = now;
    if (MOD_SaveSettings()) {
        saved_crc = crc;
        sd_seed_pending = 0U;
    }
}

uint8_t MOD_GetStorageSdMissing(void)
{
    return (uint8_t)(storage_sd_missing || storage_nocard || SD_StoreState() != SD_STORE_READY);
}

const char *MOD_GetStorageStatusLabel(void)
{
    if (storage_nocard) return "NO CARD - NOT SAVED";
    if (SD_WritesFailing()) return "SD WRITE ERRORS";
    if (SD_StoreState() == SD_STORE_READY) {
        if (MOD_StorageIncomplete()) return "SD STORE INCOMPLETE";
        if (settings_unread) return "SD SETTINGS UNREAD";
        return storage_sd_missing ? "SD SAVE FAILED" : "SD ACTIVE";
    }
    return SD_StoreStateName(SD_StoreState());
}

/* The home screen's storage line, or NULL when all is well. */
const char *MOD_StorageWarning(void)
{
    if (storage_nocard) return "NO SD CARD - SETTINGS NOT SAVED";
    if (MOD_CalNotLoaded()) return "CAL NOT LOADED - TX LOCKED";
    if (SD_WritesFailing()) return "SD CARD WRITE ERRORS - REPLACE CARD";
    switch (SD_StoreState()) {
        case SD_STORE_READY:
            if (MOD_StorageIncomplete()) return "SD STORE INCOMPLETE - SD MENU";
            return storage_sd_missing ? "SD SAVE FAILED - CHECK CARD" : NULL;
        case SD_STORE_BLANK:   return "SD CARD HAS NO SETTINGS STORE";
        case SD_STORE_FOREIGN: return "SD CARD OF ANOTHER RADIO";
        case SD_STORE_DAMAGED: return "SD STORE DAMAGED - SD MENU";
        case SD_STORE_LAYOUT:  return "SD CARD LAYOUT - FORMAT IT";
        case SD_STORE_IO:      return "SD CARD READ ERROR";
        case SD_STORE_NO_CARD:
        default:               return "SD CARD LOST - SETTINGS NOT SAVED";
    }
}

/* ---- What the operator decides about a card (NO SD CARD hold, SD menu,
 * console `storage`) ---------------------------------------------------- */

uint8_t MOD_StorageRunWithoutCard(void)
{
    storage_nocard = 1U;
    storage_sd_missing = 0U;
    MOD_EventLog(MOD_EVENT_STORAGE, 13U, 0U, (uint32_t)SD_StoreState(), 0U);
    load_current_calibration_context(); /* nothing to wait for: TX is not held */
    return 1U;
}

/* A store of this radio's on a blank card (or replacing another radio's):
 * the running settings and the band's calibration go there at once. */
uint8_t MOD_StorageCreate(void)
{
    if (storage_nocard || storage_blocking_io_allowed() == 0U) return 0U;
    if (!SD_StoreCreate(SD_STORE_ORIGIN_NEW)) return 0U;
    storage_sd_missing = 0U;
    cal_store_forget();
    return MOD_SaveSettings();
}

/* Take the store on the card as this radio's -- another radio's, or one
 * whose headers were lost -- and run on what it holds. */
uint8_t MOD_StorageAdopt(void)
{
    if (storage_nocard || storage_blocking_io_allowed() == 0U) return 0U;
    if (!SD_StoreAdopt()) return 0U;
    MOD_EventLog(MOD_EVENT_STORAGE, 14U, 0U, 0U, 0U);
    storage_sd_missing = 0U;
    cal_store_forget();
    (void)MOD_LoadSettings();
    load_current_calibration_context();
    return 1U;
}

void MOD_FactoryReset(void)
{
    if (storage_blocking_io_allowed() == 0U) return;

    rf_en_force_idle();

    /* Settings and every band's calibration go, as they did from flash --
     * but the store is copied to SYSTEM/STOREBAK.BIN first, so `storage
     * restore` can bring them back. */
    if (!storage_nocard && SD_StoreState() == SD_STORE_READY) {
        (void)MOD_StorageBackup();
        (void)SD_StoreErase(SETTINGS_SLOT);
        (void)cal_store_erase_all();
        (void)SD_StoreErase(SD_STORE_SLOT_TRAINER);
        MOD_EventLog(MOD_EVENT_STORAGE, 15U, 0U, 0U, 0U);
    }

    /* The restart lands where a blank card lands: no settings record, so
     * every setting is its compiled default (mod_state.c) and the boot seeds
     * the store with them. Setting them all here first, as this did, was a
     * second hand-kept copy of those defaults that the reset then threw away
     * (tools/defaults_consistency_check.py now checks this sequence). */
    NVIC_SystemReset();
}

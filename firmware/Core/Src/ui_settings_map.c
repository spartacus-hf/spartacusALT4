#pragma GCC optimize ("Os")

#include "ui_state.h"
#include "card_text.h"
#include "mod_morse_tables.h"
#include <stddef.h>
#include "ui_lcd.h"
#include "mod_selftest.h"
#include "usb_diag_mode.h"
#include "mod_priv.h"
#include "radio.h"
#include "radio_bpf_sweep.h"
#include "radio_af_sweep.h"
#include "radio_audio.h"
#include "radio_rx_cal.h"
#include "radio_pd.h"
#include "radio_clocks.h"
#include <stddef.h>
#include <string.h>

#if defined(UI_SETTINGS_POLICY_ONLY) && (defined(__arm__) || defined(__thumb__))
#error "UI_SETTINGS_POLICY_ONLY is a host-test seam and must not enter target builds"
#endif

#if !defined(UI_SETTINGS_POLICY_ONLY)
static float get_sideband_f(void) { return (float)MOD_GetSideband(); }
static void  set_sideband_f(float v) { MOD_SetSideband((v > 0.5f) ? SIDEBAND_LSB : SIDEBAND_USB); }
static float get_profile_f(void) { return (float)MOD_GetAudioProfile(); }
/* A discrete setting's value as an index into its `count` choices. */
static int32_t round_index(float v, int32_t count)
{
    int32_t i = (int32_t)(v + 0.5f);
    if (i < 0) i = 0;
    if (i >= count) i = count - 1;
    return i;
}

static void  set_profile_f(float v)
{
    const int32_t i = round_index(v, (int32_t)AUDIO_PROFILE_COUNT);
    MOD_SetAudioProfile((audio_profile_t)i);
}

/* Landscape UI operating controls. MODE and BAND go through the navigation
 * layer so the menu, the console and the home settings row share its guards
 * (audio busy, TX test and trainer pages, calibration runs). */
static float get_ui_mode_f(void) { return (float)UI_ModeCurrent(); }
static void  set_ui_mode_f(float v) { (void)UI_ModeApply((ui_umode_t)round_index(v, (int32_t)UI_UMODE_COUNT)); }
static float get_ui_band_f(void) { return (float)MOD_GetActiveBandIndex(); }
static void  set_ui_band_f(float v) { UI_NavSelectBand((uint8_t)round_index(v, (int32_t)MOD_GetBandCount())); }
static float get_ui_step_f(void) { return (float)UI_NavStepDigit(); }
static void  set_ui_step_f(float v) { UI_NavSetStepDigit((uint8_t)round_index(v, (int32_t)UI_TUNE_DIGITS)); }
static float get_ui_rotation_f(void)
{
    return (RADIO_Config()->boot_flags & RADIO_BOOT_FLAG_LCD_ROT270) ? 1.0f : 0.0f;
}
static float get_ui_fkey_pin_f(void) { return (float)RADIO_ConfigFuncKeyPin(RADIO_Config()); }
static void  set_ui_fkey_pin_f(float v)
{
    if (!RADIO_SetFuncKeyPin((uint8_t)round_index(v, (int32_t)RADIO_ENC_PIN_COUNT)))
        UI_NavFlash("PIN USED BY THE ENCODER");
}
static void  set_ui_rotation_f(float v)
{
    RADIO_SetBootFlag(RADIO_BOOT_FLAG_LCD_ROT270, (uint8_t)(v > 0.5f));
    LCDUI_SetRotation((uint8_t)(v > 0.5f));
}
#endif

static const ssb_analytic_backend_t ssb_analytic_normal_choices[] = {
    SSB_ANALYTIC_WEAVER,
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    SSB_ANALYTIC_H12P
#endif
};

#if MOD_DEEP_DIAG_ENABLE
static const ssb_analytic_backend_t ssb_analytic_diagnostic_choices[] = {
    SSB_ANALYTIC_WEAVER,
    SSB_ANALYTIC_HILBERT,
    SSB_ANALYTIC_HILBERT_PAIR_24,
    SSB_ANALYTIC_HILBERT_PAIR_12,
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    SSB_ANALYTIC_H12P
#endif
};
#endif

uint8_t UI_SsbAnalyticChoiceCount(void)
{
#if MOD_DEEP_DIAG_ENABLE
    if (ui_debug_visible != 0U) {
        return (uint8_t)(sizeof(ssb_analytic_diagnostic_choices) /
                         sizeof(ssb_analytic_diagnostic_choices[0]));
    }
#endif
    return (uint8_t)(sizeof(ssb_analytic_normal_choices) /
                     sizeof(ssb_analytic_normal_choices[0]));
}

ssb_analytic_backend_t UI_SsbAnalyticChoiceAt(uint8_t index)
{
#if MOD_DEEP_DIAG_ENABLE
    if (ui_debug_visible != 0U) {
        uint8_t count = (uint8_t)(sizeof(ssb_analytic_diagnostic_choices) /
                                  sizeof(ssb_analytic_diagnostic_choices[0]));
        return ssb_analytic_diagnostic_choices[index < count ? index : 0U];
    }
#endif
    {
        uint8_t count = (uint8_t)(sizeof(ssb_analytic_normal_choices) /
                                  sizeof(ssb_analytic_normal_choices[0]));
        return ssb_analytic_normal_choices[index < count ? index : 0U];
    }
}

#if !defined(UI_SETTINGS_POLICY_ONLY)
static float get_ssb_ana_f(void)
{
    ssb_analytic_backend_t current = MOD_GetSsbAnalyticBackend();
    uint8_t count = UI_SsbAnalyticChoiceCount();
    for (uint8_t index = 0U; index < count; index++) {
        if (UI_SsbAnalyticChoiceAt(index) == current) return (float)index;
    }
    return 0.0f;
}

static void  set_ssb_ana_f(float v)
{
    int32_t i = (int32_t)(v + 0.5f);
    if (i < 0) i = 0;
    if (i >= (int32_t)UI_SsbAnalyticChoiceCount()) {
        i = (int32_t)UI_SsbAnalyticChoiceCount() - 1;
    }
    MOD_SetSsbAnalyticBackend(UI_SsbAnalyticChoiceAt((uint8_t)i));
}
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
static float get_ssb_ce_f(void) { return (float)MOD_GetSsbCeEnable(); }
static void  set_ssb_ce_f(float v) { MOD_SetSsbCeEnable(v > 0.5f ? 1U : 0U); }
#endif
static float get_cal_valid_f(void) { return (float)MOD_GetCalibrationValidated(); }
static void  set_cal_valid_f(float v) { MOD_SetCalibrationValidated(v > 0.5f ? 1U : 0U); }
static float get_cal_apply_f(void) { return (float)MOD_GetCalibrationApply(); }
static void  set_cal_apply_f(float v) { MOD_SetCalibrationApply(v > 0.5f ? 1U : 0U); }
static float get_outphase_law_f(void) { return (float)MOD_GetOutphaseLaw(); }
static void  set_outphase_law_f(float v)
{
    const int32_t i = round_index(v, (int32_t)OUTPHASE_LAW_COUNT);
    MOD_SetOutphaseLaw((outphase_law_t)i);
}
static float get_branch_law_f(void)
{
    outphase_law_t law = MOD_GetOutphaseLaw();
    if (law == OUTPHASE_LAW_CLASSIC_SWAP) return 1.0f;
    if (law == OUTPHASE_LAW_CLASSIC) return 0.0f;
    return 2.0f;
}
static void set_branch_law_f(float v)
{
    MOD_SetOutphaseLaw((v > 0.5f) ? OUTPHASE_LAW_CLASSIC_SWAP
                                  : OUTPHASE_LAW_CLASSIC);
}
static float get_txbw_f(void) { return (float)MOD_GetTxBwPreset(); }
static void  set_txbw_f(float v)
{
    const int32_t i = round_index(v, (int32_t)TX_BW_COUNT);
    MOD_SetTxBwPreset((tx_bw_preset_t)i);
}
#if MOD_CESSB_EXPERIMENT_ENABLE
static float get_cessb_f(void) { return (float)MOD_GetCessbEnable(); }
static void  set_cessb_f(float v) { MOD_SetCessbEnable(v > 0.5f ? 1U : 0U); }
static float get_cessb_iter_f(void) { return (float)MOD_GetCessbIterations(); }
static void  set_cessb_iter_f(float v)
{
    const int32_t i = round_index(v, 4);
    MOD_SetCessbIterations((uint8_t)i);
}
#endif
static float get_comp_f(void) { return (float)MOD_GetCompEnable(); }
static void  set_comp_f(float v) { MOD_SetCompEnable(v > 0.5f ? 1U : 0U); }
static float get_comp_math_f(void) { return (float)MOD_GetCompMathMode(); }
static void  set_comp_math_f(float v)
{
    const int32_t i = round_index(v, (int32_t)COMP_MATH_COUNT);
    MOD_SetCompMathMode((comp_math_mode_t)i);
}
static float get_eq_f(void) { return (float)MOD_GetEqEnable(); }
static void  set_eq_f(float v) { MOD_SetEqEnable(v > 0.5f ? 1U : 0U); }
static float get_cordic_f(void) { return (float)MOD_GetCordicEnabled(); }
static void  set_cordic_f(float v) { MOD_SetCordicEnable(v > 0.5f ? 1U : 0U); }
static float get_usb_clean_f(void) { return (float)MOD_GetUsbAudioPolicy(); }
static void  set_usb_clean_f(float v)
{
    const int32_t i = round_index(v, (int32_t)USB_AUDIO_POLICY_COUNT);
    MOD_SetUsbAudioPolicy((usb_audio_policy_t)i);
}
static float get_fm_preset_f(void) { return (float)MOD_GetFmDeviationPreset(); }
static void  set_fm_preset_f(float v)
{
    const int32_t i = round_index(v, (int32_t)FM_DEVIATION_PRESET_COUNT);
    MOD_SetFmDeviationPreset((fm_deviation_preset_t)i);
}
static float get_fm_test_f(void) { return (float)MOD_GetFmTestTone(); }
static void  set_fm_test_f(float v) { MOD_SetFmTestTone(v > 0.5f ? 1U : 0U); }
static float get_am_profile_f(void) { return (float)MOD_GetAmProfile(); }
static void  set_am_profile_f(float v)
{
    const int32_t i = round_index(v, (int32_t)AM_PROFILE_COUNT);
    MOD_SetAmProfile((am_profile_t)i);
}
static float get_usb_chan_f(void) { return (float)MOD_GetUsbChannelMode(); }
static void  set_usb_chan_f(float v)
{
    const int32_t i = round_index(v, (int32_t)USB_AUDIO_CHAN_COUNT);
    MOD_SetUsbChannelMode((usb_audio_channel_mode_t)i);
}
static float get_dsp_math_f(void) { return (float)MOD_GetDspMathMode(); }
static void  set_dsp_math_f(float v)
{
    const int32_t i = round_index(v, (int32_t)DSP_MATH_COUNT);
    MOD_SetDspMathMode((dsp_math_mode_t)i);
}
static float get_gain_telem_f(void) { return (float)MOD_GetGainTelemetryMode(); }
static void  set_gain_telem_f(float v)
{
    const int32_t i = round_index(v, (int32_t)GAIN_TELEMETRY_COUNT);
    MOD_SetGainTelemetryMode((gain_telemetry_mode_t)i);
}
static float get_rf_metric_f(void) { return (float)MOD_GetRfMetricMode(); }
static void  set_rf_metric_f(float v) { MOD_SetRfMetricMode(v > 0.5f ? RF_METRIC_FAST : RF_METRIC_EXACT); }
static float get_outphase_fallback_f(void) { return (float)MOD_GetOutphaseFallbackPolicy(); }
static void  set_outphase_fallback_f(float v)
{
    const int32_t i = round_index(v, (int32_t)MOD_OUTPHASE_FALLBACK_COUNT);
    MOD_SetOutphaseFallbackPolicy((mod_outphase_fallback_t)i);
}
static float get_cw_keyer_f(void) { return (float)MOD_GetCwKeyerMode(); }
static void  set_cw_keyer_f(float v)
{
    const int32_t i = round_index(v, (int32_t)CW_KEYER_COUNT);
    MOD_SetCwKeyerMode((cw_keyer_mode_t)i);
}
static float get_cw_swap_f(void) { return (float)MOD_GetCwSwap(); }
static void  set_cw_swap_f(float v) { MOD_SetCwSwap(v > 0.5f ? 1U : 0U); }
static float get_cw_autospace_f(void)
{
    uint8_t d = MOD_GetCwAutoSpaceDits();
    if (d == 2U) return 1.0f;
    if (d == 3U) return 2.0f;
    if (d == 4U) return 3.0f;
    return 0.0f;
}
static void  set_cw_autospace_f(float v)
{
    int32_t i = (int32_t)(v + 0.5f);
    if (i <= 0) MOD_SetCwAutoSpaceDits(0U);
    else if (i == 1) MOD_SetCwAutoSpaceDits(2U);
    else if (i == 2) MOD_SetCwAutoSpaceDits(3U);
    else MOD_SetCwAutoSpaceDits(4U);
}
static float get_cw_breakin_f(void) { return (float)MOD_GetCwBreakInMode(); }
static void  set_cw_breakin_f(float v)
{
    const int32_t i = round_index(v, (int32_t)CW_BREAKIN_COUNT);
    MOD_SetCwBreakInMode((cw_breakin_mode_t)i);
}
static float get_cw_practice_f(void) { return (float)MOD_GetCwPractice(); }
static void  set_cw_practice_f(float v) { MOD_SetCwPractice(v > 0.5f ? 1U : 0U); }
static float get_cw_theta_trim_f(void) { return MOD_GetCwThetaTrim(); }
static void  set_cw_theta_trim_f(float v) { MOD_SetCwThetaTrim(v); }
static float get_cw_theta_model_f(void) { return (float)MOD_GetCwThetaModel(); }
static void  set_cw_theta_model_f(float v)
{
    const int32_t i = round_index(v, (int32_t)CW_THETA_MODEL_COUNT);
    MOD_SetCwThetaModel((cw_theta_model_t)i);
}
/* The Morse trainer's preferences: a byte each of mtrain_prefs_t, put in
 * range by MTRAIN_PrefsChanged. One getter and setter pair a byte. */
static float tp_get(uint32_t at) { return (float)((const uint8_t *)MTRAIN_Prefs())[at]; }
static void tp_set(uint32_t at, float v)
{
    ((uint8_t *)MTRAIN_Prefs())[at] = (uint8_t)((v < 0.0f) ? 0.0f : ((v > 255.0f) ? 255.0f : v + 0.5f));
    MTRAIN_PrefsChanged();
}
#define TP(f) \
    static float get_tp_##f(void) { return tp_get((uint32_t)offsetof(mtrain_prefs_t, f)); } \
    static void set_tp_##f(float v) { tp_set((uint32_t)offsetof(mtrain_prefs_t, f), v); }
TP(act) TP(src) TP(course) TP(carousel) TP(group) TP(listen_wpm) TP(pace_wpm) TP(key_wpm) TP(manual)
TP(ics) TP(iws) TP(retries) TP(think_s) TP(show) TP(tone) TP(boost) TP(adapt) TP(minutes)
#undef TP
/* SAVE PROGRESS, kept as its opposite so a record without it reads ON. */
static float get_tp_save(void) { return 1.0f - tp_get((uint32_t)offsetof(mtrain_prefs_t, nosave)); }
static void set_tp_save(float v) { tp_set((uint32_t)offsetof(mtrain_prefs_t, nosave), 1.0f - v); }
/* Answer wait in seconds, kept in tenths. */
static float get_tp_wait(void) { return tp_get((uint32_t)offsetof(mtrain_prefs_t, wait_ds)) * 0.1f; }
static void set_tp_wait(float v) { tp_set((uint32_t)offsetof(mtrain_prefs_t, wait_ds), v * 10.0f); }
/* The lesson is the learner's record, per course. */
static float get_tp_lesson(void)
{
    (void)MOD_TrainerRecordLoad(); /* once per card */
    const uint8_t l = MTRAIN_Course() ? MTRAIN_View()->lesson : MTRAIN_Progress()->lesson[MTRAIN_Prefs()->course];
    return (float)(l ? l : 2U);
}
static void set_tp_lesson(float v)
{
    MTRAIN_SetLesson((uint8_t)(v + 0.5f));
    (void)MOD_TrainerRecordSave();
}
/* Keyer presets: Iambic B with Morserino-32's Curtis 75/45, or classic
 * Iambic B (Curtis 0/0: every squeeze remembered). */
static void cw_preset(float dit, float dah, const char *done)
{
    MOD_SetCwKeyerMode(CW_KEYER_IAMBIC_B);
    MOD_SetCwCurtisBDitPct(dit);
    MOD_SetCwCurtisBDahPct(dah);
    UI_NavFlash(done);
}
static void set_cw_preset_m32(float v) { (void)v; cw_preset(75.0f, 45.0f, "IAMBIC B, CURTIS 75/45"); }
static void set_cw_preset_classic(float v) { (void)v; cw_preset(0.0f, 0.0f, "CLASSIC IAMBIC B"); }
static void set_cw_spot(float v) { (void)v; UI_NavFlash(MOD_CwSpot() ? "SPOT ON" : "SPOT OFF"); }
/* Radio memories: line n of CW/MEMORIES.TXT through the keyer; a click
 * while one is sending (or waiting to repeat) stops it. */
static void cw_memory(uint8_t n)
{
    char text[64];
    uint8_t code[48];
    const char *say = "SENDING";
    if (MOD_CwMemoryBusy()) {
        MOD_CwMemoryStop();
        say = "MEMORY STOPPED";
    } else if (!UI_CwMemoryText(n, text, sizeof(text))) {
        say = "NO CW/MEMORIES.TXT ON THE CARD";
    } else {
        const uint8_t len = MORSE_ParseText(text, code, sizeof(code));
        if (!len) say = text[0] ? "NOT MORSE OR OVER 48 SYMBOLS" : "EMPTY LINE";
        else if (!MOD_CwMemorySend(code, len)) say = "KEYER BUSY OR NOT ARMED";
    }
    UI_NavFlash(say);
}
#define MEM_SET(n) static void set_cw_mem##n(float v) { (void)v; cw_memory(n - 1U); }
MEM_SET(1) MEM_SET(2) MEM_SET(3) MEM_SET(4) MEM_SET(5) MEM_SET(6) MEM_SET(7) MEM_SET(8)
#undef MEM_SET
static float get_cw_mem_repeat(void) { return MOD_GetCwMemoryRepeatS(); }
static void set_cw_mem_repeat(float v) { MOD_SetCwMemoryRepeatS(v); }
static void set_mtrain_enter_f(float v)
{
    (void)v;
    UI_EnterCwTrainerPage();
}

static float get_cw_pdl_prot_f(void) { return (float)MOD_GetCwPaddleProtectMode(); }
static void  set_cw_pdl_prot_f(float v)
{
    const int32_t i = round_index(v, (int32_t)CW_PADDLE_PROTECT_COUNT);
    MOD_SetCwPaddleProtectMode((cw_paddle_protect_mode_t)i);
}
static float get_cw_both_f(void) { return (float)MOD_GetCwBothPolicy(); }
static void  set_cw_both_f(float v)
{
    const int32_t i = round_index(v, (int32_t)CW_BOTH_COUNT);
    MOD_SetCwBothPolicy((cw_both_policy_t)i);
}
static float get_txt_both_f(void) { return (float)MOD_GetTxTestBothPolicy(); }
static void  set_txt_both_f(float v)
{
    const int32_t i = round_index(v, (int32_t)TXTEST_BOTH_COUNT);
    MOD_SetTxTestBothPolicy((tx_test_both_policy_t)i);
}
static float get_micagc_f(void) { return (float)MOD_GetMicAgcEnable(); }
static void  set_micagc_f(float v) { MOD_SetMicAgcEnable(v > 0.5f ? 1U : 0U); }
static float get_ng_f(void) { return (float)MOD_GetNoiseGateEnable(); }
static void  set_ng_f(float v) { MOD_SetNoiseGateEnable(v > 0.5f ? 1U : 0U); }
static float get_alc_f(void) { return (float)MOD_GetAlcEnable(); }
static void  set_alc_f(float v) { MOD_SetAlcEnable(v > 0.5f ? 1U : 0U); }
static float get_deadtime_f(void) { return MOD_GetDeadTimeRequestedNs(); }
static void  set_deadtime_f(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > MOD_GetDeadTimeMaxNs()) v = MOD_GetDeadTimeMaxNs();
    MOD_SetDeadTimeNs(v);
}
static float get_deadtime_rise_f(void) { return MOD_GetDeadTimeRiseRequestedNs(); }
static void  set_deadtime_rise_f(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > MOD_GetDeadTimeMaxNs()) v = MOD_GetDeadTimeMaxNs();
    MOD_SetDeadTimeRiseNs(v);
}
static float get_deadtime_fall_f(void) { return MOD_GetDeadTimeFallRequestedNs(); }
static void  set_deadtime_fall_f(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > MOD_GetDeadTimeMaxNs()) v = MOD_GetDeadTimeMaxNs();
    MOD_SetDeadTimeFallNs(v);
}
static float get_ac_offset_f(void) { return (float)MOD_GetBranchAcOffsetTicks(); }
static void  set_ac_offset_f(float v) {
    int32_t i = (v >= 0.0f) ? (int32_t)(v + 0.5f) : (int32_t)(v - 0.5f);
    if (i > 1024) i = 1024;
    if (i < -1024) i = -1024;
    MOD_SetBranchAcOffsetTicks((int16_t)i);
}
static float get_duty_f(void) { return (float)MOD_GetDutyOffset(); }
static void  set_duty_f(float v) {
    int32_t i = (v >= 0.0f) ? (int32_t)(v + 0.5f) : (int32_t)(v - 0.5f);
    if (i > 1024) i = 1024;
    if (i < -1024) i = -1024;
    MOD_SetDutyOffset((int16_t)i);
}
/* Offered NORMAL first; the stored values keep the v8.05 order (pa_geometry_t). */
static float get_pa_geometry_f(void)
{
    return (MOD_GetPaGeometry() == PA_GEOMETRY_A_INVERTED) ? 1.0f : 0.0f;
}
static void set_pa_geometry_f(float v)
{
    MOD_SetPaGeometry((v > 0.5f) ? PA_GEOMETRY_A_INVERTED : PA_GEOMETRY_NORMAL);
}
static float get_mute_f(void) { return (float)MOD_GetMuted(); }
static void  set_mute_f(float v) { MOD_SetMuted(v > 0.5f ? 1U : 0U); }
static float get_rt_diag_f(void) { return (float)MOD_GetRuntimeDiagEnable(); }
static void  set_rt_diag_f(float v) { MOD_SetRuntimeDiagEnable(v > 0.5f ? 1U : 0U); }
static float get_cpu_diag_f(void) { return (float)MOD_GetCpuDiagEnable(); }
static void  set_cpu_diag_f(float v) { MOD_SetCpuDiagEnable(v > 0.5f ? 1U : 0U); }
static float get_cpu_scope_f(void) { return (float)MOD_GetCpuScopeMode(); }
static void  set_cpu_scope_f(float v)
{
    const int32_t i = round_index(v, (int32_t)MOD_CPU_SCOPE_COUNT);
    MOD_SetCpuScopeMode((mod_cpu_scope_mode_t)i);
}
static float get_ssb_pipe_f(void) { return (float)MOD_GetSsbPipelineMode(); }
static void  set_ssb_pipe_f(float v)
{
    const int32_t i = round_index(v, (int32_t)SSB_PIPELINE_COUNT);
    MOD_SetSsbPipelineMode((ssb_pipeline_mode_t)i);
}
static float get_dbg_branch_f(void) { return (float)MOD_GetDebugBranchMode(); }
static void  set_dbg_branch_f(float v)
{
    const int32_t i = round_index(v, (int32_t)DEBUG_BRANCH_COUNT);
    MOD_SetDebugBranchMode((debug_branch_mode_t)i);
}
static float get_dbg_cal_f(void) { return (float)MOD_GetDebugCalBypass(); }
static void  set_dbg_cal_f(float v) { MOD_SetDebugCalBypass(v > 0.5f ? 1U : 0U); }
static float get_rfen_on_f(void) { return MOD_GetRfEnAssertMetric(); }
static void  set_rfen_on_f(float v) { MOD_SetRfEnAssertMetric(v); }
static float get_rfen_off_f(void) { return MOD_GetRfEnReleaseMetric(); }
static void  set_rfen_off_f(float v) { MOD_SetRfEnReleaseMetric(v); }
static float get_rfen_hold_f(void) { return MOD_GetRfEnHoldFrames(); }
static void  set_rfen_hold_f(float v)
{
    const int32_t i = round_index(v, 51);
    MOD_SetRfEnHoldFrames((uint8_t)i);
}

static float get_clk_policy_f(void) { return (float)MOD_GetCarrierClockPolicy(); }
static void  set_clk_policy_f(float v)
{
    const int32_t i = round_index(v, (int32_t)MOD_CLOCK_POLICY_COUNT);
    MOD_SetCarrierClockPolicy((mod_clock_policy_t)i);
}

/* REF CAL: the reference correction, shown and entered in ppm to 1 ppb. */
static float get_ref_ppm_f(void) { return (float)RADIO_Config()->ref_ppb * 0.001f; }
static void  set_ref_ppm_f(float v)
{
    v = fminf(100.0f, fmaxf(-100.0f, v));
    RADIO_SetRefPpb((int32_t)(v * 1000.0f + (v >= 0.0f ? 0.5f : -0.5f)));
}
/* REF SOURCE: USB HOST follows the host's SOF (REV1's DSC1003 wanders by a
 * ppm in seconds; radio_clocks.c), LOCAL trusts the board's own oscillator
 * with REF CAL alone -- for a TCXO, which the host's clock would only make
 * worse. The same switch as the console's `clock usb on|off`. */
static float get_ref_source_f(void) { return RADIO_RefFollowEnabled() ? 0.0f : 1.0f; }
static void  set_ref_source_f(float v) { RADIO_RefFollowEnable((uint8_t)(v < 0.5f)); }
static float clock_flag_get(uint8_t flag) { return (RADIO_Config()->clock_flags & flag) ? 1.0f : 0.0f; }
static void clock_flag_set(uint8_t flag, float v)
{
    const uint8_t flags = RADIO_Config()->clock_flags;
    RADIO_SetClockFlags((uint8_t)(v > 0.5f ? (flags | flag) : (flags & (uint8_t)~flag)));
}
static float get_clk_park_f(void) { return clock_flag_get(RADIO_CLOCK_FLAG_PARK_TX); }
static void  set_clk_park_f(float v) { clock_flag_set(RADIO_CLOCK_FLAG_PARK_TX, v); }
static float get_clk_wide_f(void) { return clock_flag_get(RADIO_CLOCK_FLAG_WIDE); }
static void  set_clk_wide_f(float v) { clock_flag_set(RADIO_CLOCK_FLAG_WIDE, v); }
static float get_clk_phase_f(void) { return clock_flag_get(RADIO_CLOCK_FLAG_PHASE384); }
static void  set_clk_phase_f(float v) { clock_flag_set(RADIO_CLOCK_FLAG_PHASE384, v); }
static float get_rx_qcomb_f(void) { return clock_flag_get(RADIO_CLOCK_FLAG_QCOMB); }
static void  set_rx_qcomb_f(float v) { clock_flag_set(RADIO_CLOCK_FLAG_QCOMB, v); }

/* CW receive. Written in place (RADIO_SetCwReceive): the receiver restarts
 * with the new pitch, filter or AGC at once, so it can be judged by ear. */
static void cw_rx_set(uint8_t pitch_10hz, uint8_t bw_10hz, uint8_t flags)
{
    (void)RADIO_SetCwReceive(pitch_10hz, bw_10hz, flags);
}
static float get_cw_pitch_f(void) { return (float)RADIO_ConfigCwPitchHz(RADIO_Config()); }
static void  set_cw_pitch_f(float v)
{
    const radio_config_t *c = RADIO_Config();
    int32_t hz = (int32_t)(v + 5.0f) / 10 * 10;
    if (hz < (int32_t)RADIO_CW_PITCH_MIN_HZ) hz = (int32_t)RADIO_CW_PITCH_MIN_HZ;
    if (hz > (int32_t)RADIO_CW_PITCH_MAX_HZ) hz = (int32_t)RADIO_CW_PITCH_MAX_HZ;
    cw_rx_set((uint8_t)(hz / 10), c->cw_bw_10hz, c->cw_flags);
}
static float get_cw_rx_bw_f(void) { return (float)RADIO_ConfigCwBandwidthIndex(RADIO_Config()); }
static void  set_cw_rx_bw_f(float v)
{
    static const uint16_t bandwidths[RADIO_CW_BW_COUNT] = RADIO_CW_BW_TABLE;
    const radio_config_t *c = RADIO_Config();
    const int32_t i = round_index(v, (int32_t)RADIO_CW_BW_COUNT);
    cw_rx_set(c->cw_pitch_10hz, (uint8_t)(bandwidths[i] / 10U), c->cw_flags);
}
static float get_cw_agc_f(void) { return RADIO_ConfigCwAgc(RADIO_Config()) == RADIO_CW_AGC_FAST ? 1.0f : 0.0f; }
static void  set_cw_agc_f(float v)
{
    const radio_config_t *c = RADIO_Config();
    const uint8_t agc = v > 0.5f ? RADIO_CW_AGC_FAST : RADIO_CW_AGC_SLOW;
    cw_rx_set(c->cw_pitch_10hz, c->cw_bw_10hz, (uint8_t)((c->cw_flags & (uint8_t)~RADIO_CW_AGC_MASK) | agc));
}
static float get_cw_decode_f(void) { return (float)RADIO_ConfigCwDecode(RADIO_Config()); }
static void  set_cw_decode_f(float v)
{
    const radio_config_t *c = RADIO_Config();
    const uint8_t flags = v > 0.5f ? (uint8_t)(c->cw_flags & (uint8_t)~RADIO_CW_FLAG_NO_DECODE)
                                   : (uint8_t)(c->cw_flags | RADIO_CW_FLAG_NO_DECODE);
    cw_rx_set(c->cw_pitch_10hz, c->cw_bw_10hz, flags);
}

/* Voice receive (RADIO_SetVoiceReceive), written in place like CW's: the
 * receiver restarts with the new filter, AGC or squelch at once. */
static void voice_rx_set(uint8_t ssb, uint8_t am, uint8_t agc, uint8_t sql)
{
    (void)RADIO_SetVoiceReceive(ssb, am, agc, sql);
}
static int32_t index_clamp(float v, int32_t count)
{
    int32_t i = (int32_t)(v + 0.5f);
    return i < 0 ? 0 : (i >= count ? count - 1 : i);
}
static float get_ssb_rx_bw_f(void) { return (float)RADIO_ConfigSsbBandwidthIndex(RADIO_Config()); }
static void  set_ssb_rx_bw_f(float v)
{
    const radio_config_t *c = RADIO_Config();
    const uint16_t hz = RADIO_SsbBandwidthAt((uint8_t)index_clamp(v, (int32_t)RADIO_SSB_BW_COUNT));
    voice_rx_set((uint8_t)(hz / 100U), c->am_bw_khz, c->rx_agc, c->fm_squelch);
}
static float get_am_rx_bw_f(void) { return (float)RADIO_ConfigAmBandwidthIndex(RADIO_Config()); }
static void  set_am_rx_bw_f(float v)
{
    const radio_config_t *c = RADIO_Config();
    const uint16_t hz = RADIO_AmBandwidthAt((uint8_t)index_clamp(v, (int32_t)RADIO_AM_BW_COUNT));
    voice_rx_set(c->ssb_bw_100hz, (uint8_t)(hz / 1000U), c->rx_agc, c->fm_squelch);
}
/* AGC choices in the order the knob walks them: OFF, SLOW, MED, FAST. */
static float get_rx_agc_f(void) { return (float)(RADIO_ConfigRxAgc(RADIO_Config()) - RADIO_RX_AGC_OFF); }
static void  set_rx_agc_f(float v)
{
    const radio_config_t *c = RADIO_Config();
    voice_rx_set(c->ssb_bw_100hz, c->am_bw_khz, (uint8_t)(RADIO_RX_AGC_OFF + index_clamp(v, 4)), c->fm_squelch);
}
static float get_fm_squelch_f(void) { return (float)RADIO_Config()->fm_squelch; }
static void  set_fm_squelch_f(float v)
{
    const radio_config_t *c = RADIO_Config();
    voice_rx_set(c->ssb_bw_100hz, c->am_bw_khz, c->rx_agc, (uint8_t)index_clamp(v, (int32_t)RADIO_FM_SQUELCH_MAX + 1));
}

static float get_home_layout_f(void) { return (float)MOD_GetUiHomeLayout(); }
static void  set_home_layout_f(float v)
{
    const int32_t i = round_index(v, (int32_t)UI_HOME_LAYOUT_COUNT);
    MOD_SetUiHomeLayout((ui_home_layout_t)i);
}
static float get_home_meter_f(void) { return (MOD_GetUiHomeFlags() & UI_HOME_FLAG_METER) ? 1.0f : 0.0f; }
static void  set_home_meter_f(float v) { uint8_t flags = MOD_GetUiHomeFlags(); if (v > 0.5f) flags |= UI_HOME_FLAG_METER; else flags &= (uint8_t)~UI_HOME_FLAG_METER; MOD_SetUiHomeFlags(flags); }
static float get_home_gains_f(void) { return (MOD_GetUiHomeFlags() & UI_HOME_FLAG_GAINS) ? 1.0f : 0.0f; }
static void  set_home_gains_f(float v) { uint8_t flags = MOD_GetUiHomeFlags(); if (v > 0.5f) flags |= UI_HOME_FLAG_GAINS; else flags &= (uint8_t)~UI_HOME_FLAG_GAINS; MOD_SetUiHomeFlags(flags); }
static float get_home_health_f(void) { return (MOD_GetUiHomeFlags() & UI_HOME_FLAG_HEALTH) ? 1.0f : 0.0f; }
static void  set_home_health_f(float v) { uint8_t flags = MOD_GetUiHomeFlags(); if (v > 0.5f) flags |= UI_HOME_FLAG_HEALTH; else flags &= (uint8_t)~UI_HOME_FLAG_HEALTH; MOD_SetUiHomeFlags(flags); }
static float get_home_rfen_f(void) { return (MOD_GetUiHomeFlags() & UI_HOME_FLAG_RFEN) ? 1.0f : 0.0f; }
static void  set_home_rfen_f(float v) { uint8_t flags = MOD_GetUiHomeFlags(); if (v > 0.5f) flags |= UI_HOME_FLAG_RFEN; else flags &= (uint8_t)~UI_HOME_FLAG_RFEN; MOD_SetUiHomeFlags(flags); }
static float get_home_vref_f(void) { return (MOD_GetUiHomeFlags() & UI_HOME_FLAG_VREF) ? 1.0f : 0.0f; }
static void  set_home_vref_f(float v) { uint8_t flags = MOD_GetUiHomeFlags(); if (v > 0.5f) flags |= UI_HOME_FLAG_VREF; else flags &= (uint8_t)~UI_HOME_FLAG_VREF; MOD_SetUiHomeFlags(flags); }
static float get_home_hints_f(void) { return (MOD_GetUiHomeFlags() & UI_HOME_FLAG_HINTS) ? 1.0f : 0.0f; }
static void  set_home_hints_f(float v) { uint8_t flags = MOD_GetUiHomeFlags(); if (v > 0.5f) flags |= UI_HOME_FLAG_HINTS; else flags &= (uint8_t)~UI_HOME_FLAG_HINTS; MOD_SetUiHomeFlags(flags); }
static float get_shortcut_up_f(void) { return (float)MOD_GetUiShortcutUp(); }
static void  set_shortcut_up_f(float v)
{
    const int32_t i = round_index(v, (int32_t)UI_SHORTCUT_COUNT);
    MOD_SetUiShortcutUp((ui_shortcut_action_t)i);
}
static float get_shortcut_down_f(void) { return (float)MOD_GetUiShortcutDown(); }
static void  set_shortcut_down_f(float v)
{
    const int32_t i = round_index(v, (int32_t)UI_SHORTCUT_COUNT);
    MOD_SetUiShortcutDown((ui_shortcut_action_t)i);
}

/* The REV1 board block's plain fields. Each setting copies the
 * configuration, clamps the value to [lo, hi], stores it in the field's own
 * type (rounded half up when `round`) and commits only what
 * RADIO_ConfigValid accepts -- the fail-closed write each of them spelled
 * out on its own before. A field is its offset and type in radio_config_t,
 * so the pair of accessors per setting is two tail calls. */
enum { R1_U8, R1_U16, R1_U32, R1_F32 };
typedef struct {
    uint16_t offset;
    uint8_t  type, round;
    float    lo, hi;
} rev1_field_t;
#define R1_TYPE(field) _Generic(((radio_config_t *)0)->field, \
    uint8_t: R1_U8, uint16_t: R1_U16, uint32_t: R1_U32, float: R1_F32)
static float rev1_get(const rev1_field_t *f)
{
    const void *p = (const uint8_t *)RADIO_Config() + f->offset;
    switch (f->type) {
        case R1_U8:  return (float)*(const uint8_t *)p;
        case R1_U16: return (float)*(const uint16_t *)p;
        case R1_U32: return (float)*(const uint32_t *)p;
        default:     return *(const float *)p;
    }
}
static void rev1_set(const rev1_field_t *f, float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    v = fminf(f->hi, fmaxf(f->lo, v));
    if (f->round) v += 0.5f;
    void *p = (uint8_t *)&c + f->offset;
    switch (f->type) {
        case R1_U8:  *(uint8_t *)p = (uint8_t)v; break;
        case R1_U16: *(uint16_t *)p = (uint16_t)v; break;
        case R1_U32: *(uint32_t *)p = (uint32_t)v; break;
        default:     *(float *)p = v; break;
    }
    if (RADIO_ConfigValid(&c)) RADIO_SetConfig(&c);
}
#define REV1_FIELD(name, field, lo, hi, round) \
    static const rev1_field_t rev1_##name = {(uint16_t)offsetof(radio_config_t, field), \
                                             R1_TYPE(field), round, lo, hi}; \
    static float get_rev1_##name(void) { return rev1_get(&rev1_##name); } \
    static void set_rev1_##name(float v) { rev1_set(&rev1_##name, v); }

REV1_FIELD(DRIVER_A, driver_a, 0.0f, 2.0f, 0U)
REV1_FIELD(DRIVER_B, driver_b, 0.0f, 2.0f, 0U)
static float get_rev1_DRAIN_A(void) { return (float)RADIO_Config()->drain_a / 1.0f; }
static void set_rev1_DRAIN_A(float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    v = fminf(2.0f, fmaxf(0.0f, v));
    RADIO_ConfigSetDrain(&c, 0U, (uint8_t)v);
    if (RADIO_ConfigValid(&c)) RADIO_SetConfig(&c);
}
static float get_rev1_DRAIN_B(void) { return (float)RADIO_Config()->drain_b / 1.0f; }
static void set_rev1_DRAIN_B(float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    v = fminf(2.0f, fmaxf(0.0f, v));
    RADIO_ConfigSetDrain(&c, 1U, (uint8_t)v);
    if (RADIO_ConfigValid(&c)) RADIO_SetConfig(&c);
}
REV1_FIELD(BUTTONS, buttons_high, 0.0f, 1.0f, 0U)
REV1_FIELD(SW4, sw4, 0.0f, (float)RADIO_SW_LAST, 0U)
REV1_FIELD(SW5, sw5, 0.0f, (float)RADIO_SW_LAST, 0U)

/* Encoder pin pickers. RADIO_ConfigValid rejects a duplicate selection, so the
 * whole-struct commit below simply refuses it and the menu keeps the old pin;
 * that is the same fail-closed behaviour every other board setting has. */
REV1_FIELD(ENC_A, enc_a, 0.0f, (float)(RADIO_ENC_PIN_COUNT - 1U), 0U)
REV1_FIELD(ENC_B, enc_b, 0.0f, (float)(RADIO_ENC_PIN_COUNT - 1U), 0U)
REV1_FIELD(ENC_SW, enc_sw, 0.0f, (float)(RADIO_ENC_PIN_COUNT - 1U), 0U)
static float get_rev1_enc_flag(uint8_t mask) { return (RADIO_Config()->enc_flags & mask) ? 1.0f : 0.0f; }
static void set_rev1_enc_flag(uint8_t mask, float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    c.enc_flags = (uint8_t)(v > 0.5f ? (c.enc_flags | mask) : (c.enc_flags & (uint8_t)~mask));
    if (RADIO_ConfigValid(&c)) RADIO_SetConfig(&c);
}
static float get_rev1_ENC_HIGH(void) { return get_rev1_enc_flag(RADIO_ENC_FLAG_HIGH); }
static void set_rev1_ENC_HIGH(float v) { set_rev1_enc_flag(RADIO_ENC_FLAG_HIGH, v); }
static float get_rev1_ENC_REVERSE(void) { return get_rev1_enc_flag(RADIO_ENC_FLAG_REVERSE); }
static void set_rev1_ENC_REVERSE(float v) { set_rev1_enc_flag(RADIO_ENC_FLAG_REVERSE, v); }
static float get_rev1_ENC_NOPULL(void) { return get_rev1_enc_flag(RADIO_ENC_FLAG_NOPULL); }
static void set_rev1_ENC_NOPULL(float v) { set_rev1_enc_flag(RADIO_ENC_FLAG_NOPULL, v); }
REV1_FIELD(ENC_DETENT, enc_detent, (float)RADIO_ENC_DETENT_MIN, (float)RADIO_ENC_DETENT_MAX, 0U)
static float get_rev1_BACKLIGHT(void) { return (float)RADIO_Config()->backlight / 1.0f; }
static void set_rev1_BACKLIGHT(float v) { RADIO_SetBacklight((uint8_t)fminf(100.0f, fmaxf((float)RADIO_BACKLIGHT_MIN, v + 0.5f))); }
static float get_rev1_VOLUME(void) { return (float)RADIO_Config()->volume / 1.0f; }
/* Operator trims write in place (RADIO_SetVolume and friends): going through
 * RADIO_SetConfig would restart the receiver on every detent. */
static void set_rev1_VOLUME(float v) { RADIO_SetVolume((uint8_t)fminf(100.0f, fmaxf(0.0f, v + 0.5f))); }
static float get_rev1_RX_MODE(void) { return (float)RADIO_Config()->rx_mode / 1.0f; }
static void set_rev1_RX_MODE(float v) { (void)RADIO_SetRxMode((uint8_t)fminf(4.0f, fmaxf(0.0f, v + 0.5f))); }
REV1_FIELD(MIC, mic_source, 0.0f, 3.0f, 0U)
static float get_rev1_MIC_GAIN(void) { return (float)RADIO_Config()->mic_gain / 1.0f; }
static void set_rev1_MIC_GAIN(float v) { RADIO_SetMicGain(v); }
REV1_FIELD(USB_IQ, usb_iq, 0.0f, 1.0f, 0U)
REV1_FIELD(ENV_MODE, envelope_mode, 0.0f, 2.0f, 0U)
REV1_FIELD(BIAS, bias_ma, (float)RADIO_BIAS_MA_OFF, (float)RADIO_BIAS_MA_MAX, 1U)
REV1_FIELD(BIAS_LIMIT, bias_limit_mv, 0.0f, 2850.0f, 0U)
REV1_FIELD(VIN_LIMIT, vin_limit_mv, 5000.0f, 14700.0f, 0U)
REV1_FIELD(ENV_LIMIT, envelope_limit_mv, 0.0f, 14000.0f, 0U)
REV1_FIELD(ENV_A, envelope_dac_per_volt[0], 0.0f, 4095.0f, 0U)
REV1_FIELD(ENV_B, envelope_dac_per_volt[1], 0.0f, 4095.0f, 0U)
REV1_FIELD(SWR, swr_limit_x100, 120.0f, 1000.0f, 0U)
REV1_FIELD(FWD_ZERO, swr_offset[0], -2.9f, 2.9f, 0U)
REV1_FIELD(REV_ZERO, swr_offset[1], -2.9f, 2.9f, 0U)
REV1_FIELD(FWD_GAIN, swr_gain[0], 0.01f, 100.0f, 0U)
REV1_FIELD(REV_GAIN, swr_gain[1], 0.01f, 100.0f, 0U)
/* The meter's settings write in place (RADIO_SetMeterConfig): changing them
 * does not restart the receiver the way the board block's others do. */
static float get_tx_panel_f(void) { return (float)RADIO_ConfigTxPanel(RADIO_Config()); }
static void set_tx_panel_f(float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    if (v > 0.5f) c.meter_flags &= (uint8_t)~RADIO_METER_FLAG_PANEL_OFF;
    else c.meter_flags |= RADIO_METER_FLAG_PANEL_OFF;
    (void)RADIO_SetMeterConfig(&c);
}
static float get_meter_update_f(void) { return (float)RADIO_ConfigMeterUpdateMs(RADIO_Config()); }
static void set_meter_update_f(float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    v = fminf((float)RADIO_METER_UPDATE_MAX_MS, fmaxf((float)RADIO_METER_UPDATE_MIN_MS, v));
    c.meter_update_10ms = (uint8_t)(v * 0.1f + 0.5f);
    (void)RADIO_SetMeterConfig(&c);
}
static float get_meter_hang_f(void) { return (float)RADIO_ConfigMeterHangMs(RADIO_Config()); }
static void set_meter_hang_f(float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    v = fminf((float)RADIO_METER_HANG_MAX_MS, fmaxf(100.0f, v));
    c.meter_hang_100ms = (uint8_t)(v * 0.01f + 0.5f);
    (void)RADIO_SetMeterConfig(&c);
}
static float get_usb_vox_f(void) { return (float)MOD_GetUsbVox(); }
static void  set_usb_vox_f(float v) { MOD_SetUsbVox((uint8_t)(v > 0.5f)); }
static float get_digi_tone_f(void) { return (float)MOD_GetDigiTone(); }
static void  set_digi_tone_f(float v) { MOD_SetDigiTone((uint8_t)(v > 0.5f)); }
static float get_tune_pct_f(void) { return (float)RADIO_ConfigTunePct(RADIO_Config()); }
static void set_tune_pct_f(float v)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    v = fminf(100.0f, fmaxf((float)RADIO_TUNE_PCT_MIN, v));
    c.tune_pct = (uint8_t)(v + 0.5f);
    (void)RADIO_SetMeterConfig(&c);
}
REV1_FIELD(AUX, aux_tx_mask, 0.0f, 7.0f, 0U)
REV1_FIELD(PD, pd_enable, 0.0f, 1.0f, 0U)
REV1_FIELD(IQ_INVERT, rx_iq_invert, 0.0f, 1.0f, 0U)
/* Starts the BMC receiver on the orientation sampled at boot. R3/R4 remain
 * the only Rd termination; the internal pulls stay off. */
static void rev1_pd_fix(float v) { (void)v; (void)RADIO_PdProbeStart(); }
REV1_FIELD(PD_RD, pd_internal_rd, 0.0f, 1.0f, 0U)
REV1_FIELD(PD_MV, pd_target_mv, 5000.0f, 14000.0f, 0U)
REV1_FIELD(PD_MAX, pd_max_mv, 5000.0f, 14000.0f, 0U)
REV1_FIELD(PD_MA, pd_current_ma, 100.0f, 3000.0f, 0U)
REV1_FIELD(IQ_I, rx_i_gain, 0.1f, 10.0f, 0U)
REV1_FIELD(IQ_Q, rx_q_gain, 0.1f, 10.0f, 0U)
REV1_FIELD(IQ_SKEW, rx_skew_samples, -RADIO_IQ_SKEW_MAX, RADIO_IQ_SKEW_MAX, 0U)
/* Filter slots follow the PCB channel, not frequency order; only keep the
 * configured upper edges distinct (RADIO_ConfigValid rejects duplicates). */
static void set_filter_edge(uint8_t lpf, unsigned slot, float mhz)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    uint32_t *edges = lpf ? c.lpf_upper_hz : c.bpf_upper_hz;
    unsigned count = lpf ? 5U : 4U;
    uint32_t hz = (uint32_t)(fminf(28.333333f, fmaxf(0.0f, mhz)) * 1000000.0f);
    for (unsigned pass = 0U; hz && pass < count; ++pass)
        for (unsigned j = 0U; j < count; ++j)
            if (j != slot && edges[j] == hz && hz + 100000UL <= 28333333UL) hz += 100000UL;
    edges[slot] = hz;
    if (RADIO_ConfigValid(&c)) RADIO_SetConfig(&c);
}
static float get_rev1_LPF0(void) { return (float)RADIO_Config()->lpf_upper_hz[0] / 1000000.0f; }
static void set_rev1_LPF0(float v) { set_filter_edge(1U, 0U, v); }
static float get_rev1_LPF1(void) { return (float)RADIO_Config()->lpf_upper_hz[1] / 1000000.0f; }
static void set_rev1_LPF1(float v) { set_filter_edge(1U, 1U, v); }
static float get_rev1_LPF2(void) { return (float)RADIO_Config()->lpf_upper_hz[2] / 1000000.0f; }
static void set_rev1_LPF2(float v) { set_filter_edge(1U, 2U, v); }
static float get_rev1_LPF3(void) { return (float)RADIO_Config()->lpf_upper_hz[3] / 1000000.0f; }
static void set_rev1_LPF3(float v) { set_filter_edge(1U, 3U, v); }
static float get_rev1_LPF4(void) { return (float)RADIO_Config()->lpf_upper_hz[4] / 1000000.0f; }
static void set_rev1_LPF4(float v) { set_filter_edge(1U, 4U, v); }
/* Harmonic-safe TX floor of an LPF slot, MHz in the UI, kHz in the config. */
static void set_lpf_tx_min(unsigned slot, float mhz)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    c.lpf_tx_min_khz[slot] = (uint16_t)(fminf(28.333f, fmaxf(0.0f, mhz)) * 1000.0f + 0.5f);
    if (RADIO_ConfigValid(&c)) RADIO_SetConfig(&c);
}
static float get_lpf_tx_min(unsigned slot) { return (float)RADIO_Config()->lpf_tx_min_khz[slot] / 1000.0f; }
static float get_rev1_LPF_TX0(void) { return get_lpf_tx_min(0U); }
static void set_rev1_LPF_TX0(float v) { set_lpf_tx_min(0U, v); }
static float get_rev1_LPF_TX1(void) { return get_lpf_tx_min(1U); }
static void set_rev1_LPF_TX1(float v) { set_lpf_tx_min(1U, v); }
static float get_rev1_LPF_TX2(void) { return get_lpf_tx_min(2U); }
static void set_rev1_LPF_TX2(float v) { set_lpf_tx_min(2U, v); }
static float get_rev1_LPF_TX3(void) { return get_lpf_tx_min(3U); }
static void set_rev1_LPF_TX3(float v) { set_lpf_tx_min(3U, v); }
static float get_rev1_LPF_TX4(void) { return get_lpf_tx_min(4U); }
static void set_rev1_LPF_TX4(float v) { set_lpf_tx_min(4U, v); }
static float get_rev1_BPF0(void) { return (float)RADIO_Config()->bpf_upper_hz[0] / 1000000.0f; }
static void set_rev1_BPF0(float v) { set_filter_edge(0U, 0U, v); }
static float get_rev1_BPF1(void) { return (float)RADIO_Config()->bpf_upper_hz[1] / 1000000.0f; }
static void set_rev1_BPF1(float v) { set_filter_edge(0U, 1U, v); }
static float get_rev1_BPF2(void) { return (float)RADIO_Config()->bpf_upper_hz[2] / 1000000.0f; }
static void set_rev1_BPF2(float v) { set_filter_edge(0U, 2U, v); }
static float get_rev1_BPF3(void) { return (float)RADIO_Config()->bpf_upper_hz[3] / 1000000.0f; }
static void set_rev1_BPF3(float v) { set_filter_edge(0U, 3U, v); }
REV1_FIELD(QUAL, qualified_bands, 0.0f, 31.0f, 0U)
REV1_FIELD(CONFIRM, confirmed, 0.0f, 1.0f, 0U)
static void rev1_clear(float v) { (void)v; RADIO_ClearFault(); }
static void rev1_off(float v) { (void)v; RADIO_Shutdown(); }

static uint16_t rev1_cal_code = 64U;
static float get_rev1_cal_code(void) { return (float)rev1_cal_code; }
static void set_rev1_cal_code(float v) { rev1_cal_code = (uint16_t)fminf(1024.0f, fmaxf(0.0f, v)); }
/* The v7 BOARD SIGNALS page: its readings are the diagnostics POWER page now. */
static void rev1_monitor(float v) { (void)v; UI_NavOpenDiag(UI_DIAG_PAGE_POWER); }
static void rev1_pd_info(float v) { (void)v; UI_NavOpenPage(PAGE_PD); }
static void rev1_cal_run(uint8_t mode)
{
    if (RADIO_CalStart(mode, rev1_cal_code)) rev1_monitor(0.0f);
    else UI_NavFlash("CALIBRATION BLOCKED");
}
static void rev1_cal_bias(float v) { (void)v; rev1_cal_run(RADIO_CAL_BIAS); }
static void rev1_cal_a(float v) { (void)v; rev1_cal_run(RADIO_CAL_ENV_A); }
static void rev1_cal_b(float v) { (void)v; rev1_cal_run(RADIO_CAL_ENV_B); }
static void rev1_cal_rx(float v) { (void)v; rev1_cal_run(RADIO_CAL_RX_TEST); }
static void rev1_cal_stop(float v) { (void)v; RADIO_CalStop(); }
static void rev1_flash(const char *text) { UI_NavFlash(text); }
__attribute__((cold)) static void rev1_bpf_sweep(float v)
{
    (void)v;
    if (RADIO_CalStart(RADIO_CAL_BPF_SWEEP, 0U)) UI_NavOpenPage(PAGE_BPF_SWEEP);
    else rev1_flash("SWEEP NEEDS RECEIVE");
}
/* The QRP Labs hardware tests that have an LCD page. Each reserves the same
 * calibration interlock as BPF SWEEP and opens its plot; the sweep engines
 * themselves are in radio_bpf_sweep.c and radio_af_sweep.c. */
__attribute__((cold)) static void rev1_sweep_axis(uint16_t axis, const char *busy)
{
    if (RADIO_CalStart(RADIO_CAL_BPF_SWEEP, axis)) UI_NavOpenPage(PAGE_BPF_SWEEP);
    else rev1_flash(busy);
}
__attribute__((cold)) static void rev1_lpf_sweep(float v)
{
    (void)v;
    rev1_sweep_axis(RADIO_SWEEP_LPF, "LPF SWEEP NEEDS RECEIVE");
}
__attribute__((cold)) static void rev1_rf_sweep(float v)
{
    (void)v;
    rev1_sweep_axis(RADIO_SWEEP_BAND, "RF SWEEP NEEDS RECEIVE");
}
__attribute__((cold)) static void rev1_af_sweep_run(float v)
{
    (void)v;
    if (RADIO_CalStart(RADIO_CAL_AF_SWEEP, 0U)) UI_NavOpenPage(PAGE_AF_SWEEP);
    else rev1_flash("AF SWEEP NEEDS RECEIVE");
}
__attribute__((cold)) static void rev1_image_sweep_run(float v)
{
    (void)v;
    if (RADIO_CalStart(RADIO_CAL_AF_SWEEP, 1U)) UI_NavOpenPage(PAGE_AF_SWEEP);
    else rev1_flash("IMAGE SWEEP NEEDS RECEIVE");
}
__attribute__((cold)) static void rev1_iq_scope(float v)
{
    (void)v;
    /* Inject as QMX does, then take the next whole I2S half. The page redraws
     * on the diagnostics interval, so the capture has landed by the time it
     * is first read. */
    if (RADIO_CalActive() != RADIO_CAL_RX_TEST && !RADIO_CalStart(RADIO_CAL_RX_TEST, 0U)) {
        rev1_flash("I/Q SCOPE NEEDS RECEIVE");
        return;
    }
    RADIO_AudioIqSnapshotArm();
    UI_NavOpenPage(PAGE_IQ_SCOPE);
}

__attribute__((cold)) static void rev1_bpf_apply(float v)
{
    (void)v;
    const uint32_t *edges = RADIO_SweepEdges();
    radio_config_t c;
    RADIO_GetConfig(&c);
    if (!edges) { rev1_flash("NO COMPLETED SWEEP"); return; }
    memcpy(c.bpf_upper_hz, edges, sizeof(c.bpf_upper_hz));
    if (RADIO_ConfigValid(&c)) { RADIO_SetConfig(&c); rev1_flash("BPF EDGES APPLIED - SAVE"); }
    else rev1_flash("SWEEP EDGES INVALID");
}
/* The QSD I/Q measurement. Where QMX leaves quadrature entirely to the
 * Si5351 and applies no correction, this measures what is left on this board
 * and offers the three coefficients that cancel it. Results appear in the
 * self test's receive group and on the USB console. */
__attribute__((cold)) static void rev1_rxcal(float v)
{
    (void)v;
#if MOD_DEEP_DIAG_ENABLE
    rev1_flash("NOT IN THIS FLAVOUR");
#else
    rev1_flash(RADIO_CalStart(RADIO_CAL_RX_IQ, 1U) ? "RX IQ CAL RUNNING ~2S"
                                                   : "RX IQ CAL NEEDS RECEIVE");
#endif
}
__attribute__((cold)) static void rev1_rxcal_apply(float v)
{
    (void)v;
#if MOD_DEEP_DIAG_ENABLE
    rev1_flash("NOT IN THIS FLAVOUR");
#else
    rev1_flash(RADIO_IqCalApply() ? "RX IQ APPLIED - SAVE" : "NO USABLE RX IQ RESULT");
#endif
}
__attribute__((cold)) static void rev1_bpf_qmx(float v)
{
    (void)v;
    radio_config_t c;
    RADIO_GetConfig(&c);
    RADIO_ConfigBpfDefaults(&c);
    if (RADIO_ConfigValid(&c)) { RADIO_SetConfig(&c); rev1_flash("QMX 60-15M BPF EDGES - SAVE"); }
}
__attribute__((cold)) static void rev1_lpf_qmxp(float v)
{
    (void)v;
    radio_config_t c;
    RADIO_GetConfig(&c);
    RADIO_ConfigLpfDefaults(&c);
    if (RADIO_ConfigValid(&c)) { RADIO_SetConfig(&c); rev1_flash("QMX+ LPF EDGES + TX MIN - SAVE"); }
}

__attribute__((cold)) static void rev1_selftest_run(uint8_t mask)
{
    if (!MOD_SelfTestStart(mask)) { rev1_flash("SELF TEST BLOCKED: RADIO BUSY"); return; }
    UI_NavOpenPage(PAGE_SELFTEST);
}
static void rev1_selftest(float v) { (void)v; rev1_selftest_run(ST_GROUP_DEFAULT); }
static void rev1_selftest_panel(float v) { (void)v; rev1_selftest_run((uint8_t)(ST_GROUP_ALL & ~ST_GROUP_ANALOG)); }
static void rev1_selftest_arm(float v)
{
    (void)v;
    MOD_SelfTestArmAnalog();
    rev1_flash("ANALOG ARMED: PULL JP10/JP11 FIRST");
}
__attribute__((cold)) static void rev1_usb_diag(float v)
{
    (void)v;
    rev1_flash(USB_DiagModeEnter() ? "USB DIAG CONSOLE ACTIVE" : "USB BUSY: TRY AGAIN");
}

static const setting_item_t settings[SET_COUNT] = {
    [SET_REV1_MONITOR] = {NULL, rev1_monitor, STEP_0, 0U, STY_ACTION},
    [SET_REV1_PD_INFO] = {NULL, rev1_pd_info, STEP_0, 0U, STY_ACTION},
    [SET_REV1_CAL_CODE] = {get_rev1_cal_code, set_rev1_cal_code, STEP_16, 0U, STY_NUMERIC},
    [SET_REV1_CAL_BIAS] = {NULL, rev1_cal_bias, STEP_0, 0U, STY_ACTION},
    [SET_REV1_CAL_A] = {NULL, rev1_cal_a, STEP_0, 0U, STY_ACTION},
    [SET_REV1_CAL_B] = {NULL, rev1_cal_b, STEP_0, 0U, STY_ACTION},
    [SET_REV1_CAL_RX] = {NULL, rev1_cal_rx, STEP_0, 0U, STY_ACTION},
    [SET_REV1_CAL_STOP] = {NULL, rev1_cal_stop, STEP_0, 0U, STY_ACTION},
    [SET_REV1_BPF_SWEEP] = {NULL, rev1_bpf_sweep, STEP_0, 0U, STY_ACTION},
    [SET_REV1_BPF_APPLY] = {NULL, rev1_bpf_apply, STEP_0, 0U, STY_ACTION},
    [SET_REV1_LPF_SWEEP] = {NULL, rev1_lpf_sweep, STEP_0, 0U, STY_ACTION},
    [SET_REV1_RF_SWEEP] = {NULL, rev1_rf_sweep, STEP_0, 0U, STY_ACTION},
    [SET_REV1_AF_SWEEP] = {NULL, rev1_af_sweep_run, STEP_0, 0U, STY_ACTION},
    [SET_REV1_IMAGE_SWEEP] = {NULL, rev1_image_sweep_run, STEP_0, 0U, STY_ACTION},
    [SET_REV1_IQ_SCOPE] = {NULL, rev1_iq_scope, STEP_0, 0U, STY_ACTION},
    [SET_REV1_BPF_QMX] = {NULL, rev1_bpf_qmx, STEP_0, 0U, STY_ACTION},
    [SET_REV1_RXCAL] = {NULL, rev1_rxcal, STEP_0, 0U, STY_ACTION},
    [SET_REV1_RXCAL_APPLY] = {NULL, rev1_rxcal_apply, STEP_0, 0U, STY_ACTION},
    [SET_REV1_LPF_TX0] = {get_rev1_LPF_TX0, set_rev1_LPF_TX0, STEP_0_05, 2U, STY_NUMERIC},
    [SET_REV1_LPF_TX1] = {get_rev1_LPF_TX1, set_rev1_LPF_TX1, STEP_0_05, 2U, STY_NUMERIC},
    [SET_REV1_LPF_TX2] = {get_rev1_LPF_TX2, set_rev1_LPF_TX2, STEP_0_05, 2U, STY_NUMERIC},
    [SET_REV1_LPF_TX3] = {get_rev1_LPF_TX3, set_rev1_LPF_TX3, STEP_0_05, 2U, STY_NUMERIC},
    [SET_REV1_LPF_TX4] = {get_rev1_LPF_TX4, set_rev1_LPF_TX4, STEP_0_05, 2U, STY_NUMERIC},
    [SET_REV1_LPF_QMXP] = {NULL, rev1_lpf_qmxp, STEP_0, 0U, STY_ACTION},
    [SET_REV1_SELFTEST] = {NULL, rev1_selftest, STEP_0, 0U, STY_ACTION},
    [SET_REV1_SELFTEST_PANEL] = {NULL, rev1_selftest_panel, STEP_0, 0U, STY_ACTION},
    [SET_REV1_SELFTEST_ARM] = {NULL, rev1_selftest_arm, STEP_0, 0U, STY_ACTION},
    [SET_REV1_USB_DIAG] = {NULL, rev1_usb_diag, STEP_0, 0U, STY_ACTION},
    [SET_REV1_DRIVER_A] = {get_rev1_DRIVER_A, set_rev1_DRIVER_A, STEP_1, 0U, STY_DRIVER},
    [SET_REV1_DRIVER_B] = {get_rev1_DRIVER_B, set_rev1_DRIVER_B, STEP_1, 0U, STY_DRIVER},
    [SET_REV1_DRAIN_A] = {get_rev1_DRAIN_A, set_rev1_DRAIN_A, STEP_1, 0U, STY_DRAIN},
    [SET_REV1_DRAIN_B] = {get_rev1_DRAIN_B, set_rev1_DRAIN_B, STEP_1, 0U, STY_DRAIN},
    [SET_REV1_BUTTONS] = {get_rev1_BUTTONS, set_rev1_BUTTONS, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_SW4] = {get_rev1_SW4, set_rev1_SW4, STEP_1, 0U, STY_SWITCH},
    [SET_REV1_SW5] = {get_rev1_SW5, set_rev1_SW5, STEP_1, 0U, STY_SWITCH},
    [SET_REV1_ENC_A] = {get_rev1_ENC_A, set_rev1_ENC_A, STEP_1, 0U, STY_ENCPIN},
    [SET_REV1_ENC_B] = {get_rev1_ENC_B, set_rev1_ENC_B, STEP_1, 0U, STY_ENCPIN},
    [SET_REV1_ENC_SW] = {get_rev1_ENC_SW, set_rev1_ENC_SW, STEP_1, 0U, STY_ENCPIN},
    [SET_REV1_ENC_HIGH] = {get_rev1_ENC_HIGH, set_rev1_ENC_HIGH, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_ENC_REVERSE] = {get_rev1_ENC_REVERSE, set_rev1_ENC_REVERSE, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_ENC_NOPULL] = {get_rev1_ENC_NOPULL, set_rev1_ENC_NOPULL, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_ENC_DETENT] = {get_rev1_ENC_DETENT, set_rev1_ENC_DETENT, STEP_1, 0U, STY_NUMERIC},
    [SET_REV1_BACKLIGHT] = {get_rev1_BACKLIGHT, set_rev1_BACKLIGHT, STEP_5, 0U, STY_NUMERIC},
    [SET_REV1_VOLUME] = {get_rev1_VOLUME, set_rev1_VOLUME, STEP_1, 0U, STY_NUMERIC},
    [SET_REV1_RX_MODE] = {get_rev1_RX_MODE, set_rev1_RX_MODE, STEP_1, 0U, STY_RXMODE},
    [SET_REV1_MIC] = {get_rev1_MIC, set_rev1_MIC, STEP_1, 0U, STY_MIC},
    [SET_REV1_MIC_GAIN] = {get_rev1_MIC_GAIN, set_rev1_MIC_GAIN, STEP_0_1, 1U, STY_NUMERIC},
    [SET_REV1_USB_IQ] = {get_rev1_USB_IQ, set_rev1_USB_IQ, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_ENV_MODE] = {get_rev1_ENV_MODE, set_rev1_ENV_MODE, STEP_1, 0U, STY_ENVELOPE},
    [SET_REV1_BIAS] = {get_rev1_BIAS, set_rev1_BIAS, STEP_1, 0U, STY_NUMERIC},
    [SET_REV1_BIAS_LIMIT] = {get_rev1_BIAS_LIMIT, set_rev1_BIAS_LIMIT, STEP_10, 0U, STY_NUMERIC},
    [SET_REV1_VIN_LIMIT] = {get_rev1_VIN_LIMIT, set_rev1_VIN_LIMIT, STEP_100, 0U, STY_NUMERIC},
    [SET_REV1_ENV_LIMIT] = {get_rev1_ENV_LIMIT, set_rev1_ENV_LIMIT, STEP_100, 0U, STY_NUMERIC},
    [SET_REV1_ENV_A] = {get_rev1_ENV_A, set_rev1_ENV_A, STEP_1, 0U, STY_NUMERIC},
    [SET_REV1_ENV_B] = {get_rev1_ENV_B, set_rev1_ENV_B, STEP_1, 0U, STY_NUMERIC},
    [SET_REV1_SWR] = {get_rev1_SWR, set_rev1_SWR, STEP_10, 0U, STY_NUMERIC},
    [SET_REV1_FWD_ZERO] = {get_rev1_FWD_ZERO, set_rev1_FWD_ZERO, STEP_0_001, 3U, STY_NUMERIC},
    [SET_REV1_REV_ZERO] = {get_rev1_REV_ZERO, set_rev1_REV_ZERO, STEP_0_001, 3U, STY_NUMERIC},
    [SET_REV1_FWD_GAIN] = {get_rev1_FWD_GAIN, set_rev1_FWD_GAIN, STEP_0_01, 2U, STY_NUMERIC},
    [SET_REV1_REV_GAIN] = {get_rev1_REV_GAIN, set_rev1_REV_GAIN, STEP_0_01, 2U, STY_NUMERIC},
    [SET_REV1_AUX] = {get_rev1_AUX, set_rev1_AUX, STEP_1, 0U, STY_NUMERIC},
    [SET_REV1_PD] = {get_rev1_PD, set_rev1_PD, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_PD_MV] = {get_rev1_PD_MV, set_rev1_PD_MV, STEP_20, 0U, STY_NUMERIC},
    [SET_REV1_PD_MAX] = {get_rev1_PD_MAX, set_rev1_PD_MAX, STEP_100, 0U, STY_NUMERIC},
    [SET_REV1_PD_MA] = {get_rev1_PD_MA, set_rev1_PD_MA, STEP_50, 0U, STY_NUMERIC},
    [SET_REV1_PD_RD] = {get_rev1_PD_RD, set_rev1_PD_RD, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_PD_FIX] = {NULL, rev1_pd_fix, STEP_0, 0U, STY_ACTION},
    [SET_REV1_IQ_I] = {get_rev1_IQ_I, set_rev1_IQ_I, STEP_0_001, 3U, STY_NUMERIC},
    [SET_REV1_IQ_Q] = {get_rev1_IQ_Q, set_rev1_IQ_Q, STEP_0_001, 3U, STY_NUMERIC},
    [SET_REV1_IQ_PHASE] = {get_rev1_IQ_SKEW, set_rev1_IQ_SKEW, STEP_0_001, 3U, STY_NUMERIC},
    [SET_REV1_IQ_INVERT] = {get_rev1_IQ_INVERT, set_rev1_IQ_INVERT, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_LPF0] = {get_rev1_LPF0, set_rev1_LPF0, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_LPF1] = {get_rev1_LPF1, set_rev1_LPF1, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_LPF2] = {get_rev1_LPF2, set_rev1_LPF2, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_LPF3] = {get_rev1_LPF3, set_rev1_LPF3, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_LPF4] = {get_rev1_LPF4, set_rev1_LPF4, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_BPF0] = {get_rev1_BPF0, set_rev1_BPF0, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_BPF1] = {get_rev1_BPF1, set_rev1_BPF1, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_BPF2] = {get_rev1_BPF2, set_rev1_BPF2, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_BPF3] = {get_rev1_BPF3, set_rev1_BPF3, STEP_0_1, 3U, STY_NUMERIC},
    [SET_REV1_QUAL] = {get_rev1_QUAL, set_rev1_QUAL, STEP_1, 0U, STY_NUMERIC},
    [SET_REV1_CONFIRM] = {get_rev1_CONFIRM, set_rev1_CONFIRM, STEP_1, 0U, STY_TOGGLE},
    [SET_REV1_CLEAR] = {NULL, rev1_clear, STEP_0, 0U, STY_ACTION},
    [SET_REV1_OFF] = {NULL, rev1_off, STEP_0, 0U, STY_ACTION},
    [SET_PROFILE]      = {get_profile_f,         set_profile_f,         STEP_1,   0U, STY_PROFILE},
    [SET_SIDEBAND]     = {get_sideband_f,        set_sideband_f,        STEP_1,   0U, STY_SIDEBAND},
    [SET_FREQ]         = {MOD_GetCarrierFreqMHz, MOD_SetCarrierFreqMHz, STEP_0_001, 6U, STY_NUMERIC},
    [SET_DRIVE]        = {MOD_GetDrive,          MOD_SetDrive,          STEP_0_05,  2U, STY_NUMERIC},
    [SET_CW_MODE]      = {get_cw_keyer_f,        set_cw_keyer_f,        STEP_1,   0U, STY_CWKEYER},
    [SET_CW_WPM]       = {MOD_GetCwWpm,          MOD_SetCwWpm,          STEP_1,   0U, STY_NUMERIC},
    [SET_CW_SWAP]      = {get_cw_swap_f,         set_cw_swap_f,         STEP_1,   0U, STY_TOGGLE},
    [SET_CW_WEIGHT]    = {MOD_GetCwWeight,       MOD_SetCwWeight,       STEP_1,   0U, STY_NUMERIC},
    [SET_CW_AUTOSPACE] = {get_cw_autospace_f,    set_cw_autospace_f,    STEP_1,   0U, STY_CWACS},
    [SET_CW_CURTIS_DIT]={MOD_GetCwCurtisBDitPct,MOD_SetCwCurtisBDitPct,STEP_5,   0U, STY_NUMERIC},
    [SET_CW_CURTIS_DAH]={MOD_GetCwCurtisBDahPct,MOD_SetCwCurtisBDahPct,STEP_5,   0U, STY_NUMERIC},
    [SET_CW_LATENCY]   = {MOD_GetCwLatencyEighths,MOD_SetCwLatencyEighths,STEP_1, 0U, STY_NUMERIC},
    [SET_CW_BREAKIN]   = {get_cw_breakin_f,      set_cw_breakin_f,      STEP_1,   0U, STY_CWBREAKIN},
    [SET_CW_TAIL]      = {MOD_GetCwSemiQskTail,  MOD_SetCwSemiQskTail,  STEP_0_5,   1U, STY_NUMERIC},
    [SET_CW_RAMP]      = {MOD_GetCwRampMs,       MOD_SetCwRampMs,       STEP_0_1,   1U, STY_NUMERIC},
    [SET_CW_DRIVE]     = {MOD_GetCwDrive,        MOD_SetCwDrive,        STEP_0_05,  2U, STY_NUMERIC},
    [SET_CW_THETA]     = {get_cw_theta_trim_f,   set_cw_theta_trim_f,   STEP_0_005, 3U, STY_NUMERIC},
    [SET_CW_THETA_MODEL]={get_cw_theta_model_f,  set_cw_theta_model_f,  STEP_1,   0U, STY_CWTHETA},
    [SET_CW_PRACTICE]  = {get_cw_practice_f,     set_cw_practice_f,     STEP_1,   0U, STY_TOGGLE},
    [SET_CW_DEBOUNCE]  = {MOD_GetCwDebounceMs,   MOD_SetCwDebounceMs,   STEP_0_1,   1U, STY_NUMERIC},
    [SET_CW_PDL_PROT]  = {get_cw_pdl_prot_f,     set_cw_pdl_prot_f,     STEP_1,   0U, STY_CWPDLPROT},
    [SET_CW_ARM_OPEN]  = {MOD_GetCwPaddleArmOpenMs, MOD_SetCwPaddleArmOpenMs, STEP_10, 0U, STY_MS_TIMEOUT},
    [SET_CW_ARM_PRESS] = {MOD_GetCwPaddleArmPressMs,MOD_SetCwPaddleArmPressMs,STEP_10, 0U, STY_MS_TIMEOUT},
    [SET_CW_ARM_RELEASE]={MOD_GetCwPaddleArmReleaseMs,MOD_SetCwPaddleArmReleaseMs,STEP_10, 0U, STY_MS_TIMEOUT},
    [SET_CW_BOTH_POLICY]={get_cw_both_f,         set_cw_both_f,         STEP_1,   0U, STY_CWBOTH},
    [SET_CW_STUCK_MS]  = {MOD_GetCwPaddleStuckMs, MOD_SetCwPaddleStuckMs, STEP_500, 0U, STY_MS_TIMEOUT},
    [SET_TRAIN_ACTIVITY]={get_tp_act,            set_tp_act,            STEP_1, 0U, STY_MTRAIN_ACT},
    [SET_TRAIN_CONTENT]= {get_tp_src,            set_tp_src,            STEP_1, 0U, STY_MTRAIN_SRC},
    [SET_TRAIN_GROUP]  = {get_tp_group,          set_tp_group,          STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_LESSON] = {get_tp_lesson,         set_tp_lesson,         STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_RETRIES]= {get_tp_retries,        set_tp_retries,        STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_BOOST]  = {get_tp_boost,          set_tp_boost,          STEP_1, 0U, STY_TOGGLE},
    [SET_TRAIN_WAIT]   = {get_tp_wait,           set_tp_wait,           STEP_0_5, 1U, STY_NUMERIC},
    [SET_TRAIN_TONE]   = {get_tp_tone,           set_tp_tone,           STEP_1, 0U, STY_TOGGLE},
    [SET_TRAIN_COURSE] = {get_tp_course,         set_tp_course,         STEP_1, 0U, STY_MTRAIN_COURSE},
    [SET_TRAIN_OPEN]   = {NULL,                  set_mtrain_enter_f,    STEP_0, 0U, STY_ACTION},
    [SET_TXTEST_BOTH_POLICY]={get_txt_both_f,       set_txt_both_f,        STEP_1,   0U, STY_TXTESTBOTH},
    [SET_TXTEST_STUCK_MS]={MOD_GetTxTestPaddleStuckMs, MOD_SetTxTestPaddleStuckMs, STEP_250, 0U, STY_MS_TIMEOUT},
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    [SET_SSB_BIAS]     = {MOD_GetSsbPhaseBias,   MOD_SetSsbPhaseBias,   STEP_0_02,  2U, STY_NUMERIC},
#endif
    [SET_SSB_ANA]      = {get_ssb_ana_f,         set_ssb_ana_f,         STEP_1,   0U, STY_ANALYTIC},
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    [SET_SSB_CE]       = {get_ssb_ce_f,          set_ssb_ce_f,          STEP_1,   0U, STY_TOGGLE},
#endif
    [SET_BRANCH_LAW]   = {get_branch_law_f,      set_branch_law_f,      STEP_1,   0U, STY_BRANCH_LAW},
    [SET_TH_LAW]       = {get_outphase_law_f,    set_outphase_law_f,    STEP_1,   0U, STY_OUTPHASE},
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    [SET_CE_MIX]       = {MOD_GetSsbCeMix,       MOD_SetSsbCeMix,       STEP_0_01,  2U, STY_NUMERIC},
    [SET_CE_FLOOR]     = {MOD_GetSsbCeFloor,     MOD_SetSsbCeFloor,     STEP_0_005, 3U, STY_NUMERIC},
    [SET_CE_KNEE]      = {MOD_GetSsbCeKnee,      MOD_SetSsbCeKnee,      STEP_0_01,  2U, STY_NUMERIC},
#endif
    [SET_LOW_CUT]      = {MOD_GetSsbLowCut,      MOD_SetSsbLowCut,      STEP_0_001, 3U, STY_NUMERIC},
    [SET_TX_BW]        = {get_txbw_f,            set_txbw_f,            STEP_1,   0U, STY_TXBW},
    [SET_GATE]         = {MOD_GetGate,           MOD_SetGate,           STEP_0_001, 3U, STY_NUMERIC},
    [SET_OUTPH_SSB_CAP]= {MOD_GetOutphaseSsbMaxTheta, MOD_SetOutphaseSsbMaxTheta, STEP_0_01, 3U, STY_NUMERIC},
    [SET_CAL_SKEW]     = {MOD_GetCalibrationDifferentialSkew, MOD_SetCalibrationDifferentialSkew, STEP_0_001, 3U, STY_NUMERIC},
    [SET_FM_DEV]       = {MOD_GetFmDeviation,    MOD_SetFmDeviation,    STEP_100, 0U, STY_NUMERIC},
    [SET_FM_PRESET]    = {get_fm_preset_f,       set_fm_preset_f,       STEP_1,   0U, STY_FM_PRESET},
    [SET_FM_GAIN]      = {MOD_GetFmInputGain,    MOD_SetFmInputGain,    STEP_0_05,  2U, STY_NUMERIC},
    [SET_FM_TEST_TONE] = {get_fm_test_f,         set_fm_test_f,         STEP_1,   0U, STY_TOGGLE},
    [SET_OUTPH_FM_THETA]= {MOD_GetOutphaseFmTheta, MOD_SetOutphaseFmTheta, STEP_0_005, 3U, STY_NUMERIC},
    [SET_AM_PROFILE]   = {get_am_profile_f,      set_am_profile_f,      STEP_1,   0U, STY_AM_PROFILE},
    [SET_AM_CARRIER]   = {MOD_GetAmCarrier,      MOD_SetAmCarrier,      STEP_0_05,  2U, STY_NUMERIC},
    [SET_AM_DEPTH]     = {MOD_GetAmDepth,       MOD_SetAmDepth,        STEP_0_05,  2U, STY_NUMERIC},
    [SET_OUTPH_AM_CAP] = {MOD_GetOutphaseAmMaxTheta, MOD_SetOutphaseAmMaxTheta, STEP_0_01, 3U, STY_NUMERIC},
    [SET_CORDIC]       = {get_cordic_f,          set_cordic_f,          STEP_1,   0U, STY_TOGGLE},
    [SET_USB_CLEAN]    = {get_usb_clean_f,     set_usb_clean_f,       STEP_1,   0U, STY_USBPOLICY},
    [SET_USB_GAIN]     = {MOD_GetUsbInputGain,   MOD_SetUsbInputGain,   STEP_0_05,  2U, STY_NUMERIC},
    [SET_USB_CHAN]     = {get_usb_chan_f,        set_usb_chan_f,        STEP_1,   0U, STY_USBCHAN},
    [SET_DSP_MATH]     = {get_dsp_math_f,        set_dsp_math_f,        STEP_1,   0U, STY_DSPMATH},
    [SET_GAIN_TELEM]   = {get_gain_telem_f,      set_gain_telem_f,      STEP_1,   0U, STY_GAINTELEM},
    [SET_RF_METRIC]    = {get_rf_metric_f,       set_rf_metric_f,       STEP_1,   0U, STY_RFMETRIC},
#if MOD_CESSB_EXPERIMENT_ENABLE
    [SET_CESSB]        = {get_cessb_f,          set_cessb_f,           STEP_1,   0U, STY_TOGGLE},
    [SET_CESSB_CLIP]   = {MOD_GetCessbClip,      MOD_SetCessbClip,      STEP_0_02,  2U, STY_NUMERIC},
    [SET_CESSB_ITER]   = {get_cessb_iter_f,      set_cessb_iter_f,      STEP_1,   0U, STY_NUMERIC},
#endif
    [SET_COMP]         = {get_comp_f,            set_comp_f,            STEP_1,   0U, STY_TOGGLE},
    [SET_COMP_MATH]    = {get_comp_math_f,       set_comp_math_f,       STEP_1,   0U, STY_COMPMATH},
    [SET_COMP_TGT]     = {MOD_GetCompTarget,     MOD_SetCompTarget,     STEP_0_02,  2U, STY_NUMERIC},
    [SET_COMP_RATIO]   = {MOD_GetCompRatio,      MOD_SetCompRatio,      STEP_1,   0U, STY_NUMERIC},
    [SET_EQ]           = {get_eq_f,              set_eq_f,              STEP_1,   0U, STY_TOGGLE},
    [SET_EQ_BASS]      = {MOD_GetEqBass,         MOD_SetEqBass,         STEP_0_5,   1U, STY_NUMERIC},
    [SET_EQ_TREBLE]    = {MOD_GetEqTreble,       MOD_SetEqTreble,       STEP_0_5,   1U, STY_NUMERIC},
    [SET_MICAGC]       = {get_micagc_f,          set_micagc_f,          STEP_1,   0U, STY_TOGGLE},
    [SET_MICAGC_TGT]   = {MOD_GetMicAgcTarget,   MOD_SetMicAgcTarget,   STEP_0_02,  2U, STY_NUMERIC},
    [SET_MICAGC_MAX]   = {MOD_GetMicAgcMaxGain,  MOD_SetMicAgcMaxGain,  STEP_1,   0U, STY_NUMERIC},
    [SET_NG]           = {get_ng_f,              set_ng_f,              STEP_1,   0U, STY_TOGGLE},
    [SET_NG_THR]       = {MOD_GetNoiseGateThresh,MOD_SetNoiseGateThresh,STEP_0_002, 3U, STY_NUMERIC},
    [SET_ALC]          = {get_alc_f,             set_alc_f,             STEP_1,   0U, STY_TOGGLE},
    [SET_ALC_CEIL]     = {MOD_GetAlcCeiling,     MOD_SetAlcCeiling,     STEP_0_01,  2U, STY_NUMERIC},
    [SET_CAL_VALID]    = {get_cal_valid_f,       set_cal_valid_f,       STEP_1,   0U, STY_TOGGLE},
    [SET_CAL_APPLY]    = {get_cal_apply_f,       set_cal_apply_f,       STEP_1,   0U, STY_TOGGLE},
    [SET_DEADTIME]     = {get_deadtime_f,        set_deadtime_f,        STEP_DEADTIME,  3U, STY_NUMERIC},
    [SET_DEADTIME_RISE]= {get_deadtime_rise_f,   set_deadtime_rise_f,   STEP_DEADTIME,  3U, STY_NUMERIC},
    [SET_DEADTIME_FALL]= {get_deadtime_fall_f,   set_deadtime_fall_f,   STEP_DEADTIME,  3U, STY_NUMERIC},
    [SET_AC_OFFSET]    = {get_ac_offset_f,       set_ac_offset_f,       STEP_1,   0U, STY_NUMERIC},
    [SET_DUTY]         = {get_duty_f,            set_duty_f,            STEP_1,   0U, STY_NUMERIC},
    [SET_PA_GEOMETRY]  = {get_pa_geometry_f,     set_pa_geometry_f,     STEP_1,   0U, STY_PA_GEOMETRY},
    [SET_DBG_BRANCH]   = {get_dbg_branch_f,      set_dbg_branch_f,      STEP_1,   0U, STY_BRANCH},
    [SET_DBG_CAL]      = {get_dbg_cal_f,         set_dbg_cal_f,         STEP_1,   0U, STY_TOGGLE},
    [SET_RFEN_ON]      = {get_rfen_on_f,         set_rfen_on_f,         STEP_0_001, 3U, STY_NUMERIC},
    [SET_RFEN_OFF]     = {get_rfen_off_f,        set_rfen_off_f,        STEP_0_001, 3U, STY_NUMERIC},
    [SET_RFEN_HOLD]    = {get_rfen_hold_f,       set_rfen_hold_f,       STEP_1,   0U, STY_NUMERIC},
    [SET_OUTPH_CW_THETA]= {MOD_GetOutphaseCwTheta, MOD_SetOutphaseCwTheta, STEP_0_005, 3U, STY_NUMERIC},
    [SET_OUTPH_TEST_THETA]= {MOD_GetOutphaseTestTheta, MOD_SetOutphaseTestTheta, STEP_0_005, 3U, STY_NUMERIC},
    [SET_OUTPH_FALLBACK]= {get_outphase_fallback_f, set_outphase_fallback_f, STEP_1, 0U, STY_OUTPH_FALLBACK},
    [SET_CLK_POLICY]   = {get_clk_policy_f,      set_clk_policy_f,      STEP_1,   0U, STY_CLK_POLICY},
    [SET_HOME_LAYOUT]  = {get_home_layout_f,     set_home_layout_f,     STEP_1,   0U, STY_HOME_LAYOUT},
    [SET_HOME_METER]   = {get_home_meter_f,      set_home_meter_f,      STEP_1,   0U, STY_TOGGLE},
    [SET_HOME_GAINS]   = {get_home_gains_f,      set_home_gains_f,      STEP_1,   0U, STY_TOGGLE},
    [SET_HOME_HEALTH]  = {get_home_health_f,     set_home_health_f,     STEP_1,   0U, STY_TOGGLE},
    [SET_HOME_RFEN]    = {get_home_rfen_f,       set_home_rfen_f,       STEP_1,   0U, STY_TOGGLE},
    [SET_HOME_VREF]    = {get_home_vref_f,       set_home_vref_f,       STEP_1,   0U, STY_TOGGLE},
    [SET_HOME_HINTS]   = {get_home_hints_f,      set_home_hints_f,      STEP_1,   0U, STY_TOGGLE},
    [SET_SHORTCUT_UP]  = {get_shortcut_up_f,     set_shortcut_up_f,     STEP_1,   0U, STY_SHORTCUT},
    [SET_SHORTCUT_DOWN]= {get_shortcut_down_f,   set_shortcut_down_f,   STEP_1,   0U, STY_SHORTCUT},
    [SET_RT_DIAG]      = {get_rt_diag_f,         set_rt_diag_f,         STEP_1,   0U, STY_TOGGLE},
    [SET_CPU_DIAG]     = {get_cpu_diag_f,        set_cpu_diag_f,        STEP_1,   0U, STY_TOGGLE},
    [SET_CPU_SCOPE]    = {get_cpu_scope_f,       set_cpu_scope_f,       STEP_1,   0U, STY_CPUSCOPE},
    [SET_SSB_PIPELINE] = {get_ssb_pipe_f,        set_ssb_pipe_f,        STEP_1,   0U, STY_SSBPIPE},
    [SET_MUTE]         = {get_mute_f,            set_mute_f,            STEP_1,   0U, STY_TOGGLE},
    [SET_SAVE]         = {NULL,                  NULL,                  STEP_0,   0U, STY_ACTION},
    [SET_UI_MODE]      = {get_ui_mode_f,         set_ui_mode_f,         STEP_1,   0U, STY_UMODE},
    [SET_UI_BAND]      = {get_ui_band_f,         set_ui_band_f,         STEP_1,   0U, STY_BAND},
    [SET_UI_STEP]      = {get_ui_step_f,         set_ui_step_f,         STEP_1,   0U, STY_STEP},
    [SET_UI_ROTATION]  = {get_ui_rotation_f,     set_ui_rotation_f,     STEP_1,   0U, STY_ROTATION},
    [SET_UI_FKEY_PIN]  = {get_ui_fkey_pin_f,     set_ui_fkey_pin_f,     STEP_1,   0U, STY_ENCPIN},
    [SET_REF_PPM]      = {get_ref_ppm_f,         set_ref_ppm_f,         STEP_0_001, 3U, STY_NUMERIC},
    [SET_CLK_PARK_TX]  = {get_clk_park_f,        set_clk_park_f,        STEP_1,   0U, STY_TOGGLE},
    [SET_CLK_WIDE]     = {get_clk_wide_f,       set_clk_wide_f,        STEP_1,   0U, STY_TOGGLE},
    [SET_CLK_PHASE384] = {get_clk_phase_f,       set_clk_phase_f,       STEP_1,   0U, STY_TOGGLE},
    [SET_RX_QCOMB]     = {get_rx_qcomb_f,       set_rx_qcomb_f,        STEP_1,   0U, STY_TOGGLE},
    [SET_CW_PITCH]     = {get_cw_pitch_f,        set_cw_pitch_f,        STEP_10,  0U, STY_NUMERIC},
    [SET_CW_RX_BW]     = {get_cw_rx_bw_f,        set_cw_rx_bw_f,        STEP_1,   0U, STY_CWBW},
    [SET_CW_AGC]       = {get_cw_agc_f,          set_cw_agc_f,          STEP_1,   0U, STY_CWAGC},
    [SET_CW_DECODE]    = {get_cw_decode_f,       set_cw_decode_f,       STEP_1,   0U, STY_TOGGLE},
    [SET_SSB_RX_BW]    = {get_ssb_rx_bw_f,       set_ssb_rx_bw_f,       STEP_1,   0U, STY_SSBBW},
    [SET_AM_RX_BW]     = {get_am_rx_bw_f,        set_am_rx_bw_f,        STEP_1,   0U, STY_AMBW},
    [SET_RX_AGC]       = {get_rx_agc_f,          set_rx_agc_f,          STEP_1,   0U, STY_RXAGC},
    [SET_FM_SQUELCH]   = {get_fm_squelch_f,      set_fm_squelch_f,      STEP_1,   0U, STY_SQUELCH},
    [SET_TX_PANEL]     = {get_tx_panel_f,        set_tx_panel_f,        STEP_1,   0U, STY_TOGGLE},
    [SET_METER_UPDATE] = {get_meter_update_f,    set_meter_update_f,    STEP_10,  0U, STY_NUMERIC},
    [SET_METER_HANG]   = {get_meter_hang_f,      set_meter_hang_f,      STEP_100, 0U, STY_NUMERIC},
    [SET_TUNE_PCT]     = {get_tune_pct_f,        set_tune_pct_f,        STEP_5,   0U, STY_NUMERIC},
    [SET_USB_VOX]      = {get_usb_vox_f,         set_usb_vox_f,         STEP_1,   0U, STY_TOGGLE},
    [SET_DIGI_TONE]    = {get_digi_tone_f,       set_digi_tone_f,       STEP_1,   0U, STY_TOGGLE},
    [SET_REF_SOURCE]   = {get_ref_source_f,      set_ref_source_f,      STEP_1,   0U, STY_REFSRC},
    [SET_TRAIN_LISTEN_WPM]={get_tp_listen_wpm,     set_tp_listen_wpm,     STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_PACE_WPM]={get_tp_pace_wpm,       set_tp_pace_wpm,       STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_KEY_WPM]= {get_tp_key_wpm,        set_tp_key_wpm,        STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_CAROUSEL]={get_tp_carousel,       set_tp_carousel,       STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_MANUAL] = {get_tp_manual,         set_tp_manual,         STEP_1, 0U, STY_TOGGLE},
    [SET_TRAIN_ICS]    = {get_tp_ics,            set_tp_ics,            STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_IWS]    = {get_tp_iws,            set_tp_iws,            STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_THINK]  = {get_tp_think_s,        set_tp_think_s,        STEP_1, 0U, STY_NUMERIC},
    [SET_TRAIN_SHOW]   = {get_tp_show,           set_tp_show,           STEP_1, 0U, STY_MTRAIN_SHOW},
    [SET_TRAIN_ADAPT]  = {get_tp_adapt,          set_tp_adapt,          STEP_1, 0U, STY_TOGGLE},
    [SET_TRAIN_MINUTES]= {get_tp_minutes,        set_tp_minutes,        STEP_1, 0U, STY_NUMERIC},
    [SET_CW_PRESET_M32]= {NULL,                  set_cw_preset_m32,     STEP_0, 0U, STY_ACTION},
    [SET_CW_PRESET_CLASSIC]={NULL,                  set_cw_preset_classic, STEP_0, 0U, STY_ACTION},
    [SET_CW_SPOT]      = {NULL,                  set_cw_spot,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM1]      = {NULL,                  set_cw_mem1,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM2]      = {NULL,                  set_cw_mem2,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM3]      = {NULL,                  set_cw_mem3,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM4]      = {NULL,                  set_cw_mem4,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM5]      = {NULL,                  set_cw_mem5,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM6]      = {NULL,                  set_cw_mem6,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM7]      = {NULL,                  set_cw_mem7,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM8]      = {NULL,                  set_cw_mem8,           STEP_0, 0U, STY_ACTION},
    [SET_CW_MEM_REPEAT]= {get_cw_mem_repeat,     set_cw_mem_repeat,     STEP_5, 0U, STY_NUMERIC},
    [SET_TRAIN_SAVE]   = {get_tp_save,           set_tp_save,           STEP_1, 0U, STY_TOGGLE}
};
#endif

static const setting_id_t group_quick_hidden[] = {
    SET_COUNT
};
/* CW: what an operator reaches for, the rest under ADVANCED CW (ui_menu.c
 * lists these sections there). */
static const setting_id_t group_cw[] = {
    SET_CW_WPM, SET_CW_PITCH, SET_REV1_VOLUME, SET_CW_SWAP, SET_CW_MODE, SET_CW_RX_BW, SET_CW_DECODE
};
static const setting_id_t group_cw_timing[] = {
    SET_CW_CURTIS_DIT, SET_CW_CURTIS_DAH, SET_CW_PRESET_M32, SET_CW_PRESET_CLASSIC, SET_CW_WEIGHT,
    SET_CW_AUTOSPACE, SET_CW_LATENCY, SET_CW_DEBOUNCE
};
static const setting_id_t group_cw_breakin[] = {SET_CW_BREAKIN, SET_CW_TAIL, SET_CW_PRACTICE};
static const setting_id_t group_cw_memories[] = {
    SET_CW_MEM1, SET_CW_MEM2, SET_CW_MEM3, SET_CW_MEM4, SET_CW_MEM5, SET_CW_MEM6, SET_CW_MEM7, SET_CW_MEM8,
    SET_CW_MEM_REPEAT
};
static const setting_id_t group_cw_receive[] = {SET_CW_AGC, SET_CW_SPOT};
static const setting_id_t group_cw_shaping[] = {
    SET_CW_RAMP, SET_CW_DRIVE, SET_CW_THETA, SET_OUTPH_CW_THETA, SET_CW_THETA_MODEL
};
/* The trainer's settings, from its hub: the everyday ones, then ADVANCED
 * TRAINING in sections. */
static const setting_id_t group_cw_train[] = {
    SET_TRAIN_ACTIVITY, SET_TRAIN_LESSON, SET_TRAIN_GROUP, SET_TRAIN_LISTEN_WPM, SET_TRAIN_PACE_WPM,
    SET_TRAIN_KEY_WPM, SET_REV1_VOLUME, SET_TRAIN_SAVE
};
static const setting_id_t group_train_course[] = {SET_TRAIN_COURSE, SET_TRAIN_CAROUSEL, SET_TRAIN_CONTENT};
static const setting_id_t group_train_spacing[] = {SET_TRAIN_MANUAL, SET_TRAIN_ICS, SET_TRAIN_IWS};
static const setting_id_t group_train_answer[] = {
    SET_TRAIN_RETRIES, SET_TRAIN_WAIT, SET_TRAIN_THINK, SET_TRAIN_SHOW, SET_TRAIN_TONE
};
static const setting_id_t group_train_session[] = {SET_TRAIN_BOOST, SET_TRAIN_ADAPT, SET_TRAIN_MINUTES};
static const setting_id_t group_cw_rf[] = {
    SET_CW_PDL_PROT, SET_CW_ARM_OPEN, SET_CW_ARM_PRESS, SET_CW_ARM_RELEASE,
    SET_CW_BOTH_POLICY, SET_CW_STUCK_MS, SET_TXTEST_BOTH_POLICY, SET_TXTEST_STUCK_MS
};
static const setting_id_t group_ssb[] = {
    SET_SIDEBAND,
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    SET_SSB_BIAS,
#endif
    SET_SSB_ANA,
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    SET_SSB_CE, SET_CE_MIX, SET_CE_FLOOR, SET_CE_KNEE,
#endif
    SET_LOW_CUT, SET_TX_BW, SET_GATE, SET_OUTPH_SSB_CAP, SET_SSB_RX_BW, SET_RX_AGC
};
static const setting_id_t group_fm[] = {
    SET_FM_SQUELCH, SET_FM_PRESET, SET_FM_DEV, SET_FM_GAIN, SET_FM_TEST_TONE,
    SET_OUTPH_FM_THETA
};
static const setting_id_t group_am[] = {
    SET_AM_RX_BW, SET_AM_PROFILE, SET_AM_CARRIER, SET_AM_DEPTH, SET_OUTPH_AM_CAP
};
static const setting_id_t group_usb_input[] = {
    SET_USB_CLEAN, SET_USB_GAIN, SET_USB_CHAN, SET_USB_VOX, SET_DIGI_TONE
};
static const setting_id_t group_dsp_engine[] = {
    SET_CORDIC, SET_DSP_MATH, SET_GAIN_TELEM, SET_RF_METRIC
};
static const setting_id_t group_voice[] = {
    SET_PROFILE,
#if MOD_CESSB_EXPERIMENT_ENABLE
    SET_CESSB, SET_CESSB_CLIP, SET_CESSB_ITER, SET_COMP, SET_COMP_MATH, SET_COMP_TGT, SET_COMP_RATIO,
#else
    SET_COMP, SET_COMP_MATH, SET_COMP_TGT, SET_COMP_RATIO,
#endif
    SET_EQ, SET_EQ_BASS, SET_EQ_TREBLE, SET_MICAGC, SET_MICAGC_TGT, SET_MICAGC_MAX,
    SET_NG, SET_NG_THR, SET_ALC, SET_ALC_CEIL
};
static const setting_id_t group_pa_rf[] = {
    SET_DRIVE, SET_PA_GEOMETRY, SET_BRANCH_LAW, SET_CAL_SKEW, SET_CAL_VALID, SET_CAL_APPLY, SET_DEADTIME,
    SET_DEADTIME_RISE, SET_DEADTIME_FALL, SET_AC_OFFSET, SET_DUTY,
    SET_RFEN_ON, SET_RFEN_OFF, SET_RFEN_HOLD, SET_OUTPH_TEST_THETA, SET_TUNE_PCT, SET_OUTPH_FALLBACK, SET_CLK_POLICY,
    SET_REF_PPM, SET_REF_SOURCE, SET_CLK_PARK_TX, SET_CLK_WIDE, SET_CLK_PHASE384
};
/* HOME LAYOUT and the SHOW GAIN/HEALTH/RFEN/VREF lines were the v7 home
 * screen's engineering line; the diagnostics pages replaced it, so they are
 * no longer offered (the console still reads and writes them). */
static const setting_id_t group_ui[] = {
    SET_UI_ROTATION, SET_HOME_METER, SET_TX_PANEL, SET_METER_UPDATE, SET_METER_HANG,
    SET_HOME_HINTS, SET_SHORTCUT_UP, SET_SHORTCUT_DOWN,
    SET_REV1_BUTTONS
};
/* The menu's first category: what the home settings row adjusts, plus the
 * separate receive mode and sideband for bench use. */
static const setting_id_t group_operate[] = {
    SET_UI_MODE, SET_UI_BAND, SET_UI_STEP, SET_REV1_VOLUME, SET_MUTE,
    SET_REV1_RX_MODE, SET_SIDEBAND, SET_CLK_POLICY
};
static const setting_id_t group_debug[] = {
    SET_RT_DIAG, SET_CPU_DIAG, SET_CPU_SCOPE, SET_SSB_PIPELINE, SET_DBG_BRANCH, SET_DBG_CAL,
    SET_TH_LAW
};

/* The REV1 board block is a commissioning tool, not a set of operating
 * controls: it is worked through once per board, in roughly this order. One
 * 65-row scroll made that unusable, so each step gets its own short page. */
static const setting_id_t group_board_assembly[] = {
    SET_REV1_SELFTEST, SET_REV1_SELFTEST_PANEL,
    SET_REV1_DRIVER_A, SET_REV1_DRIVER_B, SET_REV1_DRAIN_A, SET_REV1_DRAIN_B,
    SET_REV1_ENV_MODE, SET_REV1_QUAL, SET_REV1_CONFIRM, SET_SAVE
};
static const setting_id_t group_board_controls[] = {
    SET_REV1_BUTTONS, SET_REV1_SW4, SET_REV1_SW5,
    SET_REV1_ENC_A, SET_REV1_ENC_B, SET_REV1_ENC_SW, SET_REV1_ENC_DETENT,
    SET_REV1_ENC_HIGH, SET_REV1_ENC_REVERSE, SET_REV1_ENC_NOPULL, SET_UI_FKEY_PIN,
    SET_REV1_BACKLIGHT, SET_REV1_VOLUME
};
static const setting_id_t group_board_filters[] = {
    SET_REV1_LPF0, SET_REV1_LPF1, SET_REV1_LPF2, SET_REV1_LPF3, SET_REV1_LPF4,
    SET_REV1_LPF_TX0, SET_REV1_LPF_TX1, SET_REV1_LPF_TX2, SET_REV1_LPF_TX3, SET_REV1_LPF_TX4,
    SET_REV1_LPF_QMXP, SET_REV1_LPF_SWEEP,
    SET_REV1_BPF0, SET_REV1_BPF1, SET_REV1_BPF2, SET_REV1_BPF3, SET_REV1_BPF_QMX,
    SET_REV1_RF_SWEEP
};
static const setting_id_t group_board_bias[] = {
    SET_REV1_BIAS, SET_REV1_BIAS_LIMIT, SET_REV1_VIN_LIMIT, SET_REV1_ENV_LIMIT,
    SET_REV1_ENV_A, SET_REV1_ENV_B, SET_REV1_SWR,
    SET_REV1_FWD_ZERO, SET_REV1_REV_ZERO, SET_REV1_FWD_GAIN, SET_REV1_REV_GAIN
};
static const setting_id_t group_board_rxtrim[] = {
    SET_REV1_RX_MODE, SET_REV1_MIC, SET_REV1_MIC_GAIN, SET_REV1_USB_IQ,
    SET_REV1_IQ_I, SET_REV1_IQ_Q, SET_REV1_IQ_PHASE, SET_REV1_IQ_INVERT,
    SET_REV1_IQ_SCOPE, SET_REV1_AF_SWEEP, SET_REV1_IMAGE_SWEEP, SET_RX_QCOMB
};
static const setting_id_t group_board_power[] = {
    SET_REV1_PD, SET_REV1_PD_MV, SET_REV1_PD_MAX, SET_REV1_PD_MA, SET_REV1_PD_RD,
    SET_REV1_PD_FIX,
    SET_REV1_AUX, SET_REV1_PD_INFO, SET_REV1_USB_DIAG, SET_REV1_OFF
};
static const setting_id_t group_board_cal[] = {
    SET_REV1_MONITOR, SET_REV1_CAL_CODE, SET_REV1_CAL_BIAS, SET_REV1_CAL_A, SET_REV1_CAL_B,
    SET_REV1_CAL_RX, SET_REV1_RXCAL, SET_REV1_RXCAL_APPLY, SET_REV1_CAL_STOP,
    SET_REV1_SELFTEST_ARM, SET_REV1_BPF_SWEEP, SET_REV1_BPF_APPLY,
    SET_REV1_CLEAR, SET_SAVE
};

static const audio_group_info_t audio_groups[AUDIO_GROUP_COUNT] = {
    [AUDIO_GROUP_BOARD] = {"REV1 BOARD", group_quick_hidden, 0U}, /* split into the pages below */
    [AUDIO_GROUP_BOARD_ASSEMBLY] = {"ASSEMBLY", group_board_assembly,
        (uint8_t)(sizeof(group_board_assembly) / sizeof(group_board_assembly[0]))},
    [AUDIO_GROUP_BOARD_CONTROLS] = {"CONTROLS", group_board_controls,
        (uint8_t)(sizeof(group_board_controls) / sizeof(group_board_controls[0]))},
    [AUDIO_GROUP_BOARD_FILTERS] = {"FILTERS", group_board_filters,
        (uint8_t)(sizeof(group_board_filters) / sizeof(group_board_filters[0]))},
    [AUDIO_GROUP_BOARD_BIAS] = {"BIAS + PROTECT", group_board_bias,
        (uint8_t)(sizeof(group_board_bias) / sizeof(group_board_bias[0]))},
    [AUDIO_GROUP_BOARD_RXTRIM] = {"RX TRIM", group_board_rxtrim,
        (uint8_t)(sizeof(group_board_rxtrim) / sizeof(group_board_rxtrim[0]))},
    [AUDIO_GROUP_BOARD_POWER] = {"POWER / PD", group_board_power,
        (uint8_t)(sizeof(group_board_power) / sizeof(group_board_power[0]))},
    [AUDIO_GROUP_BOARD_CAL] = {"CALIBRATE", group_board_cal,
        (uint8_t)(sizeof(group_board_cal) / sizeof(group_board_cal[0]))},
    [AUDIO_GROUP_QUICK]     = {"HIDDEN",    group_quick_hidden, 0U},
    [AUDIO_GROUP_CW]        = {"CW",        group_cw,        (uint8_t)(sizeof(group_cw) / sizeof(group_cw[0]))},
    [AUDIO_GROUP_CW_TRAIN]  = {"TRAINER",   group_quick_hidden, 0U}, /* the trainer list: ui_menu.c */
    [AUDIO_GROUP_CW_RF]     = {"KEY SAFETY",group_cw_rf,     (uint8_t)(sizeof(group_cw_rf) / sizeof(group_cw_rf[0]))},
    [AUDIO_GROUP_SSB]       = {"SSB",       group_ssb,       (uint8_t)(sizeof(group_ssb) / sizeof(group_ssb[0]))},
    [AUDIO_GROUP_FM]        = {"FM",        group_fm,        (uint8_t)(sizeof(group_fm) / sizeof(group_fm[0]))},
    [AUDIO_GROUP_AM]        = {"AM",        group_am,        (uint8_t)(sizeof(group_am) / sizeof(group_am[0]))},
    [AUDIO_GROUP_USB_INPUT] = {"USB INPUT", group_usb_input, (uint8_t)(sizeof(group_usb_input) / sizeof(group_usb_input[0]))},
    [AUDIO_GROUP_DSP_ENGINE]= {"DSP ENGINE",group_dsp_engine,(uint8_t)(sizeof(group_dsp_engine) / sizeof(group_dsp_engine[0]))},
    [AUDIO_GROUP_VOICE]     = {"VOICE PROC",group_voice,     (uint8_t)(sizeof(group_voice) / sizeof(group_voice[0]))},
    [AUDIO_GROUP_PA_RF]     = {"RF OUTPUT", group_pa_rf,     (uint8_t)(sizeof(group_pa_rf) / sizeof(group_pa_rf[0]))},
    [AUDIO_GROUP_UI]        = {"DISPLAY",   group_ui,        (uint8_t)(sizeof(group_ui) / sizeof(group_ui[0]))},
    [AUDIO_GROUP_DEBUG]     = {"DEBUG",     group_debug,     (uint8_t)(sizeof(group_debug) / sizeof(group_debug[0]))},
    [AUDIO_GROUP_OPERATE]   = {"OPERATE",   group_operate,   (uint8_t)(sizeof(group_operate) / sizeof(group_operate[0]))},
#define GROUP(title, list) {title, list, (uint8_t)(sizeof(list) / sizeof(list[0]))}
    [AUDIO_GROUP_CW_ADVANCED] = {"ADVANCED CW", group_quick_hidden, 0U}, /* sections: ui_menu.c */
    [AUDIO_GROUP_CW_TIMING]   = GROUP("KEYER TIMING", group_cw_timing),
    [AUDIO_GROUP_CW_BREAKIN]  = GROUP("BREAK-IN", group_cw_breakin),
    [AUDIO_GROUP_CW_RECEIVE]  = GROUP("RECEIVE", group_cw_receive),
    [AUDIO_GROUP_CW_SHAPING]  = GROUP("RF SHAPING", group_cw_shaping),
    [AUDIO_GROUP_TRAIN_ADVANCED] = {"ADVANCED TRAINING", group_quick_hidden, 0U},
    [AUDIO_GROUP_TRAIN_COURSE]  = GROUP("COURSE", group_train_course),
    [AUDIO_GROUP_TRAIN_SPACING] = GROUP("SPACING", group_train_spacing),
    [AUDIO_GROUP_TRAIN_ANSWER]  = GROUP("ANSWERS", group_train_answer),
    [AUDIO_GROUP_TRAIN_SESSION] = GROUP("SESSION", group_train_session),
    [AUDIO_GROUP_TRAIN_BASIC]   = GROUP("SETTINGS", group_cw_train),
    [AUDIO_GROUP_CW_MEMORIES]   = GROUP("MEMORIES", group_cw_memories),
#undef GROUP
};

static const uint8_t visible_groups_normal[] = {
    AUDIO_GROUP_CW, AUDIO_GROUP_CW_MEMORIES, AUDIO_GROUP_CW_TIMING, AUDIO_GROUP_CW_BREAKIN, AUDIO_GROUP_CW_RECEIVE, AUDIO_GROUP_CW_RF,
    AUDIO_GROUP_CW_SHAPING, AUDIO_GROUP_TRAIN_BASIC, AUDIO_GROUP_TRAIN_COURSE, AUDIO_GROUP_TRAIN_SPACING,
    AUDIO_GROUP_TRAIN_ANSWER, AUDIO_GROUP_TRAIN_SESSION,
    AUDIO_GROUP_SSB, AUDIO_GROUP_FM, AUDIO_GROUP_AM, AUDIO_GROUP_USB_INPUT,
    AUDIO_GROUP_VOICE, AUDIO_GROUP_PA_RF, AUDIO_GROUP_UI
};
static const uint8_t visible_groups_debug[] = {
    AUDIO_GROUP_CW, AUDIO_GROUP_CW_MEMORIES, AUDIO_GROUP_CW_TIMING, AUDIO_GROUP_CW_BREAKIN, AUDIO_GROUP_CW_RECEIVE, AUDIO_GROUP_CW_RF,
    AUDIO_GROUP_CW_SHAPING, AUDIO_GROUP_TRAIN_BASIC, AUDIO_GROUP_TRAIN_COURSE, AUDIO_GROUP_TRAIN_SPACING,
    AUDIO_GROUP_TRAIN_ANSWER, AUDIO_GROUP_TRAIN_SESSION,
    AUDIO_GROUP_SSB, AUDIO_GROUP_FM, AUDIO_GROUP_AM, AUDIO_GROUP_USB_INPUT,
    AUDIO_GROUP_DSP_ENGINE, AUDIO_GROUP_VOICE, AUDIO_GROUP_PA_RF,
    AUDIO_GROUP_UI, AUDIO_GROUP_DEBUG
};
/* Order follows the bring-up sequence in docs/REV1.md: declare the assembly,
 * set up the controls, enter the filters, then bias, receive trim, supply and
 * finally the bench routines. */
static const uint8_t board_groups[] = {
    AUDIO_GROUP_BOARD_ASSEMBLY, AUDIO_GROUP_BOARD_CONTROLS, AUDIO_GROUP_BOARD_FILTERS,
    AUDIO_GROUP_BOARD_BIAS, AUDIO_GROUP_BOARD_RXTRIM, AUDIO_GROUP_BOARD_POWER,
    AUDIO_GROUP_BOARD_CAL
};
static const uint8_t modulation_groups_normal[] = {
    AUDIO_GROUP_SSB, AUDIO_GROUP_FM, AUDIO_GROUP_AM, AUDIO_GROUP_USB_INPUT
};
static const uint8_t modulation_groups_debug[] = {
    AUDIO_GROUP_SSB, AUDIO_GROUP_FM, AUDIO_GROUP_AM, AUDIO_GROUP_USB_INPUT,
    AUDIO_GROUP_DSP_ENGINE
};

uint8_t UI_SettingIsBandScoped(setting_id_t id)
{
    switch (id) {
        case SET_CAL_SKEW:
        case SET_CAL_VALID:
        case SET_CAL_APPLY:
        case SET_DEADTIME:
        case SET_DEADTIME_RISE:
        case SET_DEADTIME_FALL:
        case SET_AC_OFFSET:
        case SET_DUTY:
            return 1U;
        default:
            return 0U;
    }
}

#if !defined(UI_SETTINGS_POLICY_ONLY)
const setting_item_t *UI_GetSetting(setting_id_t id)
{
    if ((uint32_t)id >= (uint32_t)SET_COUNT) return NULL;
    return &settings[id];
}

float UI_SettingStep(const setting_item_t *it)
{
    static const float steps[STEP_DEADTIME] = {
        [STEP_0] = 0.0f, [STEP_0_001] = 0.001f, [STEP_0_002] = 0.002f, [STEP_0_005] = 0.005f,
        [STEP_0_01] = 0.01f, [STEP_0_02] = 0.02f, [STEP_0_05] = 0.05f, [STEP_0_1] = 0.1f,
        [STEP_0_5] = 0.5f, [STEP_1] = 1.0f, [STEP_5] = 5.0f, [STEP_10] = 10.0f, [STEP_16] = 16.0f,
        [STEP_20] = 20.0f, [STEP_50] = 50.0f, [STEP_100] = 100.0f, [STEP_250] = 250.0f,
        [STEP_500] = 500.0f
    };
    if (it->step < STEP_DEADTIME) return steps[it->step];
    return MOD_GetDeadTimeStepNs(); /* the deadtime rows: one tick at this clock */
}
#endif

/* UI_SettingHelp is in ui_help.c: the help text is Core/Inc/ui_help_text.def,
   packed by tools/gen_help_text.py. */

#if !defined(UI_SETTINGS_POLICY_ONLY)
/* The Morse trainer's numbers: whole units, the trainer holds the lesson to
 * its course and SPACING WPM to LISTEN WPM. */
static uint8_t train_range(setting_id_t id, float *lo, float *hi)
{
    static const struct { uint8_t id, lo, hi; } r[] = {
        {SET_TRAIN_GROUP, 1U, MTRAIN_GROUP_MAX}, {SET_TRAIN_LESSON, 1U, MORSE_SYMBOLS},
        {SET_TRAIN_RETRIES, 0U, MTRAIN_RETRY_ALWAYS}, {SET_TRAIN_WAIT, 1U, 10U},
        {SET_TRAIN_LISTEN_WPM, 5U, 60U}, {SET_TRAIN_KEY_WPM, 5U, 60U}, {SET_TRAIN_PACE_WPM, 2U, 60U},
        {SET_TRAIN_CAROUSEL, 0U, MTRAIN_CAROUSEL_MAX}, {SET_TRAIN_ICS, 3U, 15U}, {SET_TRAIN_IWS, 7U, 30U},
        {SET_TRAIN_THINK, 2U, 30U}, {SET_TRAIN_MINUTES, 0U, 60U}, {SET_CW_MEM_REPEAT, 0U, 60U},
    };
    for (uint32_t i = 0U; i < sizeof(r) / sizeof(r[0]); ++i)
        if (r[i].id == (uint8_t)id) {
            *lo = (float)r[i].lo;
            *hi = (float)r[i].hi;
            return 1U;
        }
    return 0U;
}

uint8_t UI_SettingRange(setting_id_t id, float *min_value, float *max_value)
{
    float lo = 0.0f;
    float hi = 1.0f;
    uint8_t ok = 1U;

    if (train_range(id, &lo, &hi)) id = SET_COUNT; /* found: the switch has nothing to add */
    switch (id) {
        case SET_COUNT: break;
        case SET_REV1_CAL_CODE: lo = 0.0f; hi = 1024.0f; break;
        case SET_REV1_DRIVER_A: lo = 0.0f; hi = 2.0f; break;
        case SET_REV1_DRIVER_B: lo = 0.0f; hi = 2.0f; break;
        case SET_REV1_DRAIN_A: lo = 0.0f; hi = 2.0f; break;
        case SET_REV1_DRAIN_B: lo = 0.0f; hi = 2.0f; break;
        case SET_REV1_BUTTONS: lo = 0.0f; hi = 1.0f; break;
        case SET_REV1_SW4: lo = 0.0f; hi = (float)RADIO_SW_LAST; break;
        case SET_REV1_SW5: lo = 0.0f; hi = (float)RADIO_SW_LAST; break;
        case SET_REV1_ENC_A:
        case SET_REV1_ENC_B:
        case SET_REV1_ENC_SW: lo = 0.0f; hi = (float)(RADIO_ENC_PIN_COUNT - 1U); break;
        case SET_REV1_ENC_HIGH:
        case SET_REV1_ENC_REVERSE:
        case SET_REV1_ENC_NOPULL: lo = 0.0f; hi = 1.0f; break;
        case SET_REV1_ENC_DETENT: lo = (float)RADIO_ENC_DETENT_MIN; hi = (float)RADIO_ENC_DETENT_MAX; break;
        case SET_REV1_BACKLIGHT: lo = (float)RADIO_BACKLIGHT_MIN; hi = 100.0f; break;
        case SET_REV1_VOLUME: lo = 0.0f; hi = 100.0f; break;
        case SET_UI_MODE: lo = 0.0f; hi = (float)(UI_UMODE_COUNT - 1U); break;
        case SET_UI_BAND: lo = 0.0f; hi = (float)(MOD_GetBandCount() - 1U); break;
        case SET_UI_STEP: lo = 0.0f; hi = (float)(UI_TUNE_DIGITS - 1U); break;
        case SET_UI_ROTATION: lo = 0.0f; hi = 1.0f; break;
        case SET_UI_FKEY_PIN: lo = 0.0f; hi = (float)(RADIO_ENC_PIN_COUNT - 1U); break;
        case SET_REF_PPM: lo = -100.0f; hi = 100.0f; break;
        case SET_REV1_RX_MODE: lo = 0.0f; hi = 4.0f; break;
        case SET_REV1_MIC: lo = 0.0f; hi = 3.0f; break;
        case SET_REV1_MIC_GAIN: lo = 0.1f; hi = 100.0f; break;
        case SET_REV1_USB_IQ: lo = 0.0f; hi = 1.0f; break;
        case SET_REV1_ENV_MODE: lo = 0.0f; hi = 2.0f; break;
        case SET_REV1_BIAS: lo = (float)RADIO_BIAS_MA_OFF; hi = (float)RADIO_BIAS_MA_MAX; break;
        case SET_REV1_BIAS_LIMIT: lo = 0.0f; hi = 2850.0f; break;
        case SET_REV1_VIN_LIMIT: lo = 5000.0f; hi = 14700.0f; break;
        case SET_REV1_ENV_LIMIT: lo = 0.0f; hi = 14000.0f; break;
        case SET_REV1_ENV_A: lo = 0.0f; hi = 4095.0f; break;
        case SET_REV1_ENV_B: lo = 0.0f; hi = 4095.0f; break;
        case SET_REV1_SWR: lo = 120.0f; hi = 1000.0f; break;
        case SET_TX_PANEL: lo = 0.0f; hi = 1.0f; break;
        case SET_METER_UPDATE: lo = (float)RADIO_METER_UPDATE_MIN_MS; hi = (float)RADIO_METER_UPDATE_MAX_MS; break;
        case SET_METER_HANG: lo = 100.0f; hi = (float)RADIO_METER_HANG_MAX_MS; break;
        case SET_TUNE_PCT: lo = (float)RADIO_TUNE_PCT_MIN; hi = 100.0f; break;
        case SET_REV1_FWD_ZERO: lo = -2.9f; hi = 2.9f; break;
        case SET_REV1_REV_ZERO: lo = -2.9f; hi = 2.9f; break;
        case SET_REV1_FWD_GAIN: lo = 0.01f; hi = 100.0f; break;
        case SET_REV1_REV_GAIN: lo = 0.01f; hi = 100.0f; break;
        case SET_REV1_AUX: lo = 0.0f; hi = 7.0f; break;
        case SET_REV1_PD: lo = 0.0f; hi = 1.0f; break;
        case SET_REV1_PD_MV: lo = 5000.0f; hi = 14000.0f; break;
        case SET_REV1_PD_MAX: lo = 5000.0f; hi = 14000.0f; break;
        case SET_REV1_PD_MA: lo = 100.0f; hi = 3000.0f; break;
        case SET_REV1_PD_RD: lo = 0.0f; hi = 1.0f; break;
        case SET_REV1_IQ_I: lo = 0.1f; hi = 10.0f; break;
        case SET_REV1_IQ_Q: lo = 0.1f; hi = 10.0f; break;
        case SET_REV1_IQ_PHASE: lo = -0.5f; hi = 0.5f; break;
        case SET_REV1_IQ_INVERT: lo = 0.0f; hi = 1.0f; break;
        case SET_REV1_LPF0: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_LPF1: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_LPF2: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_LPF3: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_LPF4: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_LPF_TX0:
        case SET_REV1_LPF_TX1:
        case SET_REV1_LPF_TX2:
        case SET_REV1_LPF_TX3:
        case SET_REV1_LPF_TX4: lo = 0.0f; hi = 28.333f; break;
        case SET_REV1_BPF0: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_BPF1: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_BPF2: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_BPF3: lo = 0.0f; hi = 28.333333f; break;
        case SET_REV1_QUAL: lo = 0.0f; hi = 31.0f; break;
        case SET_REV1_CONFIRM: lo = 0.0f; hi = 1.0f; break;

        case SET_FREQ:          lo = (float)MOD_CARRIER_MIN_HZ * 0.000001f; hi = (float)MOD_CARRIER_MAX_HZ * 0.000001f; break;
        case SET_DRIVE:         lo = 0.10f; hi = 1.00f; break;
        case SET_CW_WPM:        lo = CW_WPM_MIN; hi = CW_WPM_MAX; break;
        case SET_CW_WEIGHT:     lo = CW_WEIGHT_MIN; hi = CW_WEIGHT_MAX; break;
        case SET_CW_CURTIS_DIT:
        case SET_CW_CURTIS_DAH: lo = CW_CURTIS_B_PCT_MIN; hi = CW_CURTIS_B_PCT_MAX; break;
        case SET_CW_LATENCY:    lo = 0.0f; hi = (float)CW_LATENCY_EIGHTHS_MAX; break;
        case SET_CW_TAIL:       lo = CW_SEMI_QSK_TAIL_MIN; hi = CW_SEMI_QSK_TAIL_MAX; break;
        case SET_CW_PITCH:      lo = (float)RADIO_CW_PITCH_MIN_HZ; hi = (float)RADIO_CW_PITCH_MAX_HZ; break;
        case SET_CW_RAMP:       lo = CW_RAMP_MS_MIN; hi = CW_RAMP_MS_MAX; break;
        case SET_CW_DRIVE:      lo = CW_DRIVE_MIN; hi = CW_DRIVE_MAX; break;
        case SET_CW_THETA:      lo = CW_THETA_TRIM_MIN; hi = CW_THETA_TRIM_MAX; break;
        case SET_CW_DEBOUNCE:   lo = CW_DEBOUNCE_MS_MIN; hi = CW_DEBOUNCE_MS_MAX; break;
        case SET_CW_ARM_OPEN:
        case SET_CW_ARM_PRESS:
        case SET_CW_ARM_RELEASE:lo = CW_PADDLE_ARM_MS_MIN; hi = CW_PADDLE_ARM_MS_MAX; break;
        case SET_CW_STUCK_MS:   lo = CW_PADDLE_STUCK_MS_MIN; hi = CW_PADDLE_STUCK_MS_MAX; break;
        case SET_TXTEST_STUCK_MS: lo = TXTEST_PADDLE_STUCK_MS_MIN; hi = TXTEST_PADDLE_STUCK_MS_MAX; break;
        case SET_SSB_BIAS:      lo = 0.0f; hi = 0.35f; break;
        case SET_CE_MIX:        lo = 0.0f; hi = 0.80f; break;
        case SET_CE_FLOOR:      lo = 0.0f; hi = 0.18f; break;
        case SET_CE_KNEE:       lo = 0.25f; hi = 0.85f; break;
        case SET_LOW_CUT:       lo = 0.0f; hi = 0.012f; break;
        case SET_GATE:          lo = 0.0f; hi = 0.05f; break;
        case SET_OUTPH_SSB_CAP: lo = 0.0f; hi = OUTPHASE_THETA_HARD_MAX; break;
        case SET_FM_DEV:        lo = FM_DEVIATION_MIN_HZ; hi = FM_DEVIATION_MAX_HZ; break;
        case SET_OUTPH_FM_THETA: lo = 0.0f; hi = OUTPHASE_THETA_HARD_MAX; break;
        case SET_AM_CARRIER:    lo = 0.10f; hi = 1.00f; break;
        case SET_AM_DEPTH:      lo = 0.0f; hi = 1.00f; break;
        case SET_OUTPH_AM_CAP:  lo = 0.0f; hi = OUTPHASE_THETA_HARD_MAX; break;
        case SET_CESSB_CLIP:    lo = 0.30f; hi = 1.00f; break;
        case SET_CESSB_ITER:    lo = 0.0f; hi = (float)CESSB_ITER_MAX; break;
        case SET_COMP_TGT:      lo = 0.10f; hi = 0.90f; break;
        case SET_COMP_RATIO:    lo = 1.0f; hi = 20.0f; break;
        case SET_EQ_BASS:
        case SET_EQ_TREBLE:     lo = -6.0f; hi = 6.0f; break;
        case SET_MICAGC_TGT:    lo = 0.05f; hi = 0.70f; break;
        case SET_MICAGC_MAX:    lo = 1.0f; hi = 40.0f; break;
        case SET_NG_THR:        lo = 0.0f; hi = 0.10f; break;
        case SET_ALC_CEIL:      lo = 0.50f; hi = 0.99f; break;
        case SET_CAL_SKEW:      lo = -0.45f; hi = 0.45f; break;
        case SET_DEADTIME:
        case SET_DEADTIME_RISE:
        case SET_DEADTIME_FALL: lo = 0.0f; hi = MOD_GetDeadTimeMaxNs(); break;
        case SET_AC_OFFSET:
        case SET_DUTY:          lo = -1024.0f; hi = 1024.0f; break;
        case SET_RFEN_ON:       lo = RF_EN_GPIO_ASSERT_METRIC_MIN; hi = RF_EN_GPIO_ASSERT_METRIC_MAX; break;
        case SET_RFEN_OFF:      lo = RF_EN_GPIO_RELEASE_METRIC_MIN; hi = RF_EN_GPIO_RELEASE_METRIC_MAX; break;
        case SET_RFEN_HOLD:     lo = 0.0f; hi = (float)RF_EN_GPIO_HOLD_FRAMES_MAX; break;
        case SET_OUTPH_CW_THETA:
        case SET_OUTPH_TEST_THETA: lo = 0.0f; hi = 0.25f; break;
        case SET_OUTPH_FALLBACK: lo = 0.0f; hi = (float)(MOD_OUTPHASE_FALLBACK_COUNT - 1U); break;
        default:                ok = 0U; break;
    }

    if (ok != 0U) {
        if (min_value != NULL) *min_value = lo;
        if (max_value != NULL) *max_value = hi;
    }
    return ok;
}

/* One table of limits, UI_SettingRange, for the panel's edits and for the
 * console's and CAT's writes alike: a value outside it never reaches a
 * setter. A second, setter-keyed copy of the limits lived here and had
 * drifted -- FM DEV stopped at 5 kHz under the 7.5 kHz range, preset and
 * setter, and its RF ENABLE branches matched no row. Items that are not
 * rows of the table (CAT's list probes) pass unchanged. */
float UI_ClampSettingValue(const setting_item_t *it, float value)
{
    const uintptr_t at = (uintptr_t)it, first = (uintptr_t)&settings[0];
    if (at < first || at >= (uintptr_t)&settings[SET_COUNT]) return value;
    float lo, hi;
    if (!UI_SettingRange((setting_id_t)((at - first) / sizeof(settings[0])), &lo, &hi)) return value;
    if (value < lo) value = lo;
    if (hi > lo && value > hi) value = hi; /* a deadtime limit of 0: no clock to say */
    return value;
}
#endif

const audio_group_info_t *UI_GetAudioGroupInfo(uint8_t group)
{
    return (group < AUDIO_GROUP_COUNT) ? &audio_groups[group] : NULL;
}

/* What each settings group is for, by audio_group_t; groups without
 * one say what a group link is. */
static const uint8_t group_help[AUDIO_GROUP_COUNT] = {
    [AUDIO_GROUP_QUICK] = TXT_GRP_QUICK,
    [AUDIO_GROUP_CW] = TXT_GRP_CW,
    [AUDIO_GROUP_CW_TRAIN] = TXT_GRP_CW_TRAIN,
    [AUDIO_GROUP_TRAIN_BASIC] = TXT_GRP_TRAIN_BASIC,
    [AUDIO_GROUP_CW_ADVANCED] = TXT_GRP_CW_ADVANCED,
    [AUDIO_GROUP_CW_MEMORIES] = TXT_GRP_CW_MEMORIES,
    [AUDIO_GROUP_CW_TIMING] = TXT_GRP_CW_TIMING,
    [AUDIO_GROUP_TRAIN_ADVANCED] = TXT_GRP_TRAIN_ADVANCED,
    [AUDIO_GROUP_CW_RF] = TXT_GRP_CW_RF,
    [AUDIO_GROUP_SSB] = TXT_GRP_SSB,
    [AUDIO_GROUP_FM] = TXT_GRP_FM,
    [AUDIO_GROUP_AM] = TXT_GRP_AM,
    [AUDIO_GROUP_USB_INPUT] = TXT_GRP_USB_INPUT,
    [AUDIO_GROUP_DSP_ENGINE] = TXT_GRP_DSP_ENGINE,
#if MOD_CESSB_EXPERIMENT_ENABLE
    [AUDIO_GROUP_VOICE] = TXT_GRP_VOICE_CESSB,
#else
    [AUDIO_GROUP_VOICE] = TXT_GRP_VOICE,
#endif
    [AUDIO_GROUP_PA_RF] = TXT_GRP_PA_RF,
    [AUDIO_GROUP_UI] = TXT_GRP_UI,
    [AUDIO_GROUP_DEBUG] = TXT_GRP_DEBUG,
    [AUDIO_GROUP_BOARD_ASSEMBLY] = TXT_GRP_BOARD_ASSEMBLY,
    [AUDIO_GROUP_BOARD_CONTROLS] = TXT_GRP_BOARD_CONTROLS,
    [AUDIO_GROUP_BOARD_FILTERS] = TXT_GRP_BOARD_FILTERS,
    [AUDIO_GROUP_BOARD_BIAS] = TXT_GRP_BOARD_BIAS,
    [AUDIO_GROUP_BOARD_RXTRIM] = TXT_GRP_BOARD_RXTRIM,
    [AUDIO_GROUP_BOARD_POWER] = TXT_GRP_BOARD_POWER,
    [AUDIO_GROUP_BOARD_CAL] = TXT_GRP_BOARD_CAL,
};

/* On the card (card_text.def): drawn from the help cache, "" until read. */
const char *UI_AudioGroupHelp(uint8_t group)
{
    const uint8_t id = (group < (uint8_t)AUDIO_GROUP_COUNT) ? group_help[group] : 0U;
    return UI_CardTextCached(id ? (card_text_t)id : TXT_GRP_DEFAULT);
}

uint8_t UI_AudioGroupTotalItems(uint8_t group)
{
    const audio_group_info_t *g = UI_GetAudioGroupInfo(group);
    return g ? (uint8_t)(g->count + 1U) : 0U;
}

#if !defined(UI_SETTINGS_POLICY_ONLY)
const setting_item_t *UI_AudioGroupSetting(uint8_t group, uint8_t cursor)
{
    const audio_group_info_t *g = UI_GetAudioGroupInfo(group);
    if (!g || cursor == 0U || (cursor - 1U) >= g->count) return NULL;
    return UI_GetSetting(g->items[cursor - 1U]);
}
#endif

uint8_t UI_AudioGroupSettingId(uint8_t group, uint8_t cursor, setting_id_t *out_id)
{
    const audio_group_info_t *g = UI_GetAudioGroupInfo(group);
    if (!g || cursor == 0U || (cursor - 1U) >= g->count || out_id == NULL) return 0U;
    *out_id = g->items[cursor - 1U];
    return 1U;
}

uint8_t UI_GetAudioGroupVisibleCount(void)
{
    return ui_debug_visible
        ? (uint8_t)(sizeof(visible_groups_debug) / sizeof(visible_groups_debug[0]))
        : (uint8_t)(sizeof(visible_groups_normal) / sizeof(visible_groups_normal[0]));
}

uint8_t UI_GetAudioGroupAt(uint8_t visible_idx)
{
    if (ui_debug_visible) {
        return (visible_idx < (uint8_t)(sizeof(visible_groups_debug) / sizeof(visible_groups_debug[0])))
            ? visible_groups_debug[visible_idx] : AUDIO_GROUP_CW;
    }
    return (visible_idx < (uint8_t)(sizeof(visible_groups_normal) / sizeof(visible_groups_normal[0])))
        ? visible_groups_normal[visible_idx] : AUDIO_GROUP_CW;
}

uint8_t UI_GetModulationGroupVisibleCount(void)
{
    return ui_debug_visible
        ? (uint8_t)(sizeof(modulation_groups_debug) / sizeof(modulation_groups_debug[0]))
        : (uint8_t)(sizeof(modulation_groups_normal) / sizeof(modulation_groups_normal[0]));
}

uint8_t UI_GetModulationGroupAt(uint8_t visible_idx)
{
    if (ui_debug_visible) {
        return (visible_idx < (uint8_t)(sizeof(modulation_groups_debug) / sizeof(modulation_groups_debug[0])))
            ? modulation_groups_debug[visible_idx] : AUDIO_GROUP_SSB;
    }
    return (visible_idx < (uint8_t)(sizeof(modulation_groups_normal) / sizeof(modulation_groups_normal[0])))
        ? modulation_groups_normal[visible_idx] : AUDIO_GROUP_SSB;
}

/* One place that maps a UI_AUDIO_SCOPE_* to its group list, so navigation and
 * rendering cannot disagree about which list a page is showing. */
uint8_t UI_GroupListCount(uint8_t scope)
{
    if (scope == UI_AUDIO_SCOPE_MODULATION) return UI_GetModulationGroupVisibleCount();
    if (scope == UI_AUDIO_SCOPE_BOARD) return UI_GetBoardGroupVisibleCount();
    return UI_GetAudioGroupVisibleCount();
}

uint8_t UI_GroupListAt(uint8_t scope, uint8_t index)
{
    if (scope == UI_AUDIO_SCOPE_MODULATION) return UI_GetModulationGroupAt(index);
    if (scope == UI_AUDIO_SCOPE_BOARD) return UI_GetBoardGroupAt(index);
    return UI_GetAudioGroupAt(index);
}

uint8_t UI_GetBoardGroupVisibleCount(void)
{
    return (uint8_t)(sizeof(board_groups) / sizeof(board_groups[0]));
}

uint8_t UI_GetBoardGroupAt(uint8_t visible_idx)
{
    return (visible_idx < (uint8_t)(sizeof(board_groups) / sizeof(board_groups[0])))
        ? board_groups[visible_idx] : AUDIO_GROUP_BOARD_ASSEMBLY;
}

#if !defined(UI_SETTINGS_POLICY_ONLY)
uint8_t UI_SettingIsDiscrete(setting_style_t style)
{
    return (style == STY_DRIVER || style == STY_DRAIN || style == STY_SWITCH || style == STY_ENCPIN ||
            style == STY_RXMODE || style == STY_MIC || style == STY_ENVELOPE || style == STY_TOGGLE || style == STY_PROFILE || style == STY_SIDEBAND ||
            style == STY_ANALYTIC || style == STY_TXBW || style == STY_BRANCH ||
            style == STY_BRANCH_LAW || style == STY_OUTPHASE || style == STY_PA_GEOMETRY || style == STY_USBCHAN || style == STY_DSPMATH ||
            style == STY_USBPOLICY || style == STY_FM_PRESET || style == STY_AM_PROFILE ||
            style == STY_GAINTELEM || style == STY_COMPMATH || style == STY_RFMETRIC ||
            style == STY_CWKEYER || style == STY_CWBREAKIN ||
            style == STY_CWPDLPROT || style == STY_CWBOTH || style == STY_CWACS || style == STY_CWTHETA || style == STY_TXTESTBOTH ||
            style == STY_MTRAIN_ACT || style == STY_MTRAIN_SRC || style == STY_MTRAIN_SHOW || style == STY_MTRAIN_COURSE ||
            style == STY_OUTPH_FALLBACK || style == STY_CLK_POLICY || style == STY_HOME_LAYOUT || style == STY_SHORTCUT ||
            style == STY_CPUSCOPE || style == STY_SSBPIPE ||
            style == STY_UMODE || style == STY_BAND || style == STY_STEP || style == STY_ROTATION ||
            style == STY_CWBW || style == STY_CWAGC || style == STY_SSBBW || style == STY_AMBW ||
            style == STY_RXAGC || style == STY_SQUELCH || style == STY_REFSRC) ? 1U : 0U;
}

int32_t UI_SettingDiscreteCount(setting_style_t style)
{
    switch (style) {
        case STY_DRIVER: case STY_DRAIN: case STY_ENVELOPE: return 3;
        case STY_MIC: return 4;
        case STY_SWITCH: return (int32_t)RADIO_SW_LAST + 1;
        case STY_ENCPIN: return (int32_t)RADIO_ENC_PIN_COUNT;
        case STY_RXMODE: return 5;
        case STY_UMODE: return (int32_t)UI_UMODE_COUNT;
        case STY_BAND: return (int32_t)MOD_GetBandCount();
        case STY_STEP: return (int32_t)UI_TUNE_DIGITS;
        case STY_ROTATION: return 2;
        case STY_CWBW: return (int32_t)RADIO_CW_BW_COUNT;
        case STY_CWAGC: return 2;
        case STY_SSBBW: return (int32_t)RADIO_SSB_BW_COUNT;
        case STY_AMBW: return (int32_t)RADIO_AM_BW_COUNT;
        case STY_RXAGC: return 4;
        case STY_SQUELCH: return (int32_t)RADIO_FM_SQUELCH_MAX + 1;
        case STY_PROFILE:     return (int32_t)AUDIO_PROFILE_COUNT;
        case STY_SIDEBAND:    return 2;
        case STY_REFSRC:      return 2;
        case STY_ANALYTIC:    return (int32_t)UI_SsbAnalyticChoiceCount();
        case STY_TXBW:        return (int32_t)TX_BW_COUNT;
        case STY_BRANCH:      return (int32_t)DEBUG_BRANCH_COUNT;
        case STY_BRANCH_LAW:  return 2;
        case STY_OUTPHASE:    return (int32_t)OUTPHASE_LAW_COUNT;
        case STY_PA_GEOMETRY: return (int32_t)PA_GEOMETRY_COUNT;
        case STY_USBCHAN:     return (int32_t)USB_AUDIO_CHAN_COUNT;
        case STY_USBPOLICY:   return (int32_t)USB_AUDIO_POLICY_COUNT;
        case STY_FM_PRESET:   return (int32_t)FM_DEVIATION_PRESET_COUNT;
        case STY_AM_PROFILE:  return (int32_t)AM_PROFILE_COUNT;
        case STY_DSPMATH:     return (int32_t)DSP_MATH_COUNT;
        case STY_GAINTELEM:   return (int32_t)GAIN_TELEMETRY_COUNT;
        case STY_COMPMATH:    return (int32_t)COMP_MATH_COUNT;
        case STY_RFMETRIC:    return (int32_t)RF_METRIC_COUNT;
        case STY_CWKEYER:     return (int32_t)CW_KEYER_COUNT;
        case STY_CWBREAKIN:   return (int32_t)CW_BREAKIN_COUNT;
        case STY_CWPDLPROT:   return (int32_t)CW_PADDLE_PROTECT_COUNT;
        case STY_CWBOTH:      return (int32_t)CW_BOTH_COUNT;
        case STY_CWACS:       return 4;
        case STY_CWTHETA:     return (int32_t)CW_THETA_MODEL_COUNT;
        case STY_TXTESTBOTH:  return (int32_t)TXTEST_BOTH_COUNT;
        case STY_MTRAIN_ACT:   return (int32_t)MTRAIN_ACT_COUNT;
        case STY_MTRAIN_SRC:   return (int32_t)MTRAIN_SRC_COUNT;
        case STY_MTRAIN_SHOW:  return (int32_t)MTRAIN_SHOW_COUNT;
        case STY_MTRAIN_COURSE:return (int32_t)MTRAIN_COURSES;
        case STY_OUTPH_FALLBACK:return (int32_t)MOD_OUTPHASE_FALLBACK_COUNT;
        case STY_CLK_POLICY:  return (int32_t)MOD_CLOCK_POLICY_COUNT;
        case STY_CPUSCOPE:    return (int32_t)MOD_CPU_SCOPE_COUNT;
        case STY_SSBPIPE:     return (int32_t)SSB_PIPELINE_COUNT;
        case STY_HOME_LAYOUT: return (int32_t)UI_HOME_LAYOUT_COUNT;
        case STY_SHORTCUT:    return (int32_t)UI_SHORTCUT_COUNT;
        case STY_TOGGLE:
        default:              return 2;
    }
}
#endif

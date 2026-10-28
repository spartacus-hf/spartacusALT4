#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_priv.h"
#include "mod_dsp_tables_priv.h"
#include "mod_cessb_policy.h"
#include "mod_ssb_envelope_logic.h"

/* Audio, SSB, DSP, compressor, EQ, AGC, gate, and ALC API. */

/* Each scalar audio setting with its safe default and limits
 * (mod_control_sanitize's contract): one table for the load-time sweep and
 * the setters, and one copy of the clamp. */
typedef struct { float *value; float fallback, lower, upper; } audio_limit_t;
enum {
    L_DRIVE, L_GATE, L_KNEE, L_FM_DEV, L_FM_INPUT_GAIN, L_AM_CARRIER, L_AM_DEPTH, L_USB_INPUT_GAIN,
    L_CESSB_CLIP, L_COMP_TARGET, L_COMP_RATIO, L_EQ_BASS, L_EQ_TREBLE, L_MIC_AGC_TARGET, L_MIC_AGC_MAX_GAIN,
    L_NG_THRESH, L_ALC_CEILING,
    L_COUNT
};
static const audio_limit_t audio_limits[L_COUNT] = {
    [L_DRIVE] = {&cfg_drive, SAMPLE_DRIVE, 0.1f, 1.0f},
    [L_GATE] = {&cfg_gate, SAMPLE_GATE, 0.0f, 0.05f},
    [L_KNEE] = {&cfg_knee, SOFT_KNEE, 0.3f, 0.99f},
    [L_FM_DEV] = {&cfg_fm_dev, FM_DEVIATION_HZ, FM_DEVIATION_MIN_HZ, FM_DEVIATION_MAX_HZ},
    [L_FM_INPUT_GAIN] = {&cfg_fm_input_gain, 1.0f, 0.05f, 8.0f},
    [L_AM_CARRIER] = {&cfg_am_carrier, AM_CARRIER_LEVEL, 0.1f, 1.0f},
    [L_AM_DEPTH] = {&cfg_am_depth, AM_MOD_DEPTH, 0.0f, 1.0f},
    [L_USB_INPUT_GAIN] = {&cfg_usb_input_gain, 1.0f, 0.05f, 8.0f},
    [L_CESSB_CLIP] = {&cfg_cessb_clip, 0.94f, 0.3f, 1.0f},
    [L_COMP_TARGET] = {&cfg_comp_target, 0.50f, 0.1f, 0.9f},
    [L_COMP_RATIO] = {&cfg_comp_ratio, 4.0f, 1.0f, 20.0f},
    [L_EQ_BASS] = {&cfg_eq_bass, 0.0f, -6.0f, 6.0f},
    [L_EQ_TREBLE] = {&cfg_eq_treble, 0.0f, -6.0f, 6.0f},
    [L_MIC_AGC_TARGET] = {&cfg_mic_agc_target, MIC_AGC_TARGET_DEFAULT, 0.05f, 0.70f},
    [L_MIC_AGC_MAX_GAIN] = {&cfg_mic_agc_max_gain, MIC_AGC_MAX_GAIN_DEFAULT, 1.0f, 40.0f},
    [L_NG_THRESH] = {&cfg_ng_thresh, NG_THRESH_DEFAULT, 0.0f, 0.10f},
    [L_ALC_CEILING] = {&cfg_alc_ceiling, ALC_DEFAULT_CEILING, 0.5f, 0.99f},
};

static __attribute__((noinline)) void audio_limit(uint8_t k, float v)
{
    const audio_limit_t *l = &audio_limits[k];
    *l->value = mod_control_sanitize(v, l->fallback, l->lower, l->upper);
}

void sanitize_audio_runtime_settings(void)
{
    for (uint8_t k = 0U; k < (uint8_t)L_COUNT; ++k) audio_limit(k, *audio_limits[k].value);
}

void MOD_SetDrive(float d)
{
    audio_limit(L_DRIVE, d);
}

float MOD_GetDrive(void) { return cfg_drive; }

void MOD_SetGate(float g)
{
    audio_limit(L_GATE, g);
}

float MOD_GetGate(void) { return cfg_gate; }

void MOD_SetKnee(float k)
{
    audio_limit(L_KNEE, k);
}

float MOD_GetKnee(void) { return cfg_knee; } /* DEPRECATED */

void MOD_SetFmDeviation(float d)
{
    audio_limit(L_FM_DEV, d);
    cfg_fm_deviation_preset = FM_DEVIATION_PRESET_USER;
}

float MOD_GetFmDeviation(void) { return cfg_fm_dev; }

void MOD_SetFmDeviationPreset(fm_deviation_preset_t preset)
{
    if ((uint32_t)preset >= (uint32_t)FM_DEVIATION_PRESET_COUNT) preset = FM_DEVIATION_PRESET_NFM_2K5;
    cfg_fm_deviation_preset = preset;
    switch (preset) {
        case FM_DEVIATION_PRESET_NFM_2K5: cfg_fm_dev = 2500.0f; break;
        case FM_DEVIATION_PRESET_FM_5K:   cfg_fm_dev = 5000.0f; break;
        case FM_DEVIATION_PRESET_TEST_7K5:cfg_fm_dev = 7500.0f; break;
        case FM_DEVIATION_PRESET_USER:
        default:                          break;
    }
}

fm_deviation_preset_t MOD_GetFmDeviationPreset(void)
{
    return ((uint32_t)cfg_fm_deviation_preset < (uint32_t)FM_DEVIATION_PRESET_COUNT)
         ? cfg_fm_deviation_preset : FM_DEVIATION_PRESET_NFM_2K5;
}

void MOD_SetFmTestTone(uint8_t en) { cfg_fm_test_tone = en ? 1U : 0U; requested_reset = 1U; }

uint8_t MOD_GetFmTestTone(void) { return cfg_fm_test_tone ? 1U : 0U; }

void MOD_SetFmInputGain(float g)
{
    audio_limit(L_FM_INPUT_GAIN, g);
}

float MOD_GetFmInputGain(void) { return cfg_fm_input_gain; }

void MOD_SetAmCarrier(float c)
{
    audio_limit(L_AM_CARRIER, c);
}

float MOD_GetAmCarrier(void) { return cfg_am_carrier; }

void MOD_SetAmDepth(float d)
{
    audio_limit(L_AM_DEPTH, d);
}

float MOD_GetAmDepth(void) { return cfg_am_depth; }

void MOD_SetAmProfile(am_profile_t profile)
{
    cfg_am_profile = ((uint32_t)profile < (uint32_t)AM_PROFILE_COUNT) ? profile : AM_PROFILE_EFFICIENT;
    requested_reset = 1U;
}

am_profile_t MOD_GetAmProfile(void)
{
    return ((uint32_t)cfg_am_profile < (uint32_t)AM_PROFILE_COUNT) ? cfg_am_profile : AM_PROFILE_EFFICIENT;
}

uint32_t MOD_GetUsbFeedback10_14(void)
{
    uint32_t pm = enter_critical();
    uint32_t fb = usb_feedback_q10_14 & 0x00FFFFFFU;
    exit_critical(pm);
    return fb;
}

void MOD_SetAudioProfile(audio_profile_t profile)
{
    profile = (audio_profile_t)mod_control_enum_sanitize(
        (uint32_t)profile, (uint32_t)AUDIO_PROFILE_COUNT,
        (uint32_t)AUDIO_PROFILE_CLEAN);
    apply_audio_profile(profile);
    requested_reset = 1U;
}

audio_profile_t MOD_GetAudioProfile(void)
{
    return (audio_profile_t)mod_control_enum_sanitize(
        (uint32_t)active_audio_profile, (uint32_t)AUDIO_PROFILE_COUNT,
        (uint32_t)AUDIO_PROFILE_CLEAN);
}

const char *MOD_GetAudioProfileLabel(audio_profile_t profile)
{
    switch (profile) {
        case AUDIO_PROFILE_CLEAN:    return "CLEAN";
        case AUDIO_PROFILE_ENHANCED: return "ENH";
        case AUDIO_PROFILE_DX:       return "DX";
        case AUDIO_PROFILE_QRP:      return "QRP";
        case AUDIO_PROFILE_USB_HIFI: return "USBHI";
        case AUDIO_PROFILE_CUSTOM:   return "CUSTOM";
        default:                     return "?";
    }
}

void MOD_SetUsbAudioPolicy(usb_audio_policy_t policy)
{
    cfg_usb_clean_mode = ((uint32_t)policy < (uint32_t)USB_AUDIO_POLICY_COUNT)
                       ? (uint8_t)policy
                       : (uint8_t)USB_AUDIO_POLICY_DIGI_CLEAN;
    mark_profile_custom();
    requested_reset = 1U;
}

usb_audio_policy_t MOD_GetUsbAudioPolicy(void)
{
    return (cfg_usb_clean_mode == (uint8_t)USB_AUDIO_POLICY_VOICE_PROC)
         ? USB_AUDIO_POLICY_VOICE_PROC
         : USB_AUDIO_POLICY_DIGI_CLEAN;
}

static void usb_tx_gate_update(void)
{
    usb_tx_gate_closed = (uint8_t)((cfg_usb_vox == 0U) && (usb_cat_ptt == 0U));
}

void MOD_SetUsbVox(uint8_t on) { cfg_usb_vox = on ? 1U : 0U; usb_tx_gate_update(); }
uint8_t MOD_GetUsbVox(void) { return cfg_usb_vox; }
void MOD_SetUsbCatPtt(uint8_t on) { usb_cat_ptt = on ? 1U : 0U; usb_tx_gate_update(); }
uint8_t MOD_GetUsbCatPtt(void) { return usb_cat_ptt; }

void MOD_SetDigiTone(uint8_t on)
{
    on = on ? 1U : 0U;
    if (on == cfg_digi_tone) return;
    cfg_digi_tone = on;
    requested_reset = 1U;
}
uint8_t MOD_GetDigiTone(void) { return cfg_digi_tone; }

void MOD_SetUsbInputGain(float g)
{
    audio_limit(L_USB_INPUT_GAIN, g);
}

float MOD_GetUsbInputGain(void) { return cfg_usb_input_gain; }

void MOD_SetUsbCleanMode(uint8_t en) { MOD_SetUsbAudioPolicy(en ? USB_AUDIO_POLICY_DIGI_CLEAN : USB_AUDIO_POLICY_VOICE_PROC); }

uint8_t MOD_GetUsbCleanMode(void) { return cfg_usb_clean_mode; }

void MOD_SetUsbChannelMode(usb_audio_channel_mode_t mode)
{
    cfg_usb_channel_mode = (usb_audio_channel_mode_t)mod_control_enum_sanitize(
        (uint32_t)mode, (uint32_t)USB_AUDIO_CHAN_COUNT,
        (uint32_t)USB_AUDIO_CHAN_AVERAGE);
    requested_reset = 1U;
}

usb_audio_channel_mode_t MOD_GetUsbChannelMode(void)
{
    return (usb_audio_channel_mode_t)mod_control_enum_sanitize(
        (uint32_t)cfg_usb_channel_mode, (uint32_t)USB_AUDIO_CHAN_COUNT,
        (uint32_t)USB_AUDIO_CHAN_AVERAGE);
}

void MOD_SetDspMathMode(dsp_math_mode_t mode)
{
#if MOD_DEBUG_SWITCHES
    cfg_dsp_math_mode = (dsp_math_mode_t)mod_control_enum_sanitize(
        (uint32_t)mode, (uint32_t)DSP_MATH_COUNT,
        (uint32_t)DSP_MATH_SAFE);
    requested_reset = 1U;
#else
    (void)mode;
#endif
}

dsp_math_mode_t MOD_GetDspMathMode(void)
{
    return (dsp_math_mode_t)mod_control_enum_sanitize(
        (uint32_t)cfg_dsp_math_mode, (uint32_t)DSP_MATH_COUNT,
        (uint32_t)DSP_MATH_SAFE);
}

void MOD_SetGainTelemetryMode(gain_telemetry_mode_t mode)
{
#if MOD_DEBUG_SWITCHES
    cfg_gain_telemetry_mode = (gain_telemetry_mode_t)mod_control_enum_sanitize(
        (uint32_t)mode, (uint32_t)GAIN_TELEMETRY_COUNT,
        (uint32_t)GAIN_TELEMETRY_OFF);
#else
    (void)mode;
#endif
}

gain_telemetry_mode_t MOD_GetGainTelemetryMode(void)
{
    return (gain_telemetry_mode_t)mod_control_enum_sanitize(
        (uint32_t)cfg_gain_telemetry_mode, (uint32_t)GAIN_TELEMETRY_COUNT,
        (uint32_t)GAIN_TELEMETRY_OFF);
}

void MOD_SetCompMathMode(comp_math_mode_t mode)
{
#if MOD_DEBUG_SWITCHES
    cfg_comp_math_mode = (comp_math_mode_t)mod_control_enum_sanitize(
        (uint32_t)mode, (uint32_t)COMP_MATH_COUNT,
        (uint32_t)COMP_MATH_LUT);
    mod_tx_pipeline_comp_lut_sync();
#else
    (void)mode;
#endif
}

comp_math_mode_t MOD_GetCompMathMode(void)
{
    return (comp_math_mode_t)mod_control_enum_sanitize(
        (uint32_t)cfg_comp_math_mode, (uint32_t)COMP_MATH_COUNT,
        (uint32_t)COMP_MATH_LUT);
}

void MOD_SetRfMetricMode(rf_metric_mode_t mode)
{
#if MOD_DEBUG_SWITCHES
    cfg_rf_metric_mode = (rf_metric_mode_t)mod_control_enum_sanitize(
        (uint32_t)mode, (uint32_t)RF_METRIC_COUNT,
        (uint32_t)RF_METRIC_EXACT);
#else
    (void)mode;
#endif
}

rf_metric_mode_t MOD_GetRfMetricMode(void)
{
    return (rf_metric_mode_t)mod_control_enum_sanitize(
        (uint32_t)cfg_rf_metric_mode, (uint32_t)RF_METRIC_COUNT,
        (uint32_t)RF_METRIC_EXACT);
}

void MOD_SetSsbPipelineMode(ssb_pipeline_mode_t mode)
{
    if ((uint32_t)mode >= (uint32_t)SSB_PIPELINE_COUNT) mode = SSB_PIPELINE_LEGACY;
    cfg_ssb_pipeline_mode = mode;
    rate_plan_fault_latched = 0U;
    mark_profile_custom();
    requested_reset = 1U;
}

ssb_pipeline_mode_t MOD_GetSsbPipelineMode(void)
{
    return ((uint32_t)cfg_ssb_pipeline_mode < (uint32_t)SSB_PIPELINE_COUNT)
         ? cfg_ssb_pipeline_mode : SSB_PIPELINE_LEGACY;
}

const char *MOD_GetSsbPipelineModeLabel(ssb_pipeline_mode_t mode)
{
    switch (mode) {
        case SSB_PIPELINE_STRICT_QMX12_EXP: return "QMX12*";
        case SSB_PIPELINE_LEGACY:
        default:                            return "LEGACY";
    }
}

const mod_rate_plan_t *MOD_GetActiveRatePlan(void)
{
    static mod_rate_plan_t plan;
    ssb_analytic_backend_t backend = MOD_GetSsbAnalyticBackend();
    ssb_pipeline_mode_t pipe = MOD_GetSsbPipelineMode();

    plan.input_rate_hz = AUDIO_RATE_HZ_U32;
    plan.rf_update_rate_hz = AUDIO_RATE_HZ_U32;
    plan.rf_samples_per_frame = AUDIO_FRAME_SIZE;
    plan.strict_frame = 0U;
    plan.backend = (uint8_t)backend;

    if (active_mod_type != MOD_TYPE_SSB) {
        plan.dsp_rate_hz = AUDIO_RATE_HZ_U32;
        plan.hilbert_taps = 0U;
        plan.decimation = 1U;
        plan.interpolation = 1U;
        plan.low_rate_samples_per_frame = AUDIO_FRAME_SIZE;
        plan.label = "48K";
        return &plan;
    }

    switch (backend) {
        case SSB_ANALYTIC_HILBERT:
            plan.dsp_rate_hz = AUDIO_RATE_HZ_U32;
            plan.hilbert_taps = HILB_NUM_TAPS;
            plan.decimation = 1U;
            plan.interpolation = 1U;
            plan.low_rate_samples_per_frame = AUDIO_FRAME_SIZE;
            plan.label = "H48";
            break;
        case SSB_ANALYTIC_HILBERT_PAIR_24:
            plan.dsp_rate_hz = 24000U;
            plan.hilbert_taps = HILB_PAIR24_NUM_TAPS;
            plan.decimation = HILB_PAIR24_DECIM;
            plan.interpolation = HILB_PAIR24_DECIM;
            plan.low_rate_samples_per_frame = (uint8_t)(AUDIO_FRAME_SIZE / HILB_PAIR24_DECIM);
            plan.label = "H24";
            break;
        case SSB_ANALYTIC_HILBERT_PAIR_12:
            plan.dsp_rate_hz = 12000U;
            plan.hilbert_taps = HILB_PAIR12_NUM_TAPS;
            plan.decimation = HILB_PAIR12_DECIM;
            plan.interpolation = HILB_PAIR12_DECIM;
            plan.low_rate_samples_per_frame = (uint8_t)(AUDIO_FRAME_SIZE / HILB_PAIR12_DECIM);
            plan.strict_frame = (pipe == SSB_PIPELINE_STRICT_QMX12_EXP) ? 1U : 0U;
            plan.label = plan.strict_frame ? "QMX12" : "H12";
            break;
        case SSB_ANALYTIC_H12P:
            plan.dsp_rate_hz = 12000U;
            plan.hilbert_taps = 255U;
            plan.decimation = 4U;
            plan.interpolation = 4U;
            plan.low_rate_samples_per_frame = (uint8_t)(AUDIO_FRAME_SIZE / 4U);
            plan.label = "H12P";
            break;
        case SSB_ANALYTIC_WEAVER:
        default:
            plan.dsp_rate_hz = AUDIO_RATE_HZ_U32;
            plan.hilbert_taps = 0U;
            plan.decimation = 1U;
            plan.interpolation = 1U;
            plan.low_rate_samples_per_frame = AUDIO_FRAME_SIZE;
            plan.label = "WEAVER";
            break;
    }

    return &plan;
}

void mod_rate_plan_validate_active(void)
{
    static uint32_t last_sig = 0xFFFFFFFFUL;
    static uint8_t last_invalid = 1U;
    uint32_t sig = ((uint32_t)active_config & 0xFFUL)
                 | (((uint32_t)active_mod_type & 0xFUL) << 8)
                 | (((uint32_t)MOD_GetSsbAnalyticBackend() & 0xFUL) << 12)
                 | (((uint32_t)MOD_GetSsbPipelineMode() & 0xFUL) << 16);

    if (sig == last_sig) {
        if (last_invalid != 0U) {
            rate_plan_fault_latched = 1U;
        }
        return;
    }

    const mod_rate_plan_t *plan = MOD_GetActiveRatePlan();
    uint8_t invalid = 0U;
    if (plan == NULL) invalid = 1U;
    else if (plan->input_rate_hz != AUDIO_RATE_HZ_U32) invalid = 1U;
    else if (plan->rf_update_rate_hz != AUDIO_RATE_HZ_U32) invalid = 1U;
    else if (plan->rf_samples_per_frame != AUDIO_FRAME_SIZE) invalid = 1U;
    else if ((plan->decimation == 0U) || (plan->interpolation == 0U)) invalid = 1U;
    else if ((uint32_t)plan->low_rate_samples_per_frame * (uint32_t)plan->decimation != AUDIO_FRAME_SIZE) invalid = 1U;
    else if (plan->decimation != plan->interpolation) invalid = 1U;

    last_sig = sig;
    last_invalid = invalid;

    if (invalid != 0U) {
        rate_plan_fault_latched = 1U;
        mod_pipeline_note(MOD_PIPELINE_STAGE_RENDER,
                          MOD_PIPELINE_REASON_RATE_PLAN_INVALID,
                          (uint32_t)active_config);
    }
}

uint8_t MOD_GetCordicEnabled(void) { return (cfg_cordic_enable && cordic_ready) ? 1U : 0U; }

#if MOD_DEBUG_SWITCHES
void    MOD_SetCordicEnable(uint8_t en) { cfg_cordic_enable = en ? 1U : 0U; }
#else
void    MOD_SetCordicEnable(uint8_t en) { (void)en; }
#endif

void MOD_SetCessbEnable(uint8_t en)
{
    cfg_cessb_enable = mod_cessb_enable_for_build(en);
    mark_profile_custom();
}

uint8_t MOD_GetCessbEnable(void) { return mod_cessb_enable_for_build(cfg_cessb_enable); }

void  MOD_SetCessbClip(float c) { audio_limit(L_CESSB_CLIP, c); mark_profile_custom(); }

float MOD_GetCessbClip(void) { return cfg_cessb_clip; }

void  MOD_SetCompEnable(uint8_t en) { cfg_comp_enable = en ? 1U : 0U; mod_tx_pipeline_comp_lut_sync(); mark_profile_custom(); }

uint8_t MOD_GetCompEnable(void) { return cfg_comp_enable; }

void  MOD_SetCompTarget(float t) { audio_limit(L_COMP_TARGET, t); mod_tx_pipeline_comp_lut_sync(); mark_profile_custom(); }

float MOD_GetCompTarget(void) { return cfg_comp_target; }

void  MOD_SetCompRatio(float r) { audio_limit(L_COMP_RATIO, r); mod_tx_pipeline_comp_lut_sync(); mark_profile_custom(); }

float MOD_GetCompRatio(void) { return cfg_comp_ratio; }

void  MOD_SetEqEnable(uint8_t en) { cfg_eq_enable = en ? 1U : 0U; mark_profile_custom(); }

uint8_t MOD_GetEqEnable(void) { return cfg_eq_enable; }

void  MOD_SetEqBass(float b) { audio_limit(L_EQ_BASS, b); recompute_eq_coeffs(); mark_profile_custom(); }

float MOD_GetEqBass(void) { return cfg_eq_bass; }

void  MOD_SetEqTreble(float t) { audio_limit(L_EQ_TREBLE, t); recompute_eq_coeffs(); mark_profile_custom(); }

float MOD_GetEqTreble(void) { return cfg_eq_treble; }

void MOD_SetSsbPhaseBias(float b)
{
    cfg_ssb_phase_bias = mod_ssb_phase_bias_for_build(b);
    sanitize_ssb_runtime_settings();
    mark_profile_custom();
}

float MOD_GetSsbPhaseBias(void) { return mod_ssb_phase_bias_for_build(cfg_ssb_phase_bias); }

void MOD_SetSsbAnalyticBackend(ssb_analytic_backend_t v)
{
    if (((uint32_t)v >= (uint32_t)SSB_ANALYTIC_COUNT) ||
        (MOD_SsbAnalyticBackendBuildAvailable(v) == 0U)) {
        v = SSB_ANALYTIC_WEAVER;
    }
    cfg_ssb_analytic_backend = v;
    rate_plan_fault_latched = 0U;
    sanitize_ssb_runtime_settings();
    load_current_calibration_context();
    mark_profile_custom();
    requested_reset = 1U;
}

ssb_analytic_backend_t MOD_GetSsbAnalyticBackend(void)
{
    ssb_analytic_backend_t backend = cfg_ssb_analytic_backend;
    return (((uint32_t)backend < (uint32_t)SSB_ANALYTIC_COUNT) &&
            (MOD_SsbAnalyticBackendBuildAvailable(backend) != 0U))
         ? backend : SSB_ANALYTIC_WEAVER;
}

void MOD_SetSsbCeEnable(uint8_t en)
{
    cfg_ssb_ce_enable = mod_ssb_ce_enable_for_build(en);
    sanitize_ssb_runtime_settings();
    mark_profile_custom();
    requested_reset = 1U;
}

uint8_t MOD_GetSsbCeEnable(void) { return mod_ssb_ce_enable_for_build(cfg_ssb_ce_enable); }

void MOD_SetSsbCeMix(float v) { cfg_ssb_ce_mix = v; sanitize_ssb_runtime_settings(); mark_profile_custom(); }

float MOD_GetSsbCeMix(void) { return cfg_ssb_ce_mix; }

void MOD_SetSsbCeFloor(float v) { cfg_ssb_ce_floor = v; sanitize_ssb_runtime_settings(); mark_profile_custom(); }

float MOD_GetSsbCeFloor(void) { return cfg_ssb_ce_floor; }

void MOD_SetSsbCeKnee(float v) { cfg_ssb_ce_knee = v; sanitize_ssb_runtime_settings(); mark_profile_custom(); }

float MOD_GetSsbCeKnee(void) { return cfg_ssb_ce_knee; }

void MOD_SetSsbLowCut(float v) { cfg_ssb_low_cut = v; sanitize_ssb_runtime_settings(); mark_profile_custom(); }

float MOD_GetSsbLowCut(void) { return cfg_ssb_low_cut; }

void MOD_SetTxBwPreset(tx_bw_preset_t v)
{
    cfg_tx_bw_preset = (tx_bw_preset_t)mod_control_enum_sanitize(
        (uint32_t)v, (uint32_t)TX_BW_COUNT, (uint32_t)TX_BW_200_2900);
    sanitize_ssb_runtime_settings();
    mark_profile_custom();
    requested_reset = 1U;
}

tx_bw_preset_t MOD_GetTxBwPreset(void)
{
    return (tx_bw_preset_t)mod_control_enum_sanitize(
        (uint32_t)cfg_tx_bw_preset, (uint32_t)TX_BW_COUNT,
        (uint32_t)TX_BW_200_2900);
}

void MOD_SetOutphaseLaw(outphase_law_t v)
{
    cfg_outphase_law = (outphase_law_t)mod_control_enum_sanitize(
        (uint32_t)v, (uint32_t)OUTPHASE_LAW_COUNT,
        (uint32_t)OUTPHASE_LAW_CLASSIC);
    build_outphase_lut();
    mark_profile_custom();
    requested_reset = 1U;
}

outphase_law_t MOD_GetOutphaseLaw(void)
{
    return (outphase_law_t)mod_control_enum_sanitize(
        (uint32_t)cfg_outphase_law, (uint32_t)OUTPHASE_LAW_COUNT,
        (uint32_t)OUTPHASE_LAW_CLASSIC);
}

void MOD_SetOutphaseSsbMaxTheta(float theta)
{
    cfg_outphase_ssb_max_theta = theta;
    sanitize_outphase_policy_settings();
    mark_profile_custom();
}

float MOD_GetOutphaseSsbMaxTheta(void) { return cfg_outphase_ssb_max_theta; }

void MOD_SetOutphaseAmMaxTheta(float theta)
{
    cfg_outphase_am_max_theta = theta;
    sanitize_outphase_policy_settings();
    mark_profile_custom();
}

float MOD_GetOutphaseAmMaxTheta(void) { return cfg_outphase_am_max_theta; }

void MOD_SetOutphaseFmTheta(float theta)
{
    cfg_outphase_fm_theta = theta;
    sanitize_outphase_policy_settings();
    mark_profile_custom();
}

float MOD_GetOutphaseFmTheta(void) { return cfg_outphase_fm_theta; }

void MOD_SetOutphaseCwTheta(float theta)
{
    cfg_outphase_cw_theta = theta;
    sanitize_outphase_policy_settings();
    mark_profile_custom();
}

float MOD_GetOutphaseCwTheta(void) { return cfg_outphase_cw_theta; }

void MOD_SetOutphaseTestTheta(float theta)
{
    cfg_outphase_test_theta = theta;
    sanitize_outphase_policy_settings();
    mark_profile_custom();
}

float MOD_GetOutphaseTestTheta(void) { return cfg_outphase_test_theta; }

void MOD_SetOutphaseFallbackPolicy(mod_outphase_fallback_t policy)
{
    cfg_outphase_fallback_policy = ((uint32_t)policy < (uint32_t)MOD_OUTPHASE_FALLBACK_COUNT)
                                 ? policy : MOD_OUTPHASE_FALLBACK_COMPRESS;
    sanitize_outphase_policy_settings();
    mark_profile_custom();
}

mod_outphase_fallback_t MOD_GetOutphaseFallbackPolicy(void)
{
    return ((uint32_t)cfg_outphase_fallback_policy < (uint32_t)MOD_OUTPHASE_FALLBACK_COUNT)
         ? cfg_outphase_fallback_policy : MOD_OUTPHASE_FALLBACK_COMPRESS;
}

void    MOD_SetMicAgcEnable(uint8_t en) { cfg_mic_agc_enable = en ? 1U : 0U; mark_profile_custom(); }

uint8_t MOD_GetMicAgcEnable(void)       { return cfg_mic_agc_enable; }

void    MOD_SetMicAgcTarget(float t)    { audio_limit(L_MIC_AGC_TARGET, t); mark_profile_custom(); }

float   MOD_GetMicAgcTarget(void)       { return cfg_mic_agc_target; }

void    MOD_SetMicAgcMaxGain(float g)   { audio_limit(L_MIC_AGC_MAX_GAIN, g); mark_profile_custom(); }

float   MOD_GetMicAgcMaxGain(void)      { return cfg_mic_agc_max_gain; }

void    MOD_SetNoiseGateEnable(uint8_t en) { cfg_ng_enable = en ? 1U : 0U; mark_profile_custom(); }

uint8_t MOD_GetNoiseGateEnable(void)       { return cfg_ng_enable; }

void    MOD_SetNoiseGateThresh(float t)    { audio_limit(L_NG_THRESH, t); mark_profile_custom(); }

float   MOD_GetNoiseGateThresh(void)       { return cfg_ng_thresh; }

void MOD_SetAlcEnable(uint8_t en)
{
    uint8_t enabled = en ? 1U : 0U;
    if (enabled != cfg_alc_enable) requested_reset = 1U;
    cfg_alc_enable = enabled;
    mark_profile_custom();
}

uint8_t MOD_GetAlcEnable(void)       { return cfg_alc_enable; }

void    MOD_SetAlcCeiling(float c)   { audio_limit(L_ALC_CEILING, c); mark_profile_custom(); }

float   MOD_GetAlcCeiling(void)      { return cfg_alc_ceiling; }

void    MOD_SetCessbIterations(uint8_t n) { if (n > CESSB_ITER_MAX) n = CESSB_ITER_MAX; cfg_cessb_iter = n; mark_profile_custom(); }

uint8_t MOD_GetCessbIterations(void)      { return cfg_cessb_iter; }

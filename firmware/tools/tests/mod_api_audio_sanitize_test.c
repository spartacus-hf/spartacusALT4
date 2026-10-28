#include "mod_priv.h"

#include <math.h>
#include <stdio.h>

float cfg_drive;
float cfg_gate;
float cfg_knee;
float cfg_fm_dev;
float cfg_fm_input_gain;
fm_deviation_preset_t cfg_fm_deviation_preset;
float cfg_am_carrier;
float cfg_am_depth;
float cfg_usb_input_gain;
float cfg_cessb_clip;
float cfg_comp_target;
float cfg_comp_ratio;
float cfg_eq_bass;
float cfg_eq_treble;
float cfg_mic_agc_target;
float cfg_mic_agc_max_gain;
float cfg_ng_thresh;
float cfg_alc_ceiling;
volatile usb_audio_channel_mode_t cfg_usb_channel_mode;
#if MOD_DEBUG_SWITCHES
volatile dsp_math_mode_t cfg_dsp_math_mode;
volatile gain_telemetry_mode_t cfg_gain_telemetry_mode;
volatile rf_metric_mode_t cfg_rf_metric_mode;
#endif
ssb_analytic_backend_t cfg_ssb_analytic_backend;
volatile uint8_t requested_reset;

static unsigned failures;

#define CHECK_CLOSE(actual, expected) do { \
    float check_value = (actual); \
    if (!isfinite(check_value) || fabsf(check_value - (expected)) > 1.0e-7f) { \
        fprintf(stderr, "%s:%d: got %.9g, expected %.9g\n", \
                __FILE__, __LINE__, (double)check_value, (double)(expected)); \
        failures++; \
    } \
} while (0)

static void test_scalar_setters(void)
{
    MOD_SetDrive(NAN);
    CHECK_CLOSE(MOD_GetDrive(), SAMPLE_DRIVE);
    MOD_SetDrive(INFINITY);
    CHECK_CLOSE(MOD_GetDrive(), 1.0f);

    MOD_SetGate(NAN);
    CHECK_CLOSE(MOD_GetGate(), SAMPLE_GATE);
    MOD_SetGate(-INFINITY);
    CHECK_CLOSE(MOD_GetGate(), 0.0f);

    MOD_SetKnee(NAN);
    CHECK_CLOSE(MOD_GetKnee(), SOFT_KNEE);

    MOD_SetFmDeviation(NAN);
    CHECK_CLOSE(MOD_GetFmDeviation(), FM_DEVIATION_HZ);
    MOD_SetFmDeviation(INFINITY);
    CHECK_CLOSE(MOD_GetFmDeviation(), FM_DEVIATION_MAX_HZ);

    MOD_SetFmInputGain(NAN);
    CHECK_CLOSE(MOD_GetFmInputGain(), 1.0f);
    MOD_SetAmCarrier(NAN);
    CHECK_CLOSE(MOD_GetAmCarrier(), AM_CARRIER_LEVEL);
    MOD_SetAmDepth(NAN);
    CHECK_CLOSE(MOD_GetAmDepth(), AM_MOD_DEPTH);
    MOD_SetUsbInputGain(NAN);
    CHECK_CLOSE(MOD_GetUsbInputGain(), 1.0f);
}

static void test_loaded_setting_sanitizer(void)
{
    cfg_drive = NAN;
    cfg_gate = NAN;
    cfg_knee = NAN;
    cfg_fm_dev = NAN;
    cfg_fm_input_gain = NAN;
    cfg_am_carrier = NAN;
    cfg_am_depth = NAN;
    cfg_usb_input_gain = NAN;
    cfg_cessb_clip = NAN;
    cfg_comp_target = NAN;
    cfg_comp_ratio = NAN;
    cfg_eq_bass = NAN;
    cfg_eq_treble = NAN;
    cfg_mic_agc_target = NAN;
    cfg_mic_agc_max_gain = NAN;
    cfg_ng_thresh = NAN;
    cfg_alc_ceiling = NAN;

    sanitize_audio_runtime_settings();

    CHECK_CLOSE(cfg_drive, SAMPLE_DRIVE);
    CHECK_CLOSE(cfg_gate, SAMPLE_GATE);
    CHECK_CLOSE(cfg_knee, SOFT_KNEE);
    CHECK_CLOSE(cfg_fm_dev, FM_DEVIATION_HZ);
    CHECK_CLOSE(cfg_fm_input_gain, 1.0f);
    CHECK_CLOSE(cfg_am_carrier, AM_CARRIER_LEVEL);
    CHECK_CLOSE(cfg_am_depth, AM_MOD_DEPTH);
    CHECK_CLOSE(cfg_usb_input_gain, 1.0f);
    CHECK_CLOSE(cfg_cessb_clip, 0.94f);
    CHECK_CLOSE(cfg_comp_target, 0.50f);
    CHECK_CLOSE(cfg_comp_ratio, 4.0f);
    CHECK_CLOSE(cfg_eq_bass, 0.0f);
    CHECK_CLOSE(cfg_eq_treble, 0.0f);
    CHECK_CLOSE(cfg_mic_agc_target, MIC_AGC_TARGET_DEFAULT);
    CHECK_CLOSE(cfg_mic_agc_max_gain, MIC_AGC_MAX_GAIN_DEFAULT);
    CHECK_CLOSE(cfg_ng_thresh, NG_THRESH_DEFAULT);
    CHECK_CLOSE(cfg_alc_ceiling, ALC_DEFAULT_CEILING);
}

static void test_invalid_enum_fallbacks(void)
{
    MOD_SetUsbChannelMode((usb_audio_channel_mode_t)-1);
    if (MOD_GetUsbChannelMode() != USB_AUDIO_CHAN_AVERAGE) failures++;
    MOD_SetDspMathMode((dsp_math_mode_t)-1);
    if (MOD_GetDspMathMode() != DSP_MATH_SAFE) failures++;
    MOD_SetGainTelemetryMode((gain_telemetry_mode_t)-1);
    if (MOD_GetGainTelemetryMode() != GAIN_TELEMETRY_OFF) failures++;
    MOD_SetRfMetricMode((rf_metric_mode_t)-1);
    if (MOD_GetRfMetricMode() != RF_METRIC_EXACT) failures++;

    cfg_ssb_analytic_backend = (ssb_analytic_backend_t)-1;
    if (MOD_GetSsbAnalyticBackend() != SSB_ANALYTIC_WEAVER) failures++;
}

int main(void)
{
    test_scalar_setters();
    test_loaded_setting_sanitizer();
    test_invalid_enum_fallbacks();
    if (failures != 0U) return 1;
    puts("audio API sanitization: persisted scalars and invalid enums: PASS");
    return 0;
}

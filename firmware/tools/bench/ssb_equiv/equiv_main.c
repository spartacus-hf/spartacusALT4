/* Host equivalence harness for the production SSB renderer: drive
 * mod_tx_pipeline_process_frame with deterministic audio across a set of
 * configurations and dump, per frame, every compare tick, the gate flags,
 * the limit counters and the renderer's filter/gain/telemetry state
 * (harness_state, appended to the pipeline source by build.sh).
 * Build it against two source trees and compare the dumps byte for byte
 * (run.sh).
 *
 * v2 (2026-09-28): the speech tools only run with USB clean mode off (the
 * default is DIGI CLEAN), so v1's "tools1" never reached them. Adds mic AGC,
 * noise gate, live telemetry, LUT and exact compressor, EQ shelves, input
 * gain clipping, HIFI bypass, local sources, AC offset, short frames.
 * v3 (2026-10-03): every run for both SSB ANALYTIC backends (WEAVER, H12P),
 * with the H12P renderer's gate, fade, DC block and analytic state; the
 * diagnostic-image-only switches are constants elsewhere (DBG). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mod_priv.h"

/* The DSP ENGINE / DEBUG switches are constants outside the diagnostic image. */
#if MOD_DEBUG_SWITCHES
#define DBG(stmt) (stmt)
#else
#define DBG(stmt) ((void)0)
#endif

void harness_state(float *o, uint32_t *u);
#define HS_FLOATS 26U
#define HS_WORDS  12U

static FILE *out;
static uint32_t lcg = 12345U;

static int16_t sample(int kind, uint32_t n, float amp)
{
    const float t = (float)n / 48000.0f;
    float x = 0.0f;
    switch (kind) {
        case 0: x = sinf(6.2831853f * 1000.0f * t); break;
        case 1: x = 0.5f * sinf(6.2831853f * 700.0f * t) + 0.5f * sinf(6.2831853f * 1900.0f * t); break;
        case 2: lcg = lcg * 1664525U + 1013904223U; x = ((float)(int32_t)lcg) / 2147483648.0f; break;
        case 3: /* bursts: 30 ms of tone, 20 ms of silence (gate open/close) */
            x = ((n / 480U) % 5U < 3U) ? sinf(6.2831853f * 1300.0f * t) : 0.0f; break;
        case 4: /* slow sweep 200..3200 Hz */
            x = sinf(6.2831853f * (200.0f + 1500.0f * t) * t); break;
        case 5: /* speech-like: noise bursts under a 4 Hz syllable envelope, quiet floor */
        {
            lcg = lcg * 1664525U + 1013904223U;
            float e = 0.5f + 0.5f * sinf(6.2831853f * 4.0f * t);
            x = e * e * e * (((float)(int32_t)lcg) / 2147483648.0f) + 0.002f * sinf(6.2831853f * 180.0f * t);
            break;
        }
        default: x = 0.0f; break;
    }
    float v = amp * x * 32767.0f;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)lrintf(v);
}

static void run_n(const char *name, int kind, float amp, uint32_t frames, uint32_t count)
{
    static uint32_t n = 0U;
    RenderFrame f;
    int16_t m[AUDIO_FRAME_SIZE];
    uint32_t hdr[3] = { 0x4B4C4230U, (uint32_t)strlen(name), frames };
    fwrite(hdr, sizeof(hdr[0]), 3, out);
    fwrite(name, 1, strlen(name), out);
    uint32_t tick_size = (uint32_t)sizeof(render_cmp_t);
    fwrite(&tick_size, sizeof(tick_size), 1, out);
    for (uint32_t k = 0U; k < frames; k++) {
        for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; i++) m[i] = sample(kind, n++, amp);
        memset(&f, 0, sizeof(f));
        mod_tx_pipeline_process_frame(&f, m, count);
        fwrite(f.mcmp1, sizeof(f.mcmp1[0]), AUDIO_FRAME_SIZE, out);
        fwrite(f.mcmp2, sizeof(f.mcmp2[0]), AUDIO_FRAME_SIZE, out);
        uint8_t flags[4] = { f.rf_active, f.tx_live, f.rf_gate_valid, f.frame_kind };
        fwrite(flags, 1, 4, out);
        uint32_t c[3] = { outphase_limit_count, low_env_inhibit_count, (uint32_t)outphase_limit_latched };
        fwrite(c, sizeof(c[0]), 3, out);
        float hs[HS_FLOATS]; uint32_t hw[HS_WORDS];
        memset(hs, 0, sizeof(hs)); memset(hw, 0, sizeof(hw));
        harness_state(hs, hw);
        fwrite(hs, sizeof(hs[0]), HS_FLOATS, out);
        fwrite(hw, sizeof(hw[0]), HS_WORDS, out);
    }
}

static void run(const char *name, int kind, float amp, uint32_t frames)
{
    run_n(name, kind, amp, frames, AUDIO_FRAME_SIZE);
}

static void profile(mod_config_t cfg)
{
    recompute_eq_coeffs();
    mod_tx_pipeline_select_profile(cfg);
    mod_tx_pipeline_reset();
}

/* proc 0: DIGI CLEAN (tools bypassed); 1: voice processing, everything on,
 * LUT compressor, live telemetry, input gain; 2: exact compressor + ALC,
 * noise gate with a high threshold, telemetry off. */
static void set_proc(uint32_t proc)
{
    active_audio_source = MOD_AUDIO_SOURCE_USB;
    local_audio_clean = 1U;
    active_audio_profile = AUDIO_PROFILE_CLEAN;
    cfg_usb_input_gain = 1.0f;
    cfg_mic_agc_enable = 0U; cfg_ng_enable = 0U;
    cfg_comp_enable = 0U; cfg_eq_enable = 0U; cfg_alc_enable = 0U;
    cfg_eq_bass = 0.0f; cfg_eq_treble = 0.0f;
    DBG(cfg_gain_telemetry_mode = GAIN_TELEMETRY_OFF);
    DBG(cfg_comp_math_mode = COMP_MATH_LUT);
    cfg_usb_clean_mode = (uint8_t)USB_AUDIO_POLICY_DIGI_CLEAN;
    if (proc == 1U) {
        cfg_usb_clean_mode = (uint8_t)USB_AUDIO_POLICY_VOICE_PROC;
        cfg_usb_input_gain = 1.5f;
        cfg_mic_agc_enable = 1U; cfg_ng_enable = 1U;
        cfg_comp_enable = 1U; cfg_eq_enable = 1U; cfg_alc_enable = 1U;
        cfg_eq_bass = 6.0f; cfg_eq_treble = 4.0f;
        DBG(cfg_gain_telemetry_mode = GAIN_TELEMETRY_LIVE);
    } else if (proc == 2U) {
        cfg_usb_clean_mode = (uint8_t)USB_AUDIO_POLICY_VOICE_PROC;
        cfg_ng_enable = 1U;
        cfg_comp_enable = 1U; cfg_alc_enable = 1U;
        DBG(cfg_comp_math_mode = COMP_MATH_EXACT);
        DBG(cfg_gain_telemetry_mode = GAIN_TELEMETRY_UI);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    out = fopen(argv[1], "wb");
    if (!out) return 2;
    mod_tx_pipeline_init();
    build_outphase_lut();
    cfg_muted = 0U;
    mute_target = 1.0f;
    const float ng0 = cfg_ng_thresh;

    for (uint32_t be = 0U; be < 2U; be++) {
    cfg_ssb_analytic_backend = be ? SSB_ANALYTIC_H12P : SSB_ANALYTIC_WEAVER;
    static const uint16_t periods[] = { 388U, 254U, 1848U, 192U };
    static const uint8_t sidebands[] = { SIDEBAND_USB, SIDEBAND_LSB };
    char name[128];
    for (uint32_t p = 0U; p < sizeof(periods) / sizeof(periods[0]); p++) {
        custom_carrier.period_ticks = periods[p];
        custom_carrier.half_period_ticks = (uint16_t)(periods[p] / 2U);
        custom_carrier.null_ticks = (uint16_t)(periods[p] / 4U);
        custom_carrier.max_compare_ticks = (uint16_t)(periods[p] - 4U);
        for (uint32_t s = 0U; s < 2U; s++) {
            MOD_SetSideband(sidebands[s]);
            for (uint32_t proc = 0U; proc < 3U; proc++) {
                for (uint32_t bw = 0U; bw < (uint32_t)TX_BW_COUNT; bw++) {
                    set_proc(proc);
                    cfg_ng_thresh = (proc == 2U) ? 0.05f : ng0;
                    cfg_tx_bw_preset = (uint8_t)bw;
                    profile(MOD_CFG_PROD_SSB_14MHZ);
                    snprintf(name, sizeof(name), "be%u P%u sb%u proc%u bw%u", be, periods[p], s, proc, bw);
                    run(name, 0, 0.5f, 40U);
                    run("two-tone", 1, 0.9f, 40U);
                    run("loud", 0, 1.0f, 30U);
                    run("noise", 2, 0.3f, 30U);
                    run("bursts", 3, 0.7f, 60U);
                    run("sweep", 4, 0.6f, 60U);
                    run("speech", 5, 0.8f, 120U);
                }
            }
        }
    }
    cfg_ng_thresh = ng0;

    /* Feature variations at the default carrier, USB, default bandwidth. */
    custom_carrier.period_ticks = 388U; custom_carrier.half_period_ticks = 194U;
    custom_carrier.null_ticks = 97U; custom_carrier.max_compare_ticks = 384U;
    MOD_SetSideband(SIDEBAND_USB);
    cfg_tx_bw_preset = 0U;
    for (uint32_t v = 0U; v < 21U; v++) {
        const float cap0 = cfg_outphase_ssb_max_theta;
        const float drive0 = cfg_drive;
        uint32_t count = AUDIO_FRAME_SIZE;
        set_proc(1U);
        DBG(cfg_debug_branch_mode = DEBUG_BRANCH_BOTH); cfg_outphase_law = OUTPHASE_LAW_CLASSIC;
        DBG(cfg_dsp_math_mode = DSP_MATH_SAFE); active_cal_flags = 0U; cfg_gate = SAMPLE_GATE;
        DBG(cfg_rf_metric_mode = RF_METRIC_EXACT); cfg_branch_ac_offset_ticks = 0;
        switch (v) {
            case 0: cfg_outphase_ssb_max_theta = 0.30f; break;
            case 1: cfg_outphase_ssb_max_theta = 0.12f; break;
            case 2:
            case 12:
            case 13:
            case 17:
                active_cal_flags = SSB_CAL_FLAG_APPLY;
                phase_cal_active.differential_skew = 0.0137f;
                for (uint32_t b = 0U; b < PHASE_CAL_BINS; b++) {
                    phase_cal_active.common_phase_pd[b] = 0.002f * (float)b - 0.01f;
                    theta_linearity_pd[b] = 0.001f * (float)((b * 7U) % 5U) - 0.002f;
                }
                if (v == 12U) cfg_branch_ac_offset_ticks = 5;
                if (v == 13U) { cfg_branch_ac_offset_ticks = -3; DBG(cfg_debug_branch_mode = DEBUG_BRANCH_A_ONLY); }
                if (v == 17U) DBG(cfg_dsp_math_mode = DSP_MATH_FAST);
                break;
            case 3: DBG(cfg_debug_branch_mode = DEBUG_BRANCH_A_ONLY); break;
            case 4: DBG(cfg_debug_branch_mode = DEBUG_BRANCH_C_ONLY); break;
            case 5: cfg_outphase_law = OUTPHASE_LAW_CLASSIC_SWAP; break;
            case 6: cfg_outphase_law = OUTPHASE_LAW_LEGACY; break;
            case 7: DBG(cfg_dsp_math_mode = DSP_MATH_LEGACY); break;
            case 8: DBG(cfg_dsp_math_mode = DSP_MATH_FAST); break;
            case 9: cfg_gate = 0.0f; break;
            case 11: cfg_outphase_law = OUTPHASE_LAW_LEGACY; DBG(cfg_rf_metric_mode = RF_METRIC_FAST); break;
            case 14: set_proc(0U); active_audio_profile = AUDIO_PROFILE_USB_HIFI; break;
            case 15: active_audio_source = MOD_AUDIO_SOURCE_MIC; local_audio_clean = 0U; break;
            case 16: count = 30U; break;
            case 18: cfg_gate = 0.2f; break;
            case 19: cfg_drive = 3.0f; break;
            case 20: cfg_usb_input_gain = 4.0f; DBG(cfg_comp_math_mode = COMP_MATH_EXACT); break;
            default: break;
        }
        build_outphase_lut();
        profile(MOD_CFG_PROD_SSB_14MHZ);
        snprintf(name, sizeof(name), "be%u variation %u", be, v);
        run_n(name, 0, 0.8f, 30U, count);
        run_n("two-tone", 1, 1.0f, 30U, count);
        run_n("bursts", 3, 0.9f, 50U, count);
        run_n("speech", 5, 0.9f, 80U, count);
        if (v == 14U) {            /* empty frames with every element bypassed */
            run_n("speech", 5, 0.9f, 5U, AUDIO_FRAME_SIZE);
            set_proc(2U);
            cfg_ng_enable = 0U;
            cfg_usb_clean_mode = (uint8_t)USB_AUDIO_POLICY_VOICE_PROC;
            run_n("speech", 5, 0.9f, 5U, AUDIO_FRAME_SIZE);
            set_proc(0U);
            run_n("empty frames bypassed", 1, 0.8f, 3U, 0U);
        }
        if (v == 10U) {            /* mute toggles mid-stream */
            cfg_muted = 1U; mute_target = 0.0f;
            run("muting", 1, 0.8f, 20U);
            cfg_muted = 0U; mute_target = 1.0f;
            run("unmuting", 1, 0.8f, 20U);
            run_n("empty frames", 1, 0.8f, 3U, 0U);
        }
        cfg_outphase_ssb_max_theta = cap0;
        cfg_drive = drive0;
    }
    }
    fclose(out);
    return 0;
}

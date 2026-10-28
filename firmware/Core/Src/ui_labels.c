#pragma GCC optimize ("Os")

#include "ui_labels_priv.h"
#include "ui_tune.h"
#include "radio_config.h"
#include "board_rev1.h"
#include "ui_format.h"
#include "mod_priv.h"
#include <math.h>
#include <string.h>

/* UI label/value formatting helpers split out of ui_render.c.
 * These functions are intentionally pure presentation glue; drawing remains in
 * ui_render.c.
 */

const char *profile_label(audio_profile_t p) { return MOD_GetAudioProfileLabel(p); }

const char *analytic_label(ssb_analytic_backend_t v)
{
    switch (v) {
        case SSB_ANALYTIC_HILBERT:         return "H48*";
        case SSB_ANALYTIC_HILBERT_PAIR_24: return "H24*";
        case SSB_ANALYTIC_HILBERT_PAIR_12: return "H12*";
        case SSB_ANALYTIC_H12P:            return "H12P EXP";
        case SSB_ANALYTIC_WEAVER:
        default:                           return "WEAVER";
    }
}

const char *txbw_label(tx_bw_preset_t v)
{
    switch (v) {
        case TX_BW_100_3000: return "100-3000";
        case TX_BW_200_2900: return "200-2900";
        case TX_BW_300_2700: return "300-2700";
        case TX_BW_300_2400: return "300-2400";
        default:             return "300-2700";
    }
}

const char *debug_branch_label(debug_branch_mode_t v)
{
    switch (v) {
        case DEBUG_BRANCH_A_ONLY: return "A";
        case DEBUG_BRANCH_C_ONLY: return "C";
        case DEBUG_BRANCH_BOTH:
        default:                  return "BOTH";
    }
}

const char *branch_law_label(uint8_t v)
{
    if (v == 1U) return "C +THETA";
    if (v >= 2U) return "EXP";
    return "A +THETA";
}

const char *outphase_law_label(outphase_law_t v)
{
    switch (v) {
        case OUTPHASE_LAW_LEGACY:       return "LEGACY";
        case OUTPHASE_LAW_CLASSIC:      return "CLASSIC";
        case OUTPHASE_LAW_CLASSIC_SWAP: return "CLS SWP";
        default:                        return "CLASSIC";
    }
}

const char *pa_geometry_label(uint8_t v)
{
    return (v == 1U) ? "A INVERTED" : "NORMAL";
}

const char *cw_theta_model_label(cw_theta_model_t v)
{
    switch (v) {
        case CW_THETA_MODEL_V633_SAFE:     return "V633";
        case CW_THETA_MODEL_TRUE_OUTPHASE: return "OUTPH";
        default:                           return "V633";
    }
}

const char *usb_chan_label(usb_audio_channel_mode_t v)
{
    switch (v) {
        case USB_AUDIO_CHAN_LEFT:    return "LEFT";
        case USB_AUDIO_CHAN_RIGHT:   return "RIGHT";
        case USB_AUDIO_CHAN_AVERAGE: return "AVG";
        case USB_AUDIO_CHAN_STRONGER:return "STRONG";
        default:                     return "AVG";
    }
}

const char *usb_policy_label(usb_audio_policy_t v)
{
    switch (v) {
        case USB_AUDIO_POLICY_VOICE_PROC: return "VOICE";
        case USB_AUDIO_POLICY_DIGI_CLEAN:
        default:                          return "DIGI";
    }
}

const char *fm_preset_label(fm_deviation_preset_t v)
{
    switch (v) {
        case FM_DEVIATION_PRESET_NFM_2K5:  return "NFM2.5";
        case FM_DEVIATION_PRESET_FM_5K:    return "FM5";
        case FM_DEVIATION_PRESET_TEST_7K5: return "TEST7.5";
        case FM_DEVIATION_PRESET_USER:
        default:                           return "USER";
    }
}

const char *am_profile_label(am_profile_t v)
{
    switch (v) {
        case AM_PROFILE_LINEAR_TEST: return "LIN TEST";
        case AM_PROFILE_EFFICIENT:
        default:                     return "EFFIC";
    }
}

const char *dsp_math_label(dsp_math_mode_t v)
{
    switch (v) {
        case DSP_MATH_LEGACY: return "LEGACY";
        case DSP_MATH_SAFE:   return "ACCEL";
        case DSP_MATH_FAST:   return "FAST";
        default:              return "LEGACY";
    }
}

const char *dsp_math_short_label(uint8_t v)
{
    switch ((dsp_math_mode_t)v) {
        case DSP_MATH_LEGACY: return "LEG";
        case DSP_MATH_SAFE:   return "ACC";
        case DSP_MATH_FAST:   return "FST";
        default:              return "LEG";
    }
}

const char *gain_telem_label(gain_telemetry_mode_t v)
{
    switch (v) {
        case GAIN_TELEMETRY_OFF:  return "OFF";
        case GAIN_TELEMETRY_UI:   return "UI";
        case GAIN_TELEMETRY_LIVE: return "LIVE";
        default:                  return "OFF";
    }
}

const char *comp_math_label(comp_math_mode_t v)
{
    switch (v) {
        case COMP_MATH_EXACT: return "EXACT";
        case COMP_MATH_LUT:   return "LUT";
        default:              return "EXACT";
    }
}

const char *rf_metric_label(rf_metric_mode_t v)
{
    switch (v) {
        case RF_METRIC_EXACT: return "EXACT";
        case RF_METRIC_FAST:  return "FAST";
        default:              return "EXACT";
    }
}

const char *cpu_scope_label(mod_cpu_scope_mode_t v)
{
    return MOD_GetCpuScopeModeLabel(v);
}

const char *ssb_pipeline_label(ssb_pipeline_mode_t v)
{
    return MOD_GetSsbPipelineModeLabel(v);
}

const char *home_layout_label(ui_home_layout_t v)
{
    switch (v) {
        case UI_HOME_LAYOUT_COMPACT:  return "COMPACT";
        case UI_HOME_LAYOUT_STANDARD: return "STANDARD";
        case UI_HOME_LAYOUT_DETAILED:
        default:                      return "DETAILED";
    }
}

const char *shortcut_label(ui_shortcut_action_t v)
{
    switch (v) {
        case UI_SHORTCUT_AB_SWAP: return "A/B";
        case UI_SHORTCUT_QUICK:   return "FREQ";
        case UI_SHORTCUT_BAND:    return "BAND";
        case UI_SHORTCUT_MODES:   return "MODES";
        case UI_SHORTCUT_SYSTEM:  return "SYSTEM";
        case UI_SHORTCUT_MENU:    return "MENU";
        case UI_SHORTCUT_NONE:
        default:                  return "NONE";
    }
}

/* The Morse trainer's choices (mod_morse_trainer.h). */
static const char *mt_pick(const char *const *l, uint8_t n, uint8_t v) { return (v < n) ? l[v] : "?"; }
const char *mtrain_act_label(uint8_t v)
{
    static const char *const l[] = {"ECHO", "LISTEN", "LEARN NEW", "FREE KEYING", "PADDLE CHECK"};
    return mt_pick(l, 5U, v);
}
const char *mtrain_src_label(uint8_t v)
{
    static const char *const l[] = {"CHARACTERS", "WORDS", "ABBREVIATIONS", "CALL SIGNS", "MIXED"};
    return mt_pick(l, 5U, v);
}
const char *mtrain_show_label(uint8_t v)
{
    static const char *const l[] = {"SOUND", "TEXT", "SOUND + TEXT"};
    return mt_pick(l, 3U, v);
}
const char *mtrain_course_label(uint8_t v)
{
    static const char *const l[] = {"M32", "LCWO", "CW ACADEMY", "LICW", "CUSTOM"};
    return mt_pick(l, 5U, v);
}

const char *cw_keyer_label(cw_keyer_mode_t v)
{
    switch (v) {
        case CW_KEYER_STRAIGHT: return "STRAIGHT";
        case CW_KEYER_IAMBIC_A: return "IAMB A";
        case CW_KEYER_IAMBIC_B: return "IAMB B";
        case CW_KEYER_ULTIMATIC:return "ULTIM";
        case CW_KEYER_NONSQUEEZE:return "NON-SQ";
        default:                return "IAMB A";
    }
}

const char *cw_breakin_label(cw_breakin_mode_t v)
{
    switch (v) {
        case CW_BREAKIN_FULL: return "FULL";
        case CW_BREAKIN_SEMI: return "SEMI";
        default:              return "FULL";
    }
}

const char *cw_paddle_protect_label(cw_paddle_protect_mode_t v)
{
    switch (v) {
        case CW_PADDLE_PROTECT_OFF:       return "OFF";
        case CW_PADDLE_PROTECT_ARM:       return "ARM";
        case CW_PADDLE_PROTECT_ARM_STUCK: return "ARM+STK";
        case CW_PADDLE_PROTECT_CUSTOM:    return "USER";
        default:                          return "ARM+STK";
    }
}

const char *cw_both_policy_label(cw_both_policy_t v)
{
    switch (v) {
        case CW_BOTH_ALLOW:  return "ALLOW";
        case CW_BOTH_TIMER:  return "TIMER";
        case CW_BOTH_IGNORE: return "IGNORE";
        case CW_BOTH_FAULT:  return "FAULT";
        default:             return "TIMER";
    }
}

const char *tx_test_both_policy_label(tx_test_both_policy_t v)
{
    switch (v) {
        case TXTEST_BOTH_ALLOW:  return "ALLOW";
        case TXTEST_BOTH_IGNORE: return "IGNORE";
        case TXTEST_BOTH_FAULT:  return "FAULT";
        default:                 return "FAULT";
    }
}

void append_ms_timeout_label(char *dst, float v)
{
    char *p;
    if (v <= 0.5f) {
        strcat(dst, "OFF");
        return;
    }
    p = dst + strlen(dst);
    if (v >= 1000.0f) {
        p = UI_AppendF(p, v / 1000.0f, 1U);
        strcpy(p, "s");
    } else {
        p = UI_AppendF(p, v, 0U);
        strcpy(p, "ms");
    }
}

const char *stream_state(const modulator_status_t *st, uint32_t now)
{
    if (st->mod_type == MOD_TYPE_CW)             return MOD_GetCwPractice() ? "PRAC" : ((st->tx_state == TX_STATE_TX) ? "KEY" : "IDLE");
    if (st->config == MOD_CFG_TX_CARRIER_TEST) return (st->tx_state == TX_STATE_TX) ? "KEY" : "READY";
    if (st->internal_source)                      return "TEST";
    if ((st->usb_stream_active == 0U) || (st->last_usb_tick_ms == 0U)) return "WAIT";
    if (!st->transport_started)                   return "ARM";
    if ((now - st->last_usb_tick_ms) > 300U)      return "GAP";
    if (MOD_AudioRealtimeCritical(now))           return "TX";
    if ((!st->usb_audio_seen) || ((now - st->last_usb_audio_tick_ms) > USB_AUDIO_SILENCE_RELEASE_MS)) return "SIL";
    return "IDLE";
}

const char *queue_state(const modulator_status_t *st)
{
    if ((st->mod_type == MOD_TYPE_CW) && (st->render_count <= 1U)) return "LOW";
    if (st->render_count <= 2U) return "LOW";
    if (st->mod_type == MOD_TYPE_CW) return (st->render_count >= 4U) ? "HI" : "OK";
    if (st->internal_source) return (st->render_count >= 18U) ? "HI" : "OK";
    return (st->render_count >= 14U) ? "HI" : "OK";
}

const char *carrier_apply_path_label(mod_carrier_apply_path_t path)
{
    switch (path) {
        case MOD_CARRIER_APPLY_PATH_FULL_RECONFIG:        return "FULL";
        case MOD_CARRIER_APPLY_PATH_TIMING_UPDATE_STOPPED:return "STOP";
        case MOD_CARRIER_APPLY_PATH_TIMING_UPDATE_LIVE:   return "LIVE";
        default:                                          return "NONE";
    }
}

const char *tx_state_label(tx_state_t s)
{
    switch (s) {
        case TX_STATE_TX:      return "TX";
        case TX_STATE_RX:      return "RX";
        case TX_STATE_WAIT:    return "WAIT";
        case TX_STATE_SILENCE: return "SIL";
        default:               return "???";
    }
}


const char *cw_paddle_guard_label(uint8_t state)
{
    switch (state) {
        case 0U: return "PLUG/OPEN";
        case 1U: return "PRESS BOTH";
        case 2U: return "RELEASE";
        case 3U: return "ARMED";
        case 4U: return "STUCK/OPEN";
        default: return "?";
    }
}

static const char *step_label(uint8_t digit)
{
    static const char *const labels[UI_TUNE_DIGITS] = {
        "1 Hz", "10 Hz", "100 Hz", "500 Hz", "1 kHz", "10 kHz", "100 kHz", "1 MHz"
    };
    return labels[(digit < UI_TUNE_DIGITS) ? digit : UI_TUNE_DIGIT_DEF];
}

static const char *pick(const char *const *names, unsigned count, float v)
{
    const int32_t i = (int32_t)(v + 0.5f);
    return names[((i >= 0) && ((unsigned)i < count)) ? (unsigned)i : 0U];
}

/* REV1 board choices, kept apart so the main switch stays readable. */
static uint8_t format_board_value(setting_style_t style, float v, char *dst)
{
    static const char *const drivers[] = {"UNKNOWN", "UCC27524", "MC74ACT541"};
    static const char *const drains[] = {"UNKNOWN", "ENV", "VIN"};
    static const char *const switches[] = {"UNASSIGNED", "PTT", "MODE", "POWER",
                                           "BAND UP", "BAND DOWN", "MENU", "TX MUTE"};
    static const char *const rx_modes[] = {"USB", "LSB", "CW", "AM", "FM"};
    static const char *const mics[] = {"USB", "MIC1", "MIC2", "MIC3"};
    static const char *const envelopes[] = {"OUTPHASING", "ENVELOPE", "HYBRID"};
    switch (style) {
        case STY_DRIVER:   strcat(dst, pick(drivers, 3U, v)); return 1U;
        case STY_DRAIN:    strcat(dst, pick(drains, 3U, v)); return 1U;
        case STY_SWITCH:   strcat(dst, pick(switches, 8U, v)); return 1U;
        case STY_ENCPIN:   strcat(dst, BOARD_FreePinLabel((uint8_t)v)); return 1U;
        case STY_RXMODE:   strcat(dst, pick(rx_modes, 5U, v)); return 1U;
        case STY_MIC:      strcat(dst, pick(mics, 4U, v)); return 1U;
        case STY_ENVELOPE: strcat(dst, pick(envelopes, 3U, v)); return 1U;
        case STY_UMODE:    strcat(dst, UI_UModeLabel((ui_umode_t)(int32_t)(v + 0.5f))); return 1U;
        case STY_STEP:     strcat(dst, step_label((uint8_t)(v + 0.5f))); return 1U;
        case STY_ROTATION: strcat(dst, (v > 0.5f) ? "270\x05" : "90\x05"); return 1U;
        case STY_CWBW:
            strcpy(UI_AppendU(dst + strlen(dst), RADIO_CwBandwidthAt((uint8_t)(v < 0.0f ? 0.0f : v + 0.5f)), 1U), " HZ");
            return 1U;
        case STY_CWAGC:    strcat(dst, (v > 0.5f) ? "FAST" : "SLOW"); return 1U;
        case STY_SSBBW:
            strcpy(UI_AppendU(dst + strlen(dst), RADIO_SsbBandwidthAt((uint8_t)(v < 0.0f ? 0.0f : v + 0.5f)), 1U), " HZ");
            return 1U;
        case STY_AMBW:
            strcpy(UI_AppendU(dst + strlen(dst), RADIO_AmBandwidthAt((uint8_t)(v < 0.0f ? 0.0f : v + 0.5f)) / 1000U, 1U), " KHZ");
            return 1U;
        case STY_RXAGC: {
            static const char *const agc[4] = {"OFF", "SLOW", "MED", "FAST"};
            strcat(dst, pick(agc, 4U, v));
            return 1U;
        }
        case STY_SQUELCH:
            if (v < 0.5f) strcat(dst, "OFF");
            else strcat(UI_AppendI(dst + strlen(dst), (int32_t)RADIO_FM_SQUELCH_DB((uint8_t)(v + 0.5f))), " DBFS");
            return 1U;
        case STY_BAND: {
            const mod_band_t *b = MOD_GetBand((uint8_t)(v + 0.5f));
            strcat(dst, (b != NULL) ? b->name : "?");
            return 1U;
        }
        default: return 0U;
    }
}

void UI_FormatSettingValue(const setting_item_t *it, float v, char *dst)
{
    const int32_t i = (int32_t)(v + 0.5f);
    if (format_board_value(it->style, v, dst)) return;
    switch (it->style) {
        case STY_TOGGLE:        strcat(dst, v > 0.5f ? "ON" : "OFF"); break;
        case STY_PROFILE:       strcat(dst, profile_label((audio_profile_t)i)); break;
        case STY_SIDEBAND:      strcat(dst, v > 0.5f ? "LSB" : "USB"); break;
        case STY_REFSRC:        strcat(dst, v > 0.5f ? "LOCAL" : "USB HOST"); break;
        case STY_ANALYTIC:      strcat(dst, analytic_label(UI_SsbAnalyticChoiceAt((uint8_t)i))); break;
        case STY_TXBW:          strcat(dst, txbw_label((tx_bw_preset_t)i)); break;
        case STY_BRANCH:        strcat(dst, debug_branch_label((debug_branch_mode_t)i)); break;
        case STY_BRANCH_LAW:    strcat(dst, branch_law_label((uint8_t)i)); break;
        case STY_OUTPHASE:      strcat(dst, outphase_law_label((outphase_law_t)i)); break;
        case STY_PA_GEOMETRY:   strcat(dst, pa_geometry_label((uint8_t)i)); break;
        case STY_CWTHETA:       strcat(dst, cw_theta_model_label((cw_theta_model_t)i)); break;
        case STY_USBCHAN:       strcat(dst, usb_chan_label((usb_audio_channel_mode_t)i)); break;
        case STY_USBPOLICY:     strcat(dst, usb_policy_label((usb_audio_policy_t)i)); break;
        case STY_FM_PRESET:     strcat(dst, fm_preset_label((fm_deviation_preset_t)i)); break;
        case STY_AM_PROFILE:    strcat(dst, am_profile_label((am_profile_t)i)); break;
        case STY_DSPMATH:       strcat(dst, dsp_math_label((dsp_math_mode_t)i)); break;
        case STY_GAINTELEM:     strcat(dst, gain_telem_label((gain_telemetry_mode_t)i)); break;
        case STY_COMPMATH:      strcat(dst, comp_math_label((comp_math_mode_t)i)); break;
        case STY_RFMETRIC:      strcat(dst, rf_metric_label((rf_metric_mode_t)i)); break;
        case STY_CPUSCOPE:      strcat(dst, cpu_scope_label((mod_cpu_scope_mode_t)i)); break;
        case STY_SSBPIPE:       strcat(dst, ssb_pipeline_label((ssb_pipeline_mode_t)i)); break;
        case STY_CWKEYER:       strcat(dst, cw_keyer_label((cw_keyer_mode_t)i)); break;
        case STY_CWBREAKIN:     strcat(dst, cw_breakin_label((cw_breakin_mode_t)i)); break;
        case STY_CWPDLPROT:     strcat(dst, cw_paddle_protect_label((cw_paddle_protect_mode_t)i)); break;
        case STY_CWBOTH:        strcat(dst, cw_both_policy_label((cw_both_policy_t)i)); break;
        case STY_CWACS:         strcat(dst, (i == 1) ? "2D" : (i == 2) ? "3D" : (i == 3) ? "4D" : "OFF"); break;
        case STY_TXTESTBOTH:    strcat(dst, tx_test_both_policy_label((tx_test_both_policy_t)i)); break;
        case STY_MTRAIN_ACT:    strcat(dst, mtrain_act_label((uint8_t)i)); break;
        case STY_MTRAIN_SRC:    strcat(dst, mtrain_src_label((uint8_t)i)); break;
        case STY_MTRAIN_SHOW:   strcat(dst, mtrain_show_label((uint8_t)i)); break;
        case STY_MTRAIN_COURSE: strcat(dst, mtrain_course_label((uint8_t)i)); break;
        case STY_HOME_LAYOUT:   strcat(dst, home_layout_label((ui_home_layout_t)i)); break;
        case STY_SHORTCUT:      strcat(dst, shortcut_label((ui_shortcut_action_t)i)); break;
        case STY_CORDIC_RO:     strcat(dst, v > 0.5f ? "HW" : "SW"); break;
        case STY_MS_TIMEOUT:    append_ms_timeout_label(dst, v); break;
        case STY_OUTPH_FALLBACK:strcat(dst, MOD_GetOutphaseFallbackPolicyLabel((mod_outphase_fallback_t)i)); break;
        case STY_CLK_POLICY:    strcat(dst, MOD_GetCarrierClockPolicyLabel((mod_clock_policy_t)i)); break;
        case STY_ACTION:        strcat(dst, "\x02"); break;
        case STY_NUMERIC:
        default:                UI_AppendF(dst + strlen(dst), v, it->decimals); break;
    }
}

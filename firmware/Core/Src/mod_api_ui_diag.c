#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_priv.h"

/* UI preferences, diagnostics, debug toggles, and select-button GPIO API. */

void MOD_SetRuntimeDiagEnable(uint8_t en)
{
#if MOD_DEEP_DIAG_ENABLE
    cfg_runtime_diag_enable = en ? 1U : 0U;
    if (!cfg_runtime_diag_enable) {
        diag_branch_a = 0U;
        diag_branch_c = 0U;
        diag_envelope = 0.0f;
        diag_theta_deg = 0.0f;
        diag_phi_deg = 0.0f;
    }
#else
    (void)en;
    cfg_runtime_diag_enable = 0U;
#endif
}

uint8_t MOD_GetRuntimeDiagEnable(void)
{
#if MOD_DEEP_DIAG_ENABLE
    return cfg_runtime_diag_enable;
#else
    return 0U;
#endif
}

void MOD_SetUiHomeLayout(ui_home_layout_t layout)
{
    if ((uint32_t)layout >= (uint32_t)UI_HOME_LAYOUT_COUNT) layout = UI_HOME_LAYOUT_DETAILED;
    cfg_ui_home_layout = layout;
}

ui_home_layout_t MOD_GetUiHomeLayout(void)
{
    return ((uint32_t)cfg_ui_home_layout < (uint32_t)UI_HOME_LAYOUT_COUNT) ? cfg_ui_home_layout : UI_HOME_LAYOUT_DETAILED;
}

void MOD_SetUiHomeFlags(uint8_t flags)
{
    cfg_ui_home_flags = (uint8_t)(flags & UI_HOME_FLAGS_DEFAULT);
}

uint8_t MOD_GetUiHomeFlags(void)
{
    return (uint8_t)(cfg_ui_home_flags & UI_HOME_FLAGS_DEFAULT);
}

void MOD_SetUiShortcutUp(ui_shortcut_action_t action)
{
    if ((uint32_t)action >= (uint32_t)UI_SHORTCUT_COUNT) action = UI_SHORTCUT_AB_SWAP;
    cfg_ui_shortcut_up = action;
}

ui_shortcut_action_t MOD_GetUiShortcutUp(void)
{
    return ((uint32_t)cfg_ui_shortcut_up < (uint32_t)UI_SHORTCUT_COUNT) ? cfg_ui_shortcut_up : UI_SHORTCUT_AB_SWAP;
}

void MOD_SetUiShortcutDown(ui_shortcut_action_t action)
{
    if ((uint32_t)action >= (uint32_t)UI_SHORTCUT_COUNT) action = UI_SHORTCUT_BAND;
    cfg_ui_shortcut_down = action;
}

ui_shortcut_action_t MOD_GetUiShortcutDown(void)
{
    return ((uint32_t)cfg_ui_shortcut_down < (uint32_t)UI_SHORTCUT_COUNT) ? cfg_ui_shortcut_down : UI_SHORTCUT_BAND;
}

void MOD_SetDebugBranchMode(debug_branch_mode_t v)
{
#if MOD_DEBUG_SWITCHES
    cfg_debug_branch_mode = (debug_branch_mode_t)mod_control_enum_sanitize(
        (uint32_t)v, (uint32_t)DEBUG_BRANCH_COUNT,
        (uint32_t)DEBUG_BRANCH_BOTH);
    requested_reset = 1U;
#else
    (void)v;
#endif
}

debug_branch_mode_t MOD_GetDebugBranchMode(void)
{
    return (debug_branch_mode_t)mod_control_enum_sanitize(
        (uint32_t)cfg_debug_branch_mode, (uint32_t)DEBUG_BRANCH_COUNT,
        (uint32_t)DEBUG_BRANCH_BOTH);
}

void MOD_SetDebugCalBypass(uint8_t en)
{
#if MOD_DEBUG_SWITCHES
    cfg_debug_cal_bypass = en ? 1U : 0U;
    requested_reset = 1U;
#else
    (void)en;
#endif
}

uint8_t MOD_GetDebugCalBypass(void) { return cfg_debug_cal_bypass; }

/* Names for the pipeline enums. One copy, shared by the LCD fault banner and
 * the USB console, so a fault reads the same on the screen as it does over the
 * wire and neither side carries its own string table. Moved here from
 * ui_render.c, where they were file-local. */
const char *MOD_PipelineStageName(uint8_t stage)
{
    switch ((mod_pipeline_stage_t)stage) {
        case MOD_PIPELINE_STAGE_USB:       return "USB";
        case MOD_PIPELINE_STAGE_PCM:       return "PCM";
        case MOD_PIPELINE_STAGE_RENDER:    return "REN";
        case MOD_PIPELINE_STAGE_DMA:       return "DMA";
        case MOD_PIPELINE_STAGE_HRTIM:     return "RF";
        case MOD_PIPELINE_STAGE_SCHEDULER: return "SCH";
        case MOD_PIPELINE_STAGE_CONTROL:   return "RST";
        case MOD_PIPELINE_STAGE_NONE:
        default:                           return "OK";
    }
}

const char *MOD_PipelineReasonName(uint8_t reason)
{
    switch ((mod_pipeline_reason_t)reason) {
        case MOD_PIPELINE_REASON_USB_MISSING_PACKET:     return "MISS";
        case MOD_PIPELINE_REASON_USB_BAD_PACKET_SIZE:    return "SIZE";
        case MOD_PIPELINE_REASON_USB_ZERO_PACKET:        return "ZERO";
        case MOD_PIPELINE_REASON_USB_OUT_INCOMPLETE:     return "OUT";
        case MOD_PIPELINE_REASON_USB_REARM_FAILED:       return "ARM";
        case MOD_PIPELINE_REASON_USB_FEEDBACK_TX_FAILED: return "FB";
        case MOD_PIPELINE_REASON_USB_HW_ERROR:           return "HW";
        case MOD_PIPELINE_REASON_USB_FRAME_SEQUENCE:     return "FSEQ";
        case MOD_PIPELINE_REASON_PCM_OVERFLOW:           return "OVF";
        case MOD_PIPELINE_REASON_PCM_RESET_DISCARD:      return "RST";
        case MOD_PIPELINE_REASON_RENDER_UNDERRUN:        return "UND";
        case MOD_PIPELINE_REASON_RENDER_OVERFLOW:        return "OVF";
        case MOD_PIPELINE_REASON_RENDER_RESET_DISCARD:   return "RST";
        case MOD_PIPELINE_REASON_RENDER_SEQUENCE_GAP:    return "SEQ";
        case MOD_PIPELINE_REASON_RENDER_DEADLINE:        return "LATE";
        case MOD_PIPELINE_REASON_DMA_ERROR:              return "ERR";
        case MOD_PIPELINE_REASON_DMA_START_FAILED:       return "STRT";
        case MOD_PIPELINE_REASON_DMA_SEQUENCE_GAP:       return "SEQ";
        case MOD_PIPELINE_REASON_DMA_CNDTR_ANOMALY:      return "CNT";
        case MOD_PIPELINE_REASON_DMA_FLAGS_COALESCED:    return "FLAGS";
        case MOD_PIPELINE_REASON_DMA_REFILL_LATE:        return "LATE";
        case MOD_PIPELINE_REASON_DMA_CHANNEL_SKEW:       return "SKEW";
        case MOD_PIPELINE_REASON_HRTIM_COMPARE_RANGE:    return "CMP";
        case MOD_PIPELINE_REASON_HRTIM_FAULT:            return "FAULT";
        case MOD_PIPELINE_REASON_RATE_PLAN_INVALID:      return "RATE";
        case MOD_PIPELINE_REASON_SCHED_IO_DEFERRED:      return "IO";
        case MOD_PIPELINE_REASON_SCHED_UI_DEFERRED:      return "UI";
        case MOD_PIPELINE_REASON_CONTROLLED_STREAM_RESET: return "RST";
        case MOD_PIPELINE_REASON_MODE_NOT_AVAILABLE:     return "MODE";
        case MOD_PIPELINE_REASON_NONE:
        default:                                         return "NONE";
    }
}

const char *MOD_PipelineSeverityName(uint8_t severity)
{
    switch ((mod_pipeline_severity_t)severity) {
        case MOD_PIPELINE_SEVERITY_NONE:      return "NOTE";
        case MOD_PIPELINE_SEVERITY_WARNING:   return "WARN";
        case MOD_PIPELINE_SEVERITY_CONCEALED: return "CONC";
        case MOD_PIPELINE_SEVERITY_HARD:      return "HARD";
        default:                              return "?";
    }
}

/* RADIO_FAULT_* bit names, in bit order. */
const char *MOD_RadioFaultName(uint8_t bit)
{
    static const char *const names[8] = {
        "ADC", "VIN", "5V", "BIAS", "SWR", "ENV", "DMA", "CLOCK"
    };
    return bit < 8U ? names[bit] : "?";
}

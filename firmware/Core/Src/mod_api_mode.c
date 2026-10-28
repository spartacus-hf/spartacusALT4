#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_priv.h"

/* Mode/routing and A/B/mute API. */

const char *MOD_GetModeLabel(mod_config_t c)
{
    switch (c) {
        case MOD_CFG_PROD_SSB_14MHZ:        return "PROD SSB";
        case MOD_CFG_BASELINE_27MHZ:        return "RAW LEG";
        case MOD_CFG_ENHANCED_SSB_14MHZ:    return "ENH SSB";
        case MOD_CFG_RAW_DIRECT_14MHZ:      return "RAW DIR";
        case MOD_CFG_FILTERED_STD_14MHZ:    return "FLT STD";
        case MOD_CFG_FILTERED_NARROW_14MHZ: return "FLT NAR";
        case MOD_CFG_FM_NARROW_14MHZ:       return "FM NAR";
        case MOD_CFG_AM_14MHZ:              return "AM";
        case MOD_CFG_CW_14MHZ:              return "CW";
        case MOD_CFG_INT_1KHZ_TONE:         return "1KHZ TONE";
        case MOD_CFG_INT_2TONE_IMD:         return "2TONE IMD";
        case MOD_CFG_INT_2TONE_BRANCH_A:    return "BRANCH A";
        case MOD_CFG_INT_2TONE_BRANCH_C:    return "BRANCH C";
        case MOD_CFG_INT_SWEEP:             return "SWEEP";
        case MOD_CFG_INT_SSB_1KHZ:          return "SSB 1KHZ";
        case MOD_CFG_INT_SSB_2TONE:         return "SSB 2TONE";
        case MOD_CFG_INT_SSB_SWEEP:         return "SSB SWEEP";
        case MOD_CFG_INT_THETA_SWEEP:       return "THETA SWP";
        case MOD_CFG_TX_CARRIER_TEST:     return "TX TEST";
        default:                             return "???";
    }
}

mod_type_t MOD_GetModeType(mod_config_t c)
{
    if (c == MOD_CFG_FM_NARROW_14MHZ) return MOD_TYPE_FM;
    if (c == MOD_CFG_AM_14MHZ)        return MOD_TYPE_AM;
    if (c == MOD_CFG_CW_14MHZ)        return MOD_TYPE_CW;
    if (c == MOD_CFG_TX_CARRIER_TEST) return MOD_TYPE_TEST;
    if ((c >= MOD_CFG_INT_1KHZ_TONE && c <= MOD_CFG_INT_SWEEP) ||
        (c >= MOD_CFG_INT_SSB_1KHZ && c <= MOD_CFG_INT_THETA_SWEEP)) return MOD_TYPE_TEST;
    return MOD_TYPE_SSB;
}

uint8_t MOD_ModeIsFrozenBaseline(mod_config_t c) { return (c == MOD_CFG_BASELINE_27MHZ) ? 1U : 0U; }

uint8_t MOD_ModeIsProduction(mod_config_t c) { return (c == MOD_CFG_PROD_SSB_14MHZ) ? 1U : 0U; }

uint8_t MOD_ModeAvailableInBuild(mod_config_t c)
{
    if ((uint32_t)c >= MOD_CFG_COUNT) return 0U;
#if MOD_DEEP_DIAG_ENABLE
    return 1U;
#else
    /* The operating modes and TX TEST: the comparison SSB paths, the
     * internal test signals and RAW LEG are the diagnostic image's. */
    return (uint8_t)((c == MOD_CFG_PROD_SSB_14MHZ) || (c == MOD_CFG_FM_NARROW_14MHZ) ||
                     (c == MOD_CFG_AM_14MHZ) || (c == MOD_CFG_CW_14MHZ) ||
                     (c == MOD_CFG_TX_CARRIER_TEST));
#endif
}

void MOD_SetTestConfig(mod_config_t cfg)
{
    if (MOD_ModeAvailableInBuild(cfg) == 0U) {
        mod_pipeline_note_severity(MOD_PIPELINE_STAGE_CONTROL,
                                   MOD_PIPELINE_REASON_MODE_NOT_AVAILABLE,
                                   MOD_PIPELINE_SEVERITY_WARNING,
                                   (uint32_t)cfg);
        cfg = MOD_CFG_PROD_SSB_14MHZ;
    }
    /* Store previous mode for A/B swap */
    if (requested_config != (uint32_t)cfg) last_mode = requested_config;
    requested_config = (uint32_t)cfg;
}

mod_config_t MOD_GetTestConfig(void) { return (mod_config_t)requested_config; }

void MOD_ResetDSPState(void) { requested_reset = 1U; }

void MOD_HandleLongPress(void) { requested_reset = 1U; }

uint8_t MOD_ModeHasInternalSource(void)
{
    mod_config_t c = (mod_config_t)requested_config;
    return (c == MOD_CFG_CW_14MHZ            || c == MOD_CFG_INT_1KHZ_TONE       || c == MOD_CFG_INT_2TONE_IMD ||
            c == MOD_CFG_INT_2TONE_BRANCH_A  || c == MOD_CFG_INT_2TONE_BRANCH_C ||
            c == MOD_CFG_INT_SWEEP           || c == MOD_CFG_INT_SSB_1KHZ ||
            c == MOD_CFG_INT_SSB_2TONE       || c == MOD_CFG_INT_SSB_SWEEP ||
            c == MOD_CFG_INT_THETA_SWEEP || c == MOD_CFG_TX_CARRIER_TEST) ? 1U : 0U;
}

void MOD_SetSideband(sideband_t sb)
{
    active_sideband_enum = sb;
    active_sideband_sign = (sb == SIDEBAND_LSB) ? 1.0f : -1.0f;
    load_current_calibration_context();
    requested_reset      = 1U;
}

sideband_t MOD_GetSideband(void) { return active_sideband_enum; }

void MOD_SetMuted(uint8_t muted)
{
    cfg_muted   = muted ? 1U : 0U;
    mute_target = cfg_muted ? 0.0f : 1.0f;
}

uint8_t MOD_GetMuted(void)  { return cfg_muted; }

void    MOD_ToggleMute(void) { MOD_SetMuted(cfg_muted ? 0U : 1U); }

void MOD_StoreLastMode(mod_config_t cfg)
{
    if ((uint32_t)cfg != active_config) last_mode = (uint32_t)cfg;
}

mod_config_t MOD_GetLastMode(void) { return (mod_config_t)last_mode; }

void MOD_SwapWithLastMode(void)
{
    mod_config_t prev  = (mod_config_t)active_config;
    mod_config_t other = (mod_config_t)last_mode;
    if (MOD_ModeAvailableInBuild(other) == 0U) return;
    last_mode = (uint32_t)prev;
    requested_config = (uint32_t)other;
}

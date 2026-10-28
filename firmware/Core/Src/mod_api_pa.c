#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_priv.h"
#include "mod_calibration_logic.h"

/* Calibration, RF enable, dead-time, duty, branch-offset, and carrier apply API. */

static inline float phase_cal_clamp(float v, float limit)
{
    if (!isfinite(v)) return 0.0f;
    return clampf(v, -limit, limit);
}

static void calibration_mark_edited(void)
{
    active_cal_flags = mod_calibration_mark_edited(active_cal_flags);
}

void MOD_SetCalibrationDifferentialSkew(float value)
{
    active_cal->differential_skew = phase_cal_clamp(value, PHASE_CAL_DIFFERENTIAL_LIMIT);
    calibration_mark_edited();
}

float MOD_GetCalibrationDifferentialSkew(void)
{
    return active_cal->differential_skew;
}

void MOD_SetCalibrationCommonPhaseBin(uint8_t bin, float value)
{
    if (bin >= PHASE_CAL_BINS) return;
    active_cal->common_phase_pd[bin] = phase_cal_clamp(value, PHASE_CAL_COMMON_LIMIT);
    calibration_mark_edited();
}

float MOD_GetCalibrationCommonPhaseBin(uint8_t bin)
{
    return (bin < PHASE_CAL_BINS) ? active_cal->common_phase_pd[bin] : 0.0f;
}

void MOD_SetCalibrationThetaBin(uint8_t bin, float value)
{
    if (bin >= PHASE_CAL_BINS) return;
    theta_linearity_pd[bin] = phase_cal_clamp(value, PHASE_CAL_THETA_LIMIT);
    calibration_mark_edited();
}

float MOD_GetCalibrationThetaBin(uint8_t bin)
{
    return (bin < PHASE_CAL_BINS) ? theta_linearity_pd[bin] : 0.0f;
}

void MOD_SetCalibrationValidated(uint8_t validated)
{
    if (validated != 0U) {
        active_cal_flags |= SSB_CAL_FLAG_VALIDATED;
        active_cal_flags &= (uint16_t)~SSB_CAL_FLAG_LEGACY_REVALIDATE;
    } else {
        active_cal_flags &= (uint16_t)~SSB_CAL_FLAG_VALIDATED;
    }
}

uint8_t MOD_GetCalibrationValidated(void)
{
    return (active_cal_flags & SSB_CAL_FLAG_VALIDATED) ? 1U : 0U;
}

void MOD_SetCalibrationApply(uint8_t apply)
{
    if (apply != 0U) {
        active_cal_flags |= SSB_CAL_FLAG_APPLY;
    } else {
        active_cal_flags &= (uint16_t)~SSB_CAL_FLAG_APPLY;
    }
}

uint8_t MOD_GetCalibrationApply(void)
{
    return (active_cal_flags & SSB_CAL_FLAG_APPLY) ? 1U : 0U;
}

uint8_t MOD_GetCalibrationLegacyPending(void)
{
    return (active_cal_flags & SSB_CAL_FLAG_LEGACY_REVALIDATE) ? 1U : 0U;
}

void  MOD_ResetCalibration(void)
{
    memset(active_cal, 0, sizeof(*active_cal));
    memset(theta_linearity_pd, 0, sizeof(theta_linearity_pd));
    active_cal_flags = 0U;
}

const phase_cal_t *MOD_GetCalibration(void) { return active_cal; }

float MOD_GetRfEnAssertMetric(void) { return cfg_rf_en_assert_metric; }

void MOD_SetRfEnAssertMetric(float v)
{
    cfg_rf_en_assert_metric = v;
    sanitize_rf_en_settings();
    mark_profile_custom();
}

float MOD_GetRfEnReleaseMetric(void) { return cfg_rf_en_release_metric; }

void MOD_SetRfEnReleaseMetric(float v)
{
    cfg_rf_en_release_metric = v;
    sanitize_rf_en_settings();
    mark_profile_custom();
}

float MOD_GetRfEnHoldFrames(void) { return (float)cfg_rf_en_hold_frames; }

void MOD_SetRfEnHoldFrames(uint8_t n)
{
    cfg_rf_en_hold_frames = n;
    sanitize_rf_en_settings();
    mark_profile_custom();
}

static pa_band_profile_t *active_pa_profile_mut(void)
{
    return (active_band_idx < BAND_COUNT) ? &pa_band_profiles[active_band_idx]
                                          : &pa_legacy_profile;
}

static float deadtime_ps_to_ns(uint32_t ps)
{
    return (float)ps / 1000.0f;
}

static float deadtime_requested_clamp_ns(float ns)
{
    float max_ns = MOD_GetDeadTimeMaxNs();
    if (!isfinite(ns)) return 0.0f;
    if (ns < 0.0f) ns = 0.0f;
    if (max_ns > 0.0f && ns > max_ns) ns = max_ns;
    return ns;
}

void MOD_SetDeadTimeTicks(uint16_t ticks)
{
    pa_band_profile_t *p = active_pa_profile_mut();
    uint32_t ps = mod_deadtime_ps_from_ticks(ticks);
    p->deadtime_rise_ps = ps;
    p->deadtime_fall_ps = ps;
    pa_resolve_active_band_profile(0U, 1U, 0U);
}

uint16_t MOD_GetDeadTimeTicks(void) { return (uint16_t)(((uint32_t)MOD_GetDeadTimeRiseAppliedTicks() + (uint32_t)MOD_GetDeadTimeFallAppliedTicks() + 1U) / 2U); }

void MOD_SetDeadTimeRiseTicks(uint16_t ticks)
{
    pa_band_profile_t *p = active_pa_profile_mut();
    p->deadtime_rise_ps = mod_deadtime_ps_from_ticks(ticks);
    pa_resolve_active_band_profile(0U, 1U, 0U);
}

uint16_t MOD_GetDeadTimeRiseTicks(void) { return MOD_GetDeadTimeRiseAppliedTicks(); }

void MOD_SetDeadTimeFallTicks(uint16_t ticks)
{
    pa_band_profile_t *p = active_pa_profile_mut();
    p->deadtime_fall_ps = mod_deadtime_ps_from_ticks(ticks);
    pa_resolve_active_band_profile(0U, 1U, 0U);
}

uint16_t MOD_GetDeadTimeFallTicks(void) { return MOD_GetDeadTimeFallAppliedTicks(); }

void MOD_SetDeadTimeNs(float ns)
{
    if (!isfinite(ns)) return;
    pa_band_profile_t *p = active_pa_profile_mut();
    uint32_t ps = mod_control_ns_to_ps(deadtime_requested_clamp_ns(ns));
    p->deadtime_rise_ps = ps;
    p->deadtime_fall_ps = ps;
    pa_resolve_active_band_profile(0U, 1U, 0U);
}

float MOD_GetDeadTimeNs(void)
{
    return MOD_GetDeadTimeAppliedNs();
}

void MOD_SetDeadTimeRiseNs(float ns)
{
    if (!isfinite(ns)) return;
    pa_band_profile_t *p = active_pa_profile_mut();
    p->deadtime_rise_ps = mod_control_ns_to_ps(deadtime_requested_clamp_ns(ns));
    pa_resolve_active_band_profile(0U, 1U, 0U);
}

float MOD_GetDeadTimeRiseNs(void)
{
    return MOD_GetDeadTimeRiseAppliedNs();
}

void MOD_SetDeadTimeFallNs(float ns)
{
    if (!isfinite(ns)) return;
    pa_band_profile_t *p = active_pa_profile_mut();
    p->deadtime_fall_ps = mod_control_ns_to_ps(deadtime_requested_clamp_ns(ns));
    pa_resolve_active_band_profile(0U, 1U, 0U);
}

float MOD_GetDeadTimeFallNs(void)
{
    return MOD_GetDeadTimeFallAppliedNs();
}

float MOD_GetDeadTimeRequestedNs(void)
{
    return 0.5f * (MOD_GetDeadTimeRiseRequestedNs() + MOD_GetDeadTimeFallRequestedNs());
}

float MOD_GetDeadTimeRiseRequestedNs(void)
{
    return deadtime_ps_to_ns(pa_runtime.requested_deadtime_rise_ps);
}

float MOD_GetDeadTimeFallRequestedNs(void)
{
    return deadtime_ps_to_ns(pa_runtime.requested_deadtime_fall_ps);
}

float MOD_GetDeadTimeAppliedNs(void)
{
    return 0.5f * (MOD_GetDeadTimeRiseAppliedNs() + MOD_GetDeadTimeFallAppliedNs());
}

float MOD_GetDeadTimeRiseAppliedNs(void)
{
    return deadtime_ps_to_ns(pa_runtime.applied_deadtime_rise_ps);
}

float MOD_GetDeadTimeFallAppliedNs(void)
{
    return deadtime_ps_to_ns(pa_runtime.applied_deadtime_fall_ps);
}

uint16_t MOD_GetDeadTimeRiseAppliedTicks(void)
{
    return pa_runtime.applied_deadtime_rise_ticks;
}

uint16_t MOD_GetDeadTimeFallAppliedTicks(void)
{
    return pa_runtime.applied_deadtime_fall_ticks;
}

float MOD_GetDeadTimeStepNs(void)
{
    return deadtime_ps_to_ns(mod_deadtime_ps_from_ticks(1U));
}

float MOD_GetDeadTimeMaxNs(void)
{
    return deadtime_ps_to_ns(mod_deadtime_ps_limit());
}

void MOD_SetBranchAcOffsetTicks(int16_t ticks)
{
    int16_t lim = mod_branch_ac_limit_ticks();
    if (ticks >  lim) ticks =  lim;
    if (ticks < -lim) ticks = -lim;
    carrier_values_capped = 0U;
    cfg_branch_ac_offset_ticks = ticks;
    calibration_mark_edited();
}

int16_t MOD_GetBranchAcOffsetTicks(void) { return cfg_branch_ac_offset_ticks; }

void MOD_SetPaGeometry(pa_geometry_t geometry)
{
    if ((uint32_t)geometry >= (uint32_t)PA_GEOMETRY_COUNT) {
        geometry = PA_GEOMETRY_DEFAULT;
    }

    pa_geometry_t old = cfg_pa_geometry;
    cfg_pa_geometry = geometry;
    HRTIM_SetOutputSwapMask((geometry == PA_GEOMETRY_A_INVERTED) ? HRTIM_OUTPUT_SWAP_A : 0U);

    if (old == geometry) return;

    requested_reset = 1U;
    if (carrier_apply_valid != 0U) {
        carrier_apply_valid = 0U;
        mod_runtime_reconfigure_active_carrier();
    }
}

pa_geometry_t MOD_GetPaGeometry(void)
{
    return cfg_pa_geometry;
}

void MOD_SetDutyOffset(int16_t ticks)
{
    pa_band_profile_t *p = (active_band_idx < BAND_COUNT) ? &pa_band_profiles[active_band_idx]
                                                          : &pa_legacy_profile;
    p->duty_offset_half_permille =
        mod_duty_permille_from_ticks_for_carrier(active_carrier, ticks);
    pa_resolve_active_band_profile(0U, 1U, 0U);
}

int16_t MOD_GetDutyOffset(void) { return cfg_duty_offset_ticks; }

mod_carrier_apply_path_t MOD_GetLastCarrierApplyPath(void)
{
    return (mod_carrier_apply_path_t)last_carrier_apply_path;
}

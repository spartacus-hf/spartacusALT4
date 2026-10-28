#include "mod_priv.h"
#include "radio.h"
#include "mod_calibration_logic.h"

#define INV_PI             (1.0f / MOD_PI_F)

/* Layout and lookup in mod_priv.h (outphase_theta_lookup). */
uint16_t outphase_lut[OUTPHASE_LUT_SIZE];
#if MOD_DEBUG_SWITCHES
float rf_metric_lut[RF_METRIC_LUT_SIZE];
#endif

/* ═══════════════ Outphasing ═══════════════ */

#if MOD_DEBUG_SWITCHES
static void build_rf_metric_lut(void)
{
    for (uint32_t i = 0; i < RF_METRIC_LUT_SIZE; i++) {
        float theta = 0.5f * (float)i / (float)(RF_METRIC_LUT_SIZE - 1U);
        float m = cosf(theta * (float)M_PI);
        rf_metric_lut[i] = (m < 0.0f) ? -m : m;
    }
}
#endif

static inline float outphase_theta_classic_live(float r)
{
    r = clampf(r, 0.0f, 1.0f);
    if (r <= 0.0f) return 0.5f;
    if (r >= 1.0f) return 0.0f;
    return 0.5f - asinf(r) * INV_PI; /* acos(r) / pi */
}

void build_outphase_lut(void)
{
    for (uint32_t i = 0; i < OUTPHASE_LUT_SIZE; i++)
    {
        float r = (float)i / (float)(OUTPHASE_LUT_SIZE - 1U);
        if (r > 1.0f) r = 1.0f;
        switch (cfg_outphase_law) {
            case OUTPHASE_LAW_LEGACY:
                /* Experimental inverted-envelope diagnostic. For the normal
                 * PA invariant, this maps speech peaks toward cancellation. */
                outphase_lut[i] = (uint16_t)(asinf(r) * INV_PI * OUTPHASE_LUT_SCALE + 0.5f);
                break;
            case OUTPHASE_LAW_CLASSIC_SWAP:
            case OUTPHASE_LAW_CLASSIC:
            default:
                outphase_lut[i] = (uint16_t)(outphase_theta_classic_live(r) * OUTPHASE_LUT_SCALE + 0.5f);
                break;
        }
    }
#if MOD_DEBUG_SWITCHES
    build_rf_metric_lut();
#endif
}

static __attribute__((cold, noinline)) float outphase_sanitize_theta(float theta, float def, float hard_max)
{
    if (!isfinite(theta)) theta = def;
    if (theta < 0.0f) theta = 0.0f;
    if (theta > hard_max) theta = hard_max;
    return theta;
}

__attribute__((cold)) void sanitize_outphase_policy_settings(void)
{
    cfg_outphase_ssb_max_theta = outphase_sanitize_theta(cfg_outphase_ssb_max_theta,
                                                         OUTPHASE_SSB_MAX_THETA_DEFAULT,
                                                         OUTPHASE_THETA_HARD_MAX);
    cfg_outphase_am_max_theta = outphase_sanitize_theta(cfg_outphase_am_max_theta,
                                                        OUTPHASE_AM_MAX_THETA_DEFAULT,
                                                        OUTPHASE_THETA_HARD_MAX);
    cfg_outphase_fm_theta = outphase_sanitize_theta(cfg_outphase_fm_theta,
                                                    OUTPHASE_FM_THETA_DEFAULT,
                                                    OUTPHASE_THETA_HARD_MAX);
    cfg_outphase_cw_theta = outphase_sanitize_theta(cfg_outphase_cw_theta,
                                                    OUTPHASE_CW_THETA_DEFAULT,
                                                    0.250f);
    cfg_outphase_test_theta = outphase_sanitize_theta(cfg_outphase_test_theta,
                                                      OUTPHASE_TEST_THETA_DEFAULT,
                                                      0.250f);
    if ((uint32_t)cfg_outphase_fallback_policy >= (uint32_t)MOD_OUTPHASE_FALLBACK_COUNT) {
        cfg_outphase_fallback_policy = MOD_OUTPHASE_FALLBACK_COMPRESS;
    }
}

static float outphase_hardware_theta_limit_from_carrier(const CarrierProfile *carrier)
{
    if ((carrier == NULL) || (carrier->half_period_ticks == 0U)) return 0.0f;
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    /* Atomic compares reach every phase of the period
     * (mod_hrtim_phase_compare_legal), so the hardware sets no cap. The
     * legacy window below came to (P - 7) / P here, above the hard cap on
     * every band, and its division ran twice a sample in every renderer. */
    return OUTPHASE_THETA_HARD_MAX;
#else
    if (carrier->max_compare_ticks <= carrier->min_compare_ticks) return 0.0f;
    float cap = (float)((uint32_t)carrier->max_compare_ticks -
                        (uint32_t)carrier->min_compare_ticks)
              / (2.0f * (float)carrier->half_period_ticks);
    return clampf(cap, 0.0f, OUTPHASE_THETA_HARD_MAX);
#endif
}

float MOD_GetOutphaseHardwareThetaLimit(void)
{
    return outphase_hardware_theta_limit_from_carrier(active_carrier);
}

float MOD_GetOutphaseRequestedThetaLimit(mod_type_t mode)
{
    float requested;
    switch (mode) {
        case MOD_TYPE_AM:
            requested = cfg_outphase_am_max_theta;
            break;
        case MOD_TYPE_FM:
            requested = cfg_outphase_fm_theta;
            break;
        case MOD_TYPE_CW:
            requested = OUTPHASE_CW_EDGE_THETA_MAX;
            if (requested < cfg_outphase_cw_theta) requested = cfg_outphase_cw_theta;
            break;
        case MOD_TYPE_TEST:
            requested = cfg_outphase_test_theta;
            break;
        case MOD_TYPE_SSB:
        default:
            requested = cfg_outphase_ssb_max_theta;
            break;
    }
    return clampf(requested, 0.0f, OUTPHASE_THETA_HARD_MAX);
}

float MOD_GetOutphaseEffectiveThetaLimit(mod_type_t mode)
{
    float hw = MOD_GetOutphaseHardwareThetaLimit();
    float requested = MOD_GetOutphaseRequestedThetaLimit(mode);
    return (requested < hw) ? requested : hw;
}

mod_capability_t MOD_GetOutphaseCapability(mod_type_t mode)
{
    float hw = MOD_GetOutphaseHardwareThetaLimit();
    float requested = MOD_GetOutphaseRequestedThetaLimit(mode);
    if (hw <= 0.002f) return MOD_CAPABILITY_NOT_RECOMMENDED;
    if ((mode == MOD_TYPE_FM) || (mode == MOD_TYPE_TEST)) {
        return (hw + 0.0005f < requested) ? MOD_CAPABILITY_LIMITED
                                          : MOD_CAPABILITY_VALID;
    }
    if (mode == MOD_TYPE_CW) {
        return (hw + 0.0005f < cfg_outphase_cw_theta) ? MOD_CAPABILITY_LIMITED
                                                      : MOD_CAPABILITY_VALID;
    }
    if (requested <= 0.002f) return MOD_CAPABILITY_VALID;
    if (hw + 0.0005f >= requested) return MOD_CAPABILITY_VALID;
    if (hw < requested * 0.35f) return MOD_CAPABILITY_NOT_RECOMMENDED;
    if (hw < requested * 0.70f) return MOD_CAPABILITY_COMPRESSED;
    return MOD_CAPABILITY_LIMITED;
}

const char *MOD_GetOutphaseCapabilityLabel(mod_capability_t cap)
{
    switch (cap) {
        case MOD_CAPABILITY_VALID:          return "VALID";
        case MOD_CAPABILITY_LIMITED:        return "LIMITED";
        case MOD_CAPABILITY_COMPRESSED:     return "COMPRESSED";
        case MOD_CAPABILITY_NOT_RECOMMENDED:return "NOT REC";
        default:                            return "?";
    }
}

const char *MOD_GetOutphaseFallbackPolicyLabel(mod_outphase_fallback_t policy)
{
    switch (policy) {
        case MOD_OUTPHASE_FALLBACK_COMPRESS: return "COMPRESS";
        case MOD_OUTPHASE_FALLBACK_WARN:     return "WARN";
        case MOD_OUTPHASE_FALLBACK_BLOCK:    return "BLOCK";
        default:                             return "?";
    }
}

float mod_outphase_limit_theta_for_mode(mod_type_t mode, float theta)
{
    if (!isfinite(theta)) theta = 0.0f;
    theta = clampf(theta, 0.0f, OUTPHASE_THETA_HARD_MAX);
    float cap = MOD_GetOutphaseEffectiveThetaLimit(mode);
    if (theta > cap) {
        theta = cap;
        outphase_limit_latched = 1U;
        outphase_limit_count++;
    }
    return theta;
}

float mod_outphase_theta_for_env(mod_type_t mode, float env)
{
    env = clampf(sanitize_nan_default(env, 0.0f), 0.0f, 0.999f);
    return mod_outphase_limit_theta_for_mode(mode, outphase_theta(env));
}

float mod_fixed_theta_for_mode(mod_type_t mode)
{
    float theta;
    switch (mode) {
        case MOD_TYPE_FM:   theta = cfg_outphase_fm_theta; break;
        case MOD_TYPE_CW:   theta = cfg_outphase_cw_theta; break;
        case MOD_TYPE_TEST: theta = cfg_outphase_test_theta; break;
        case MOD_TYPE_AM:   theta = cfg_outphase_am_max_theta; break;
        case MOD_TYPE_SSB:
        default:            theta = cfg_outphase_ssb_max_theta; break;
    }
    return mod_outphase_limit_theta_for_mode(mode, theta);
}

static inline uint8_t outphase_swap_enabled(void)
{
    return (uint8_t)(cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP);
}

void outphase_branch_deltas(float theta, float cal, float *a_delta, float *c_delta)
{
    mod_outphase_pair_t pair = mod_outphase_branch_pair_pi(theta,
                                                           cal,
                                                           outphase_swap_enabled());
    *a_delta = pair.branch_a_pi;
    *c_delta = pair.branch_c_pi;
}

float outphase_theta(float r)
{
    return outphase_theta_lookup(r);
}

#if MOD_HRTIM_ATOMIC_EXPERIMENT
_Static_assert(MOD_HRTIM_COMPARE_FLOOR_MUL32 == HRTIM_OUTPUT_MIN_COMPARE,
               "HRTIM phase encoder and waveform compare floors must match");
#endif

/* Apply a signed offset in exact HRTIM compare ticks. The compare value is
 * wrapped modulo the active carrier period, then clamped into the legal
 * compare range expected by the HRTIM output logic. */
HOT_INLINE uint32_t compare_ticks_wrap_clamp_i32(int32_t t)
{
    int32_t p = (int32_t)active_carrier->period_ticks;
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    t -= (int32_t)MOD_HRTIM_COMPARE_FLOOR_MUL32;
    while (t < 0)  t += p;
    while (t >= p) t -= p;
    return (uint32_t)t + MOD_HRTIM_COMPARE_FLOOR_MUL32;
#else
    while (t < 0)  t += p;
    while (t >= p) t -= p;
    if (t < (int32_t)active_carrier->min_compare_ticks) t = (int32_t)active_carrier->min_compare_ticks;
    if (t > (int32_t)active_carrier->max_compare_ticks) t = (int32_t)active_carrier->max_compare_ticks;
    return (uint32_t)t;
#endif
}

HOT_INLINE uint32_t compare_ticks_offset(uint32_t tick, int16_t offset)
{
    return compare_ticks_wrap_clamp_i32((int32_t)tick + (int32_t)offset);
}

HOT_INLINE BranchMode effective_branch_mode(void)
{
    if (active_branch_mode != BRANCH_MODE_BOTH) return active_branch_mode;
    switch (cfg_debug_branch_mode) {
        case DEBUG_BRANCH_A_ONLY: return BRANCH_MODE_A_ONLY;
        case DEBUG_BRANCH_C_ONLY: return BRANCH_MODE_C_ONLY;
        case DEBUG_BRANCH_BOTH:
        default:                  return BRANCH_MODE_BOTH;
    }
}

/* For renderers that decide the routing once a frame. */
BranchMode mod_effective_branch_mode(void)
{
    return effective_branch_mode();
}

static inline void route_branch_values(uint32_t ta, uint32_t tc, uint32_t *out_a, uint32_t *out_c)
{
    BranchMode mode = effective_branch_mode();
    if (mode == BRANCH_MODE_A_ONLY) {
        *out_a = ta;
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        *out_c = phase_compare_encode_raw_tick(active_carrier->null_ticks);
#else
        *out_c = active_carrier->null_ticks;
#endif
    } else if (mode == BRANCH_MODE_C_ONLY) {
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        *out_a = phase_compare_encode_raw_tick(active_carrier->null_ticks);
#else
        *out_a = active_carrier->null_ticks;
#endif
        *out_c = tc;
    } else {
        *out_a = ta;
        *out_c = tc;
    }
}

void route_branch_pair(render_cmp_t *out_a, render_cmp_t *out_c, uint32_t ta, uint32_t tc)
{
    uint32_t a, c;
    route_branch_values(ta, tc, &a, &c);
    *out_a = (render_cmp_t)a;
    *out_c = (render_cmp_t)c;
}

void route_branch_pair_dma(uint32_t *out_a, uint32_t *out_c, uint32_t ta, uint32_t tc)
{
    route_branch_values(ta, tc, out_a, out_c);
}

void apply_branch_ac_offset(uint32_t *ta, uint32_t *tc)
{
    if ((effective_branch_mode() == BRANCH_MODE_BOTH) &&
        mod_calibration_is_active(active_cal_flags, cfg_debug_cal_bypass)) {
        int16_t ofs = cfg_branch_ac_offset_ticks;
        if (ofs != 0) {
            int16_t a_ofs = (int16_t)(-(ofs / 2));
            int16_t c_ofs = (int16_t)(ofs - (ofs / 2));
            *ta = compare_ticks_offset(*ta, a_ofs);
            *tc = compare_ticks_offset(*tc, c_ofs);
        }
    }
}

static void apply_branch_ac_offset_only(uint32_t *ta, uint32_t *tc)
{
    if (effective_branch_mode() != BRANCH_MODE_BOTH) return;
    if (!mod_calibration_is_active(active_cal_flags, cfg_debug_cal_bypass)) return;
    int16_t ofs = cfg_branch_ac_offset_ticks;
    if (ofs == 0) return;

    int16_t a_ofs = (int16_t)(-(ofs / 2));
    int16_t c_ofs = (int16_t)(ofs - (ofs / 2));
    *ta = compare_ticks_offset(*ta, a_ofs);
    *tc = compare_ticks_offset(*tc, c_ofs);
}

void rf_en_gpio_set(uint8_t on)
{
#if MOD_DEEP_DIAG_ENABLE
    on = 0U; /* Experimental DSP remains RF-inhibited. */
#endif
    if ((on != 0U) && (pipeline_hard_lockout != 0U)) on = 0U;
    if (on) RADIO_RequestTx();
    BOARD_WriteRf(on);
}

void rf_en_reset_tracking(void)
{
    rf_en_follow_metric = 0.0f;
}


static uint8_t rf_en_state = 0U;
static uint8_t rf_en_hold = 0U;
/* HAL_GetTick() | 1 when the renderer last committed a frame that radiates;
 * 0 before the first. */
static volatile uint32_t rf_rendered_tick = 0U;

/* A USB stream starts rendering: the first frame takes the renderer ~0.5 ms,
 * two receive halves, before rf_en_commit_frame can say whether it radiates;
 * mark the handover now (once per stream, so a silent one costs the receiver
 * at most the 100 ms of RADIO_TxHandover). */
void mod_tx_handover_mark(void)
{
    rf_rendered_tick = HAL_GetTick() | 1U;
}

/* Milliseconds since the renderer last produced a radiating frame, 255 at
 * most (and with none yet): the render underrun's snapshot. */
uint32_t MOD_TxRfRenderedAgeMs(uint32_t now)
{
    const uint32_t t = rf_rendered_tick;
    if (t == 0U) return 255U;
    const int32_t age = (int32_t)(now - t); /* the | 1 can put it one ahead */
    if (age <= 0) return 0U;
    return (age > 255) ? 255U : (uint32_t)age;
}

uint8_t MOD_TxRfRenderedWithin(uint32_t now, uint32_t ms)
{
    const uint32_t t = rf_rendered_tick;
    /* Signed: the | 1 puts an even tick's stamp one ahead of now. */
    return (uint8_t)((t != 0U) && ((int32_t)(now - t) <= (int32_t)ms));
}

void rf_en_commit_frame(float peak_metric, uint8_t any_active)
{
    peak_metric = mod_control_activity_metric(peak_metric, &any_active);
    if ((cfg_outphase_fallback_policy == MOD_OUTPHASE_FALLBACK_BLOCK) &&
        (MOD_GetOutphaseCapability(active_mod_type) != MOD_CAPABILITY_VALID)) {
        any_active = 0U;
        peak_metric = 0.0f;
        outphase_limit_latched = 1U;
    }

    if (any_active && peak_metric >= cfg_rf_en_assert_metric) {
        rf_en_state = 1U;
        rf_en_hold = cfg_rf_en_hold_frames;
    } else if (rf_en_state && rf_en_hold > 0U) {
        rf_en_hold--;
    } else if (!any_active || peak_metric <= cfg_rf_en_release_metric) {
        rf_en_state = 0U;
        rf_en_hold = 0U;
    }

    cw_render_frame_rf_active = rf_en_state;
    cw_render_frame_tx_live = any_active;
    if (rf_en_state) rf_rendered_tick = HAL_GetTick() | 1U;
}

uint8_t MOD_RfEnIsActiveOrHolding(void)
{
    return (rf_en_state || rf_en_hold) ? 1U : 0U;
}

void rf_en_force_idle_preserve_cw(void)
{
    rf_en_reset_tracking();
    rf_en_state = 0U;
    rf_en_hold = 0U;
    rf_en_gpio_set(0U);
}

void rf_en_force_idle(void)
{
    cw_reset_state();
    rf_en_force_idle_preserve_cw();
}


/* Build the idle/no-audio carrier pair. This keeps the historical null-tick
 * idle output, but now also applies the user A↔C branch offset so the branch
 * skew remains visible even when no modulation is present. PH TRIM and other
 * modulation-domain calibration and PA phase-sign/polarity flags do NOT belong
 * here; only the static branch-to-branch timing skew is applied in the idle
 * path so the RF-off null compare remains stable. */
void build_idle_branch_pair(uint32_t *ta, uint32_t *tc)
{
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    uint32_t a = phase_compare_encode_raw_tick(active_carrier->null_ticks);
    uint32_t c = phase_compare_encode_raw_tick(active_carrier->null_ticks);
#else
    uint32_t a = active_carrier->null_ticks;
    uint32_t c = active_carrier->null_ticks;
#endif
    apply_branch_ac_offset_only(&a, &c);
    *ta = a;
    *tc = c;
}

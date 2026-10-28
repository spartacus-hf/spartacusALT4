#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_priv.h"
#include "radio.h"
#include "radio_rx_front.h"
#include "clock_plan.h"
#include "radio_digi.h"

/* ═══════════════ Band preset table and carrier-clock retune state ═══════════════ */

static const mod_band_t band_presets[] = {
    { "160M",    1.840f },
    { "120M BC", 2.300f },
    { "90M BC",  3.200f },
    { "80M",     3.750f },
    { "75M BC",  3.950f },
    { "60M",     5.357f },
    { "49M BC",  6.050f },
    { "40M",     7.150f },
    { "41M BC",  7.250f },
    { "31M BC",  9.650f },
    { "30M",    10.130f },
    { "25M BC", 11.850f },
    { "22M BC", 13.570f },
    { "20M",    14.225f },
    { "19M BC", 15.250f },
    { "17M",    18.118f },
    { "15M",    21.300f },
    { "13M BC", 21.650f },
    { "12M",    24.940f },
    { "11M CB", 27.205f },
    { "10M CW", 28.020f },
    { "10M FT8",28.074f },
    { "10M BEC",28.200f }
};
_Static_assert((sizeof(band_presets) / sizeof(band_presets[0])) == BAND_COUNT, "band table count mismatch");

/* HRTIM compare/dead-time math is runtime-clock aware. The default remains
 * the validated 170 MHz HRTIM input clock; experimental dynamic policies may
 * retune SYSCLK/PLL/HRTIM only while the RF path is idle. */
/* The receive IF the LO planner aims for, and how far off it has to land
 * before a different SYSCLK is worth the retune. 20 kHz is above anything the
 * planner produces below 14 MHz, so nothing changes on the bands that already
 * land near the target. */
#define MOD_RX_IF_TARGET_HZ             12000UL
#define MOD_RX_IF_SHOP_ABOVE_HZ         8000UL
#define MOD_RX_IF_SHOP_GAIN_HZ          4000UL
#define MOD_RX_IF_ADC_HIGH_PENALTY_HZ   60000UL
#define MOD_HRTIM_INPUT_CLK_MIN_HZ      165000000UL
#define MOD_HRTIM_INPUT_CLK_SAFE_MIN_HZ 165000000UL
#define MOD_HRTIM_INPUT_CLK_MAX_HZ      170000000UL
#define MOD_DYNAMIC_RETUNE_SETTLE_MS    350U
/* AUTO's optional PLL upgrade restarts the receiver (~10 ms of silence and a
 * fresh DSP), so it waits until the dial has been left alone this long: at
 * 350 ms it came in the middle of tuning across a band, between encoder
 * turns, and was heard as a dropout. The receive plan never waits on it --
 * the running clock already serves the dial. */
#define MOD_AUTO_UPGRADE_IDLE_MS        3000U

enum {
    MOD_RETUNE_REASON_NONE = 0U,
    MOD_RETUNE_REASON_POLICY_FIXED = 1U,
    MOD_RETUNE_REASON_SMALL_GAIN = 2U,
    MOD_RETUNE_REASON_QUALITY_REJECT = 3U,
    MOD_RETUNE_REASON_WAIT_IDLE = 4U,
    MOD_RETUNE_REASON_APPLY_FAIL = 5U,
    MOD_RETUNE_REASON_FALLBACK_FAIL = 6U,
    MOD_RETUNE_REASON_DEFERRED_CLOCK = 7U,
    MOD_RETUNE_REASON_CLOCK_LOW = 8U,
    MOD_RETUNE_REASON_PERIOD_LOW = 9U,
    MOD_RETUNE_REASON_TX_HOP = 10U
};

typedef struct {
    uint32_t requested_hz;
    uint32_t hrtim_input_hz;
    uint16_t period_ticks;
    uint16_t null_ticks;
    uint8_t  frac_cycles;   /* the carrier fraction (clock_plan.h clock_carrier_t) */
    int8_t   frac_extra;
    uint32_t actual_hz;
    int32_t  error_hz;      /* grid - dial, rounded: what the TX NCO removes */
    uint32_t tx_err_hz;     /* CLOCK_CarrierCostHz from the spectral centre: the images */
    uint8_t  pll_m;
    uint8_t  pll_n;
    uint8_t  pll_r;
    uint8_t  valid;
    uint8_t  period_clamped; /* no legal period reaches the target */
} carrier_clock_solution_t;

/* AUTO's deferred PLL upgrade: the frequency is already applied on the
 * running clock; the better clock is taken once the radio is idle. */
static uint8_t carrier_upgrade_pending = 0U;
static uint32_t carrier_upgrade_due_ms = 0U;
/* The TX TEST page keeps the upgrade waiting (MOD_CarrierUpgradeHold): the
 * PLL does not move under a bench measurement. */
static uint8_t carrier_upgrade_hold = 0U;
/* Key-down hops taken (MOD_TxClockHop), for the console's `clock`. */
static uint32_t carrier_tx_hop_count = 0U;
/* TX TEST's grid preset in use (MOD_TxTestDiagPreset), 0 for none; whether
 * its dial was planned again after a PLL move, and the clock (10 kHz) it was
 * planned on. */
static uint8_t tx_diag_preset = 0U;
static uint8_t tx_diag_replanned = 0U;
static uint16_t tx_diag_clock_10khz = 0U;
/* MOD_CarrierReplan's request, taken by MOD_ServiceCarrierClockRetune. */
static volatile uint8_t carrier_replan_requested = 0U;

static uint32_t carrier_retune_pending_hz = 0U;
static uint32_t carrier_retune_due_ms = 0U;
static uint8_t carrier_retune_fallback_failed = 0U;
static volatile uint8_t carrier_tx_outphase_allowed_cached = 1U;

/* If an experimental dynamic PLL candidate fails but fallback to 170 MHz
 * succeeds, quarantine that exact PLL/clock candidate. This prevents repeated
 * retune/fallback churn on every subsequent frequency edit while still letting
 * the solver choose another valid precise candidate. Explicit policy changes
 * clear the quarantine so the user can deliberately retry in the lab. */
static uint8_t carrier_dynamic_quarantine_valid = 0U;
static mod_clock_policy_t carrier_dynamic_quarantine_policy = MOD_CLOCK_POLICY_FIXED_170;
static uint8_t carrier_dynamic_quarantine_pll_m = 0U;
static uint8_t carrier_dynamic_quarantine_pll_n = 0U;
static uint8_t carrier_dynamic_quarantine_pll_r = 0U;
static uint32_t carrier_dynamic_quarantine_clock_hz = 0U;

static uint32_t carrier_safe_min_improvement_hz(uint32_t requested_hz)
{
    if (requested_hz < 10000000UL) return 2000UL;
    if (requested_hz < 20000000UL) return 5000UL;
    return 10000UL;
}

static uint16_t carrier_min_period_for_policy(mod_clock_policy_t policy)
{
    uint16_t min_period;

    if (policy == MOD_CLOCK_POLICY_SAFE_DYNAMIC) {
        switch (active_mod_type) {
            case MOD_TYPE_SSB:  min_period = 160U; break;
            case MOD_TYPE_AM:   min_period = 144U; break;
            case MOD_TYPE_FM:   min_period = 128U; break;
            case MOD_TYPE_CW:   min_period = 96U;  break;
            case MOD_TYPE_TEST: min_period = 96U;  break;
            default:            min_period = 128U; break;
        }
        return (min_period < HRTIM_MIN_PERIOD_TICKS) ? HRTIM_MIN_PERIOD_TICKS : min_period;
    }

    if (policy == MOD_CLOCK_POLICY_PRECISION_DYNAMIC) {
        switch (active_mod_type) {
            case MOD_TYPE_SSB:  min_period = 128U; break;
            case MOD_TYPE_AM:   min_period = 112U; break;
            case MOD_TYPE_FM:   min_period = 96U;  break;
            case MOD_TYPE_CW:   min_period = 64U;  break;
            case MOD_TYPE_TEST: min_period = 64U;  break;
            default:            min_period = 96U;  break;
        }
        return (min_period < HRTIM_MIN_PERIOD_TICKS) ? HRTIM_MIN_PERIOD_TICKS : min_period;
    }

    return 160U;
}

static uint8_t carrier_solution_quality_ok(const carrier_clock_solution_t *sol,
                                           mod_clock_policy_t policy)
{
    if ((sol == NULL) || !sol->valid) return 0U;
    return (sol->period_ticks >= carrier_min_period_for_policy(policy)) ? 1U : 0U;
}

static uint8_t carrier_solution_within_policy(const carrier_clock_solution_t *sol,
                                              mod_clock_policy_t policy)
{
    if ((sol == NULL) || !sol->valid) return 0U;

    if ((policy == MOD_CLOCK_POLICY_SAFE_DYNAMIC) &&
        (sol->hrtim_input_hz < MOD_HRTIM_INPUT_CLK_SAFE_MIN_HZ)) {
        return 0U;
    }

    if ((policy == MOD_CLOCK_POLICY_PRECISION_DYNAMIC) &&
        (sol->hrtim_input_hz < MOD_HRTIM_INPUT_CLK_MIN_HZ)) {
        return 0U;
    }

    return carrier_solution_quality_ok(sol, policy);
}

static void carrier_dynamic_clear_quarantine(void)
{
    carrier_dynamic_quarantine_valid = 0U;
    carrier_dynamic_quarantine_policy = MOD_CLOCK_POLICY_FIXED_170;
    carrier_dynamic_quarantine_pll_m = 0U;
    carrier_dynamic_quarantine_pll_n = 0U;
    carrier_dynamic_quarantine_pll_r = 0U;
    carrier_dynamic_quarantine_clock_hz = 0U;
}

static void carrier_dynamic_quarantine_solution(const carrier_clock_solution_t *sol,
                                                mod_clock_policy_t policy)
{
    if ((sol == NULL) || !sol->valid) return;
    carrier_dynamic_quarantine_valid = 1U;
    carrier_dynamic_quarantine_policy = policy;
    carrier_dynamic_quarantine_pll_m = sol->pll_m;
    carrier_dynamic_quarantine_pll_n = sol->pll_n;
    carrier_dynamic_quarantine_pll_r = sol->pll_r;
    carrier_dynamic_quarantine_clock_hz = sol->hrtim_input_hz;
}

static uint8_t carrier_dynamic_solution_quarantined(const carrier_clock_solution_t *sol,
                                                    mod_clock_policy_t policy)
{
    if (!carrier_dynamic_quarantine_valid || (sol == NULL) || !sol->valid) return 0U;
    if (carrier_dynamic_quarantine_policy != policy) return 0U;
    return ((carrier_dynamic_quarantine_pll_m == sol->pll_m) &&
            (carrier_dynamic_quarantine_pll_n == sol->pll_n) &&
            (carrier_dynamic_quarantine_pll_r == sol->pll_r) &&
            (carrier_dynamic_quarantine_clock_hz == sol->hrtim_input_hz)) ? 1U : 0U;
}

static uint32_t mod_get_tim6_kernel_clock_hz(void)
{
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
#if defined(RCC_CFGR_PPRE1) && defined(RCC_CFGR_PPRE1_2)
    uint32_t ppre1 = RCC->CFGR & RCC_CFGR_PPRE1;
    if (ppre1 >= RCC_CFGR_PPRE1_2) {
        if (pclk1 <= 0x7FFFFFFFUL) return pclk1 * 2UL;
    }
#endif
    return pclk1;
}

pa_band_profile_t pa_legacy_profile = { PA_DEADTIME_DEFAULT_PS, PA_DEADTIME_DEFAULT_PS, 0 };
pa_band_profile_t pa_band_profiles[BAND_COUNT];
pa_resolved_runtime_t pa_runtime = {0};

HOT_INLINE int16_t clamp_permille(int32_t v)
{
    if (v > (int32_t)PA_DUTY_PERMILLE_SCALE) v = (int32_t)PA_DUTY_PERMILLE_SCALE;
    if (v < -(int32_t)PA_DUTY_PERMILLE_SCALE) v = -(int32_t)PA_DUTY_PERMILLE_SCALE;
    return (int16_t)v;
}

static void pa_profile_sanitize(pa_band_profile_t *p)
{
    if (!p) return;
    p->duty_offset_half_permille = clamp_permille((int32_t)p->duty_offset_half_permille);
}

static void clamp_branch_ac_runtime(void)
{
    int16_t ac_lim = mod_branch_ac_limit_ticks();
    if (cfg_branch_ac_offset_ticks > ac_lim) {
        cfg_branch_ac_offset_ticks = ac_lim;
        carrier_values_capped = 1U;
    }
    if (cfg_branch_ac_offset_ticks < -ac_lim) {
        cfg_branch_ac_offset_ticks = -ac_lim;
        carrier_values_capped = 1U;
    }
}

uint8_t band_index_for_freq(float mhz)
{
    uint8_t best = 0U;
    float best_err = fabsf(mhz - band_presets[0].mhz);
    for (uint8_t i = 1U; i < BAND_COUNT; i++) {
        float err = fabsf(mhz - band_presets[i].mhz);
        if (err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return best;
}

uint8_t MOD_GetBandCount(void)   { return BAND_COUNT; }
const mod_band_t *MOD_GetBand(uint8_t idx) { return (idx < BAND_COUNT) ? &band_presets[idx] : NULL; }

void pa_profiles_fill_from_legacy(void)
{
    pa_profile_sanitize(&pa_legacy_profile);
    for (uint8_t i = 0U; i < BAND_COUNT; i++) {
        pa_band_profiles[i] = pa_legacy_profile;
    }
}

uint16_t mod_deadtime_ticks_from_ps(uint32_t ps)
{
    return mod_deadtime_ticks_from_ps_at_clock(ps, MOD_GetHrtimInputClockHz());
}

uint32_t mod_deadtime_ps_from_ticks(uint16_t ticks)
{
    return mod_deadtime_ps_from_ticks_at_clock(ticks, MOD_GetHrtimInputClockHz());
}

uint16_t mod_deadtime_ticks_from_ns(uint16_t ns)
{
    return mod_deadtime_ticks_from_ps((uint32_t)ns * 1000UL);
}

uint16_t mod_deadtime_ticks_to_ns(uint16_t ticks)
{
    return (uint16_t)((mod_deadtime_ps_from_ticks(ticks) + 500UL) / 1000UL);
}

uint32_t mod_deadtime_ps_limit(void)
{
    if (active_carrier == NULL) return 0U;
    return mod_deadtime_ps_limit_at_clock(active_carrier->period_ticks,
                                          MOD_GetHrtimEffectiveClockHz(),
                                          PA_DEADTIME_HALF_CYCLE_LIMIT_PCT);
}

uint16_t mod_deadtime_ticks_limit(void)
{
    uint16_t lim = mod_deadtime_ticks_from_ps(mod_deadtime_ps_limit());
    if (lim < 1U) lim = 1U;
    if (lim > 511U) lim = 511U;
    return lim;
}

uint16_t mod_deadtime_ns_limit(void)
{
    return (uint16_t)((mod_deadtime_ps_limit() + 500UL) / 1000UL);
}

void mod_pa_runtime_sync_resolved_state(void)
{
    pa_runtime.carrier_values_capped = carrier_values_capped;
    pa_runtime.resolved_deadtime_rise_ticks = cfg_deadtime_rise_ticks;
    pa_runtime.resolved_deadtime_fall_ticks = cfg_deadtime_fall_ticks;
    pa_runtime.resolved_deadtime_rise_ps = mod_deadtime_ps_from_ticks(cfg_deadtime_rise_ticks);
    pa_runtime.resolved_deadtime_fall_ps = mod_deadtime_ps_from_ticks(cfg_deadtime_fall_ticks);
    pa_runtime.resolved_duty_offset_ticks = cfg_duty_offset_ticks;
}

void mod_pa_runtime_sync_applied_state_from_shadow(void)
{
    uint16_t rise_ticks = carrier_apply_valid ? carrier_apply_deadtime_rise_ticks
                                              : HRTIM_GetDeadTimeRiseTicks();
    uint16_t fall_ticks = carrier_apply_valid ? carrier_apply_deadtime_fall_ticks
                                              : HRTIM_GetDeadTimeFallTicks();
    pa_runtime.applied_deadtime_rise_ticks = rise_ticks;
    pa_runtime.applied_deadtime_fall_ticks = fall_ticks;
    pa_runtime.applied_deadtime_rise_ps = mod_deadtime_ps_from_ticks(rise_ticks);
    pa_runtime.applied_deadtime_fall_ps = mod_deadtime_ps_from_ticks(fall_ticks);
    pa_runtime.applied_duty_offset_ticks = carrier_apply_valid ? carrier_apply_duty_offset_ticks
                                                               : HRTIM_GetDutyOffsetTicks();
}

int16_t mod_branch_ac_limit_ticks(void)
{
    int32_t lim = ((int32_t)active_carrier->period_ticks / 2) - 8;
    if (lim < 8) lim = 8;
    if (lim > 1024) lim = 1024;
    return (int16_t)lim;
}

int16_t mod_duty_permille_from_ticks_for_carrier(const CarrierProfile *carrier, int16_t ticks)
{
    if (carrier == NULL) return 0;
    return mod_duty_permille_from_ticks(carrier->half_period_ticks, ticks,
                                        PA_DUTY_PERMILLE_SCALE);
}

int16_t mod_duty_ticks_from_permille_for_carrier(const CarrierProfile *carrier, int16_t permille)
{
    if (carrier == NULL) return 0;
    return mod_duty_ticks_from_permille(carrier->half_period_ticks, permille,
                                        PA_DUTY_PERMILLE_SCALE);
}

/* The band whose calibration context was last loaded. A carrier apply
 * reloads it only when the band changes: the record is per band, sideband
 * and backend, and the sideband and backend setters reload by themselves.
 * Reloading on every apply cost nothing while the records were in flash;
 * from the SD store it read M32CAL.BIN on every detent -- 134 ms that held
 * the receiver retune back (bench, 2026-10-01). */
static uint8_t cal_loaded_band = 0xFFU;

void pa_resolve_active_band_profile(uint8_t load_cal_context, uint8_t apply_hw, uint8_t queue_reset)
{
    pa_band_profile_t p = pa_legacy_profile;
    if (active_band_idx < BAND_COUNT) {
        p = pa_band_profiles[active_band_idx];
    }
    pa_profile_sanitize(&p);
    if (active_band_idx < BAND_COUNT) {
        pa_band_profiles[active_band_idx] = p;
    }

    carrier_values_capped = 0U;

    pa_runtime.band_idx = active_band_idx;
    pa_runtime.source_flags = (active_band_idx < BAND_COUNT) ? PA_PROFILE_SOURCE_PER_BAND
                                                             : PA_PROFILE_SOURCE_TEMPLATE;
    pa_runtime.requested_deadtime_rise_ps = p.deadtime_rise_ps;
    pa_runtime.requested_deadtime_fall_ps = p.deadtime_fall_ps;
    pa_runtime.requested_duty_offset_half_permille = p.duty_offset_half_permille;

    cfg_deadtime_rise_ticks = mod_deadtime_ticks_from_ps(p.deadtime_rise_ps);
    cfg_deadtime_fall_ticks = mod_deadtime_ticks_from_ps(p.deadtime_fall_ps);
    {
        uint16_t dt_lim = mod_deadtime_ticks_limit();
        if (cfg_deadtime_rise_ticks > dt_lim) {
            cfg_deadtime_rise_ticks = dt_lim;
            carrier_values_capped = 1U;
        }
        if (cfg_deadtime_fall_ticks > dt_lim) {
            cfg_deadtime_fall_ticks = dt_lim;
            carrier_values_capped = 1U;
        }
    }
    cfg_deadtime_ticks = (uint16_t)(((uint32_t)cfg_deadtime_rise_ticks + (uint32_t)cfg_deadtime_fall_ticks + 1U) / 2U);
    HRTIM_SetDeadTimeTicksRF(cfg_deadtime_rise_ticks, cfg_deadtime_fall_ticks);

    cfg_duty_offset_ticks = mod_duty_ticks_from_permille_for_carrier(active_carrier, p.duty_offset_half_permille);
    {
        /* Only what the HRTIM geometry can hold at this period: on 10 m as
         * little as 0 (mod_hrtim_duty_offset_bounds). */
        int16_t duty_lo = 0, duty_hi = 0;
        (void)mod_hrtim_duty_offset_bounds_frac(active_carrier->period_ticks, active_carrier->frac_extra,
                                                &duty_lo, &duty_hi);
        if (cfg_duty_offset_ticks > duty_hi) {
            cfg_duty_offset_ticks = duty_hi;
            carrier_values_capped = 1U;
        }
        if (cfg_duty_offset_ticks < duty_lo) {
            cfg_duty_offset_ticks = duty_lo;
            carrier_values_capped = 1U;
        }
    }
    HRTIM_SetDutyOffsetTicks(cfg_duty_offset_ticks);

    if (load_cal_context) {
        load_current_calibration_context();
        cal_loaded_band = active_band_idx;
    }
    clamp_branch_ac_runtime();

    mod_pa_runtime_sync_resolved_state();
    mod_pa_runtime_sync_applied_state_from_shadow();

    if (apply_hw) {
        if (mod_runtime_timing_only_update_possible()) mod_runtime_apply_active_timing_update();
        else                                           mod_runtime_reconfigure_active_carrier();
    }
    if (queue_reset) {
        requested_reset = 1U;
    }
}

/* ═══════════════ Configurable Carrier Frequency / HRTIM clock ═══════════════ */

static uint16_t carrier_compare_min_ticks_for_period(uint32_t period)
{
    (void)period;
    return HRTIM_PHASE_MIN_COMPARE;
}

static uint16_t carrier_compare_max_ticks_for_period(uint32_t period)
{
    if (period <= (uint32_t)(HRTIM_PHASE_MIN_COMPARE + 4U)) return HRTIM_PHASE_MIN_COMPARE;
    return (uint16_t)(period - 4U);
}

static uint16_t carrier_legal_compare_ticks(uint32_t period, uint32_t ticks)
{
    const uint16_t lo = carrier_compare_min_ticks_for_period(period);
    const uint16_t hi = carrier_compare_max_ticks_for_period(period);

    if (ticks < lo) ticks = lo;
    if (ticks > hi) ticks = hi;
    return (uint16_t)ticks;
}

static uint16_t carrier_safe_null_ticks(uint32_t period)
{
    uint32_t null_ticks = period / 4U;

    if ((null_ticks < HRTIM_OUTPUT_MIN_COMPARE) &&
        (period > (uint32_t)(HRTIM_OUTPUT_MIN_COMPARE + 4U))) {
        uint32_t lo = HRTIM_OUTPUT_MIN_COMPARE;
        uint32_t hi = carrier_compare_max_ticks_for_period(period);
        null_ticks = (lo + hi + 1U) / 2U;
    }
    return carrier_legal_compare_ticks(period, null_ticks);
}

static uint32_t clamp_carrier_hz(uint32_t hz)
{
    if (hz < MOD_CARRIER_MIN_HZ) hz = MOD_CARRIER_MIN_HZ;
    if (hz > MOD_CARRIER_MAX_HZ) hz = MOD_CARRIER_MAX_HZ;
    return hz;
}

uint32_t MOD_GetHrtimInputClockHz(void)
{
    uint32_t hz = mod_hrtim_input_clock_hz;
    return (hz == 0U) ? MOD_HRTIM_INPUT_CLK_DEFAULT_HZ : hz;
}

uint64_t MOD_GetHrtimEffectiveClockHz(void)
{
    return (uint64_t)MOD_GetHrtimInputClockHz() * MOD_HRTIM_HIGHRES_MUL;
}

uint32_t MOD_GetTim6InputClockHz(void)
{
    uint32_t hz = mod_tim6_input_clock_hz;
    return (hz == 0U) ? SystemCoreClock : hz;
}

static int32_t carrier_ref_ppb(void)
{
    return RADIO_Config()->ref_ppb;
}

/* The transmitter's load class (clock_plan.h) for the running mode: SSB by
 * its USB AUDIO POLICY (the speech tools, or DIGI CLEAN without them). The
 * internal test signals other than the carrier test render SSB. */
static clock_tx_load_t carrier_tx_load(void)
{
    switch (active_mod_type) {
        case MOD_TYPE_AM: return CLOCK_TX_LOAD_AM;
        case MOD_TYPE_FM: return CLOCK_TX_LOAD_FM;
        case MOD_TYPE_CW: return CLOCK_TX_LOAD_CW;
        case MOD_TYPE_TEST:
            return ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) ? CLOCK_TX_LOAD_CW
                                                                             : CLOCK_TX_LOAD_SSB;
        case MOD_TYPE_SSB:
        default:
            return mod_tx_voice_processing() ? CLOCK_TX_LOAD_SSB : CLOCK_TX_LOAD_SSB_CLEAN;
    }
}

/* Whether AUTO may run at this SYSCLK: the receive worker keeps the cycle
 * budget per PCM1821 frame (clock_plan.h) -- 164-170 MHz, or with WIDE CLOCK
 * RANGE also the divider-14 clocks from 153.3 MHz on which the transmitter,
 * in the running mode and phase rate, fits its budget too: USB-audio SSB with
 * the speech tools needed 99.7 % of the CPU at 153.3 MHz with TX PHASE 384K,
 * so voice SSB there keeps to 164-170 MHz and at 192 kHz starts at 154.7 MHz;
 * DIGI CLEAN SSB (FT8), AM, CW and FM may use them all. The
 * clock is chosen while receiving and kept through a transmission, which is
 * why the mode's worst case decides. PRECISE and EXACT keep their 165 MHz
 * floor. */
static uint8_t carrier_auto_clock_ok(uint32_t sysclk_hz)
{
    const uint32_t cycles = CLOCK_RxFrameCycles(sysclk_hz);
    if (cycles >= CLOCK_RX_FRAME_CYCLES_MIN) return 1U;
    const uint8_t flags = RADIO_Config()->clock_flags;
    if (!(flags & RADIO_CLOCK_FLAG_WIDE) || (cycles < CLOCK_RX_FRAME_CYCLES_WIDE_MIN)) return 0U;
    return CLOCK_TxFits(sysclk_hz, carrier_tx_load(), (uint8_t)((flags & RADIO_CLOCK_FLAG_PHASE384) != 0U));
}

/* Where the transmitted spectrum sits relative to the dial. The TX NCO makes
 * the frequency exact whatever the carrier grid, but it steps the phase only
 * at the phase-update rate, so its images scale with how far the modulated
 * signal is from the grid: for SSB that is the passband centre, not the
 * suppressed carrier. Only AUTO aims there; the older policies keep the dial. */
static int32_t carrier_spectral_centre_hz(void)
{
    if (active_mod_type != MOD_TYPE_SSB) return 0;
    return (active_sideband_enum == SIDEBAND_LSB) ? -1500 : 1500;
}

static int32_t carrier_tx_centre_hz(void)
{
    return (MOD_GetCarrierClockPolicy() == MOD_CLOCK_POLICY_AUTO) ? carrier_spectral_centre_hz() : 0;
}

/* Everything besides the dial that AUTO's choice depends on: where the
 * transmitted spectrum sits (the SSB sideband), the receive mode's channel,
 * the transmitter's load class, the phase rate and the WIDE range. A mode
 * change used to leave the clock that the previous mode had been tuned with:
 * 80 m LSB with WIDE and TX PHASE 384K ran at 164 or at 153.3 MHz depending
 * on whether the band or the mode had been set last. */
static uint32_t carrier_auto_inputs(void)
{
    const radio_config_t *c = RADIO_Config();
    return (uint32_t)(carrier_spectral_centre_hz() + 0x8000) |
           ((uint32_t)(c->rx_mode & 0x0FU) << 16) |
           ((uint32_t)carrier_tx_load() << 20) |
           ((uint32_t)(c->clock_flags & (RADIO_CLOCK_FLAG_WIDE | RADIO_CLOCK_FLAG_PHASE384)) << 24);
}
/* carrier_auto_inputs() when AUTO last chose (or was asked to choose) a clock. */
static uint32_t carrier_auto_planned_inputs;

static int32_t round_milli(int64_t mhz)
{
    int64_t hz = mhz >= 0 ? (mhz + 500) / 1000 : (mhz - 500) / 1000;
    if (hz > 2147483647LL) hz = 2147483647LL;
    if (hz < -2147483647LL) hz = -2147483647LL;
    return (int32_t)hz;
}

/* What the carrier fraction (clock_plan.h) depends on besides the clock and
 * the dial: the phase rate the next stream runs at, how long a master period
 * the mode may have -- half an update for CW and the carrier test, whose
 * phase does not move between keying ramps, an eighth for the modulated
 * modes, whose phase updates take effect at the master's roll-over -- and
 * the PA duty offset of the band `hz` is in, which a shortened period must
 * leave room for. */
static void carrier_tx_shape(uint32_t hz, clock_tx_shape_t *shape)
{
    shape->update_hz = MOD_TxPhaseUpdateHzNext();
    shape->master_div = (carrier_tx_load() == CLOCK_TX_LOAD_CW) ? 2U : 8U;
    const uint8_t band = band_index_for_freq((float)hz * 0.000001f);
    pa_band_profile_t p = (band < BAND_COUNT) ? pa_band_profiles[band] : pa_legacy_profile;
    pa_profile_sanitize(&p);
    shape->duty_permille = p.duty_offset_half_permille;
}

/* Carrier geometry for one PLL setting, from the exact clock-tree arithmetic
 * (clock_plan.c) with the reference correction applied: actual and error are
 * the frequencies the radio really makes, not their 8 MHz-nominal images.
 * The period carries a fraction where that lowers the TX NCO's images
 * (CLOCK_CarrierPlan). */
static void carrier_solve_for_pll(uint32_t requested_hz,
                                  uint8_t pll_m,
                                  uint8_t pll_n,
                                  uint8_t pll_r,
                                  carrier_clock_solution_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    requested_hz = clamp_carrier_hz(requested_hz);
    const clock_pll_t pll = {pll_m, pll_n, pll_r};
    if (!CLOCK_PllLegal(pll)) return;
    const uint32_t clk = CLOCK_PllNominalHz(pll);
    if ((clk < CLOCK_SYSCLK_AUTO_MIN_HZ) || (clk > MOD_HRTIM_INPUT_CLK_MAX_HZ)) return;

    const uint64_t sysclk = CLOCK_PllMicroHz(pll, carrier_ref_ppb());
    const uint32_t target = (uint32_t)((int32_t)requested_hz + carrier_tx_centre_hz());
    const uint64_t hr = sysclk * CLOCK_HRTIM_MUL;
    const uint64_t t = (uint64_t)target * 1000000ULL;
    clock_tx_shape_t shape;
    carrier_tx_shape(requested_hz, &shape);
    clock_carrier_t c;
    if (!CLOCK_CarrierPlan(sysclk, target, &shape, &c)) {
        /* No legal period: the nearest legal whole one, flagged. */
        uint64_t period = (hr + t / 2U) / t;
        if (period < (uint64_t)HRTIM_MIN_PERIOD_TICKS) period = HRTIM_MIN_PERIOD_TICKS;
        if (period > 65535ULL) period = 65535ULL;
        c.period = (uint16_t)period;
        c.cycles = 2U;
        c.extra = 0;
        out->period_clamped = 1U;
    }

    out->requested_hz = requested_hz;
    out->hrtim_input_hz = clk;
    out->period_ticks = c.period;
    out->frac_cycles = c.cycles;
    out->frac_extra = c.extra;
    out->null_ticks = carrier_safe_null_ticks(c.period);
    out->actual_hz = (uint32_t)((CLOCK_CarrierMicroHz(sysclk, &c) + 500000ULL) / 1000000ULL);
    out->error_hz = round_milli(CLOCK_CarrierErrorMilliHz(sysclk, &c, requested_hz));
    out->tx_err_hz = CLOCK_CarrierCostHz(sysclk, &c, target, shape.update_hz);
    out->pll_m = pll_m;
    out->pll_n = pll_n;
    out->pll_r = pll_r;
    out->valid = 1U;
}

/* The same for the PLL setting the radio is running on now. */
static void carrier_solve_current(uint32_t requested_hz, carrier_clock_solution_t *out)
{
    carrier_solve_for_pll(requested_hz,
                          carrier_retune_last_pll_m ? (uint8_t)carrier_retune_last_pll_m : CLOCK_BOOT_PLL_M,
                          carrier_retune_last_pll_n ? (uint8_t)carrier_retune_last_pll_n : CLOCK_BOOT_PLL_N,
                          carrier_retune_last_pll_r ? (uint8_t)carrier_retune_last_pll_r : CLOCK_BOOT_PLL_R,
                          out);
}

static uint32_t abs_i32_u32(int32_t v)
{
    return (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
}

static uint8_t solution_better(const carrier_clock_solution_t *a,
                               const carrier_clock_solution_t *b,
                               uint32_t current_clock_hz)
{
    uint32_t ea = abs_i32_u32(a->error_hz);
    uint32_t eb = abs_i32_u32(b->error_hz);
    if (ea != eb) return (ea < eb) ? 1U : 0U;
    if (a->hrtim_input_hz == current_clock_hz && b->hrtim_input_hz != current_clock_hz) return 1U;
    if (b->hrtim_input_hz == current_clock_hz && a->hrtim_input_hz != current_clock_hz) return 0U;
    return (a->hrtim_input_hz > b->hrtim_input_hz) ? 1U : 0U;
}

/* Receiver LO quality (RADIO_RxPlanLo) of a candidate SYSCLK for the tuned
 * frequency: 2 = DC-free 12 kHz-style IF, 1 = in band but touching DC. */
static uint8_t carrier_rx_quality(uint32_t requested_hz, uint32_t hrtim_input_hz)
{
    return RADIO_RxPlanLo((uint64_t)hrtim_input_hz * MOD_HRTIM_HIGHRES_MUL, requested_hz,
                          (radio_rx_mode_t)RADIO_Config()->rx_mode, NULL, NULL);
}

/* How far the receive plan's IF lands from the 12 kHz target, or UINT32_MAX
 * when this clock has no usable plan at all.
 *
 * Quality alone is not enough to choose a clock. RADIO_RxPlanLo calls any IF
 * inside the flat +-50 kHz span and clear of DC "quality 2", but the QSD's
 * residual I/Q error is a fixed skew between the two channels, so the phase
 * error it produces grows in proportion to the IF. Measured on REV1: 12.0 deg
 * at an 9.3 kHz offset and 69.9 deg at 36.0 kHz, which is the difference
 * between 50 dB of corrected image rejection and 6 dB. One HRTIM tick moves
 * the LO by rf^2 / (SYSCLK x 32), which is 36 kHz at 14 MHz, so on 20 m and up
 * the nearest tick can be tens of kilohertz from the target while still
 * counting as usable. Another legal PLL setting usually lands much closer. */
static uint32_t carrier_rx_if_error(uint32_t requested_hz, uint32_t hrtim_input_hz)
{
    int32_t offset = 0;
    uint8_t adc96 = 0U;
    if (RADIO_RxPlanLoAdc((uint64_t)hrtim_input_hz * MOD_HRTIM_HIGHRES_MUL, requested_hz,
                          (radio_rx_mode_t)RADIO_Config()->rx_mode, NULL, &offset, &adc96) < 2U)
        return UINT32_MAX;
    uint32_t magnitude = abs_i32_u32(offset);
    /* A plan that needs the ADC at 170-190 kHz, where the PCM1820 repeats
     * samples (radio_rx_front.c), ranks behind every one at ~96 kHz. */
    return (magnitude > MOD_RX_IF_TARGET_HZ ? magnitude - MOD_RX_IF_TARGET_HZ
                                            : MOD_RX_IF_TARGET_HZ - magnitude) +
           (adc96 ? 0U : MOD_RX_IF_ADC_HIGH_PENALTY_HZ);
}

/* The PLL settings between a floor and 170 MHz, ascending (clock_plan.c).
 * Recomputed only when the floor changes: the 8 MHz reference gives six in
 * PRECISE/EXACT's 165-170 MHz window and 21 down to AUTO's 150 MHz. */
static clock_pll_t plan_cand[CLOCK_PLL_CANDIDATES_MAX];
static uint8_t plan_cand_count;
static uint32_t plan_cand_floor;

static unsigned carrier_candidates(uint32_t floor_hz)
{
    if (floor_hz != plan_cand_floor) {
        plan_cand_count = (uint8_t)CLOCK_PllCandidates(floor_hz, MOD_HRTIM_INPUT_CLK_MAX_HZ,
                                                        plan_cand, CLOCK_PLL_CANDIDATES_MAX);
        plan_cand_floor = floor_hz;
    }
    return plan_cand_count;
}

/* `rx_if_rank` ranks candidates by how close the receive IF lands to the
 * target first and by transmit residual second. Off, the order is unchanged:
 * transmit residual decides and the receive plan is only a filter. */
static uint8_t carrier_dynamic_solution(uint32_t requested_hz,
                                        mod_clock_policy_t policy,
                                        carrier_clock_solution_t *best,
                                        uint8_t rx_quality_min,
                                        uint8_t rx_if_rank)
{
    uint32_t best_if = UINT32_MAX;
    uint32_t current_clock = MOD_GetHrtimInputClockHz();
    uint32_t min_clock = (policy == MOD_CLOCK_POLICY_SAFE_DYNAMIC)
                       ? MOD_HRTIM_INPUT_CLK_SAFE_MIN_HZ
                       : MOD_HRTIM_INPUT_CLK_MIN_HZ;
    uint16_t min_period = carrier_min_period_for_policy(policy);
    carrier_clock_solution_t cand;
    uint8_t found = 0U;

    if (best == NULL) return 0U;
    memset(best, 0, sizeof(*best));

    const unsigned count = carrier_candidates(min_clock);
    for (unsigned i = 0U; i < count; ++i) {
        carrier_solve_for_pll(requested_hz, plan_cand[i].m, plan_cand[i].n, plan_cand[i].r, &cand);
        if (!cand.valid || cand.period_ticks < min_period) continue;
        if (rx_quality_min && carrier_rx_quality(requested_hz, cand.hrtim_input_hz) < rx_quality_min) continue;
        if (carrier_dynamic_solution_quarantined(&cand, policy)) continue;
        if (rx_if_rank) {
            const uint32_t cand_if = carrier_rx_if_error(requested_hz, cand.hrtim_input_hz);
            if (cand_if == UINT32_MAX || (found && cand_if > best_if)) continue;
            if (!found || cand_if < best_if || solution_better(&cand, best, current_clock)) {
                *best = cand;
                best_if = cand_if;
                found = 1U;
            }
            continue;
        }
        if (!found || solution_better(&cand, best, current_clock)) {
            *best = cand;
            found = 1U;
        }
    }
    return found;
}

/* AUTO's candidates evaluated at `requested_hz` (clamped): a clock AUTO may
 * not use, or a quarantined one, gets period 0. `*current` is the running
 * setting's index, or -1. Returns the count. */
static unsigned carrier_auto_evaluate(uint32_t requested_hz, clock_eval_t *ev, int *current_out)
{
    const unsigned n = carrier_candidates(CLOCK_SYSCLK_AUTO_MIN_HZ);
    const int32_t ppb = carrier_ref_ppb();
    const int32_t centre = carrier_spectral_centre_hz();
    const radio_rx_mode_t mode = (radio_rx_mode_t)RADIO_Config()->rx_mode;
    const uint8_t cm = carrier_retune_last_pll_m ? (uint8_t)carrier_retune_last_pll_m : CLOCK_BOOT_PLL_M;
    const uint8_t cn = carrier_retune_last_pll_n ? (uint8_t)carrier_retune_last_pll_n : CLOCK_BOOT_PLL_N;
    const uint8_t cr = carrier_retune_last_pll_r ? (uint8_t)carrier_retune_last_pll_r : CLOCK_BOOT_PLL_R;
    int current = -1;
    requested_hz = clamp_carrier_hz(requested_hz);
    clock_tx_shape_t shape;
    carrier_tx_shape(requested_hz, &shape);
    carrier_auto_planned_inputs = carrier_auto_inputs();
    for (unsigned i = 0U; i < n; ++i) {
        if (plan_cand[i].m == cm && plan_cand[i].n == cn && plan_cand[i].r == cr) current = (int)i;
        if (!carrier_auto_clock_ok(CLOCK_PllNominalHz(plan_cand[i]))) {
            memset(&ev[i], 0, sizeof(ev[i]));
            continue;
        }
        CLOCK_Evaluate(plan_cand[i], ppb, requested_hz, centre, mode, &shape, &ev[i]);
        if (carrier_dynamic_quarantine_valid &&
            (carrier_dynamic_quarantine_policy == MOD_CLOCK_POLICY_AUTO) &&
            (carrier_dynamic_quarantine_pll_m == plan_cand[i].m) &&
            (carrier_dynamic_quarantine_pll_n == plan_cand[i].n) &&
            (carrier_dynamic_quarantine_pll_r == plan_cand[i].r)) ev[i].period = 0U;
    }
    *current_out = current;
    return n;
}

/* AUTO (clock_plan.c): of the PLL settings that can transmit here, the one
 * whose receive plan is best -- a clean IF near 12 kHz, where the stored I/Q
 * correction holds -- and then the smallest distance from the carrier grid to
 * the transmitted spectrum, so the TX NCO's images are as low as this clock
 * tree allows. The running setting is kept unless another is clearly better,
 * so tuning does not retune the PLL for gains nobody could measure. A clock
 * that would leave the receive worker short of cycles is never evaluated
 * (period 0), so a running one is left as soon as WIDE is turned off. */
static uint8_t carrier_auto_solution(uint32_t requested_hz, carrier_clock_solution_t *selected)
{
    clock_eval_t ev[CLOCK_PLL_CANDIDATES_MAX];
    int current;
    requested_hz = clamp_carrier_hz(requested_hz);
    const unsigned n = carrier_auto_evaluate(requested_hz, ev, &current);
    const int pick = CLOCK_ChooseAuto(ev, n, current);
    if (pick < 0) return 0U;
    carrier_solve_for_pll(requested_hz, ev[pick].pll.m, ev[pick].pll.n, ev[pick].pll.r, selected);
    return selected->valid;
}

static void carrier_default_170_solution(uint32_t requested_hz, carrier_clock_solution_t *out)
{
    carrier_solve_for_pll(requested_hz, CLOCK_BOOT_PLL_M, CLOCK_BOOT_PLL_N, CLOCK_BOOT_PLL_R, out);
}

static uint8_t carrier_select_solution(uint32_t requested_hz, carrier_clock_solution_t *selected)
{
    carrier_clock_solution_t current;
    carrier_clock_solution_t candidate;
    mod_clock_policy_t policy = MOD_GetCarrierClockPolicy();

    if (selected == NULL) return 0U;
    carrier_solve_current(requested_hz, &current);

    if (policy == MOD_CLOCK_POLICY_AUTO) {
        if (!carrier_auto_solution(requested_hz, selected)) carrier_default_170_solution(requested_hz, selected);
        carrier_retune_last_reason = (selected->hrtim_input_hz == current.hrtim_input_hz)
                                   ? MOD_RETUNE_REASON_NONE
                                   : MOD_RETUNE_REASON_POLICY_FIXED;
        return 1U;
    }

    if (policy == MOD_CLOCK_POLICY_FIXED_170) {
        carrier_default_170_solution(requested_hz, selected);
        /* The receiver shares this clock. Above ~14 MHz one HRTIM tick moves
         * the LO by more than its usable IF window, so when 170 MHz cannot give
         * a DC-free IF (or any in-band LO), use another legal PLL setting that
         * can; among those the TX residual still decides. */
        uint8_t rx_here = carrier_rx_quality(requested_hz, selected->hrtim_input_hz);
        if (rx_here < 2U &&
            (carrier_dynamic_solution(requested_hz, MOD_CLOCK_POLICY_SAFE_DYNAMIC, &candidate, 2U, 0U) ||
             (rx_here == 0U &&
              carrier_dynamic_solution(requested_hz, MOD_CLOCK_POLICY_SAFE_DYNAMIC, &candidate, 1U, 0U)))) {
            *selected = candidate;
        } else if (abs_i32_u32(selected->error_hz) > 40000UL &&
                   carrier_dynamic_solution(requested_hz, MOD_CLOCK_POLICY_SAFE_DYNAMIC, &candidate, rx_here, 0U) &&
                   abs_i32_u32(candidate.error_hz) < abs_i32_u32(selected->error_hz)) {
            *selected = candidate;
        } else if (rx_here >= 2U) {
            /* A usable plan is not necessarily a good one. See
             * carrier_rx_if_error: the image rejection this receiver can reach
             * falls off in proportion to the IF, and above 14 MHz the nearest
             * HRTIM tick can leave the IF tens of kilohertz out while still
             * passing every check the planner makes. Shop for a clock that
             * lands closer, and take it only if it is closer by a margin worth
             * a retune. */
            const uint32_t here_if = carrier_rx_if_error(requested_hz, selected->hrtim_input_hz);
            if (here_if > MOD_RX_IF_SHOP_ABOVE_HZ &&
                carrier_dynamic_solution(requested_hz, MOD_CLOCK_POLICY_SAFE_DYNAMIC, &candidate, 2U, 1U)) {
                const uint32_t there_if = carrier_rx_if_error(requested_hz, candidate.hrtim_input_hz);
                if (there_if + MOD_RX_IF_SHOP_GAIN_HZ < here_if) *selected = candidate;
            }
        }
        carrier_retune_last_reason = (selected->hrtim_input_hz == current.hrtim_input_hz)
                                   ? MOD_RETUNE_REASON_NONE
                                   : MOD_RETUNE_REASON_POLICY_FIXED;
        return 1U;
    }

    /* PRECISE and EXACT choose for the transmitter, but never at the
     * receiver's expense: a clean receive plan first, then one touching DC,
     * and only then any plan at all. */
    if (!carrier_dynamic_solution(requested_hz, policy, &candidate, 2U, 0U) &&
        !carrier_dynamic_solution(requested_hz, policy, &candidate, 1U, 0U) &&
        !carrier_dynamic_solution(requested_hz, policy, &candidate, 0U, 0U)) {
        if (carrier_solution_within_policy(&current, policy)) {
            *selected = current;
        } else {
            carrier_default_170_solution(requested_hz, selected);
        }
        carrier_retune_rejected_count++;
        carrier_retune_last_reason = carrier_solution_quality_ok(&current, policy)
                                   ? MOD_RETUNE_REASON_CLOCK_LOW
                                   : MOD_RETUNE_REASON_PERIOD_LOW;
        return 1U;
    }

    uint32_t current_abs = abs_i32_u32(current.error_hz);
    uint32_t cand_abs = abs_i32_u32(candidate.error_hz);
    uint8_t accept = 0U;
    uint8_t reject_reason = MOD_RETUNE_REASON_SMALL_GAIN;

    if (carrier_rx_quality(requested_hz, candidate.hrtim_input_hz) >
        carrier_rx_quality(requested_hz, current.hrtim_input_hz)) {
        accept = 1U;
    } else if (current_abs > 40000UL && cand_abs < current_abs) {
        accept = 1U;
    } else if (candidate.hrtim_input_hz == current.hrtim_input_hz) {
        accept = 1U;
    } else if (!carrier_solution_within_policy(&candidate, policy)) {
        accept = 0U;
        reject_reason = ((policy == MOD_CLOCK_POLICY_SAFE_DYNAMIC) &&
                         (candidate.hrtim_input_hz < MOD_HRTIM_INPUT_CLK_SAFE_MIN_HZ))
                      ? MOD_RETUNE_REASON_CLOCK_LOW
                      : MOD_RETUNE_REASON_PERIOD_LOW;
    } else if (policy == MOD_CLOCK_POLICY_PRECISION_DYNAMIC) {
        uint32_t improvement = (current_abs > cand_abs) ? (current_abs - cand_abs) : 0U;
        /* PREC remains experimental and precision-biased, but avoids
         * microscopic gains that would retune the whole system for no
         * measurable RF benefit. */
        accept = ((cand_abs < current_abs) && (improvement >= 250UL)) ? 1U : 0U;
    } else {
        uint32_t improvement = (current_abs > cand_abs) ? (current_abs - cand_abs) : 0U;
        uint8_t ratio_ok = (cand_abs < (current_abs / 4U)) ? 1U : 0U;
        uint8_t abs_ok = (improvement >= carrier_safe_min_improvement_hz(requested_hz)) ? 1U : 0U;
        accept = (ratio_ok && abs_ok) ? 1U : 0U;
    }

    if (accept) {
        *selected = candidate;
        carrier_retune_last_reason = MOD_RETUNE_REASON_NONE;
    } else {
        if (carrier_solution_within_policy(&current, policy)) {
            *selected = current;
        } else {
            carrier_default_170_solution(requested_hz, selected);
            if (reject_reason == MOD_RETUNE_REASON_SMALL_GAIN) {
                reject_reason = MOD_RETUNE_REASON_CLOCK_LOW;
            }
        }
        carrier_retune_rejected_count++;
        carrier_retune_last_reason = reject_reason;
    }
    return 1U;
}

static void apply_solution_to_geometry(const carrier_clock_solution_t *sol)
{
    uint32_t period = sol->period_ticks;
    if (period < HRTIM_MIN_PERIOD_TICKS) period = HRTIM_MIN_PERIOD_TICKS;
    custom_carrier.period_ticks      = (uint16_t)period;
    custom_carrier.half_period_ticks = (uint16_t)(period / 2U);
    custom_carrier.null_ticks        = carrier_safe_null_ticks(period);
    custom_carrier.min_compare_ticks = carrier_compare_min_ticks_for_period(period);
    custom_carrier.max_compare_ticks = carrier_compare_max_ticks_for_period(period);
    custom_carrier.frac_cycles       = (sol->frac_cycles >= 2U) ? sol->frac_cycles : 2U;
    custom_carrier.frac_extra        = (sol->frac_cycles >= 2U) ? sol->frac_extra : 0;
    current_carrier_requested_hz     = sol->requested_hz;
    current_carrier_mhz              = (float)sol->requested_hz * 0.000001f;
    active_band_idx                  = band_index_for_freq(current_carrier_mhz);
    active_carrier                   = &custom_carrier;
}

/* Whether `t` asks for another carrier geometry than the running one: the
 * period or its fraction. */
static uint8_t carrier_geometry_differs(const carrier_clock_solution_t *t)
{
    return (uint8_t)((active_carrier == NULL) ||
                     (active_carrier->period_ticks != t->period_ticks) ||
                     (active_carrier->frac_cycles != t->frac_cycles) ||
                     (active_carrier->frac_extra != t->frac_extra));
}

static HAL_StatusTypeDef carrier_switch_pll_raw(uint8_t pll_m, uint8_t pll_n, uint8_t pll_r, uint32_t new_clock_hz)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};

    rf_en_force_idle();

    o.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    o.HSIState = RCC_HSI_ON;
    o.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    o.PLL.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&o) != HAL_OK) { rf_en_force_idle(); return HAL_ERROR; }

    c.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    c.AHBCLKDivider = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV1;
    c.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_4) != HAL_OK) { rf_en_force_idle(); return HAL_ERROR; }

    memset(&o, 0, sizeof(o));
    o.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    o.HSEState = RCC_HSE_BYPASS;
    o.PLL.PLLState = RCC_PLL_ON;
    o.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM = (uint32_t)pll_m;
    o.PLL.PLLN = (uint32_t)pll_n;
    o.PLL.PLLP = RCC_PLLP_DIV2;
    o.PLL.PLLQ = RCC_PLLQ_DIV6;
    o.PLL.PLLR = (uint32_t)pll_r;
    if (HAL_RCC_OscConfig(&o) != HAL_OK) { rf_en_force_idle(); return HAL_ERROR; }

    c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_4) != HAL_OK) { rf_en_force_idle(); return HAL_ERROR; }
    __HAL_RCC_USB_CONFIG(RCC_USBCLKSOURCE_HSI48);
    SystemCoreClockUpdate();

    mod_hrtim_input_clock_hz = new_clock_hz;
    mod_tim6_input_clock_hz = mod_get_tim6_kernel_clock_hz();
    mod_tim6_scheduler_init(mod_tim6_input_clock_hz, AUDIO_RATE_HZ_U32);

    for (uint32_t attempt = 0U; attempt < MOD_HRTIM_DLL_CAL_RETRIES; attempt++) {
        if (HRTIM_DllCalibrate(&hhrtim1, MOD_HRTIM_DLL_CAL_TIMEOUT_MS) == HAL_OK) {
            RADIO_AfterRetune();
            return HAL_OK;
        }
        HAL_Delay(1U);
    }
    rf_en_force_idle();
    return HAL_ERROR;
}

static HAL_StatusTypeDef carrier_apply_sysclk_hrtim_solution(const carrier_clock_solution_t *sol)
{
    carrier_retune_fallback_failed = 0U;
    if (sol == NULL || !sol->valid) return HAL_ERROR;
    if (sol->hrtim_input_hz == MOD_GetHrtimInputClockHz()) return HAL_OK;

    rf_en_force_idle();

    (void)HAL_HRTIM_WaveformOutputStop(&hhrtim1,
        HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TE1 | HRTIM_OUTPUT_TE2);
    /* Every TX timer, Timer F (the burst-DMA trigger) too, as
     * HRTIM_ReconfigureCarrier stops them; the reconfigure after the switch
     * starts them again. */
    (void)HAL_HRTIM_WaveformCountStop(&hhrtim1,
        HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_E
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        | HRTIM_TIMERID_TIMER_F
#endif
        );

    /* Once the clock path has been stopped, force the recovery path through
     * a full HRTIM reconfiguration even if the old/new timing values match. */
    carrier_apply_valid = 0U;
    RADIO_BeforeRetune();

    HAL_StatusTypeDef st = carrier_switch_pll_raw(sol->pll_m, sol->pll_n, sol->pll_r, sol->hrtim_input_hz);
    if (st != HAL_OK) {
        HAL_StatusTypeDef fb = carrier_switch_pll_raw(CLOCK_BOOT_PLL_M, CLOCK_BOOT_PLL_N, CLOCK_BOOT_PLL_R, MOD_HRTIM_INPUT_CLK_DEFAULT_HZ);
        if (fb != HAL_OK) {
            carrier_retune_fallback_failed = 1U;
            carrier_retune_last_reason = MOD_RETUNE_REASON_FALLBACK_FAIL;
        } else {
            carrier_retune_last_reason = MOD_RETUNE_REASON_APPLY_FAIL;
            carrier_retune_last_pll_m = CLOCK_BOOT_PLL_M;
            carrier_retune_last_pll_n = CLOCK_BOOT_PLL_N;
            carrier_retune_last_pll_r = CLOCK_BOOT_PLL_R;
        }
        return HAL_ERROR;
    }

    carrier_retune_last_pll_m = sol->pll_m;
    carrier_retune_last_pll_n = sol->pll_n;
    carrier_retune_last_pll_r = sol->pll_r;
    carrier_apply_valid = 0U;
    return HAL_OK;
}

static void carrier_mark_apply_pending(uint8_t pending)
{
    carrier_frequency_apply_pending = pending ? 1U : 0U;
    if (pending) {
        rf_en_force_idle();
    }
}

uint8_t MOD_CarrierFrequencyApplyPending(void)
{
    return carrier_frequency_apply_pending ? 1U : 0U;
}

static uint8_t carrier_tx_state_ready_basic(void)
{
    if (carrier_frequency_apply_pending) return 0U;
    if (carrier_retune_fault_latched) return 0U;
    if (carrier_clock_retune_in_progress) return 0U;
    if ((carrier_retune_state == MOD_RETUNE_PENDING) ||
        (carrier_retune_state == MOD_RETUNE_WAITING_FOR_IDLE) ||
        (carrier_retune_state == MOD_RETUNE_APPLYING) ||
        (carrier_retune_state == MOD_RETUNE_FAILED) ||
        (carrier_retune_state == MOD_RETUNE_FAILED_FALLBACK)) {
        return 0U;
    }
    return 1U;
}

static uint8_t carrier_outphase_allowed_for_tx(void)
{
    if (MOD_GetOutphaseFallbackPolicy() == MOD_OUTPHASE_FALLBACK_BLOCK) {
        mod_capability_t cap = MOD_GetOutphaseCapability(active_mod_type);
        if (((active_mod_type == MOD_TYPE_SSB) || (active_mod_type == MOD_TYPE_AM)) &&
            (cap == MOD_CAPABILITY_NOT_RECOMMENDED)) {
            return 0U;
        }
    }
    return 1U;
}

void MOD_UpdateCarrierTxReadyCache(void)
{
    carrier_tx_outphase_allowed_cached = carrier_outphase_allowed_for_tx();
}

uint8_t MOD_CarrierFrequencyReadyForTxFast(void)
{
    if (carrier_tx_state_ready_basic() == 0U) return 0U;
    return carrier_tx_outphase_allowed_cached ? 1U : 0U;
}

uint8_t MOD_CarrierFrequencyReadyForTx(void)
{
    if (carrier_tx_state_ready_basic() == 0U) return 0U;
    uint8_t allowed = carrier_outphase_allowed_for_tx();
    carrier_tx_outphase_allowed_cached = allowed;
    return allowed;
}

static uint8_t carrier_timing_apply_can_run_now(void)
{
    if (carrier_clock_retune_in_progress) return 0U;
    if (carrier_retune_fault_latched) return 0U;
    if (carrier_retune_state == MOD_RETUNE_APPLYING) return 0U;
    if (carrier_retune_state == MOD_RETUNE_FAILED) return 0U;
    if (carrier_retune_state == MOD_RETUNE_FAILED_FALLBACK) return 0U;
    if ((mod_config_t)requested_config != (mod_config_t)active_config) return 0U;
    if (requested_reset) return 0U;
    if (cw_tx_live || cw_rf_enabled) return 0U;
    if (MOD_RfEnIsActiveOrHolding()) return 0U;

    uint32_t now = HAL_GetTick();
    if (MOD_AudioRealtimeCritical(now)) return 0U;

    uint32_t pm = enter_critical();
    uint32_t render_snap = render_count;
    uint32_t pcm_snap = pcm_count;
    uint8_t render_busy = (uint8_t)(render_push_in_progress | render_pop_in_progress);
    uint8_t pcm_busy = (uint8_t)(pcm_push_in_progress | pcm_pop_in_progress);
    uint8_t started = (uint8_t)transport_started;
    uint8_t usb_active = usb_stream_active;
    uint8_t dma_running = dma_streams_running;
    uint8_t internal = active_internal_source;
    uint8_t muted = cfg_muted;
    mod_config_t cfg = (mod_config_t)active_config;
    uint32_t last_usb = g_last_usb_tick;
    uint32_t last_audio = g_last_audio_tick;
    uint32_t last_usb_audio = g_last_usb_audio_tick;
    /* A held stream counts as none (mod_usb_stream_idle_held). Going by its
     * packets, a dial WSJT-X moved onto another PLL setting -- 40 m WSPR
     * from FT8 -- waited for an idle radio for as long as WSJT-X ran: the
     * receiver followed, TX stayed planned for the old dial and the panel
     * sat on RETUNE WAIT (bench, 2026-10-03). After an over the render tick
     * (last_audio) still holds the retune off. */
    if (mod_usb_stream_idle_held() != 0U) { usb_active = 0U; last_usb = 0U; last_usb_audio = 0U; }
    exit_critical(pm);

    if ((pcm_snap != 0U) || (render_snap != 0U) ||
        (pcm_busy != 0U) || (render_busy != 0U)) return 0U;
    if ((started != 0U) || (usb_active != 0U) || (dma_running != 0U)) return 0U;
    if ((last_usb != 0U) &&
        ((uint32_t)(now - last_usb) <= AUDIO_RECENT_ACTIVITY_MS)) return 0U;
    if ((last_audio != 0U) &&
        ((uint32_t)(now - last_audio) <= AUDIO_RECENT_ACTIVITY_MS)) return 0U;
    if ((last_usb_audio != 0U) &&
        ((uint32_t)(now - last_usb_audio) <= AUDIO_RECENT_ACTIVITY_MS)) return 0U;
    if ((muted == 0U) && (internal != 0U) && (cfg != MOD_CFG_TX_CARRIER_TEST) &&
        (mod_transport_cw_idle(now) == 0U)) return 0U;
    if (cfg == MOD_CFG_TX_CARRIER_TEST) {
        uint8_t dit = 0U, dah = 0U;
        if (cw_get_paddles_for_tx_test(&dit, &dah) != 0U) return 0U;
    }
    return 1U;
}

/* A PLL move needs exactly what a timing-only apply needs: the RF path and
 * both queues idle, and in TX TEST the paddle up. TX TEST used to be refused
 * outright, so a band its knob picked on another PLL setting (160 m to 10 m:
 * 164 to 170 MHz) never applied: the page sat on AUDIO BUSY with ACTUAL
 * still on the old band. */
uint8_t MOD_CarrierClockCanRetuneNow(void)
{
    return carrier_timing_apply_can_run_now();
}

void MOD_ScheduleCarrierClockRetune(uint32_t hz, uint32_t delay_ms)
{
    if (carrier_retune_fault_latched ||
        (carrier_retune_state == MOD_RETUNE_FAILED) ||
        (carrier_retune_state == MOD_RETUNE_FAILED_FALLBACK)) {
        carrier_retune_rejected_count++;
        carrier_retune_last_reason = (carrier_retune_state == MOD_RETUNE_FAILED)
                                  ? MOD_RETUNE_REASON_APPLY_FAIL
                                  : MOD_RETUNE_REASON_FALLBACK_FAIL;
        return;
    }

    carrier_retune_pending_hz = clamp_carrier_hz(hz);
    carrier_retune_due_ms = HAL_GetTick() + delay_ms;
    carrier_retune_requested_count++;
    carrier_retune_state = MOD_RETUNE_PENDING;
}

static void schedule_carrier_retune(uint32_t hz)
{
    MOD_ScheduleCarrierClockRetune(hz, MOD_DYNAMIC_RETUNE_SETTLE_MS);
}

/* Move SYSCLK/HRTIM to `target`'s PLL setting and apply its geometry: the
 * whole retune, with the fallback to 170 MHz and the fault latch if even that
 * fails. The caller has checked that the RF path is idle. */
static void carrier_switch_clock(const carrier_clock_solution_t *target)
{
    carrier_retune_state = MOD_RETUNE_APPLYING;
    carrier_clock_retune_in_progress = 1U;
    carrier_retune_last_old_clock_hz = MOD_GetHrtimInputClockHz();
    carrier_retune_last_new_clock_hz = target->hrtim_input_hz;
    carrier_retune_last_error_before_hz = MOD_GetCarrierErrorHz();
    carrier_retune_last_error_after_hz = target->error_hz;

    uint8_t streams_were_running = dma_streams_running;
    rf_en_force_idle();
    if (streams_were_running) mod_dma_stop_streams();

    if (carrier_apply_sysclk_hrtim_solution(target) == HAL_OK) {
        apply_solution_to_geometry(target);
        pa_resolve_active_band_profile(1U, 1U, 0U);
        MOD_FillAllNullBuffers();
        carrier_mark_apply_pending(0U);
        if (streams_were_running && !dma_streams_running && !requested_reset) {
            mod_dma_start_streams();
        }
        carrier_dynamic_clear_quarantine();
        carrier_retune_applied_count++;
        carrier_retune_last_status = 1U;
        if (!carrier_retune_fault_latched) carrier_retune_state = MOD_RETUNE_IDLE;
    } else {
        carrier_retune_failed_count++;
        carrier_retune_last_status = 0U;
        rf_en_force_idle();
        if (carrier_retune_fallback_failed) {
            carrier_retune_fault_latched = 1U;
            carrier_frequency_apply_pending = 1U;
            carrier_retune_state = MOD_RETUNE_FAILED_FALLBACK;
            carrier_retune_last_reason = MOD_RETUNE_REASON_FALLBACK_FAIL;
        } else {
            carrier_dynamic_quarantine_solution(target, MOD_GetCarrierClockPolicy());
            carrier_retune_state = MOD_RETUNE_FAILED_RECOVERED;
            carrier_apply_valid = 0U;
            mod_recompute_carrier_for_current_clock();
            pa_resolve_active_band_profile(1U, 1U, 0U);
            MOD_FillAllNullBuffers();
            carrier_mark_apply_pending(0U);
            if (streams_were_running && !dma_streams_running && !requested_reset) {
                mod_dma_start_streams();
            }
        }
    }
    carrier_clock_retune_in_progress = 0U;
}

/* AUTO treats a better PLL setting as an upgrade whenever the running clock
 * can stand in for it: a legal carrier period now -- the TX NCO makes the
 * frequency exact on any grid -- and some receive plan. The frequency is then
 * applied on the running clock at once, so TX is not held and the receiver
 * keeps playing, and the PLL moves later, once the radio has been idle for the
 * settle time (carrier_service_upgrade). Returns 1 with `here` filled in. */
static uint8_t carrier_auto_defer(uint32_t hz, carrier_clock_solution_t *here)
{
    if (MOD_GetCarrierClockPolicy() != MOD_CLOCK_POLICY_AUTO) return 0U;
    carrier_solve_current(hz, here);
    if (!here->valid || here->period_clamped) return 0U;
    if (carrier_rx_quality(hz, here->hrtim_input_hz) == 0U) return 0U;
    /* A running clock AUTO may no longer use (WIDE turned off, or a mode or
     * phase rate the transmitter cannot carry there) is not kept: the move
     * is scheduled now, and TX waits for it. */
    if (!carrier_auto_clock_ok(here->hrtim_input_hz)) return 0U;
    carrier_upgrade_pending = 1U;
    carrier_upgrade_due_ms = HAL_GetTick() + MOD_AUTO_UPGRADE_IDLE_MS;
    return 1U;
}

/* Take a deferred AUTO upgrade: only while receiving (or setting up) with the
 * whole TX path idle, after the settle time since the last tuning step or
 * transmission -- so the way back from a key-down hop waits for a pause in
 * the QSO, not for the end of each over -- and only if the planner still
 * wants a different clock for where the dial is now.
 * The retune itself costs the receiver about 10 ms; in DIGI it waits for the
 * FT8 slot's gap (RADIO_DigiQuiet), where it costs no signal -- the capture
 * stream WSJT-X records is not USB audio to the gate below. */
static void carrier_service_upgrade(void)
{
    if (!carrier_upgrade_pending) return;
    if (MOD_GetCarrierClockPolicy() != MOD_CLOCK_POLICY_AUTO) { carrier_upgrade_pending = 0U; return; }
    if (carrier_upgrade_hold || tx_diag_preset) return;
    const radio_state_t rs = RADIO_State();
    if ((rs == RADIO_TX_PREPARE) || (rs == RADIO_TRANSMIT)) {
        carrier_upgrade_due_ms = HAL_GetTick() + MOD_AUTO_UPGRADE_IDLE_MS;
        return;
    }
    if ((int32_t)(HAL_GetTick() - carrier_upgrade_due_ms) < 0) return;
    if (!RADIO_DigiQuiet(HAL_GetTick())) return;
    if (((carrier_retune_state != MOD_RETUNE_IDLE) && (carrier_retune_state != MOD_RETUNE_FAILED_RECOVERED)) ||
        carrier_frequency_apply_pending || carrier_retune_fault_latched) return;
    if ((rs != RADIO_RECEIVE) && (rs != RADIO_SETUP)) return;
    if (!MOD_CarrierClockCanRetuneNow()) return;
    carrier_upgrade_pending = 0U;
    carrier_clock_solution_t target;
    if (!carrier_select_solution(current_carrier_requested_hz, &target) || !target.valid) return;
    if (target.hrtim_input_hz == MOD_GetHrtimInputClockHz()) return;
    carrier_switch_clock(&target);
}

/* The clock CLOCK_ChooseTx would transmit on instead of the running one,
 * in `target`; 0 if none is worth the hop. Apart from MOD_TxClockHop so its
 * evaluations are off the stack before the switch. */
static uint8_t carrier_tx_hop_target(carrier_clock_solution_t *target)
{
    clock_eval_t ev[CLOCK_PLL_CANDIDATES_MAX];
    int current;
    const uint32_t hz = current_carrier_requested_hz;
    const unsigned n = carrier_auto_evaluate(hz, ev, &current);
    const int pick = CLOCK_ChooseTx(ev, n, current);
    if ((pick < 0) || (pick == current)) return 0U;
    carrier_solve_for_pll(hz, ev[pick].pll.m, ev[pick].pll.n, ev[pick].pll.r, target);
    return (uint8_t)(target->valid && !target->period_clamped);
}

uint8_t MOD_TxClockHop(void)
{
    if (MOD_GetCarrierClockPolicy() != MOD_CLOCK_POLICY_AUTO) return 0U;
    if (carrier_upgrade_hold || tx_diag_preset) return 0U;
    if (((carrier_retune_state != MOD_RETUNE_IDLE) && (carrier_retune_state != MOD_RETUNE_FAILED_RECOVERED)) ||
        carrier_frequency_apply_pending || carrier_retune_fault_latched || carrier_clock_retune_in_progress ||
        carrier_replan_requested) return 0U;
    const radio_state_t rs = RADIO_State();
    if ((rs != RADIO_RECEIVE) && (rs != RADIO_SETUP)) return 0U;
    if (RADIO_CalActive() || dma_streams_running || cw_rf_enabled || MOD_RfEnIsActiveOrHolding()) return 0U;
    carrier_clock_solution_t target;
    if (!carrier_tx_hop_target(&target)) return 0U;
    if (target.hrtim_input_hz == MOD_GetHrtimInputClockHz()) return 0U;
    carrier_retune_last_reason = MOD_RETUNE_REASON_TX_HOP;
    carrier_switch_clock(&target);
    MOD_UpdateCarrierTxReadyCache();
    if (carrier_retune_last_status) carrier_tx_hop_count++;
    /* Back to AUTO's receive clock once idle; a same-class one is kept by
     * CLOCK_ChooseAuto itself. */
    carrier_upgrade_pending = 1U;
    carrier_upgrade_due_ms = HAL_GetTick() + MOD_AUTO_UPGRADE_IDLE_MS;
    return 1U;
}

/* Re-plan when AUTO's inputs other than the dial have changed (a mode,
 * sideband, receive mode or phase-rate change), once the radio is receiving
 * with the TX path idle, so the re-plan applies at once instead of holding a
 * transmission. */
static void carrier_auto_follow_inputs(void)
{
    if (MOD_GetCarrierClockPolicy() != MOD_CLOCK_POLICY_AUTO) return;
    const uint32_t inputs = carrier_auto_inputs();
    if (inputs == carrier_auto_planned_inputs) return;
    if ((carrier_retune_state != MOD_RETUNE_IDLE) && (carrier_retune_state != MOD_RETUNE_FAILED_RECOVERED)) return;
    if (carrier_frequency_apply_pending || carrier_retune_fault_latched || carrier_replan_requested) return;
    const radio_state_t rs = RADIO_State();
    if ((rs != RADIO_RECEIVE) && (rs != RADIO_SETUP)) return;
    if (!MOD_CarrierClockCanRetuneNow()) return;
    if (!RADIO_DigiQuiet(HAL_GetTick())) return;  /* as the upgrade: in the slot gap */
    /* Re-plan only for a real change: another PLL setting, or another
     * carrier period or fraction (the SSB centre moves the grid, the mode
     * the master's length). Re-applying the same
     * geometry would still reset the DSP and the render queue, and at an
     * unexpected moment -- this runs up to a few seconds after the mode
     * change, once USB audio has been quiet long enough. */
    carrier_clock_solution_t target;
    const uint32_t hz = current_carrier_requested_hz;
    carrier_select_solution(hz, &target);   /* records the inputs it planned for */
    carrier_auto_planned_inputs = inputs;
    if (!target.valid) return;
    if ((target.hrtim_input_hz == MOD_GetHrtimInputClockHz()) &&
        !carrier_geometry_differs(&target)) return;
    carrier_replan_requested = 1U;
}

static void tx_diag_service(void);

void MOD_ServiceCarrierClockRetune(void)
{
    CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_RETUNE);
    carrier_auto_follow_inputs();
    tx_diag_service();
    if (carrier_replan_requested) {
        carrier_replan_requested = 0U;
        MOD_SetCarrierFreqHz(MOD_GetCarrierRequestedHz());
    }
    if (carrier_retune_state != MOD_RETUNE_PENDING && carrier_retune_state != MOD_RETUNE_WAITING_FOR_IDLE) {
        carrier_service_upgrade();
        goto done;
    }
    if ((int32_t)(HAL_GetTick() - carrier_retune_due_ms) < 0) goto done;

    /* The dynamic PLL search is cold-path work, but it is still large enough to
     * avoid while the audio/RF queues are active. Cheap readiness checks first
     * keep realtime frames ahead of experimental precision-clock bookkeeping. */
    uint8_t timing_can_run = carrier_timing_apply_can_run_now();
    uint8_t clock_can_run = MOD_CarrierClockCanRetuneNow();
    if ((timing_can_run == 0U) && (clock_can_run == 0U)) {
        carrier_retune_state = MOD_RETUNE_WAITING_FOR_IDLE;
        carrier_retune_last_reason = MOD_RETUNE_REASON_WAIT_IDLE;
        goto done;
    }

    carrier_clock_solution_t target;
    const uint32_t retune_hz = carrier_retune_pending_hz ? carrier_retune_pending_hz : current_carrier_requested_hz;
    if (!carrier_select_solution(retune_hz, &target)) goto done;
    if (target.hrtim_input_hz != MOD_GetHrtimInputClockHz()) {
        carrier_clock_solution_t here;
        if (carrier_auto_defer(retune_hz, &here)) target = here;
    }
    if (target.hrtim_input_hz == MOD_GetHrtimInputClockHz()) {
        if (carrier_frequency_apply_pending) {
            if ((timing_can_run == 0U) || (carrier_timing_apply_can_run_now() == 0U)) {
                carrier_retune_state = MOD_RETUNE_WAITING_FOR_IDLE;
                carrier_retune_last_reason = MOD_RETUNE_REASON_WAIT_IDLE;
                goto done;
            }
            apply_solution_to_geometry(&target);
            pa_resolve_active_band_profile(1U, 1U, 0U);
            MOD_FillAllNullBuffers();
            carrier_mark_apply_pending(0U);
            carrier_retune_applied_count++;
            carrier_retune_last_status = 1U;
        }
        if (!carrier_retune_fault_latched &&
            (carrier_retune_state != MOD_RETUNE_FAILED_FALLBACK)) {
            carrier_retune_state = MOD_RETUNE_IDLE;
        }
        goto done;
    }

    if ((clock_can_run == 0U) || !MOD_CarrierClockCanRetuneNow()) {
        carrier_retune_state = MOD_RETUNE_WAITING_FOR_IDLE;
        carrier_retune_last_reason = MOD_RETUNE_REASON_WAIT_IDLE;
        goto done;
    }

    carrier_switch_clock(&target);

done:
    CPU_DIAG_END(MOD_CPU_DIAG_RETUNE, cpu_diag_start);
}

void mod_recompute_carrier_for_current_clock(void)
{
    carrier_clock_solution_t sol;
    carrier_solve_current(current_carrier_requested_hz, &sol);
    if (sol.valid) apply_solution_to_geometry(&sol);
}

void update_custom_carrier_geometry_hz(uint32_t hz)
{
    carrier_clock_solution_t sol;
    hz = clamp_carrier_hz(hz);
    carrier_solve_current(hz, &sol);
    if (sol.valid) apply_solution_to_geometry(&sol);
}

/* MHz as a float to whole Hz without double arithmetic. For mhz >= 1 the
 * whole megahertz split off exactly, and the fraction keeps its full
 * mantissa through the multiply, so this lands on the hertz the double
 * expression it replaces did. */
static uint32_t mhz_to_hz(float mhz)
{
    const uint32_t whole = (uint32_t)mhz;
    return whole * 1000000U + (uint32_t)((mhz - (float)whole) * 1000000.0f + 0.5f);
}

void update_custom_carrier_geometry(float mhz)
{
    if (!isfinite(mhz)) mhz = 14.02f;
    if (mhz < 1.0f) mhz = 1.0f;
    if (mhz > 28.333333f) mhz = 28.333333f;
    update_custom_carrier_geometry_hz(mhz_to_hz(mhz));
}

void MOD_SetCarrierFreqHz(uint32_t hz)
{
    hz = clamp_carrier_hz(hz);
    if (carrier_retune_fault_latched) {
        carrier_retune_rejected_count++;
        carrier_retune_last_reason = MOD_RETUNE_REASON_FALLBACK_FAIL;
        carrier_mark_apply_pending(1U);
        return;
    }
    if (((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) ||
        ((mod_config_t)requested_config == MOD_CFG_TX_CARRIER_TEST)) {
        MOD_TxCarrierTestStop();
    }

    carrier_clock_solution_t target;
    carrier_select_solution(hz, &target);
    if (target.valid && target.hrtim_input_hz != MOD_GetHrtimInputClockHz()) {
        carrier_clock_solution_t here;
        if (carrier_auto_defer(hz, &here)) target = here;
    }

    if (target.valid && target.hrtim_input_hz != MOD_GetHrtimInputClockHz()) {
        current_carrier_requested_hz = hz;
        current_carrier_mhz = (float)hz * 0.000001f;
        carrier_retune_last_reason = MOD_RETUNE_REASON_DEFERRED_CLOCK;
        carrier_mark_apply_pending(1U);
        if (!MOD_CarrierClockCanRetuneNow()) carrier_retune_deferred_count++;
        schedule_carrier_retune(hz);
        return;
    }

    carrier_mark_apply_pending(1U);
    current_carrier_requested_hz = hz;
    current_carrier_mhz = (float)hz * 0.000001f;
    if (carrier_timing_apply_can_run_now() == 0U) {
        carrier_retune_last_reason = MOD_RETUNE_REASON_WAIT_IDLE;
        carrier_retune_deferred_count++;
        MOD_ScheduleCarrierClockRetune(hz, MOD_DYNAMIC_RETUNE_SETTLE_MS);
        return;
    }
    apply_solution_to_geometry(&target);
    pa_resolve_active_band_profile((uint8_t)(active_band_idx != cal_loaded_band), 1U, 1U);
    MOD_FillAllNullBuffers();
    carrier_mark_apply_pending(0U);
    carrier_retune_applied_count++;
    carrier_retune_last_status = 1U;
    carrier_retune_last_reason = MOD_RETUNE_REASON_NONE;
    if (!carrier_retune_fault_latched &&
        (carrier_retune_state != MOD_RETUNE_FAILED_FALLBACK)) {
        carrier_retune_state = MOD_RETUNE_IDLE;
    }
}

/* Re-plan the carrier for the dial it is on. REF CAL moves every frequency
 * the clock tree makes, so the period, the NCO residual and possibly the
 * best PLL setting change with it. Only a request: the configuration and
 * console paths that change the reference must not reach the carrier apply
 * (and through it the transmit path) themselves; MOD_ServiceCarrierClockRetune
 * does it from the main loop. */
void MOD_CarrierReplan(void)
{
    carrier_replan_requested = 1U;
}

void MOD_SetCarrierFreqMHz(float mhz)
{
    if (!isfinite(mhz)) mhz = 14.02f;
    if (mhz <  1.0f) mhz =  1.0f;
    if (mhz > 28.333333f) mhz = 28.333333f;
    MOD_SetCarrierFreqHz(mhz_to_hz(mhz));
}

float MOD_GetCarrierFreqMHz(void) { return (float)MOD_GetCarrierRequestedHz() * 0.000001f; }

uint32_t MOD_GetCarrierRequestedHz(void)
{
    return current_carrier_requested_hz ? current_carrier_requested_hz : 14020000UL;
}

/* SYSCLK as the radio really runs it: the PLL setting in use, from the
 * reference corrected by REF CAL (clock_plan.c). Exact to a micro-hertz,
 * unlike SystemCoreClock, which CMSIS computes from the nominal HSE_VALUE. */
uint64_t MOD_SysclkMicroHz(void)
{
    const clock_pll_t pll = {
        carrier_retune_last_pll_m ? (uint8_t)carrier_retune_last_pll_m : CLOCK_BOOT_PLL_M,
        carrier_retune_last_pll_n ? (uint8_t)carrier_retune_last_pll_n : CLOCK_BOOT_PLL_N,
        carrier_retune_last_pll_r ? (uint8_t)carrier_retune_last_pll_r : CLOCK_BOOT_PLL_R,
    };
    return CLOCK_PllMicroHz(pll, carrier_ref_ppb());
}

/* The running carrier as clock_plan.h describes it: period and fraction. */
static clock_carrier_t active_carrier_plan(void)
{
    clock_carrier_t c = {0U, 2U, 0};
    if (active_carrier != NULL) {
        c.period = active_carrier->period_ticks;
        c.cycles = active_carrier->frac_cycles;
        c.extra = active_carrier->frac_extra;
    }
    return c;
}

/* The carrier grid frequency the HRTIM makes (period and fraction), before
 * the TX NCO. */
uint32_t MOD_GetActualCarrierFreqHz(void)
{
    const clock_carrier_t c = active_carrier_plan();
    if (c.period == 0U) return 0U;
    return (uint32_t)((CLOCK_CarrierMicroHz(MOD_SysclkMicroHz(), &c) + 500000ULL) / 1000000ULL);
}

/* Grid minus dial in milli-hertz: exactly what the TX NCO rotates out. */
int64_t MOD_GetCarrierErrorMilliHz(void)
{
    const clock_carrier_t c = active_carrier_plan();
    if (c.period == 0U) return 0;
    return CLOCK_CarrierErrorMilliHz(MOD_SysclkMicroHz(), &c, MOD_GetCarrierRequestedHz());
}

uint8_t MOD_GetCarrierFraction(int8_t *extra)
{
    const clock_carrier_t c = active_carrier_plan();
    if (extra != NULL) *extra = c.extra;
    return c.cycles;
}

int32_t MOD_GetCarrierErrorHz(void)
{
    return round_milli(MOD_GetCarrierErrorMilliHz());
}

/* Level of the strongest image the carrier grid leaves: the TX NCO's at
 * +-(phase-update rate), for the grid residual measured from the transmitted
 * spectrum's centre, or the fraction's nudge if that is higher. */
int32_t MOD_GetCarrierImageDbc(void)
{
    const clock_carrier_t c = active_carrier_plan();
    if (c.period == 0U) return 0;
    const uint32_t rate = MOD_TxPhaseUpdateHz();
    const int64_t centre = (int64_t)MOD_GetCarrierRequestedHz() + carrier_spectral_centre_hz();
    if (centre <= 0) return 0;
    return CLOCK_ImageDbc(CLOCK_CarrierCostHz(MOD_SysclkMicroHz(), &c, (uint32_t)centre, rate), rate);
}

float MOD_GetActualCarrierFreqMHz(void)
{
    return (float)MOD_GetActualCarrierFreqHz() * 0.000001f;
}

uint16_t MOD_GetCarrierPeriodTicks(void)
{
    return (active_carrier != NULL) ? active_carrier->period_ticks : 0U;
}

uint16_t MOD_GetCarrierHalfPeriodTicks(void)
{
    return (active_carrier != NULL) ? active_carrier->half_period_ticks : 0U;
}

uint8_t MOD_GetActiveBandIndex(void)
{
    return (active_band_idx < BAND_COUNT) ? active_band_idx : 0U;
}

void MOD_SetCarrierClockPolicy(mod_clock_policy_t policy)
{
    if ((uint32_t)policy >= (uint32_t)MOD_CLOCK_POLICY_COUNT) policy = MOD_CLOCK_POLICY_FIXED_170;
    if (policy != cfg_carrier_clock_policy) {
        carrier_dynamic_clear_quarantine();
    }
    if (carrier_retune_fault_latched) {
        carrier_retune_rejected_count++;
        carrier_retune_last_reason = MOD_RETUNE_REASON_FALLBACK_FAIL;
        carrier_mark_apply_pending(1U);
        return;
    }

    cfg_carrier_clock_policy = policy;
    cfg_precision_clock_arm = (policy == MOD_CLOCK_POLICY_PRECISION_DYNAMIC) ? 1U : 0U;

    carrier_clock_solution_t target;
    if (!carrier_select_solution(MOD_GetCarrierRequestedHz(), &target) || !target.valid) {
        carrier_mark_apply_pending(1U);
        return;
    }
    if (target.hrtim_input_hz != MOD_GetHrtimInputClockHz()) {
        carrier_clock_solution_t here;
        if (carrier_auto_defer(MOD_GetCarrierRequestedHz(), &here)) target = here;
    }

    if (target.hrtim_input_hz == MOD_GetHrtimInputClockHz()) {
        if (carrier_geometry_differs(&target) || carrier_frequency_apply_pending) {
            carrier_mark_apply_pending(1U);
            if (carrier_timing_apply_can_run_now() == 0U) {
                carrier_retune_last_reason = MOD_RETUNE_REASON_WAIT_IDLE;
                carrier_retune_deferred_count++;
                MOD_ScheduleCarrierClockRetune(MOD_GetCarrierRequestedHz(), MOD_DYNAMIC_RETUNE_SETTLE_MS);
                return;
            }
            apply_solution_to_geometry(&target);
            pa_resolve_active_band_profile((uint8_t)(active_band_idx != cal_loaded_band), 1U, 1U);
            MOD_FillAllNullBuffers();
        }
        carrier_mark_apply_pending(0U);
        carrier_retune_last_reason = MOD_RETUNE_REASON_NONE;
        if (!carrier_retune_fault_latched &&
            (carrier_retune_state != MOD_RETUNE_FAILED_FALLBACK)) {
            carrier_retune_state = MOD_RETUNE_IDLE;
        }
        return;
    }

    carrier_mark_apply_pending(1U);
    MOD_ScheduleCarrierClockRetune(MOD_GetCarrierRequestedHz(), MOD_DYNAMIC_RETUNE_SETTLE_MS);
}

mod_clock_policy_t MOD_GetCarrierClockPolicy(void)

{
    if ((uint32_t)cfg_carrier_clock_policy >= (uint32_t)MOD_CLOCK_POLICY_COUNT) {
        return MOD_CLOCK_POLICY_FIXED_170;
    }
    if ((cfg_carrier_clock_policy == MOD_CLOCK_POLICY_PRECISION_DYNAMIC) &&
        (cfg_precision_clock_arm == 0U)) {
        return MOD_CLOCK_POLICY_FIXED_170;
    }
    return cfg_carrier_clock_policy;
}

const char *MOD_GetCarrierClockPolicyLabel(mod_clock_policy_t policy)
{
    switch (policy) {
        case MOD_CLOCK_POLICY_AUTO:              return "AUTO";
        case MOD_CLOCK_POLICY_SAFE_DYNAMIC:      return "PRECISE";
        case MOD_CLOCK_POLICY_PRECISION_DYNAMIC: return "EXACT";
        case MOD_CLOCK_POLICY_FIXED_170:
        default:                                 return "SAFE";
    }
}

mod_retune_state_t MOD_GetCarrierRetuneState(void)
{
    return carrier_retune_state;
}

/* The clock tree itself is in question: a PLL switch under way, or one that
 * failed and could not even fall back. Only then must the receiver stop; a
 * frequency or clock change that is merely pending leaves it running on the
 * current clock, which it plans its own LO for. */
uint8_t MOD_CarrierClockUnsettled(void)
{
    return (uint8_t)(carrier_retune_fault_latched || carrier_clock_retune_in_progress ||
                     (carrier_retune_state == MOD_RETUNE_APPLYING) ||
                     (carrier_retune_state == MOD_RETUNE_FAILED) ||
                     (carrier_retune_state == MOD_RETUNE_FAILED_FALLBACK));
}

uint8_t MOD_CarrierClockUpgradePending(void)
{
    return carrier_upgrade_pending;
}

void MOD_CarrierUpgradeHold(uint8_t hold)
{
    carrier_upgrade_hold = hold ? 1U : 0U;
}

/* TX TEST's grid presets, a bench aid for the gate waveform on 20, 17, 15,
 * 12 and 10 m, three per band around the band's preset dial, on the running
 * clock. GRID puts the dial exactly on a whole carrier period: no fraction,
 * and the TX NCO steps for the reference trim alone -- an odd period, whose
 * master of three moves the roll-over through the whole fHRTIM cycle, inside
 * the band. HALF puts it just short of halfway from the period over the
 * band's dial to the next, where a whole period alone left the NCO its
 * largest step at every update (the smeared gate of v8.11) and the fraction
 * is now 1/2: k = +1, as k = -1 smeared on the first fractional build. WORST
 * is the dial in that step the carrier plan serves worst: the largest image
 * cost it can be left with, NCO steps of up to that many ticks. */
static const uint8_t tx_diag_band[] = {13U, 15U, 16U, 18U, 21U}; /* 20M 17M 15M 12M 10M FT8 */
static const uint16_t tx_diag_edge_khz[][2] = {
    {14000U, 14350U}, {18068U, 18168U}, {21000U, 21450U}, {24890U, 24990U}, {28000U, 29700U}};
_Static_assert(sizeof(tx_diag_edge_khz) / sizeof(tx_diag_edge_khz[0]) == sizeof(tx_diag_band),
               "band edges per diagnostic band");
static const char *const tx_diag_label[] = {
    "20M GRID", "20M HALF", "20M WORST", "17M GRID", "17M HALF", "17M WORST",
    "15M GRID", "15M HALF", "15M WORST", "12M GRID", "12M HALF", "12M WORST",
    "10M GRID", "10M HALF", "10M WORST"
};
_Static_assert(sizeof(tx_diag_label) / sizeof(tx_diag_label[0]) == 3U * sizeof(tx_diag_band),
               "a GRID, a HALF and a WORST label per band");

uint8_t MOD_TxTestDiagPresetCount(void)
{
    return (uint8_t)(3U * sizeof(tx_diag_band));
}

static uint32_t tx_diag_period_hz(uint64_t sysclk, uint32_t period)
{
    return (uint32_t)((CLOCK_PeriodMicroHz(sysclk, period) + 500000ULL) / 1000000ULL);
}

/* The dial in lo..hi with the costliest carrier plan, on a 256-point search
 * (the cost is piecewise linear in the dial, so this lands within a point of
 * the true worst). Some 30 ms of planning, once per preset. */
static uint32_t tx_diag_worst_hz(uint64_t sysclk, uint32_t lo, uint32_t hi)
{
    uint32_t worst = lo, worst_cost = 0U;
    for (uint32_t j = 1U; j < 256U; ++j) {
        const uint32_t f = lo + (uint32_t)((uint64_t)(hi - lo) * j / 256U);
        clock_tx_shape_t shape;
        carrier_tx_shape(f, &shape);
        clock_carrier_t c;
        if (!CLOCK_CarrierPlan(sysclk, f, &shape, &c)) continue;
        const uint32_t cost = CLOCK_CarrierCostHz(sysclk, &c, f, shape.update_hz);
        if (cost > worst_cost) {
            worst_cost = cost;
            worst = f;
        }
    }
    return worst;
}

/* GRID's dial: the odd whole period nearest period `p` that lands in band
 * `b`, within four periods; else `p`'s own. The receiver is left at DC there
 * whatever the clock: its LO is on the same grid. */
static uint32_t tx_diag_grid_hz(uint64_t sysclk, uint16_t p, uint8_t b)
{
    for (uint32_t i = 0U; i <= 8U; ++i) {
        const uint32_t q = (i & 1U) ? (uint32_t)p + (i + 1U) / 2U : (uint32_t)p - i / 2U;
        if (!(q & 1U) || (q < CLOCK_MIN_PERIOD_TICKS) || (q > 65535U)) continue;
        const uint32_t f = tx_diag_period_hz(sysclk, q);
        if ((f >= tx_diag_edge_khz[b][0] * 1000U) && (f <= tx_diag_edge_khz[b][1] * 1000U)) return f;
    }
    return tx_diag_period_hz(sysclk, p);
}

static uint32_t tx_diag_dial_hz(uint8_t preset)
{
    const uint8_t i = (uint8_t)(preset - 1U);
    const uint32_t f0 = (uint32_t)(band_presets[tx_diag_band[i / 3U]].mhz * 1000.0f + 0.5f) * 1000U;
    const uint64_t sysclk = MOD_SysclkMicroHz();
    const uint16_t p = CLOCK_CarrierPeriod(sysclk, f0);
    if (p <= CLOCK_MIN_PERIOD_TICKS) return f0;
    /* The step from period q + 1 up to q that holds f0. */
    const uint16_t q = (tx_diag_period_hz(sysclk, p) > f0) ? p : (uint16_t)(p - 1U);
    switch (i % 3U) {
    case 0U:
        return tx_diag_grid_hz(sysclk, p, (uint8_t)(i / 3U));
    case 1U:
        /* The planner rounds sysclk x 32 / f to a period, so q and q + 1 meet
         * at sysclk x 32 / (q + 1/2); 200 Hz below that, q + 1 is still the
         * nearest. */
        return (uint32_t)((sysclk * 64ULL / (2ULL * q + 1ULL)) / 1000000ULL) - 200U;
    default:
        return tx_diag_worst_hz(sysclk, tx_diag_period_hz(sysclk, q + 1U), tx_diag_period_hz(sysclk, q));
    }
}

static void tx_diag_apply(void)
{
    tx_diag_clock_10khz = (uint16_t)(MOD_GetHrtimInputClockHz() / 10000UL);
    MOD_SetCarrierFreqHz(tx_diag_dial_hz(tx_diag_preset));
}

void MOD_TxTestDiagPreset(uint8_t preset)
{
    if (preset > MOD_TxTestDiagPresetCount()) preset = 0U;
    tx_diag_preset = preset;
    tx_diag_replanned = 0U;
    if (preset) tx_diag_apply();
}

uint8_t MOD_TxTestDiagPresetActive(void)
{
    return tx_diag_preset;
}

const char *MOD_TxTestDiagLabel(void)
{
    return tx_diag_preset ? tx_diag_label[tx_diag_preset - 1U] : NULL;
}

/* A dial the running clock could not receive on moved the PLL at once: plan
 * the preset again, once, on the clock it landed on. */
static void tx_diag_service(void)
{
    if (!tx_diag_preset || tx_diag_replanned) return;
    if (carrier_frequency_apply_pending || carrier_clock_retune_in_progress ||
        (carrier_retune_state != MOD_RETUNE_IDLE)) return;
    if ((uint16_t)(MOD_GetHrtimInputClockHz() / 10000UL) == tx_diag_clock_10khz) return;
    tx_diag_replanned = 1U;
    tx_diag_apply();
}

void MOD_GetClockSnapshot(mod_clock_snapshot_t *s)
{
    s->policy = (uint8_t)MOD_GetCarrierClockPolicy();
    s->pll_m = (uint8_t)(carrier_retune_last_pll_m ? carrier_retune_last_pll_m : CLOCK_BOOT_PLL_M);
    s->pll_n = (uint8_t)(carrier_retune_last_pll_n ? carrier_retune_last_pll_n : CLOCK_BOOT_PLL_N);
    s->pll_r = (uint8_t)(carrier_retune_last_pll_r ? carrier_retune_last_pll_r : CLOCK_BOOT_PLL_R);
    s->retune_state = (uint8_t)carrier_retune_state;
    s->last_reason = carrier_retune_last_reason;
    s->upgrade_pending = carrier_upgrade_pending;
    s->apply_pending = carrier_frequency_apply_pending;
    s->fault_latched = carrier_retune_fault_latched;
    s->tx_timers_parked = MOD_TxTimersParked();
    s->period_ticks = MOD_GetCarrierPeriodTicks();
    s->applied = carrier_retune_applied_count;
    s->failed = carrier_retune_failed_count;
    s->deferred = carrier_retune_deferred_count;
    s->rejected = carrier_retune_rejected_count;
    s->tx_hops = carrier_tx_hop_count;
}

void MOD_ValidateCarrierClockForCurrentModeEx(uint8_t already_in_apply_config)
{
    if (carrier_retune_fault_latched) {
        carrier_mark_apply_pending(1U);
        return;
    }

    carrier_clock_solution_t target;
    if (!carrier_select_solution(MOD_GetCarrierRequestedHz(), &target) || !target.valid) {
        return;
    }
    if (target.hrtim_input_hz != MOD_GetHrtimInputClockHz()) {
        carrier_clock_solution_t here;
        if (carrier_auto_defer(MOD_GetCarrierRequestedHz(), &here)) target = here;
    }

    if (target.hrtim_input_hz != MOD_GetHrtimInputClockHz()) {
        carrier_mark_apply_pending(1U);
        MOD_ScheduleCarrierClockRetune(MOD_GetCarrierRequestedHz(), MOD_DYNAMIC_RETUNE_SETTLE_MS);
        return;
    }

    if (carrier_frequency_apply_pending || carrier_geometry_differs(&target)) {
        carrier_mark_apply_pending(1U);
        if ((already_in_apply_config == 0U) &&
            (carrier_timing_apply_can_run_now() == 0U)) {
            carrier_retune_last_reason = MOD_RETUNE_REASON_WAIT_IDLE;
            carrier_retune_deferred_count++;
            MOD_ScheduleCarrierClockRetune(MOD_GetCarrierRequestedHz(), MOD_DYNAMIC_RETUNE_SETTLE_MS);
            return;
        }
        apply_solution_to_geometry(&target);
        pa_resolve_active_band_profile(1U, 1U,
                                       (uint8_t)(already_in_apply_config ? 0U : 1U));
        MOD_FillAllNullBuffers();
        carrier_mark_apply_pending(0U);
    }

    if (!carrier_retune_fault_latched &&
        (carrier_retune_state != MOD_RETUNE_FAILED_FALLBACK) &&
        ((carrier_retune_state == MOD_RETUNE_PENDING) ||
         (carrier_retune_state == MOD_RETUNE_WAITING_FOR_IDLE))) {
        carrier_retune_state = MOD_RETUNE_IDLE;
    }
}

void MOD_ValidateCarrierClockForCurrentMode(void)
{
    MOD_ValidateCarrierClockForCurrentModeEx(0U);
}

/* Presets are whole kilohertz, as UI_BandPresetHz takes them. Through a float
 * MHz they are not -- 21.3f is 21.2999992 -- so the TX TEST page landed 15 m,
 * 12 m and two 10 m presets a hertz off. */
void MOD_SelectBand(uint8_t idx)
{
    if (idx < BAND_COUNT) {
        MOD_SetCarrierFreqHz((uint32_t)(band_presets[idx].mhz * 1000.0f + 0.5f) * 1000U);
    }
}

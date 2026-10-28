#include "mod_transport_logic.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

#define PS_PER_SECOND_LOGIC 1000000000000ULL
#define DEADTIME_MAX_TICKS_LOGIC 511ULL

uint16_t mod_deadtime_ticks_from_ps_at_clock(uint32_t ps, uint32_t hrtim_input_hz)
{
    if ((ps == 0U) || (hrtim_input_hz == 0U)) return 0U;
    uint64_t fine_clk = (uint64_t)hrtim_input_hz * MOD_DEADTIME_FINE_MULTIPLIER;
    if (fine_clk > (UINT64_MAX - PS_PER_SECOND_LOGIC / 2ULL) / ps) {
        return (uint16_t)DEADTIME_MAX_TICKS_LOGIC;
    }
    uint64_t ticks = ((uint64_t)ps * fine_clk + PS_PER_SECOND_LOGIC / 2ULL) /
                     PS_PER_SECOND_LOGIC;
    if (ticks == 0ULL) ticks = 1ULL;
    if (ticks > DEADTIME_MAX_TICKS_LOGIC) ticks = DEADTIME_MAX_TICKS_LOGIC;
    return (uint16_t)ticks;
}

uint32_t mod_deadtime_ps_from_ticks_at_clock(uint16_t ticks, uint32_t hrtim_input_hz)
{
    if ((ticks == 0U) || (hrtim_input_hz == 0U)) return 0U;
    uint64_t fine_clk = (uint64_t)hrtim_input_hz * MOD_DEADTIME_FINE_MULTIPLIER;
    uint64_t ps = ((uint64_t)ticks * PS_PER_SECOND_LOGIC + fine_clk / 2ULL) / fine_clk;
    return (ps > UINT32_MAX) ? UINT32_MAX : (uint32_t)ps;
}

uint32_t mod_deadtime_ps_limit_at_clock(uint16_t period_ticks,
                                        uint64_t effective_compare_hz,
                                        uint32_t half_cycle_limit_pct)
{
    if ((period_ticks == 0U) || (effective_compare_hz == 0U) ||
        (half_cycle_limit_pct == 0U)) return 0U;
    if (half_cycle_limit_pct > 100U) half_cycle_limit_pct = 100U;
    /* Production HRTIM compare clocks are input_clock * 32 (5.44 GHz at
     * 170 MHz), so this API must remain 64-bit end-to-end. For an arbitrary
     * out-of-domain clock too large to multiply by 200, the physical result
     * rounds below one picosecond and the established minimum is 1 ps. */
    if (effective_compare_hz > UINT64_MAX / 200ULL) return 1U;
    uint64_t numerator = (uint64_t)period_ticks * half_cycle_limit_pct *
                         PS_PER_SECOND_LOGIC;
    uint64_t denominator = 200ULL * effective_compare_hz;
    uint64_t result = (numerator + denominator / 2ULL) / denominator;
    if (result == 0ULL) result = 1ULL;
    return (result > UINT32_MAX) ? UINT32_MAX : (uint32_t)result;
}

static int32_t clamp_scale(int32_t value, int32_t scale)
{
    if (value > scale) return scale;
    if (value < -scale) return -scale;
    return value;
}

int16_t mod_duty_permille_from_ticks(uint16_t half_period_ticks,
                                     int16_t ticks,
                                     int16_t permille_scale)
{
    if ((half_period_ticks == 0U) || (ticks == 0) || (permille_scale <= 0)) return 0;
    int32_t numerator = (int32_t)ticks * permille_scale;
    int32_t denominator = half_period_ticks;
    numerator += (numerator >= 0) ? denominator / 2 : -(denominator / 2);
    return (int16_t)clamp_scale(numerator / denominator, permille_scale);
}

int16_t mod_duty_ticks_from_permille(uint16_t half_period_ticks,
                                     int16_t permille,
                                     int16_t permille_scale)
{
    if ((half_period_ticks == 0U) || (permille == 0) || (permille_scale <= 0)) return 0;
    int32_t value = clamp_scale(permille, permille_scale);
    int64_t numerator = (int64_t)value * half_period_ticks;
    numerator += (numerator >= 0) ? permille_scale / 2 : -(permille_scale / 2);
    int64_t ticks = numerator / permille_scale;
    if (ticks > INT16_MAX) ticks = INT16_MAX;
    if (ticks < INT16_MIN) ticks = INT16_MIN;
    return (int16_t)ticks;
}

uint8_t mod_hrtim_phase_geometry(uint16_t carrier_period_ticks,
                                 int16_t duty_offset_ticks,
                                 uint16_t raw_phase_tick,
                                 mod_hrtim_phase_geometry_t *geometry)
{
    return mod_hrtim_phase_geometry_frac(carrier_period_ticks, duty_offset_ticks, raw_phase_tick,
                                         2U, 0, geometry);
}

uint8_t mod_hrtim_phase_geometry_frac(uint16_t carrier_period_ticks,
                                      int16_t duty_offset_ticks,
                                      uint16_t raw_phase_tick,
                                      uint8_t cycles,
                                      int8_t extra,
                                      mod_hrtim_phase_geometry_t *geometry)
{
    if (geometry == NULL) return 0U;
    geometry->carrier_period_ticks = 0U;
    geometry->master_period_ticks = 0U;
    geometry->local_compare_ticks = 0U;
    geometry->master_compare_ticks = 0U;

    if ((carrier_period_ticks < MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32) ||
        (carrier_period_ticks > MOD_HRTIM_MAX_CARRIER_PERIOD_MUL32) ||
        (raw_phase_tick >= carrier_period_ticks)) {
        return 0U;
    }
    const int32_t nudge = (int32_t)extra;
    if ((cycles < 2U) || (nudge < -MOD_HRTIM_FRAC_EXTRA_MAX) || (nudge > MOD_HRTIM_FRAC_EXTRA_MAX) ||
        ((int32_t)cycles <= (nudge < 0 ? -nudge : nudge))) {
        return 0U;
    }

    int32_t local_compare = (int32_t)(carrier_period_ticks / 2U) +
                            (int32_t)duty_offset_ticks;
    /* CMP1 and PER drive complementary output edges. RM0440 requires at least
     * three tHRTIM periods (0x60 high-resolution ticks at CKPSC=0) between
     * those edges for high-resolution behavior to be guaranteed. Applying the
     * same floor to both pulse polarities also prevents a nominally legal
     * compare value from creating an unsupported near-period pulse. A
     * negative nudge resets the timer |k| ticks before its period ends,
     * shortening that low half by as much. */
    int32_t local_max = (int32_t)carrier_period_ticks -
                        (int32_t)MOD_HRTIM_COMPARE_FLOOR_MUL32 + (nudge < 0 ? nudge : 0);
    if ((local_compare < (int32_t)MOD_HRTIM_COMPARE_FLOOR_MUL32) ||
        (local_compare > local_max)) {
        return 0U;
    }

    /* The master runs N carrier periods plus k ticks; the branch timer rolls
     * over on its own N - 1 times in between and is reset k ticks after (or
     * before) its N-th roll-over: the carrier averages P + k/N. */
    const int32_t master_period = (int32_t)cycles * (int32_t)carrier_period_ticks + nudge;
    if (master_period > (int32_t)MOD_HRTIM_MAX_PERIOD_REGISTER_MUL32) return 0U;
    uint32_t master_compare = MOD_HRTIM_COMPARE_FLOOR_MUL32 +
                              (uint32_t)raw_phase_tick;
    if ((master_compare < MOD_HRTIM_COMPARE_FLOOR_MUL32) ||
        ((int32_t)master_compare >= master_period)) {
        return 0U;
    }

    geometry->carrier_period_ticks = carrier_period_ticks;
    geometry->master_period_ticks = (uint16_t)master_period;
    geometry->local_compare_ticks = (uint16_t)local_compare;
    geometry->master_compare_ticks = (uint16_t)master_compare;
    return 1U;
}

/* CMP1 = P/2 + offset must stay within [floor, P - floor]: on 10 m that is
 * only 0 at P = 192 (the 28.33 MHz top at 170 MHz), 0..+1 tick at P = 193 and
 * -1..+1 at P = 194. The PA profile clamp allowed
 * +-(P/2 - 8), and a DUTY outside these bounds failed every carrier apply on
 * that band -- Error_Handler, the radio halted, and at boot when saved. */
uint8_t mod_hrtim_duty_offset_bounds(uint16_t carrier_period_ticks, int16_t *lo, int16_t *hi)
{
    return mod_hrtim_duty_offset_bounds_frac(carrier_period_ticks, 0, lo, hi);
}

uint8_t mod_hrtim_duty_offset_bounds_frac(uint16_t carrier_period_ticks, int8_t extra,
                                          int16_t *lo, int16_t *hi)
{
    int32_t low = 0, high = 0;
    const uint8_t ok = (uint8_t)((carrier_period_ticks >= MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32) &&
                                 (carrier_period_ticks <= MOD_HRTIM_MAX_CARRIER_PERIOD_MUL32));
    if (ok != 0U) {
        const int32_t half = (int32_t)(carrier_period_ticks / 2U);
        low = (int32_t)MOD_HRTIM_COMPARE_FLOOR_MUL32 - half;
        high = (int32_t)carrier_period_ticks - (int32_t)MOD_HRTIM_COMPARE_FLOOR_MUL32 - half +
               (extra < 0 ? (int32_t)extra : 0);
        if (high < low) high = low;
        if (low < INT16_MIN) low = INT16_MIN;
        if (high > INT16_MAX) high = INT16_MAX;
    }
    if (lo != NULL) *lo = (int16_t)low;
    if (hi != NULL) *hi = (int16_t)high;
    return ok;
}

mod_hrtim_branch_edges_t mod_hrtim_branch_edges(uint8_t swapped, uint32_t phase_reset)
{
    /* Output 1 is high from the start of each carrier period to CMP1. A period
     * starts at the timer's own roll-over or at the master compare that resets
     * the timer to set the branch phase, whichever comes first: a reset that
     * lands before the counter reaches PER cancels that roll-over, and with
     * PER as the only edge the rising edge went with it, holding the pair in
     * one state for 1.5 periods. The reset comes early whenever the phase
     * advances -- every NCO step on a band whose grid lies below the dial --
     * so 160 m (-298 Hz at 170 MHz) lost a pulse at each 192 kHz update,
     * about every ten cycles, while 60 m (+2.6 kHz at 170 MHz) was clean.
     * The master compare only ever doubles
     * PER's own edge, never the opposite one, so the two can coincide
     * without a set/reset conflict. */
    const uint32_t start = MOD_HRTIM_EDGE_PER | phase_reset;
    mod_hrtim_branch_edges_t e;
    e.set1 = start;
    e.reset1 = MOD_HRTIM_EDGE_CMP1;
    e.set2 = MOD_HRTIM_EDGE_CMP1;
    e.reset2 = start;
    if (swapped != 0U) {
        e.set1 = MOD_HRTIM_EDGE_CMP1;
        e.reset1 = start;
        e.set2 = start;
        e.reset2 = MOD_HRTIM_EDGE_CMP1;
    }
    return e;
}

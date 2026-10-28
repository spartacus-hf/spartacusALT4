#ifndef MOD_CALIBRATION_LOGIC_H
#define MOD_CALIBRATION_LOGIC_H

#include "modulator.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static inline float mod_calibration_interp(const float table[PHASE_CAL_BINS], float envelope)
{
    if (!isfinite(envelope)) {
        return (envelope > 0.0f) ? table[PHASE_CAL_BINS - 1U] : table[0];
    }
    if (envelope <= 0.0f) return table[0];
    if (envelope >= 1.0f) return table[PHASE_CAL_BINS - 1U];
    float position = envelope * (float)(PHASE_CAL_BINS - 1U);
    uint32_t lower = (uint32_t)position;
    if (lower >= PHASE_CAL_BINS - 1U) return table[PHASE_CAL_BINS - 1U];
    float fraction = position - (float)lower;
    return table[lower] * (1.0f - fraction) + table[lower + 1U] * fraction;
}

static inline uint8_t mod_calibration_is_active(uint16_t flags, uint8_t debug_bypass)
{
    return (uint8_t)(((flags & SSB_CAL_FLAG_APPLY) != 0U) && (debug_bypass == 0U));
}

static inline uint16_t mod_calibration_mark_edited(uint16_t flags)
{
    /* Preserve an explicit APPLY choice so manual bin edits remain observable,
     * but require the adjusted context to be validated again. */
    return flags & (uint16_t)~(SSB_CAL_FLAG_VALIDATED |
                               SSB_CAL_FLAG_LEGACY_REVALIDATE);
}

/* Exact decomposition of the legacy branch equations:
 *
 * old A = phi + theta + static + envelope(r) + amp_a
 * old C = phi - theta - static - envelope(r) + amp_c
 *
 * new A = phi + common(r) + theta + theta_pd(r) + differential
 * new C = phi + common(r) - theta - theta_pd(r) - differential
 */
static inline void mod_calibration_convert_legacy(phase_cal_t *phase,
                                                   float theta_pd[PHASE_CAL_BINS],
                                                   const phase_cal_legacy_t *legacy,
                                                   const float legacy_theta_pd[PHASE_CAL_BINS])
{
    memset(phase, 0, sizeof(*phase));
    phase->differential_skew = legacy->static_trim
                             + 0.5f * (legacy->amp_trim_a - legacy->amp_trim_c);
    float common = 0.5f * (legacy->amp_trim_a + legacy->amp_trim_c);
    for (uint32_t i = 0U; i < PHASE_CAL_BINS; i++) {
        phase->common_phase_pd[i] = common;
        theta_pd[i] = legacy->envelope_table[i]
                    + ((legacy_theta_pd != NULL) ? legacy_theta_pd[i] : 0.0f);
    }
}

#endif /* MOD_CALIBRATION_LOGIC_H */

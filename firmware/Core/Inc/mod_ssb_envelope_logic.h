#ifndef MOD_SSB_ENVELOPE_LOGIC_H
#define MOD_SSB_ENVELOPE_LOGIC_H

#include "modulator.h"

#include <math.h>
#include <stdint.h>

#define MOD_SSB_PHASE_BIAS_MAX 0.35f
#define MOD_SSB_CE_MIX_MAX     0.80f
#define MOD_SSB_CE_FLOOR_MAX   0.18f
#define MOD_SSB_CE_KNEE_MIN    0.25f
#define MOD_SSB_CE_KNEE_MAX    0.85f
#define MOD_SSB_LOW_CUT_MAX    0.012f

static inline float mod_ssb_phase_bias_for_build(float requested)
{
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    return requested;
#else
    (void)requested;
    return 0.0f;
#endif
}

static inline uint8_t mod_ssb_ce_enable_for_build(uint8_t requested)
{
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    return requested ? 1U : 0U;
#else
    (void)requested;
    return 0U;
#endif
}

static inline float mod_ssb_env_clamp(float value, float lower, float upper)
{
    if (!isfinite(value)) return (value > 0.0f) ? upper : lower;
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
}

#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
static inline float mod_ssb_env_smoothstep01(float value)
{
    if (value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value * value * (3.0f - 2.0f * value);
}
#endif

/* Production preserves analytic magnitude apart from the accepted low-level
 * inhibit. The legacy bias/floor curve is compiled only for RF-inhibited
 * diagnostic comparison and makes no linearity, spectrum, or efficiency claim. */
static inline float mod_ssb_theta_envelope_map(float envelope,
                                                float low_cut,
                                                float phase_bias,
                                                uint8_t ce_enable,
                                                float ce_mix,
                                                float ce_floor,
                                                float ce_knee)
{
    float env = mod_ssb_env_clamp(envelope, 0.0f, 0.999f);
    low_cut = mod_ssb_env_clamp(low_cut, 0.0f, MOD_SSB_LOW_CUT_MAX);
    if (env <= low_cut) return 0.0f;

#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    if (ce_enable != 0U) {
        float floor = mod_ssb_env_clamp(ce_floor, 0.0f, MOD_SSB_CE_FLOOR_MAX);
        float knee = mod_ssb_env_clamp(ce_knee, MOD_SSB_CE_KNEE_MIN,
                                      MOD_SSB_CE_KNEE_MAX);
        float mix = mod_ssb_env_clamp(ce_mix, 0.0f, MOD_SSB_CE_MIX_MAX);
        if (floor < low_cut) floor = low_cut;
        if (knee <= floor + 1.0e-6f) knee = floor + 0.05f;
        if (knee > MOD_SSB_CE_KNEE_MAX) knee = MOD_SSB_CE_KNEE_MAX;
        if (env < knee) {
            float t = (env - low_cut) / (knee - low_cut);
            float shaped = floor + (knee - floor) * mod_ssb_env_smoothstep01(t);
            env += mix * (shaped - env);
            env = mod_ssb_env_clamp(env, 0.0f, 0.999f);
        }
    }

    phase_bias = mod_ssb_env_clamp(phase_bias, 0.0f, MOD_SSB_PHASE_BIAS_MAX);
    env += phase_bias * (1.0f - env);
#else
    (void)phase_bias;
    (void)ce_enable;
    (void)ce_mix;
    (void)ce_floor;
    (void)ce_knee;
#endif
    return mod_ssb_env_clamp(env, 0.0f, 0.999f);
}

#endif /* MOD_SSB_ENVELOPE_LOGIC_H */

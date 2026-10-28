#ifndef MOD_MODE_MATH_H
#define MOD_MODE_MATH_H

/*
 * Pure, host-testable mathematical kernels used by the FM, AM, and CW
 * renderers.  These functions describe command-domain behavior; target-only
 * scheduling, RF safety, calibration, and HRTIM conversion remain elsewhere.
 */

#include "modulator.h"

#include <math.h>

#if defined(__GNUC__)
#define MOD_MODE_MATH_INLINE static inline __attribute__((always_inline))
#else
#define MOD_MODE_MATH_INLINE static inline
#endif

MOD_MODE_MATH_INLINE float mod_fm_phase_step_pi(float audio,
                                                 float deviation_hz,
                                                 float sample_rate_hz)
{
    if (!isfinite(audio) || !isfinite(deviation_hz) ||
        !isfinite(sample_rate_hz) || (sample_rate_hz <= 0.0f)) {
        return 0.0f;
    }
    return audio * deviation_hz * (2.0f / sample_rate_hz);
}

/*
 * Existing bounded FM pre-emphasis: a first-order low-pass state is
 * subtracted from the input to form a stable high shelf.  This is deliberately
 * named as a bounded approximation; it is not an exact unbounded 50 us lead
 * transfer function.
 */
MOD_MODE_MATH_INLINE float mod_fm_preemphasis_bounded(float input,
                                                       float *lowpass_state,
                                                       float sample_rate_hz,
                                                       float tau_seconds,
                                                       float shelf_gain)
{
    if (!isfinite(input)) input = 0.0f;
    if (lowpass_state == 0) return input;
    if (!isfinite(*lowpass_state)) *lowpass_state = 0.0f;
    if (!isfinite(sample_rate_hz) || !isfinite(tau_seconds) ||
        !isfinite(shelf_gain) || (sample_rate_hz <= 0.0f) ||
        (tau_seconds <= 0.0f) || (shelf_gain < 0.0f)) {
        return input;
    }

    float coefficient = 1.0f / (1.0f + sample_rate_hz * tau_seconds);
    *lowpass_state += coefficient * (input - *lowpass_state);
    return input + shelf_gain * (input - *lowpass_state);
}

MOD_MODE_MATH_INLINE float mod_am_envelope_unclamped(float audio,
                                                      float carrier,
                                                      float depth,
                                                      am_profile_t profile)
{
    if (!isfinite(audio)) audio = 0.0f;
    if (!isfinite(carrier)) carrier = 0.0f;
    if (!isfinite(depth)) depth = 0.0f;
    if (profile == AM_PROFILE_LINEAR_TEST) {
        return carrier * (1.0f + depth * audio);
    }
    return carrier + 0.5f * depth * audio;
}

MOD_MODE_MATH_INLINE float mod_cw_blackman_harris_ramp(float position)
{
    const float pi = 3.14159265358979323846f;
    if (!isfinite(position) || (position <= 0.0f)) return 0.0f;
    if (position >= 1.0f) return 1.0f;
    float weight = 0.35875f
                 - 0.48829f * cosf(pi * position)
                 + 0.14128f * cosf(2.0f * pi * position)
                 - 0.01168f * cosf(3.0f * pi * position);
    if (weight <= 0.0f) return 0.0f;
    if (weight >= 1.0f) return 1.0f;
    return weight;
}

#undef MOD_MODE_MATH_INLINE

#endif /* MOD_MODE_MATH_H */

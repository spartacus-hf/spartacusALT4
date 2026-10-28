#ifndef MOD_ALC_LOGIC_H
#define MOD_ALC_LOGIC_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MOD_ALC_LOOKAHEAD_SAMPLES 128U

typedef struct {
    float delay[MOD_ALC_LOOKAHEAD_SAMPLES];
    float peaks[MOD_ALC_LOOKAHEAD_SAMPLES];
    uint32_t index;
    float gain;
    float max_peak;
} mod_alc_state_t;

static inline void mod_alc_reset(mod_alc_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->gain = 1.0f;
}

/* Exact-ceiling look-ahead limiter. The outgoing peak remains in max_peak
 * until its delayed sample has been emitted. A final gain bound covers reset,
 * configuration changes, extreme inputs, and floating-point attack residue. */
static inline float mod_alc_process_sample(mod_alc_state_t *state,
                                           float input,
                                           float ceiling,
                                           float attack_coeff,
                                           float release_coeff)
{
    if (state == NULL) return 0.0f;
    if ((state->index >= MOD_ALC_LOOKAHEAD_SAMPLES) ||
        !isfinite(state->gain) || (state->gain < 0.0f) || (state->gain > 1.0f) ||
        !isfinite(state->max_peak) || (state->max_peak < 0.0f)) {
        mod_alc_reset(state);
    }
    if (!isfinite(input)) input = 0.0f;
    if (!isfinite(ceiling) || (ceiling <= 0.0f)) ceiling = 1.0f;
    if (!isfinite(attack_coeff)) attack_coeff = 1.0f;
    if (!isfinite(release_coeff)) release_coeff = 0.0f;
    if (attack_coeff < 0.0f) attack_coeff = 0.0f;
    if (attack_coeff > 1.0f) attack_coeff = 1.0f;
    if (release_coeff < 0.0f) release_coeff = 0.0f;
    if (release_coeff > 1.0f) release_coeff = 1.0f;

    uint32_t index = state->index;
    float delayed = state->delay[index];
    float old_peak = state->peaks[index];
    if (!isfinite(delayed)) delayed = 0.0f;
    if (!isfinite(old_peak) || (old_peak < 0.0f)) old_peak = 0.0f;
    float new_peak = fabsf(input);

    /* The gain for this output sees both the delayed sample at the read slot
     * and the newly arrived sample at the far end of the look-ahead window. */
    if (new_peak > state->max_peak) state->max_peak = new_peak;
    float peak_for_output = state->max_peak;
    float wanted_gain = (peak_for_output > ceiling)
                      ? (ceiling / peak_for_output)
                      : 1.0f;
    float coeff = (wanted_gain < state->gain) ? attack_coeff : release_coeff;
    state->gain += coeff * (wanted_gain - state->gain);
    if (state->gain < 0.0f) state->gain = 0.0f;
    if (state->gain > 1.0f) state->gain = 1.0f;

    float delayed_peak = fabsf(delayed);
    if ((delayed_peak > 0.0f) && (state->gain * delayed_peak > ceiling)) {
        state->gain = ceiling / delayed_peak;
    }
    float output = delayed * state->gain;
    if (output > ceiling) output = ceiling;
    if (output < -ceiling) output = -ceiling;

    /* Retire the old slot only after its sample has been emitted, then make
     * the rolling maximum exact for the next output call. */
    state->delay[index] = input;
    state->peaks[index] = new_peak;
    if ((old_peak >= state->max_peak) && (new_peak < old_peak)) {
        float next_peak = 0.0f;
        for (uint32_t k = 0U; k < MOD_ALC_LOOKAHEAD_SAMPLES; k++) {
            if (state->peaks[k] > next_peak) next_peak = state->peaks[k];
        }
        state->max_peak = next_peak;
    }
    state->index = (index + 1U) % MOD_ALC_LOOKAHEAD_SAMPLES;
    return output;
}

/* mod_alc_process_sample over x[0..n-1] in place, bit for bit. The
 * configuration and the state are checked once, as the first sample's call
 * would; after that the loop keeps them valid by construction (index wraps
 * below the window, gain is clamped to [0, 1] from finite operands, peaks are
 * magnitudes of finite inputs), so the per-sample call's re-checks, which
 * were most of its cost, never fire and are left out. The arithmetic and its
 * order are those of the per-sample call. */
static inline void mod_alc_process_block(mod_alc_state_t *state,
                                         float *x,
                                         uint32_t n,
                                         float ceiling,
                                         float attack_coeff,
                                         float release_coeff)
{
    if ((state == NULL) || (x == NULL) || (n == 0U)) return;
    if ((state->index >= MOD_ALC_LOOKAHEAD_SAMPLES) ||
        !isfinite(state->gain) || (state->gain < 0.0f) || (state->gain > 1.0f) ||
        !isfinite(state->max_peak) || (state->max_peak < 0.0f)) {
        mod_alc_reset(state);
    }
    if (!isfinite(ceiling) || (ceiling <= 0.0f)) ceiling = 1.0f;
    if (!isfinite(attack_coeff)) attack_coeff = 1.0f;
    if (!isfinite(release_coeff)) release_coeff = 0.0f;
    if (attack_coeff < 0.0f) attack_coeff = 0.0f;
    if (attack_coeff > 1.0f) attack_coeff = 1.0f;
    if (release_coeff < 0.0f) release_coeff = 0.0f;
    if (release_coeff > 1.0f) release_coeff = 1.0f;

    uint32_t index = state->index;
    float gain = state->gain;
    float max_peak = state->max_peak;
    for (uint32_t i = 0U; i < n; i++) {
        float input = x[i];
        if (!isfinite(input)) input = 0.0f;
        float delayed = state->delay[index];
        float old_peak = state->peaks[index];
        if (!isfinite(delayed)) delayed = 0.0f;
        if (!isfinite(old_peak) || (old_peak < 0.0f)) old_peak = 0.0f;
        const float new_peak = fabsf(input);

        if (new_peak > max_peak) max_peak = new_peak;
        const float wanted_gain = (max_peak > ceiling) ? (ceiling / max_peak) : 1.0f;
        const float coeff = (wanted_gain < gain) ? attack_coeff : release_coeff;
        gain += coeff * (wanted_gain - gain);
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 1.0f) gain = 1.0f;

        const float delayed_peak = fabsf(delayed);
        if ((delayed_peak > 0.0f) && (gain * delayed_peak > ceiling)) {
            gain = ceiling / delayed_peak;
        }
        float output = delayed * gain;
        if (output > ceiling) output = ceiling;
        if (output < -ceiling) output = -ceiling;

        state->delay[index] = input;
        state->peaks[index] = new_peak;
        if ((old_peak >= max_peak) && (new_peak < old_peak)) {
            float next_peak = 0.0f;
            for (uint32_t k = 0U; k < MOD_ALC_LOOKAHEAD_SAMPLES; k++) {
                if (state->peaks[k] > next_peak) next_peak = state->peaks[k];
            }
            max_peak = next_peak;
        }
        if (++index >= MOD_ALC_LOOKAHEAD_SAMPLES) index = 0U;
        x[i] = output;
    }
    state->index = index;
    state->gain = gain;
    state->max_peak = max_peak;
}

#endif /* MOD_ALC_LOGIC_H */

#ifndef MOD_SSB_GATE_LOGIC_H
#define MOD_SSB_GATE_LOGIC_H

#include <math.h>
#include <stdint.h>

#define MOD_SSB_GATE_HYST_FACTOR   1.8f
#define MOD_SSB_GATE_CLOSE_FACTOR  0.65f
#define MOD_SSB_GATE_HOLD_SAMPLES  96U
#define MOD_SSB_GATE_FADE_COEFF    0.998f
#define MOD_SSB_GATE_NULL_ENV      0.002f
#define MOD_SSB_PHASE_BLEND_START  0.020f
#define MOD_SSB_PHASE_BLEND_FULL   0.080f
#define MOD_SSB_PHASE_DECAY_COEFF  0.995f

typedef struct {
    uint8_t open;
    uint16_t hang;
    float last_phi;
} mod_ssb_gate_state_t;

typedef struct {
    float gate_fade;
} mod_ssb_gate_fade_state_t;

static inline float mod_ssb_gate_clamp(float value, float lower, float upper)
{
    if (!isfinite(value)) return (value > 0.0f) ? upper : lower;
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
}

static inline uint8_t mod_ssb_gate_process(mod_ssb_gate_state_t *state,
                                            float envelope,
                                            float gate_threshold)
{
    if (state == 0) return 0U;
    if (!isfinite(envelope) || !isfinite(gate_threshold)) {
        state->open = 0U;
        state->hang = 0U;
        return 0U;
    }
    envelope = mod_ssb_gate_clamp(envelope, 0.0f, 0.999f);
    gate_threshold = mod_ssb_gate_clamp(gate_threshold, 0.0f, 0.999f);
    float open_threshold = gate_threshold * MOD_SSB_GATE_HYST_FACTOR;
    float close_threshold = gate_threshold * MOD_SSB_GATE_CLOSE_FACTOR;
    if (envelope >= open_threshold) {
        state->open = 1U;
        state->hang = MOD_SSB_GATE_HOLD_SAMPLES;
        return 1U;
    }
    if (state->open != 0U) {
        if (envelope < close_threshold) {
            if (state->hang == 0U) state->open = 0U;
            else state->hang--;
        } else {
            state->hang = MOD_SSB_GATE_HOLD_SAMPLES;
        }
        return 1U;
    }
    return 0U;
}

static inline void mod_ssb_gate_update_phi(mod_ssb_gate_state_t *state,
                                            float phase,
                                            uint8_t good_phase)
{
    if (state == 0) return;
    if (good_phase != 0U) {
        state->last_phi = isfinite(phase) ? phase : 0.0f;
    } else {
        state->last_phi = isfinite(state->last_phi)
                        ? state->last_phi * MOD_SSB_PHASE_DECAY_COEFF
                        : 0.0f;
    }
}

static inline float mod_ssb_gate_fade_update(mod_ssb_gate_fade_state_t *state,
                                              uint8_t rendering,
                                              float live_envelope)
{
    if (state == 0) return 0.0f;
    if (!isfinite(live_envelope)) {
        state->gate_fade = 0.0f;
        return 0.0f;
    }
    live_envelope = mod_ssb_gate_clamp(live_envelope, 0.0f, 0.999f);
    if (!isfinite(state->gate_fade)) state->gate_fade = 0.0f;
    if (rendering != 0U) state->gate_fade = live_envelope;
    else state->gate_fade *= MOD_SSB_GATE_FADE_COEFF;
    return state->gate_fade;
}

static inline float mod_ssb_phase_wrap_norm(float phase)
{
    if (!isfinite(phase)) return 0.0f;
    while (phase >= 1.0f) phase -= 2.0f;
    while (phase < -1.0f) phase += 2.0f;
    return phase;
}

static inline float mod_ssb_phase_blend_norm(float previous,
                                              float current,
                                              float weight)
{
    if (!isfinite(previous)) previous = 0.0f;
    if (!isfinite(current)) current = 0.0f;
    if (!isfinite(weight)) weight = 0.0f;
    if (weight <= 0.0f) return mod_ssb_phase_wrap_norm(previous);
    if (weight >= 1.0f) return mod_ssb_phase_wrap_norm(current);
    float difference = current - previous;
    while (difference > 1.0f) difference -= 2.0f;
    while (difference < -1.0f) difference += 2.0f;
    return mod_ssb_phase_wrap_norm(previous + weight * difference);
}

static inline float mod_ssb_phase_stabilize_low_env(float envelope,
                                                     float previous,
                                                     float current)
{
    if (!isfinite(envelope)) envelope = 0.0f;
    envelope = mod_ssb_gate_clamp(envelope, 0.0f, 0.999f);
    if (envelope <= MOD_SSB_PHASE_BLEND_START) {
        return mod_ssb_phase_wrap_norm(previous);
    }
    if (envelope >= MOD_SSB_PHASE_BLEND_FULL) {
        return mod_ssb_phase_wrap_norm(current);
    }
    float weight = (envelope - MOD_SSB_PHASE_BLEND_START)
                 / (MOD_SSB_PHASE_BLEND_FULL - MOD_SSB_PHASE_BLEND_START);
    return mod_ssb_phase_blend_norm(previous, current, weight);
}

#endif /* MOD_SSB_GATE_LOGIC_H */

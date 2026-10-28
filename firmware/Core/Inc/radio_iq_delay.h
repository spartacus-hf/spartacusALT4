#ifndef RADIO_IQ_DELAY_H
#define RADIO_IQ_DELAY_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "radio_config.h"

/* Delay the leading PCM1821 channel by a fractional input sample. The two-tap
 * interpolator is deliberately small: longer FIR and all-pass versions both
 * exceeded the REV1 receive DMA deadline. RADIO_IqDelayResponse lets the
 * receiver remove its exact gain and phase error at the planned IF, leaving
 * only the small change across the 3 kHz-wide channel. */
typedef struct {
    float previous_i, previous_q, skew_samples;
    uint8_t active, delay_q;
} radio_iq_delay_t;

static inline void RADIO_IqDelayInit(radio_iq_delay_t *s, float skew_samples)
{
    memset(s, 0, sizeof(*s));
    if (!isfinite(skew_samples) || fabsf(skew_samples) < 0.00001f ||
        fabsf(skew_samples) > RADIO_IQ_SKEW_MAX) return;
    s->skew_samples = skew_samples;
    s->delay_q = skew_samples > 0.0f ? 1U : 0U;
    s->active = 1U;
}

static inline void RADIO_IqDelayResponse(const radio_iq_delay_t *s, float cycles_per_sample,
                                         float *out_i, float *out_q)
{
    *out_i = 1.0f;
    *out_q = 0.0f;
    if (!s->active || !isfinite(cycles_per_sample)) return;
    const float d = fabsf(s->skew_samples);
    const float w = 6.28318530718f * cycles_per_sample;
    *out_i = (1.0f - d) + d * cosf(w);
    *out_q = -d * sinf(w);
}

static inline void RADIO_IqDelaySample(radio_iq_delay_t *s, float i, float q,
                                       float *out_i, float *out_q)
{
    if (!s->active) {
        *out_i = i;
        *out_q = q;
        return;
    }
    const float d = fabsf(s->skew_samples);
    if (s->delay_q) {
        *out_i = i;
        *out_q = q + d * (s->previous_q - q);
    } else {
        *out_i = i + d * (s->previous_i - i);
        *out_q = q;
    }
    s->previous_i = i;
    s->previous_q = q;
}

#endif

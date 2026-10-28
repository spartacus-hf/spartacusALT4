#ifndef RADIO_MIC_FRONT_H
#define RADIO_MIC_FRONT_H

#include <stdint.h>

/* QMX/QMX+ microphone front end. QRP Labs samples both the microphone and a
 * correlated reference input at about 700 ksps, downsamples them to the
 * 12 ksps SSB rate, subtracts the reference, removes DC, then applies 50 dB
 * of fixed microphone gain. Keeping the sums until an output boundary is
 * important: it retains the fractional ADC information that a 12-bit result
 * shifted back to its original scale would discard. */
#define RADIO_MIC_FRONT_OUTPUT_HZ       12000UL
#define RADIO_MIC_FRONT_GAIN_50DB       316.227766f
#define RADIO_MIC_FRONT_REFERENCE_QMX   0.68f

typedef struct {
    uint32_t input_hz;
    uint32_t phase;
    uint32_t count;
    uint32_t mic_sum;
    uint32_t reference_sum;
    float reference_scale;
    float dc;
    /* Separate blockers for the two uncancelled streams. They exist so the
     * console can stream microphone and reference side by side and the
     * cancellation coefficient can be measured on a board instead of
     * inherited: QRP Labs' 0.68 is their hardware's number, not a constant. */
    float mic_dc;
    float reference_dc;
    uint8_t dc_ready;
} radio_mic_front_t;

static inline void RADIO_MicFrontInit(radio_mic_front_t *s, uint32_t input_hz,
                                      float reference_scale)
{
    s->input_hz = input_hz;
    s->phase = 0U;
    s->count = 0U;
    s->mic_sum = 0U;
    s->reference_sum = 0U;
    s->reference_scale = reference_scale;
    s->dc = 0.0f;
    s->mic_dc = 0.0f;
    s->reference_dc = 0.0f;
    s->dc_ready = 0U;
}

/* Consume one high-rate microphone/reference pair. Returns one normalized
 * 12 kHz sample when the rational decimator crosses an output boundary.
 * `gain_trim == 1` is QMX's default +50 dB microphone gain.
 *
 * `pair`, when not NULL, additionally receives the two uncancelled streams --
 * [0] microphone, [1] reference -- at the same scale. Subtracting
 * `reference_scale * pair[1]` from `pair[0]` reproduces `*output`, so a host
 * that records the pair can solve for the coefficient that actually cancels
 * on this board. */
static inline uint8_t RADIO_MicFrontPush(radio_mic_front_t *s, uint16_t microphone,
                                         uint16_t reference, float gain_trim,
                                         float *output, float *pair)
{
    s->mic_sum += microphone;
    s->reference_sum += reference;
    ++s->count;
    s->phase += RADIO_MIC_FRONT_OUTPUT_HZ;
    if (s->phase < s->input_hz) return 0U;
    s->phase -= s->input_hz;

    const float inverse_count = 1.0f / (float)s->count;
    const float mic_mean = (float)s->mic_sum * inverse_count;
    const float reference_mean = (float)s->reference_sum * inverse_count;
    const float value = mic_mean - s->reference_scale * reference_mean;
    s->count = 0U;
    s->mic_sum = 0U;
    s->reference_sum = 0U;

    if (!s->dc_ready) {
        s->dc = value;
        s->mic_dc = mic_mean;
        s->reference_dc = reference_mean;
        s->dc_ready = 1U;
    } else {
        /* Same ~7.5 Hz corner as the former 0.001-at-48-kHz blocker. */
        s->dc += (value - s->dc) * (1.0f / 256.0f);
        s->mic_dc += (mic_mean - s->mic_dc) * (1.0f / 256.0f);
        s->reference_dc += (reference_mean - s->reference_dc) * (1.0f / 256.0f);
    }
    const float scale = RADIO_MIC_FRONT_GAIN_50DB * gain_trim * (1.0f / 2048.0f);
    *output = (value - s->dc) * scale;
    if (pair) {
        pair[0] = (mic_mean - s->mic_dc) * scale;
        pair[1] = (reference_mean - s->reference_dc) * scale;
    }
    return 1U;
}

#endif

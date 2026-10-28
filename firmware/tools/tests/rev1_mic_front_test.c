#include "radio_mic_front.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void qmx_rate_is_exact_over_time(void)
{
    radio_mic_front_t s;
    RADIO_MicFrontInit(&s, 700000U, 0.0f);
    unsigned outputs = 0U;
    float out;
    for (unsigned n = 0U; n < 700000U; ++n)
        outputs += RADIO_MicFrontPush(&s, 2048U, 0U, 1.0f, &out, NULL);
    assert(outputs == RADIO_MIC_FRONT_OUTPUT_HZ);
    assert(s.count == 0U);
}

static void same_adc_noise_cancels(void)
{
    radio_mic_front_t s;
    RADIO_MicFrontInit(&s, 700000U, 1.0f);
    float peak = 0.0f, out = 0.0f;
    unsigned outputs = 0U;
    for (unsigned n = 0U; n < 70000U; ++n) {
        const uint16_t spur = (uint16_t)(2048U + ((n / 37U) & 1U));
        if (RADIO_MicFrontPush(&s, spur, spur, 1.0f, &out, NULL)) {
            if (fabsf(out) > peak) peak = fabsf(out);
            ++outputs;
        }
    }
    assert(outputs == 1200U);
    assert(peak < 1.0e-7f);
}

static void sub_lsb_information_survives_decimation(void)
{
    radio_mic_front_t s;
    RADIO_MicFrontInit(&s, 696000U, 0.0f); /* exactly 58 input samples/output */
    float out = 0.0f, peak = 0.0f;
    unsigned outputs = 0U;
    for (unsigned n = 0U; n < 69600U; ++n) {
        /* One ADC code of 1 kHz square-wave signal plus deterministic dither.
         * A 58-sample average is fractional even though every input is not. */
        const unsigned audio_phase = (n * 12000U / 696000U) % 12U;
        const int signal = audio_phase < 6U ? 1 : -1;
        const int dither = (n & 3U) == 0U ? 1 : 0;
        uint16_t sample = (uint16_t)(2048 + signal + dither);
        if (RADIO_MicFrontPush(&s, sample, 0U, 1.0f, &out, NULL)) {
            if (outputs > 100U && fabsf(out) > peak) peak = fabsf(out);
            ++outputs;
        }
    }
    assert(outputs == 1200U);
    assert(peak > 0.05f);
    assert(peak < 0.35f);
}

/* The split diagnostic is only useful if the host can reconstruct what the
 * radio would have transmitted, so pair[0] - scale * pair[1] must be the
 * cancelled output to within float rounding. */
static void split_pair_reproduces_the_cancelled_output(void)
{
    radio_mic_front_t s;
    RADIO_MicFrontInit(&s, 700000U, 0.68f);
    float out = 0.0f, pair[2] = { 0.0f, 0.0f };
    unsigned checked = 0U;
    for (unsigned n = 0U; n < 70000U; ++n) {
        const uint16_t mic = (uint16_t)(2048U + ((n / 11U) % 7U));
        const uint16_t ref = (uint16_t)(2048U + ((n / 23U) % 5U));
        if (!RADIO_MicFrontPush(&s, mic, ref, 1.0f, &out, pair)) continue;
        if (++checked <= 400U) continue;   /* let the blockers settle */
        const float rebuilt = pair[0] - 0.68f * pair[1];
        assert(fabsf(rebuilt - out) < 1.0e-3f * (1.0f + fabsf(out)));
    }
    assert(checked == 1200U);
}

int main(void)
{
    qmx_rate_is_exact_over_time();
    same_adc_noise_cancels();
    sub_lsb_information_survives_decimation();
    split_pair_reproduces_the_cancelled_output();
    printf("REV1 QMX microphone front end: 700k/12k rate, reference cancellation, split pair and retained precision: PASS\n");
    return 0;
}

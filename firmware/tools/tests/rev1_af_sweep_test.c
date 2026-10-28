#include "radio_af_sweep.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* QMX-equivalent audio filter sweep (8.6.1) and image sweep (8.6.3): the
 * frequency grid, the demodulator offsets that place a fixed carrier in the
 * wanted and the opposite sideband, and the image axis derived from the
 * injection offset. */

static void grid_test(void)
{
    /* QMX's own grid: 100 Hz to 3850 Hz in 50 Hz steps. */
    assert(RADIO_AfPointHz(0U) == 100);
    assert(RADIO_AfPointHz(RADIO_AF_POINTS - 1U) == 3850);
    assert(RADIO_AfPointHz(1000U) == 3850); /* clamped, never past the end */
    for (unsigned k = 1U; k < RADIO_AF_POINTS; ++k)
        assert(RADIO_AfPointHz(k) - RADIO_AfPointHz(k - 1U) == RADIO_AF_STEP_HZ);
    /* The 150 Hz and 3.2 kHz channel-filter corners both have points either
     * side of them, or the plot cannot show where the filter rolls off. */
    unsigned below_low = 0U, above_high = 0U;
    for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k) {
        if (RADIO_AfPointHz(k) < 150) ++below_low;
        if (RADIO_AfPointHz(k) > 3200) ++above_high;
    }

    /* The image sweep labels the actual frequency in the 48 kHz stream, but
     * the demodulator API takes dial offset and adds the SSB centre itself. */
    assert(fabsf(RADIO_AfDemodOffsetHz(9381.0f, 0U) - 7731.0f) < 0.01f);
    assert(fabsf(RADIO_AfDemodOffsetHz(-9381.0f, 1U) + 7731.0f) < 0.01f);
    assert(below_low >= 1U && above_high >= 8U);
}

/* Where the demodulator has to sit for the carrier to land at audio `want`,
 * given the carrier is at `tone` in the 48 kHz stream: offset = tone - want. */
static double audio_of(double tone, double offset) { return tone - offset; }

static void offset_test(void)
{
    /* A carrier anywhere in the stream, and the two sideband senses. */
    static const double tone[] = {-2730.0, 0.0, 5000.0, -21000.0};
    for (unsigned t = 0U; t < sizeof(tone) / sizeof(tone[0]); ++t) {
        for (unsigned lsb = 0U; lsb <= 1U; ++lsb) {
            const double sign = lsb ? -1.0 : 1.0;
            for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k) {
                const double f = RADIO_AfPointHz(k);
                const double wanted = audio_of(tone[t],
                    RADIO_AfOffsetHz((float)tone[t], k, 0U, (uint8_t)lsb));
                const double opposite = audio_of(tone[t],
                    RADIO_AfOffsetHz((float)tone[t], k, 1U, (uint8_t)lsb));
                /* Wanted: the audio frequency the point asked for, on the
                 * side of the dial the mode demodulates. */
                assert(fabs(wanted - sign * f) < 0.6);
                /* Opposite: the mirror of it about the dial. */
                assert(fabs(opposite + sign * f) < 0.6);
                /* So the two are never the same measurement, at any point --
                 * an offset error that shifts both by the passband centre
                 * makes them coincide at 1650 Hz and read 0 dB of rejection
                 * where the filter is in fact deepest. */
                assert(fabs(wanted - opposite) >= 2.0 * 100.0 - 0.6);
            }
        }
    }
    /* USB and LSB are mirror images of each other, point for point. */
    for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k)
        for (unsigned curve = 0U; curve < RADIO_AF_CURVES; ++curve)
            assert(fabsf(RADIO_AfOffsetHz(0.0f, k, curve, 0U) +
                         RADIO_AfOffsetHz(0.0f, k, curve, 1U)) < 0.01f);
}

static void image_test(void)
{
    float first = 0.0f, step = 0.0f;
    /* 40 m: the LO planner targets a 12 kHz IF and one HRTIM tick is about
     * 9.3 kHz there, so the carrier sits near -2.7 kHz and its image near
     * -21.3 kHz. Both fit, and the axis has to cover both with margin. */
    const float tone = 9270.0f - 12000.0f, image = -(9270.0f + 12000.0f);
    assert(RADIO_ImageSpan(tone, image, &first, &step));
    assert(step > 0.0f);
    const float last = RADIO_ImagePointHz(first, step, RADIO_AF_POINTS - 1U);
    assert(first < image && last > tone);
    assert(first > -24000.0f && last < 24000.0f); /* never wraps the stream */
    assert(fabsf(first - (image - RADIO_IMAGE_MARGIN_HZ)) < 0.5f);
    assert(fabsf(last - (tone + RADIO_IMAGE_MARGIN_HZ)) < 0.5f);
    /* Monotone, and both responses have points near them to resolve a peak. */
    unsigned near_tone = 0U, near_image = 0U;
    float previous = first - 1.0f;
    for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k) {
        const float hz = RADIO_ImagePointHz(first, step, k);
        assert(hz > previous);
        previous = hz;
        if (fabsf(hz - tone) < 1000.0f) ++near_tone;
        if (fabsf(hz - image) < 1000.0f) ++near_image;
    }
    assert(near_tone >= 3U && near_image >= 3U);
    assert(RADIO_ImagePointHz(first, step, 1000U) == last); /* clamped */

    /* 30 m: one tick is about 18.8 kHz, so the image lands at -30.8 kHz,
     * outside the 48 kHz stream. That has to be refused rather than folded
     * over into a picture of the wrong thing. */
    assert(!RADIO_ImageSpan(18800.0f - 12000.0f, -(18800.0f + 12000.0f), &first, &step));
    /* Either response alone being out of range is enough to refuse. */
    assert(!RADIO_ImageSpan(0.0f, -30000.0f, &first, &step));
    assert(!RADIO_ImageSpan(30000.0f, 0.0f, &first, &step));
    /* Exactly at the limit is out; just inside is in. */
    assert(!RADIO_ImageSpan(0.0f, -RADIO_IMAGE_LIMIT_HZ, &first, &step));
    assert(RADIO_ImageSpan(0.0f, -(RADIO_IMAGE_LIMIT_HZ - 1.0f), &first, &step));
    /* Two responses on top of each other cannot be told apart, so a delta
     * that small is refused rather than plotted as one peak. */
    assert(!RADIO_ImageSpan(1000.0f, 0.0f, &first, &step));
    assert(!RADIO_ImageSpan(0.0f, 0.0f, &first, &step));
    /* The order of the pair must not matter. */
    float fa = 0.0f, sa = 0.0f, fb = 0.0f, sb = 0.0f;
    assert(RADIO_ImageSpan(tone, image, &fa, &sa));
    assert(RADIO_ImageSpan(image, tone, &fb, &sb));
    assert(fabsf(fa - fb) < 0.01f && fabsf(sa - sb) < 0.01f);
}


static void span_test(void)
{
    /* A carrier near the middle of the stream sweeps fine. */
    assert(RADIO_AfSpanFits(0.0f));
    assert(RADIO_AfSpanFits(-1803.0f));   /* 40 m with the injection mirrored */
    assert(RADIO_AfSpanFits(15000.0f));
    /* 40 m as RADIO_IqChoosePeriod first picks it: the LO sits above the
     * carrier, so a +9.3 kHz delta and a -11.1 kHz intermediate add to
     * +20.4 kHz and the top of the sweep would fall outside the stream. That
     * has to be refused, not run until the first point reports no data. */
    assert(!RADIO_AfSpanFits(20419.0f));
    /* Symmetric, and the edge is where the last point plus the carrier reach
     * the limit rather than the carrier alone. */
    assert(!RADIO_AfSpanFits(-20419.0f));
    const float edge = RADIO_AF_STREAM_LIMIT_HZ - (float)RADIO_AfPointHz(RADIO_AF_POINTS - 1U);
    assert(RADIO_AfSpanFits(edge - 1.0f) && !RADIO_AfSpanFits(edge + 1.0f));
    assert(!RADIO_AfSpanFits(RADIO_AF_STREAM_LIMIT_HZ));
}

int main(void)
{
    grid_test();
    offset_test();
    image_test();
    span_test();
    printf("REV1 audio filter sweep grid and offsets, image sweep axis and its stream limit: PASS\n");
    return 0;
}

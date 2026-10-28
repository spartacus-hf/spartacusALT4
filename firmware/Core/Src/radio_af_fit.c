#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio_af_sweep.h"

/* Pure axis and offset arithmetic for the QMX-equivalent audio and image
 * sweeps, split out of the runtime so a host suite can drive it, the way
 * radio_bpf_fit.c splits out of radio_bpf_sweep.c. */

int32_t RADIO_AfPointHz(unsigned k)
{
    if (k >= RADIO_AF_POINTS) k = RADIO_AF_POINTS - 1U;
    return RADIO_AF_FIRST_HZ + (int32_t)k * RADIO_AF_STEP_HZ;
}

float RADIO_AfOffsetHz(float tone, unsigned k, unsigned curve, uint8_t lsb)
{
    /* RADIO_DspInit mixes by (offset_hz + centre) and the BFO puts the centre
     * back, so `offset_hz` is the dial frequency in stream terms: a carrier at
     * `tone` comes out at audio (tone - offset), and the channel filter passes
     * +100..+3200 Hz of that in USB and -3200..-100 Hz in LSB. So the wanted
     * curve wants the carrier at +f of the dial in USB and at -f in LSB, and
     * the opposite curve wants it on the other side. No passband-centre term
     * belongs here; putting one in offsets both curves by 1650 Hz and makes
     * them coincide at the centre frequency. */
    const float f = (float)RADIO_AfPointHz(k);
    const float sign = lsb ? -1.0f : 1.0f;
    return curve ? tone + sign * f : tone - sign * f;
}

float RADIO_AfDemodOffsetHz(float stream_hz, uint8_t lsb)
{
    return stream_hz - (lsb ? -1650.0f : 1650.0f);
}

uint8_t RADIO_AfSpanFits(float tone)
{
    if (!(tone > -RADIO_AF_STREAM_LIMIT_HZ) || !(tone < RADIO_AF_STREAM_LIMIT_HZ)) return 0U;
    const float reach = (float)RADIO_AfPointHz(RADIO_AF_POINTS - 1U);
    const float high = tone < 0.0f ? -tone : tone;
    return (high + reach < RADIO_AF_STREAM_LIMIT_HZ) ? 1U : 0U;
}

uint8_t RADIO_ImageSpan(float tone, float image, float *first, float *step)
{
    if (!(tone > -RADIO_IMAGE_LIMIT_HZ) || !(tone < RADIO_IMAGE_LIMIT_HZ) ||
        !(image > -RADIO_IMAGE_LIMIT_HZ) || !(image < RADIO_IMAGE_LIMIT_HZ)) return 0U;
    const float low = tone < image ? tone : image;
    const float high = tone < image ? image : tone;
    /* Both responses must be resolvable, so they cannot sit on top of each
     * other: 2*delta is at least 12 kHz by construction, and this rejects a
     * caller that computed them wrongly rather than trusting it. */
    if (!(high - low > 3000.0f)) return 0U;
    const float begin = low - RADIO_IMAGE_MARGIN_HZ;
    const float end = high + RADIO_IMAGE_MARGIN_HZ;
    *first = begin;
    *step = (end - begin) / (float)(RADIO_AF_POINTS - 1U);
    return 1U;
}

float RADIO_ImagePointHz(float first, float step, unsigned k)
{
    if (k >= RADIO_AF_POINTS) k = RADIO_AF_POINTS - 1U;
    return first + step * (float)k;
}

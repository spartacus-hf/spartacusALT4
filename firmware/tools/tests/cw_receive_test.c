/* CW receive DSP: the selectable channel filters, the pitch and the hang AGC,
 * through the production RADIO_DspDemodulate at 48 kHz.
 *
 *   - Each CW bandwidth: -6 dB width within 12 % of its name, at least 60 dB
 *     down 400 Hz beyond the passband edge, and the tail of a keyed element
 *     under -40 dB within 30 ms (the narrow elliptic took 59 ms, which is
 *     why 200-400 Hz are Butterworth).
 *   - A carrier on the dial comes out at the configured pitch.
 *   - AGC: a strong keyed signal over band noise, 20 WPM. Between elements
 *     and characters the audio noise must stay within 3 dB of its level
 *     under the signal; the v8.05 peak follower let it rise ~17 dB in a dit
 *     gap. After the hang the gain recovers, and the leading edge of a
 *     strong element after silence does not clip (look-ahead). */
#include "radio_dsp.h"
#include "radio_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define TAU 6.283185307179586
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static const uint16_t bandwidths[RADIO_CW_BW_COUNT] = RADIO_CW_BW_TABLE;

static void cw_init(radio_dsp_t *s, uint16_t pitch, uint8_t bw, uint8_t agc)
{
    RADIO_DspInit(s, (float)FS, 0.0f, RADIO_RX_CW);
    RADIO_DspSetCw(s, pitch, bw, agc);
}

/* Channel power (before the AGC) for a steady carrier `hz` from the dial. */
static double channel_level(uint8_t bw, double hz)
{
    radio_dsp_t s;
    cw_init(&s, 600U, bw, RADIO_CW_AGC_SLOW);
    double e = 0.0;
    unsigned n = 0U;
    for (unsigned k = 0U; k < 48000U; ++k) {
        const double a = TAU * hz * k / FS;
        (void)RADIO_DspDemodulate(&s, (float)(0.01 * cos(a)), (float)(0.01 * sin(a)));
        if (k >= 24000U) { e += RADIO_DspLastPower(&s); ++n; }
    }
    return sqrt(e / n) / 0.01;
}

/* Time from key-up until the channel envelope stays 40 dB under its keyed
 * level: 60 ms element at the pitch, 5 ms raised-cosine edges. */
static double ring_ms(uint8_t bw)
{
    radio_dsp_t s;
    cw_init(&s, 600U, bw, RADIO_CW_AGC_SLOW);
    const unsigned on = 4800U, off = 4800U + 2880U, ramp = 240U, total = 24000U;
    double keyed = 0.0;
    unsigned last_loud = off + ramp;
    for (unsigned k = 0U; k < total; ++k) {
        double env = 0.0;
        if (k >= on && k < on + ramp) env = 0.5 - 0.5 * cos(M_PI * (k - on) / ramp);
        else if (k >= on + ramp && k < off) env = 1.0;
        else if (k >= off && k < off + ramp) env = 0.5 + 0.5 * cos(M_PI * (k - off) / ramp);
        (void)RADIO_DspDemodulate(&s, (float)(0.01 * env), 0.0f);
        const double level = sqrt(RADIO_DspLastPower(&s));
        if (k == off - 1U) keyed = level;
        if (k >= off + ramp && level > keyed * 0.01) last_loud = k;
    }
    return (last_loud - (off + ramp)) * 1000.0 / FS;
}

static uint64_t rng = 88172645463325252ULL;
static double gauss(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    const double u = ((rng >> 11) + 1.0) / 9007199254740993.0;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    const double v = (rng >> 11) / 9007199254740992.0;
    return sqrt(-2.0 * log(u)) * cos(TAU * v);
}

int main(void)
{
    /* Filters. */
    for (uint8_t b = 0U; b < RADIO_CW_BW_COUNT; ++b) {
        const double bw = bandwidths[b], edge = bw / 2.0;
        const double ref = channel_level(b, 0.0);
        double half6 = 0.0;
        for (double f = 0.0; f < 2000.0; f += 2.0)
            if (channel_level(b, f) < ref * 0.5) { half6 = f; break; }
        const double width = 2.0 * half6;
        /* Since v8.08 every width is a soft-edged Butterworth: a signal
         * drifting past the edge fades (at most 20 dB down 20 % past it)
         * instead of dropping off a brick wall, and one full width from
         * the centre it is still 45 dB down. */
        const double fade = 20.0 * log10(ref / fmax(channel_level(b, 1.2 * edge), 1e-12));
        const double reject = 20.0 * log10(ref / fmax(channel_level(b, bw), 1e-12));
        const double ring = ring_ms(b);
        printf("PASS? %4.0f Hz filter: -6 dB width %4.0f Hz, %5.1f dB at 1.2 x edge, %5.1f dB one width off, rings %4.1f ms to -40 dB\n",
               bw, width, fade, reject, ring);
        CHECK(fabs(width - bw) <= 0.12 * bw, "%u Hz filter: -6 dB width %.0f Hz", (unsigned)bw, width);
        CHECK(fade <= 20.0, "%u Hz filter: a cliff, %.1f dB at 1.2 x edge", (unsigned)bw, fade);
        CHECK(reject >= 45.0, "%u Hz filter: only %.1f dB one width off", (unsigned)bw, reject);
        CHECK(ring <= 30.0, "%u Hz filter rings %.1f ms", (unsigned)bw, ring);
    }
    CHECK(bandwidths[4] == 700U && RADIO_CW_BW_DEFAULT_HZ == 700U, "700 Hz is the default CW filter (v8.08)");

    /* Pitch: a carrier on the dial sounds at the configured pitch. */
    for (uint16_t pitch = RADIO_CW_PITCH_MIN_HZ; pitch <= RADIO_CW_PITCH_MAX_HZ; pitch += 100U) {
        radio_dsp_t s;
        cw_init(&s, pitch, 3U, RADIO_CW_AGC_SLOW);
        float last = 0.0f;
        unsigned crossings = 0U;
        for (unsigned k = 0U; k < 48000U; ++k) {
            const float out = RADIO_DspDemodulate(&s, 0.01f, 0.0f);
            if (k > 9600U && last < 0.0f && out >= 0.0f) ++crossings;
            last = out;
        }
        const double hz = crossings / (38400.0 / FS);
        CHECK(fabs(hz - pitch) < 3.0, "pitch %u Hz sounds at %.1f Hz", (unsigned)pitch, hz);
    }

    /* AGC: 20 WPM dits, the carrier about 40 dB over the band noise in the
     * 500 Hz filter. Warm up on 24 elements with 1- and 3-dit gaps, key one
     * more, then measure the noise audio across the 3-dit gap that follows,
     * in four 30 ms slices after the filter and look-ahead tails: with the
     * gain held they stay level. */
    {
        radio_dsp_t s;
        cw_init(&s, 600U, 3U, RADIO_CW_AGC_SLOW);
        s.agc = RADIO_DSP_AGC_MAX_GAIN;
        const double sigma = 5e-3, amp = 0.1; /* ~37 dB over the noise in 500 Hz */
        const unsigned dit = 2880U; /* 60 ms */
        for (unsigned el = 0U; el < 24U; ++el) {
            const unsigned gap = (el % 4U == 3U) ? 3U * dit : dit;
            for (unsigned k = 0U; k < dit + gap; ++k)
                (void)RADIO_DspDemodulate(&s, (float)((k < dit ? amp : 0.0) + sigma * gauss()), (float)(sigma * gauss()));
        }
        for (unsigned k = 0U; k < dit; ++k)
            (void)RADIO_DspDemodulate(&s, (float)(amp + sigma * gauss()), (float)(sigma * gauss()));
        const unsigned lead = RADIO_DSP_CW_LOOKAHEAD + 960U;
        double gap_min = 1e30, gap_max = 0.0;
        for (unsigned seg = 0U; seg < 6U; ++seg) {
            double p = 0.0;
            for (unsigned k = 0U; k < 1440U; ++k) {
                const float out = RADIO_DspDemodulate(&s, (float)(sigma * gauss()), (float)(sigma * gauss()));
                if (seg * 1440U + k >= lead) p += (double)out * out;
            }
            if (seg > 0U) { if (p < gap_min) gap_min = p; if (p > gap_max) gap_max = p; }
        }
        const double swell = 10.0 * log10(gap_max / gap_min);
        printf("PASS? SLOW AGC: band noise across a 180 ms character gap varies %.1f dB\n", swell);
        CHECK(swell < 3.0, "SLOW AGC: noise swells %.1f dB in a character gap", swell);
    }
    /* After the hang the gain comes back: 2 s of noise alone restores it by
     * about 15 dB/s x (2 - 0.25) s. */
    {
        radio_dsp_t s;
        cw_init(&s, 600U, 3U, RADIO_CW_AGC_SLOW);
        s.agc = 1.0f;
        for (unsigned k = 0U; k < 9600U; ++k) (void)RADIO_DspDemodulate(&s, 0.1f, 0.0f); /* strong carrier */
        const float held = s.agc;
        for (unsigned k = 0U; k < 96000U; ++k) (void)RADIO_DspDemodulate(&s, 0.0f, 0.0f);
        const double rise = 20.0 * log10(s.agc / held);
        printf("PASS? SLOW AGC recovers %.1f dB in 2 s after a strong carrier\n", rise);
        CHECK(rise > 20.0 && rise < 32.0, "SLOW AGC recovered %.1f dB in 2 s", rise);
    }
    /* Look-ahead: a strong element after silence at full gain. */
    {
        radio_dsp_t s;
        cw_init(&s, 600U, 3U, RADIO_CW_AGC_SLOW);
        s.agc = RADIO_DSP_AGC_MAX_GAIN;
        float peak = 0.0f;
        for (unsigned k = 0U; k < 9600U; ++k) {
            const double env = k < 2400U ? 0.0 : (k < 2640U ? 0.5 - 0.5 * cos(M_PI * (k - 2400U) / 240.0) : 1.0);
            const float out = RADIO_DspDemodulate(&s, (float)(0.3 * env), 0.0f);
            if (fabsf(out) > peak) peak = fabsf(out);
        }
        printf("PASS? leading edge of a -10 dBFS element after silence peaks at %.2f FS\n", (double)peak);
        CHECK(peak < 0.99f, "leading edge clipped (%.2f)", (double)peak);
    }
    if (failures) return 1;
    puts("CW receive filters, pitch and hang AGC: PASS");
    return 0;
}

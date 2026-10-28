/* Host test for radio_spectrum_dsp.h, the arithmetic behind the waterfall:
 * the 512-point FFT against a direct DFT, the Q15 window tables and their
 * dBFS calibration, the WIDE view's per-bin I/Q delay, the fast log, the
 * colour levels and the half-band decimator the zoomed spans capture
 * through. */

#include "radio_spectrum_dsp.h"

#include <assert.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static void test_fft_matches_dft(void)
{
    static float x[2U * SPEC_FFT_N], ref[2U * SPEC_FFT_N];
    srand(7U);
    for (unsigned n = 0U; n < 2U * SPEC_FFT_N; ++n) ref[n] = x[n] = frand();
    spec_fft512(x);
    double worst = 0.0, scale = 0.0;
    for (unsigned k = 0U; k < SPEC_FFT_N; ++k) {
        double re = 0.0, im = 0.0;
        for (unsigned n = 0U; n < SPEC_FFT_N; ++n) {
            const double a = -2.0 * M_PI * (double)((k * n) % SPEC_FFT_N) / SPEC_FFT_N;
            re += ref[2U * n] * cos(a) - ref[2U * n + 1U] * sin(a);
            im += ref[2U * n] * sin(a) + ref[2U * n + 1U] * cos(a);
        }
        const unsigned p = spec_bitrev9(k);
        const double dr = x[2U * p] - re, di = x[2U * p + 1U] - im;
        const double e = sqrt(dr * dr + di * di);
        if (e > worst) worst = e;
        const double m = sqrt(re * re + im * im);
        if (m > scale) scale = m;
    }
    printf("fft: worst error %.3g of peak %.3g\n", worst, scale);
    assert(worst < 2e-4 * scale);
    /* bit reversal is an involution over 9 bits */
    for (unsigned k = 0U; k < SPEC_FFT_N; ++k) assert(spec_bitrev9(spec_bitrev9(k)) == k);
    assert(spec_bitrev9(1U) == 256U && spec_bitrev9(511U) == 511U);
}

/* A full-scale complex tone on a bin centre reads 0 dBFS through every
 * window, once the window's db8 is taken off. */
static void test_window_calibration(void)
{
    static float x[2U * SPEC_FFT_N];
    static int16_t win[SPEC_WIN_HALF];
    for (unsigned w = 0U; w < 4U; ++w) {
        spec_window_table(&spec_windows[w], win);
        const int k0 = 37;
        for (unsigned n = 0U; n < SPEC_FFT_N; ++n) {
            const double a = 2.0 * M_PI * k0 * (double)n / SPEC_FFT_N;
            x[2U * n] = (float)cos(a);
            x[2U * n + 1U] = (float)sin(a);
        }
        spec_prepare(x, NULL, win);
        spec_fft512(x);
        const int db8 = spec_db8(spec_power(x, k0)) - spec_windows[w].db8;
        const int far = spec_db8(spec_power(x, k0 + 40)) - spec_windows[w].db8;
        printf("window %u: tone %+.2f dBFS, 40 bins away %+.1f dBFS\n", w, db8 / 8.0, far / 8.0);
        assert(abs(db8) <= 1);
        if (w != 3U) assert(far < -8 * 70); /* every real window is far below at 40 bins */
    }
    /* The table is the window: Hann peaks at the middle, zero at the edge. */
    spec_window_table(&spec_windows[0], win);
    assert(win[0] == 0 && win[256] == 32767 && abs(win[128] - 16384) <= 1);
}

static void test_db8(void)
{
    double worst = 0.0;
    for (double db = -190.0; db <= 60.0; db += 0.37) {
        const float p = (float)pow(10.0, db / 10.0);
        const double e = fabs(spec_db8(p) / 8.0 - db);
        if (e > worst) worst = e;
    }
    printf("db8: worst error %.3f dB (truncation to 1/8 included)\n", worst);
    assert(worst < 0.13);
    assert(spec_db8(0.0f) == SPEC_DB8_MIN && spec_db8(NAN) == SPEC_DB8_MIN);
    assert(spec_level(-800, -800, 400) == 0 && spec_level(-400, -800, 400) == 15);
    assert(spec_level(-600, -800, 400) == 8 && spec_level(-2000, -800, 400) == 0);
}

/* Amplitude of the decimated output for a complex tone at `f` (cycles per
 * input sample), after the filter has settled. */
static double hb_gain(unsigned stages, double f)
{
    spec_hb_t hb[2];
    memset(hb, 0, sizeof(hb));
    double peak = 0.0;
    unsigned outputs = 0U;
    for (unsigned n = 0U; n < 8192U; ++n) {
        float i = (float)cos(2.0 * M_PI * f * n), q = (float)sin(2.0 * M_PI * f * n);
        if (!spec_hb_push(&hb[0], i, q, &i, &q)) continue;
        if (stages > 1U && !spec_hb_push(&hb[1], i, q, &i, &q)) continue;
        if (++outputs < 64U) continue;
        const double m = sqrt((double)i * i + (double)q * q);
        if (m > peak) peak = m;
    }
    return peak;
}

static void test_halfband(void)
{
    /* Impulse response: 0.5 at the centre, the pairs symmetric around it. */
    spec_hb_t hb;
    memset(&hb, 0, sizeof(hb));
    float h[32], i, q;
    unsigned got = 0U;
    for (unsigned n = 0U; n < 64U && got < 32U; ++n) {
        if (spec_hb_push(&hb, n == 1U ? 1.0f : 0.0f, 0.0f, &i, &q)) h[got++] = i;
        (void)q;
    }
    /* Outputs sample the response at odd offsets from the impulse: every
     * output carries one of the pair taps, and the centre shows up when the
     * impulse is on the even phase instead. */
    double sum = 0.0;
    for (unsigned n = 0U; n < got; ++n) sum += h[n];
    memset(&hb, 0, sizeof(hb));
    for (unsigned n = 0U; n < 64U; ++n)
        if (spec_hb_push(&hb, n == 0U ? 1.0f : 0.0f, 0.0f, &i, &q)) sum += i;
    printf("halfband: dc gain %.5f\n", sum);
    assert(fabs(sum - 1.0) < 0.002);
    const double pass = hb_gain(1U, 0.19), edge = hb_gain(1U, 0.198), stop = hb_gain(1U, 0.31);
    const double stop2 = hb_gain(2U, 0.155), pass2 = hb_gain(2U, 0.045);
    printf("halfband: 0.19 fS %.4f dB, 0.198 %.4f dB, 0.31 %.1f dB; two stages: pass %.3f dB, fold %.1f dB\n",
           20 * log10(pass), 20 * log10(edge), 20 * log10(stop), 20 * log10(pass2), 20 * log10(stop2));
    assert(fabs(20 * log10(pass)) < 0.05 && fabs(20 * log10(edge)) < 0.05);
    assert(20 * log10(stop) < -58.0);
    assert(fabs(20 * log10(pass2)) < 0.05 && 20 * log10(stop2) < -58.0);
}

/* Image of a raw tone whose Q leads by `skew` samples (as REV1's does,
 * 0.657 at 170.8 kHz), corrected with the radio's own model: dB under the
 * tone at its mirror bin. `linear` instead delays Q in time with the
 * front end's two-tap interpolator, as the waterfall did before. */
static double image_db(int k0, float skew, int linear)
{
    static float x[2U * SPEC_FFT_N];
    static int16_t win[SPEC_WIN_HALF];
    spec_window_table(&spec_windows[1], win);
    const double w = 2.0 * M_PI * (k0 + 0.3) / SPEC_FFT_N; /* off a bin centre */
    float prev = 0.0f;
    for (unsigned n = 0U; n < SPEC_FFT_N; ++n) {
        x[2U * n] = (float)cos(w * n);
        float q = (float)sin(w * (n + skew));
        if (linear) { const float c = q; q += skew * (prev - q); prev = c; }
        x[2U * n + 1U] = q;
    }
    spec_iq_t iq = {1.0f, 1.0f, 1.0f, skew, (uint8_t)!linear, 1U};
    spec_prepare(x, &iq, win);
    spec_fft512(x);
    spec_iq_delay_bins(x, &iq);
    return (spec_db8(spec_power(x, k0)) - spec_db8(spec_power(x, -k0))) / 8.0;
}

static void test_iq_delay_bins(void)
{
    /* 333.7 Hz bins at 170.8 kHz: 6, 20, 40 and 60 kHz from the LO. */
    static const int bins[4] = {18, 60, 120, 180};
    for (unsigned j = 0U; j < 4U; ++j) {
        const double exact = image_db(bins[j], 0.657f, 0), old = image_db(bins[j], 0.657f, 1);
        const double neg = image_db(-bins[j], 0.657f, 0);
        printf("iq delay: tone %5.1f kHz, image %5.1f dB down (%5.1f below the LO), two-tap %5.1f dB\n",
               bins[j] * 170.8333 / 512.0, exact, neg, old);
        assert(exact > 75.0 && neg > 75.0);
    }
    /* A delay on I is the same turn the other way. */
    static float x[2U * SPEC_FFT_N];
    static int16_t win[SPEC_WIN_HALF];
    spec_window_table(&spec_windows[1], win);
    const double w = 2.0 * M_PI * 40.3 / SPEC_FFT_N;
    for (unsigned n = 0U; n < SPEC_FFT_N; ++n) {
        x[2U * n] = (float)cos(w * (n + 0.4));
        x[2U * n + 1U] = (float)sin(w * n);
    }
    spec_iq_t iq = {1.0f, 1.0f, 1.0f, 0.4f, 1U, 0U};
    spec_prepare(x, &iq, win);
    spec_fft512(x);
    spec_iq_delay_bins(x, &iq);
    assert((spec_db8(spec_power(x, 40)) - spec_db8(spec_power(x, -40))) / 8.0 > 75.0);
}

int main(void)
{
    test_fft_matches_dft();
    test_window_calibration();
    test_iq_delay_bins();
    test_db8();
    test_halfband();
    puts("spectrum_dsp: ok");
    return 0;
}

#include "radio_iq_fit.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* QSD I/Q imbalance measurement and correction, against a modelled receiver.
 *
 * The model is the one the closed form assumes and the one the hardware
 * produces: a carrier delta hertz from the LO arrives as
 *     I = A cos(w n)
 *     Q = A (1 + g) sin(w n + theta)
 * where g is the amplitude imbalance of the two QSD branches (T401, U9/U12,
 * the two RC integrators) and theta is their departure from 90 degrees.
 *
 * Every case proves the round trip rather than just the arithmetic: solve for
 * the coefficients, apply the expression RADIO_RxFrontSample actually uses,
 * and measure what is left. A sign error anywhere makes the residual worse
 * instead of better and cannot survive that. */

#define RATE_HZ      177083.0
#define SAMPLES      65536U
#define AMPLITUDE    0.05   /* about -26 dBFS, as the 100 k injector gives */
#define TWO_PI       6.283185307179586
#define DEG          (TWO_PI / 360.0)

static double lcg_state = 1.0;
static double uniform(void)
{
    lcg_state = fmod(lcg_state * 16807.0, 2147483647.0);
    return lcg_state / 2147483647.0;
}
static double gaussian(void)
{
    double u = uniform(), v = uniform();
    return sqrt(-2.0 * log(u + 1e-300)) * cos(TWO_PI * v);
}

typedef struct {
    double g, theta;        /* the imbalance to synthesise */
    double amplitude, cycles, phi0;
    double noise;           /* per-sample Gaussian, both channels */
    double tone_cycles, tone_amp; /* an off-channel carrier, balanced */
    double dc_i, dc_q;      /* op-amp offset and LO leakage */
    int conjugate;          /* I/Q swapped, as a mis-set RX IQ INVERT gives */
} model_t;

static void feed(radio_iq_corr_t *c, const model_t *m, unsigned samples)
{
    const double w = TWO_PI * m->cycles;
    const double wt = TWO_PI * m->tone_cycles;
    for (unsigned n = 0U; n < samples; ++n) {
        double phase = w * (double)n + m->phi0;
        double i = m->amplitude * cos(phase);
        double q = m->amplitude * (1.0 + m->g) * sin(phase + m->theta);
        if (m->tone_amp != 0.0) {
            /* Through the same detector, so it carries the same imbalance. */
            double t = wt * (double)n + 0.9;
            i += m->tone_amp * cos(t);
            q += m->tone_amp * (1.0 + m->g) * sin(t + m->theta);
        }
        i += m->dc_i;
        q += m->dc_q;
        if (m->noise != 0.0) { i += m->noise * gaussian(); q += m->noise * gaussian(); }
        if (m->conjugate) q = -q;
        RADIO_IqCorrAdd(c, (float)i, (float)q);
    }
}

/* One measurement of the model through the correlator, with an optional
 * correction already in place -- the verification pass. */
static uint8_t measure(const model_t *m, uint8_t invert, float q_gain, float skew_samples,
                       radio_iq_result_t *out)
{
    radio_iq_corr_t c;
    RADIO_IqCorrInit(&c, (float)m->cycles, SAMPLES);
    RADIO_IqCorrCorrection(&c, invert, q_gain, skew_samples);
    feed(&c, m, SAMPLES);
    assert(RADIO_IqCorrDone(&c));
    return RADIO_IqSolve(&c, out);
}

/* The two sideband levels the model must show with no correction. An
 * imbalanced pair splits the carrier as A |1 + k| / 2 and A |1 - conj(k)| / 2
 * for k = (1 + g) e^(j theta), so the wanted bin is not at A either.
 * Independent of the code under test. */
static void expected_sidebands(double g, double theta, double amplitude,
                               double *wanted_db, double *image_db)
{
    double kr = (1.0 + g) * cos(theta), ki = (1.0 + g) * sin(theta);
    double nr = 1.0 - kr, ni = ki;          /* 1 - conj(k) */
    double dr = 1.0 + kr, di = ki;          /* 1 + k */
    double num = sqrt(nr * nr + ni * ni), den = sqrt(dr * dr + di * di);
    if (wanted_db) *wanted_db = 20.0 * log10(amplitude * den / 2.0);
    if (image_db) *image_db = -20.0 * log10(num / den);
}

static void delta_test(void)
{
    /* SYSCLK cancels, so the exact reference is the rational expression. */
    for (unsigned div = 14U; div <= 15U; ++div) {
        const uint32_t periods[4] = {767U, 538U, 1511U, 2863U}; /* 40, 30, 80, 160 m */
        for (unsigned k = 0U; k < 4U; ++k) {
            const uint32_t p = periods[k];
            for (uint32_t off = 1U; off <= 8U; ++off) {
                double want = 2048.0 * div * ((double)p - (double)(p - off)) /
                              ((double)p * (double)(p - off));
                float got = RADIO_IqDeltaCycles(p, p - off, div);
                /* Two float divisions: a few parts in ten million, which is
                 * milliradians of drift over a whole run. */
                assert(fabs((double)got - want) < 1e-6 * want);
                assert(RADIO_IqDeltaCycles(p, p + off, div) < 0.0f);
            }
        }
    }
    assert(RADIO_IqDeltaCycles(0U, 100U, 15U) == 0.0f);
    assert(RADIO_IqDeltaCycles(100U, 100U, 15U) == 0.0f);

    /* One HRTIM tick is about 9.3 kHz at 7 MHz and 18.8 kHz at 10.1 MHz, so
     * the smallest usable offset differs per band. The window is 6-45 kHz:
     * clear of the PCM1821's ~44 Hz DC-removal filter and inside the
     * linear-phase filter's flat +-50 kHz. */
    struct { uint32_t p_lo; double low_khz, high_khz; } bands[] = {
        {767U, 8.0, 11.0},     /* 40 m: one tick */
        {538U, 17.0, 21.0},    /* 30 m: one tick */
        {1511U, 6.0, 10.0},    /* 80 m: a few ticks */
        {2863U, 6.0, 9.0},     /* 160 m: a dozen ticks */
    };
    for (unsigned k = 0U; k < sizeof bands / sizeof bands[0]; ++k) {
        uint32_t p_test = 0U;
        assert(RADIO_IqChoosePeriod(bands[k].p_lo, 15U, (float)RATE_HZ, 6000.0f, 45000.0f, &p_test));
        assert(p_test < bands[k].p_lo); /* above the LO: wanted bin positive */
        double delta = fabs((double)RADIO_IqDeltaCycles(bands[k].p_lo, p_test, 15U)) * RATE_HZ;
        assert(delta > bands[k].low_khz * 1000.0 && delta < bands[k].high_khz * 1000.0);
        assert(p_test >= RADIO_IQ_PERIOD_MIN && p_test <= RADIO_IQ_PERIOD_MAX);
    }
    /* Near the HRTIM ceiling one tick already overshoots the window. */
    uint32_t unused = 0U;
    assert(!RADIO_IqChoosePeriod(200U, 15U, (float)RATE_HZ, 6000.0f, 45000.0f, &unused));
    /* At the ceiling itself the period above the LO would be below the HRTIM
     * minimum, so the injector has to go the other way. */
    uint32_t edge = 0U;
    assert(RADIO_IqChoosePeriod(RADIO_IQ_PERIOD_MIN, 15U, (float)RATE_HZ,
                                6000.0f, 200000.0f, &edge));
    assert(edge == RADIO_IQ_PERIOD_MIN + 1U);
    /* The upper bound is a contract on what the function may hand to
     * radio_clocks.c rather than a reachable branch: reaching it needs the
     * period below the LO to be illegal at the same time, which cannot happen
     * for any p_lo the search accepts. It stays because the search order is
     * what makes it unreachable, and that is easy to change. */
    assert(!RADIO_IqChoosePeriod(RADIO_IQ_PERIOD_MIN - 1U, 15U, (float)RATE_HZ, 6000.0f, 45000.0f, &unused));
    assert(!RADIO_IqChoosePeriod(767U, 15U, 0.0f, 6000.0f, 45000.0f, &unused));
    assert(!RADIO_IqChoosePeriod(767U, 15U, (float)RATE_HZ, 45000.0f, 6000.0f, &unused));
}

static void solve_test(void)
{
    static const double gains[] = {0.0, 0.01, 0.05, -0.04, 0.12, -0.10};
    static const double phases_deg[] = {0.0, 0.3, 1.5, -2.0, 5.0, -7.5};
    double worst_residual = 1e9, worst_amp = 0.0, worst_phase = 0.0;
    for (unsigned a = 0U; a < sizeof gains / sizeof gains[0]; ++a) {
        for (unsigned p = 0U; p < sizeof phases_deg / sizeof phases_deg[0]; ++p) {
            model_t m;
            memset(&m, 0, sizeof m);
            m.g = gains[a];
            m.theta = phases_deg[p] * DEG;
            m.amplitude = AMPLITUDE;
            m.cycles = 9300.0 / RATE_HZ;
            m.phi0 = 0.7 + 0.31 * (double)(a * 6U + p); /* arbitrary start phase */
            m.dc_i = 0.002; m.dc_q = -0.003;            /* offsets and LO leakage */
            radio_iq_result_t r;
            assert(measure(&m, 0U, 1.0f, 0.0f, &r));
            assert(r.valid && !r.inverted && !r.clipped);
            /* The bins themselves. */
            double want_db = 0.0, want_rejection = 0.0;
            expected_sidebands(m.g, m.theta, AMPLITUDE, &want_db, &want_rejection);
            assert(fabs((double)r.wanted_dbfs - want_db) < 0.05);
            if (gains[a] != 0.0 || phases_deg[p] != 0.0)
                assert(fabs((double)r.image_db - want_rejection) < 0.5);
            /* The imbalance. */
            double amp_err = fabs((double)r.amp_db - 20.0 * log10(1.0 + m.g));
            double phase_err = fabs((double)r.phase_deg - phases_deg[p]);
            assert(amp_err < 0.02 && phase_err < 0.05);
            if (amp_err > worst_amp) worst_amp = amp_err;
            if (phase_err > worst_phase) worst_phase = phase_err;
            assert(fabs((double)r.i_gain - 1.0) < 1e-6);
            /* DC is reported, and does not disturb the bins. */
            assert(fabs((double)r.dc_i - 0.002) < 2e-4 && fabs((double)r.dc_q + 0.003) < 2e-4);
            /* Verification pass: the residual is measured, not predicted. */
            radio_iq_result_t v;
            assert(measure(&m, 0U, r.q_gain, r.skew_samples, &v));
            assert(v.image_db > 80.0);
            if (v.image_db < worst_residual) worst_residual = v.image_db;
        }
    }
    printf("solve: worst amplitude error %.3f dB, phase error %.3f deg, "
           "worst corrected image %.1f dB\n", worst_amp, worst_phase, worst_residual);
}

static void inverted_test(void)
{
    /* A board whose I and Q are swapped puts the carrier in the negative bin.
     * The fix is RX IQ INVERT, which conjugates the stream and exchanges the
     * bins; the same coefficients must come back out. */
    model_t m;
    memset(&m, 0, sizeof m);
    m.g = 0.06; m.theta = 3.2 * DEG;
    m.amplitude = AMPLITUDE;
    m.cycles = 9300.0 / RATE_HZ;
    m.phi0 = 1.1;
    m.conjugate = 1;
    radio_iq_result_t r;
    assert(measure(&m, 0U, 1.0f, 0.0f, &r));
    assert(r.valid && r.inverted);
    assert(fabs((double)r.amp_db - 20.0 * log10(1.0 + m.g)) < 0.02);
    assert(fabs((double)r.phase_deg - 3.2) < 0.05);
    double want_rejection = 0.0;
    expected_sidebands(m.g, m.theta, AMPLITUDE, NULL, &want_rejection);
    assert(fabs((double)r.image_db - want_rejection) < 0.5);
    /* Setting the invert and the coefficients together clears it. */
    radio_iq_result_t v;
    assert(measure(&m, 1U, r.q_gain, r.skew_samples, &v));
    assert(!v.inverted && v.image_db > 80.0);
    /* And the same board with the invert already set reports normal. */
    m.conjugate = 0;
    radio_iq_result_t n;
    assert(measure(&m, 1U, 1.0f, 0.0f, &n));
    assert(n.valid && n.inverted);
}

static void fixed_skew_test(void)
{
    /* The board defect is a time skew, so its angle must scale with offset.
     * One stored sample delay must remove it at every usable calibration
     * frequency, including the top of the PCM1821's flat input span. */
    static const double hz[] = {7500.0, 9381.0, 18830.0, 36043.0, 45000.0};
    static const double skew[] = {0.63, -0.63};
    for (unsigned sign = 0U; sign < sizeof skew / sizeof skew[0]; ++sign) {
        for (unsigned f = 0U; f < sizeof hz / sizeof hz[0]; ++f) {
            model_t m;
            memset(&m, 0, sizeof m);
            m.g = 0.03;
            m.cycles = hz[f] / RATE_HZ;
            m.theta = TWO_PI * m.cycles * skew[sign];
            m.amplitude = AMPLITUDE;
            m.phi0 = 0.37;
            radio_iq_result_t raw, corrected;
            assert(measure(&m, 0U, 1.0f, 0.0f, &raw));
            assert(fabs((double)raw.skew_samples - skew[sign]) < 0.002);
            assert(measure(&m, 0U, raw.q_gain, raw.skew_samples, &corrected));
            assert(corrected.image_db > 45.0f);
        }
    }
}

static void noise_test(void)
{
    /* With an antenna instead of a dummy load, the measurement competes with
     * band noise and with off-air carriers. The Hann window is what keeps a
     * strong nearby carrier out of the bins. */
    model_t m;
    memset(&m, 0, sizeof m);
    m.g = 0.04; m.theta = 2.5 * DEG;
    m.amplitude = AMPLITUDE;
    m.cycles = 9300.0 / RATE_HZ;
    m.phi0 = 0.4;
    m.noise = 0.002;                       /* about 28 dB below the injection */
    m.tone_amp = 0.02;                     /* an S9 station 900 Hz away */
    m.tone_cycles = (9300.0 + 900.0) / RATE_HZ;
    radio_iq_result_t r;
    assert(measure(&m, 0U, 1.0f, 0.0f, &r));
    assert(r.valid && !r.inverted);
    assert(fabs((double)r.amp_db - 20.0 * log10(1.0 + m.g)) < 0.15);
    assert(fabs((double)r.phase_deg - 2.5) < 0.5);
    radio_iq_result_t v;
    assert(measure(&m, 0U, r.q_gain, r.skew_samples, &v));
    printf("noise: corrected image %.1f dB with band noise and an off-channel carrier\n",
           v.image_db);
    assert(v.image_db > 45.0);

    /* Two carriers as strong as the injection: one 60 Hz above it, one 60 Hz
     * from its mirror below the LO. The first is harmless whatever the window,
     * because it reaches the detector through the same imbalance and obeys the
     * same Y = -j k X relation. The second is the one that matters: its
     * conjugate component lands 60 Hz -- about twenty bins -- from the same
     * bin, carrying -conj(k). This pair is what the Hann window is for. */
    model_t close;
    memset(&close, 0, sizeof close);
    close.g = 0.04; close.theta = 2.5 * DEG;
    close.amplitude = AMPLITUDE;
    close.cycles = 9300.0 / RATE_HZ;
    close.phi0 = 2.2;
    close.tone_amp = AMPLITUDE;                        /* as strong as the injection */
    close.tone_cycles = (9300.0 + 60.0) / RATE_HZ;     /* 60 Hz away, ~22 bins */
    radio_iq_result_t n;
    assert(measure(&close, 0U, 1.0f, 0.0f, &n));
    assert(n.valid && !n.inverted);
    printf("close carrier: amplitude error %.4f dB, phase error %.4f deg\n",
           fabs((double)n.amp_db - 20.0 * log10(1.0 + close.g)),
           fabs((double)n.phase_deg - 2.5));
    assert(fabs((double)n.amp_db - 20.0 * log10(1.0 + close.g)) < 0.02);
    assert(fabs((double)n.phase_deg - 2.5) < 0.05);
    radio_iq_result_t cv;
    assert(measure(&close, 0U, n.q_gain, n.skew_samples, &cv));
    assert(cv.image_db > 70.0);
    close.tone_cycles = -(9300.0 + 60.0) / RATE_HZ;    /* mirrored below the LO */
    radio_iq_result_t mirror;
    assert(measure(&close, 0U, 1.0f, 0.0f, &mirror));
    printf("mirror carrier: amplitude error %.4f dB, phase error %.4f deg\n",
           fabs((double)mirror.amp_db - 20.0 * log10(1.0 + close.g)),
           fabs((double)mirror.phase_deg - 2.5));
    assert(fabs((double)mirror.amp_db - 20.0 * log10(1.0 + close.g)) < 0.02);
    assert(fabs((double)mirror.phase_deg - 2.5) < 0.05);

    /* Noise alone, with nothing injected, must not look like a calibration. */
    model_t quiet;
    memset(&quiet, 0, sizeof quiet);
    quiet.cycles = 9300.0 / RATE_HZ;
    quiet.noise = 0.002;
    radio_iq_result_t q;
    uint8_t solved = measure(&quiet, 0U, 1.0f, 0.0f, &q);
    printf("quiet: wanted %.1f dBFS, image rejection %.1f dB (solved=%u)\n",
           (double)q.wanted_dbfs, (double)q.image_db, (unsigned)solved);
    assert(q.wanted_dbfs < -60.0f);
    assert(fabs((double)q.image_db) < 12.0); /* noise has no preferred bin */
}

/* The correlator takes any run length, and the oscillator it carries loses
 * magnitude a little on every complex multiply. A run four times longer than
 * the firmware's own must still report the level correctly. */
static void long_run_test(void)
{
    const unsigned samples = 1U << 20;
    const double amplitude = 0.05, cycles = 9300.0 / RATE_HZ;
    radio_iq_corr_t c;
    RADIO_IqCorrInit(&c, (float)cycles, samples);
    for (unsigned n = 0U; n < samples; ++n) {
        double phase = TWO_PI * cycles * (double)n + 0.4;
        RADIO_IqCorrAdd(&c, (float)(amplitude * cos(phase)),
                            (float)(amplitude * sin(phase)));
    }
    radio_iq_result_t r;
    assert(RADIO_IqSolve(&c, &r));
    double level_err = fabs((double)r.wanted_dbfs - 20.0 * log10(amplitude));
    printf("long run: %u samples, level error %.4f dB\n", samples, level_err);
    assert(level_err < 0.01);
    assert(fabs((double)r.amp_db) < 0.01 && fabs((double)r.phase_deg) < 0.05);
}

static void guard_test(void)
{
    radio_iq_corr_t c;
    radio_iq_result_t r;
    /* A run that has not finished never solves. */
    RADIO_IqCorrInit(&c, 0.0525f, SAMPLES);
    assert(!RADIO_IqCorrDone(&c));
    assert(!RADIO_IqSolve(&c, &r));
    assert(r.i_gain == 1.0f && r.q_gain == 1.0f && !r.valid);

    /* Digital silence: no carrier, no coefficients. */
    model_t dead;
    memset(&dead, 0, sizeof dead);
    dead.cycles = 0.0525;
    assert(!measure(&dead, 0U, 1.0f, 0.0f, &r));
    assert(!r.valid);

    /* Non-finite samples are ignored rather than poisoning the sums. */
    RADIO_IqCorrInit(&c, 0.0525f, 256U);
    for (unsigned n = 0U; n < 256U; ++n)
        RADIO_IqCorrAdd(&c, n == 7U ? NAN : 0.05f * cosf(0.3299f * (float)n),
                            n == 7U ? INFINITY : 0.05f * sinf(0.3299f * (float)n));
    assert(RADIO_IqCorrDone(&c));
    assert(RADIO_IqSolve(&c, &r));
    assert(isfinite(r.q_gain) && isfinite(r.skew_samples) && isfinite(r.image_db));

    /* Past the run length nothing accumulates. */
    const float before = c.weight;
    RADIO_IqCorrAdd(&c, 1.0f, 1.0f);
    assert(c.weight == before && c.count == 256U);

    /* Clipping is reported. */
    model_t hot;
    memset(&hot, 0, sizeof hot);
    hot.g = 0.02; hot.amplitude = 0.99; hot.cycles = 9300.0 / RATE_HZ;
    assert(measure(&hot, 0U, 1.0f, 0.0f, &r));
    assert(r.clipped);
}

#define PI_D 3.14159265358979323846

/* QMX 8.6.7 "Test ADC I/Q": a short raw capture read as numbers. */
static void snapshot_test(void)
{
    enum { N = 64 };
    static int32_t pairs[N * 2];
    radio_iq_snapshot_t st;

    /* A balanced detector: equal amplitude, exactly 90 degrees apart. */
    for (unsigned n = 0U; n < N; ++n) {
        const double w = 2.0 * PI_D * 5.0 * n / (double)N; /* whole cycles */
        pairs[2U * n] = (int32_t)(0.25 * 2147483648.0 * cos(w));
        pairs[2U * n + 1U] = (int32_t)(0.25 * 2147483648.0 * sin(w));
    }
    RADIO_IqSnapshotStats(pairs, N, &st);
    assert(st.valid && !st.clipped);
    assert(fabsf(st.amp_db) < 0.05f);
    assert(fabsf(st.phase_deg) < 0.5f);
    assert(fabsf(st.i_rms - 0.25f / 1.41421356f) < 0.01f);
    assert(st.i_max > 0 && st.i_min < 0 && st.q_max > 0 && st.q_min < 0);

    /* A known imbalance has to come back as itself. The range goes out to
     * +-40 degrees on purpose: QMX's screen exists to catch gross faults like
     * a miswired trifilar transformer, and sin(theta) is only theta for small
     * angles -- dropping the arcsine still looks right at 12 degrees and is
     * 3 degrees wrong at 40. */
    for (double g_db = -3.0; g_db <= 3.01; g_db += 1.5) {
        for (double theta = -40.0; theta <= 40.01; theta += 8.0) {
            const double gain = pow(10.0, g_db / 20.0);
            const double rad = theta * PI_D / 180.0;
            for (unsigned n = 0U; n < N; ++n) {
                const double w = 2.0 * PI_D * 5.0 * n / (double)N;
                pairs[2U * n] = (int32_t)(0.25 * 2147483648.0 * cos(w));
                pairs[2U * n + 1U] = (int32_t)(0.25 * gain * 2147483648.0 * sin(w + rad));
            }
            RADIO_IqSnapshotStats(pairs, N, &st);
            assert(st.valid);
            assert(fabs(st.amp_db - g_db) < 0.1);
            assert(fabs(st.phase_deg - theta) < 0.2);
        }
    }

    /* Digital silence is not a measurement, and saying so is the point: this
     * is what a dead PCM1821 or a missing injection looks like. */
    memset(pairs, 0, sizeof pairs);
    RADIO_IqSnapshotStats(pairs, N, &st);
    assert(!st.valid && !st.clipped && st.i_max == 0 && st.i_min == 0);

    /* A railed pre-amp clips and is flagged, and the peaks say which way. */
    for (unsigned n = 0U; n < N; ++n) {
        pairs[2U * n] = 2147483000;
        pairs[2U * n + 1U] = -2147483000;
    }
    RADIO_IqSnapshotStats(pairs, N, &st);
    assert(st.clipped && st.i_max > 2080374784 && st.q_min < -2080374784);

    /* No samples at all is handled rather than dividing by zero. */
    RADIO_IqSnapshotStats(pairs, 0U, &st);
    assert(!st.valid);
    RADIO_IqSnapshotStats(NULL, N, &st);
    assert(!st.valid);
}

int main(void)
{
    delta_test();
    snapshot_test();
    solve_test();
    inverted_test();
    fixed_skew_test();
    noise_test();
    long_run_test();
    guard_test();
    puts("REV1 QSD I/Q imbalance: offset planning, closed-form solve, "
         "orientation and measured residual: PASS");
    return 0;
}

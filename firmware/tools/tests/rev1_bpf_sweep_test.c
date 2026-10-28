#include "radio_bpf_sweep.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Receive BPF sweep: grid, level codes, crossover fit and the chopped
 * same-period injection detector against a PCM1821-like front end. */

static double lcg_state = 1.0;
static double uniform(void)
{
    lcg_state = fmod(lcg_state * 16807.0, 2147483647.0);
    return lcg_state / 2147483647.0;
}
static double gaussian(void)
{
    double u = uniform(), v = uniform();
    return sqrt(-2.0 * log(u + 1e-300)) * cos(6.283185307179586 * v);
}

/* The runtime's RADIO_SweepPointHz is this with the loaded run's parameters. */
static uint32_t bpf_hz(unsigned k) { return RADIO_SweepGridHz(RADIO_SWEEP_BPF, 0U, k); }

static void grid_test(void)
{
    assert(bpf_hz(0U) == RADIO_SWEEP_FIRST_HZ);
    assert(bpf_hz(RADIO_SWEEP_POINTS - 1U) == RADIO_RX_MAX_HZ);
    assert(bpf_hz(1000U) == RADIO_RX_MAX_HZ);
    double worst = 0.0;
    for (unsigned k = 1U; k < RADIO_SWEEP_POINTS; ++k) {
        double r = (double)bpf_hz(k) / (double)bpf_hz(k - 1U);
        assert(r > 1.0);
        if (r > worst) worst = r;
    }
    assert(worst < 1.033); /* about 3 % between points */

    /* The low-pass axis has to reach down to the 160 m passband, which the
     * band-pass axis does not, and it must still end at the top of the range.
     * Every point has to be somewhere the test clock can actually go. */
    assert(RADIO_SweepGridHz(RADIO_SWEEP_LPF, 0U, 0U) == 1000000UL);
    assert(RADIO_SweepGridHz(RADIO_SWEEP_LPF, 0U, RADIO_SWEEP_POINTS - 1U) == RADIO_RX_MAX_HZ);
    worst = 0.0;
    for (unsigned k = 1U; k < RADIO_SWEEP_POINTS; ++k) {
        double r = (double)RADIO_SweepGridHz(RADIO_SWEEP_LPF, 0U, k) /
                   (double)RADIO_SweepGridHz(RADIO_SWEEP_LPF, 0U, k - 1U);
        assert(r > 1.0);
        if (r > worst) worst = r;
    }
    assert(worst < 1.056);
    /* Each QMX+ low-pass filter's passband must contain several points, or a
     * five-curve plot cannot show where one filter takes over from the next. */
    static const uint32_t corner[5] = {2500000UL, 5600000UL, 10300000UL, 21450000UL, RADIO_RX_MAX_HZ};
    for (unsigned slot = 0U; slot < 5U; ++slot) {
        unsigned inside = 0U;
        const uint32_t low = slot ? corner[slot - 1U] : 1000000UL;
        for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k) {
            const uint32_t hz = RADIO_SweepGridHz(RADIO_SWEEP_LPF, 0U, k);
            if (hz >= low && hz <= corner[slot]) ++inside;
        }
        assert(inside >= 4U);
    }

    /* The per-band axis is linear, centred, and never leaves the range the
     * test clock can reach even on 160 m or at the top of 10 m. */
    static const uint32_t centre[] = {1900000UL, 7100000UL, 14074000UL, 28000000UL};
    for (unsigned b = 0U; b < sizeof(centre) / sizeof(centre[0]); ++b) {
        uint32_t previous = 0U;
        unsigned below = 0U, above = 0U;
        for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k) {
            const uint32_t hz = RADIO_SweepGridHz(RADIO_SWEEP_BAND, centre[b], k);
            assert(hz >= 1000000UL && hz <= RADIO_RX_MAX_HZ);
            assert(hz >= previous);
            previous = hz;
            if (hz < centre[b]) ++below; else ++above;
        }
        /* Both skirts have to be on the plot, not just one. */
        assert(below >= 8U && above >= 8U);
    }
    /* An unset centre must still give a usable axis rather than a division by
     * zero or 64 identical points. */
    assert(RADIO_SweepGridHz(RADIO_SWEEP_BAND, 0U, 0U) <
           RADIO_SweepGridHz(RADIO_SWEEP_BAND, 0U, RADIO_SWEEP_POINTS - 1U));
    assert(RADIO_SweepCode(0.0f) == 255U && RADIO_SweepCode(5.0f) == 255U);
    assert(RADIO_SweepCode(-0.5f) == 254U && RADIO_SweepCode(-127.0f) == 1U);
    assert(RADIO_SweepCode(-300.0f) == 1U && RADIO_SweepCode(NAN) == 1U);
    for (int half = -254; half <= 0; ++half) {
        uint8_t code = RADIO_SweepCode(0.5f * (float)half);
        assert(fabsf(RADIO_SweepCodeDb(code) - 0.5f * (float)half) < 0.01f);
    }
}

/* Series-resonant BPF sections like the QMX 60-15m set, with the deep 20/17m
 * notch seen just above 18.3 MHz in the QMX RF sweep. */
static double bpf_db(unsigned slot, double hz)
{
    static const double f0[4] = {21.2e6, 16.2e6, 10.4e6, 7.0e6};
    static const double q[4] = {1.9, 1.7, 1.0, 2.2};
    double x = hz / f0[slot] - f0[slot] / hz;
    double db = -10.0 * log10(1.0 + q[slot] * q[slot] * x * x);
    if (slot == 1U) db -= 14.0 * exp(-pow((hz - 19.5e6) / 0.6e6, 2.0));
    return db - 40.0;
}

static void fill_levels(uint8_t level[RADIO_SWEEP_POINTS][RADIO_SWEEP_SLOTS], double noise_db)
{
    for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k)
        for (unsigned s = 0U; s < 4U; ++s)
            level[k][s] = RADIO_SweepCode((float)(bpf_db(s, bpf_hz(k)) + noise_db * gaussian()));
}

/* The best contiguous-by-order assignment on a fine axis, for reference. */
static double true_crossover(unsigned low, unsigned high, double from, double to)
{
    double hz = from;
    while (hz < to && bpf_db(low, hz) >= bpf_db(high, hz)) hz *= 1.0005;
    return hz;
}

static void fit_test(void)
{
    uint8_t level[RADIO_SWEEP_POINTS][RADIO_SWEEP_SLOTS];
    uint32_t edges[4], current[4] = {RADIO_RX_MAX_HZ, 18500000UL, 13500000UL, 8500000UL};
    fill_levels(level, 0.0);
    assert(RADIO_SweepFitEdges(level, current, edges));
    assert(edges[0] == RADIO_RX_MAX_HZ);
    assert(edges[3] < edges[2] && edges[2] < edges[1] && edges[1] < edges[0]);
    const double step = 1.033;
    double x32 = true_crossover(3U, 2U, 7.0e6, 12.0e6);
    double x21 = true_crossover(2U, 1U, 10.4e6, 16.2e6);
    double x10 = true_crossover(1U, 0U, 16.2e6, 21.2e6);
    printf("model crossovers %.2f %.2f %.2f MHz, fit %.2f %.2f %.2f MHz\n", x32 / 1e6, x21 / 1e6, x10 / 1e6,
           edges[3] / 1e6, edges[2] / 1e6, edges[1] / 1e6);
    assert(edges[3] > x32 / step && edges[3] < x32 * step);
    assert(edges[2] > x21 / step && edges[2] < x21 * step);
    assert(edges[1] > x10 / step && edges[1] < x10 * step);
    /* The fitted set must keep every QMX ham band on its QMX filter here. */
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    /* BPF selection is what this checks; RADIO_ConfigFilters also needs a
     * covering LPF, so use the full QMX+ set rather than the subset whose
     * toroids happen to be wound on this build. */
    RADIO_ConfigLpfDefaults(&c);
    memcpy(c.bpf_upper_hz, edges, sizeof(edges));
    assert(RADIO_ConfigValid(&c));
    static const struct { uint32_t hz; uint8_t slot; } band[] = {
        {5357000U, 3U}, {7074000U, 3U}, {10136000U, 2U}, {14074000U, 1U}, {18100000U, 1U}, {21074000U, 0U},
    };
    uint8_t lp, bp;
    for (unsigned i = 0U; i < sizeof(band) / sizeof(band[0]); ++i)
        assert(RADIO_ConfigFilters(&c, band[i].hz, &lp, &bp) && bp == band[i].slot);

    /* Measurement noise of +-1 dB moves crossovers by at most a couple of
     * points; the selected path loses little against the per-point best. */
    for (unsigned trial = 0U; trial < 20U; ++trial) {
        fill_levels(level, 1.0);
        assert(RADIO_SweepFitEdges(level, current, edges));
        double loss = 0.0;
        for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k) {
            uint32_t hz = bpf_hz(k);
            memcpy(c.bpf_upper_hz, edges, sizeof(edges));
            assert(RADIO_ConfigFilters(&c, hz, &lp, &bp));
            double best = -1e9;
            for (unsigned s = 0U; s < 4U; ++s) if (bpf_db(s, hz) > best) best = bpf_db(s, hz);
            loss += best - bpf_db(bp, hz);
        }
        assert(loss / RADIO_SWEEP_POINTS < 0.25);
    }

    /* Slot order follows the current edges, whatever the PCB channel. */
    fill_levels(level, 0.0);
    uint8_t swapped[RADIO_SWEEP_POINTS][RADIO_SWEEP_SLOTS] = {{0}};
    for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k) {
        swapped[k][0] = level[k][3]; swapped[k][1] = level[k][2];
        swapped[k][2] = level[k][1]; swapped[k][3] = level[k][0];
    }
    uint32_t reversed[4] = {8500000UL, 13500000UL, 18500000UL, RADIO_RX_MAX_HZ}, edges2[4];
    assert(RADIO_SweepFitEdges(level, current, edges));
    assert(RADIO_SweepFitEdges(swapped, reversed, edges2));
    for (unsigned s = 0U; s < 4U; ++s) assert(edges2[s] == edges[3U - s]);

    /* Unconfigured slots stay unconfigured; two slots still fit. */
    uint32_t two[4] = {0U, 18500000UL, 0U, 8500000UL};
    assert(RADIO_SweepFitEdges(level, two, edges));
    assert(edges[0] == 0U && edges[2] == 0U && edges[1] == RADIO_RX_MAX_HZ && edges[3] > 7000000UL &&
           edges[3] < 17000000UL);
    uint32_t one[4] = {0U, 0U, 0U, 8500000UL};
    edges[0] = 12345U;
    assert(!RADIO_SweepFitEdges(level, one, edges) && edges[0] == 12345U);

    /* Unmeasured points do not vote; with no data nothing is fitted. A flat
     * (tied) region keeps the crossover nearest the current edge. */
    uint8_t empty[RADIO_SWEEP_POINTS][RADIO_SWEEP_SLOTS];
    memset(empty, 0, sizeof(empty));
    assert(!RADIO_SweepFitEdges(empty, current, edges));
    for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k) memset(empty[k], 200, 4U);
    assert(RADIO_SweepFitEdges(empty, current, edges));
    for (unsigned s = 1U; s < 4U; ++s)
        assert(fabs(log((double)edges[s] / (double)current[s])) < log(1.034));
    memcpy(empty, level, sizeof(level));
    for (unsigned k = 0U; k < 10U; ++k) empty[k][2] = 0U; /* partial point */
    assert(RADIO_SweepFitEdges(empty, current, edges));
}

/* Time-domain model of the measurement: the test clock at the LO period puts
 * the carrier at DC with a fixed complex amplitude; the gate toggles at each
 * 64-frame DMA boundary after an IRQ latency; the ADC path adds op-amp DC,
 * LO leakage, an off-channel station, white noise, a slowly settling DC step
 * from the last filter switch, a linear-phase decimation delay (17 samples)
 * and the 0.00025 fS DC-removal high-pass filter. */
typedef struct { double amplitude, phase, noise, interferer, step; unsigned latency; uint8_t inject; } chop_case_t;

static float chop_measure(const chop_case_t *m)
{
    enum { HALF = 64, SETTLE = 40, HALVES = 128, FIRST = 40, TAPS = 35 };
    const double fs = 177083.0, a_hpf = exp(-6.283185307179586 * 0.00025);
    double fir[TAPS], line_i[TAPS] = {0}, line_q[TAPS] = {0}, sum = 0.0;
    for (int t = 0; t < TAPS; ++t) {
        double x = (double)(t - TAPS / 2);
        fir[t] = (x == 0.0 ? 0.9 : sin(0.9 * 3.141592653589793 * x) / (3.141592653589793 * x)) *
                 (0.42 + 0.5 * cos(6.283185307179586 * x / (TAPS - 1)) + 0.08 * cos(12.56637061 * x / (TAPS - 1)));
        sum += fir[t];
    }
    for (int t = 0; t < TAPS; ++t) fir[t] /= sum;
    double hp_i = 0.0, hp_q = 0.0, prev_i = 0.0, prev_q = 0.0;
    radio_chop_t acc;
    memset(&acc, 0, sizeof(acc));
    /* DMA boundary h*64: the IRQ records the gate that drives half h and
     * toggles it; the ADC input sees the new gate 'latency' samples later.
     * PendSV integrates frames >= 40 with that recorded state. */
    uint8_t gate = 0U, recorded = 0U;
    const unsigned total = (SETTLE + HALVES + 24U) * HALF;
    for (unsigned n = 0U; n < total; ++n) {
        const unsigned frame = n % HALF, half = n / HALF;
        if (frame == m->latency) {
            if (half) gate ^= 1U;
            recorded = gate;
        }
        const double on = (m->inject && gate) ? 1.0 : 0.0;
        const double t = n / fs;
        const double drift = m->step * exp(-t / 0.004);
        const double in_i = on * m->amplitude * cos(m->phase) + 0.012 + drift +
                            m->interferer * cos(6.283185307179586 * 3170.0 * t) + m->noise * gaussian();
        const double in_q = on * m->amplitude * sin(m->phase) - 0.007 - 0.5 * drift +
                            m->interferer * sin(6.283185307179586 * 3170.0 * t) + m->noise * gaussian();
        memmove(line_i + 1, line_i, sizeof(double) * (TAPS - 1));
        memmove(line_q + 1, line_q, sizeof(double) * (TAPS - 1));
        line_i[0] = in_i; line_q[0] = in_q;
        double di = 0.0, dq = 0.0;
        for (int k = 0; k < TAPS; ++k) { di += fir[k] * line_i[k]; dq += fir[k] * line_q[k]; }
        hp_i = a_hpf * (hp_i + di - prev_i); prev_i = di;
        hp_q = a_hpf * (hp_q + dq - prev_q); prev_q = dq;
        if (half >= SETTLE + 24U && half < SETTLE + 24U + HALVES && frame >= FIRST)
            RADIO_ChopAdd(&acc, recorded, RADIO_ChopWeight(half - (SETTLE + 24U), HALVES), (float)hp_i, (float)hp_q);
    }
    return RADIO_ChopDbfs(&acc);
}

static void chop_test(void)
{
    chop_case_t m = {0.01, 0.3, 0.002, 0.02, 0.05, 6U, 1U};
    float base = chop_measure(&m);
    printf("chop detector: -40 dBFS carrier reads %.2f dBFS\n", base);
    /* The HPF droop over a 64-sample half is a small fixed scale. With an
     * off-channel station 6 dB above the carrier, noise and a settling DC
     * step, the reading holds 0.2 dB over carrier phase; IRQ latency only
     * moves the droop (a constant offset on a given board). */
    assert(fabsf(base + 40.0f) < 0.2f);
    for (unsigned i = 0U; i < 8U; ++i) {
        m.phase = 0.785398 * i;
        assert(fabsf(chop_measure(&m) + 40.0f) < 0.2f);
    }
    m.phase = 0.3;
    m.amplitude = 0.01 * pow(10.0, -12.0 / 20.0);
    assert(fabsf(chop_measure(&m) + 52.0f) < 0.3f);
    m.amplitude = 0.01;
    for (unsigned latency = 0U; latency <= 20U; latency += 5U) {
        m.latency = latency;
        assert(fabsf(chop_measure(&m) + 40.0f) < 0.4f);
    }
    m.latency = 6U;
    m.inject = 0U;
    float floor = chop_measure(&m);
    printf("chop detector floor with 2e-3 noise, 0.02 off-channel tone: %.1f dBFS\n", floor);
    assert(floor < base - 34.0f);
    m.inject = 1U;
    m.amplitude = 0.001;
    assert(fabsf(chop_measure(&m) + 60.0f) < 1.5f);
}

int main(void)
{
    grid_test();
    fit_test();
    chop_test();
    puts("REV1 BPF sweep grid, QMX 60-15m crossover fit and chopped-injection detector: PASS");
    return 0;
}

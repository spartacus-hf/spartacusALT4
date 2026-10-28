#include "radio_rf_meter.h"
#include "radio_config.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The REV1 power and SWR meter (radio_rf_meter.c) and its SWR trip, against
 * an independent model of the bridge's detector: the ideal peak detector
 * with a resistive load, exp(Vdc/nVt) (1 + Vdc/(Is R)) = I0(Vport/nVt), with
 * the SS14-like values the built-in table was generated for. The ADC path is
 * the one radio_analog.c runs: the detector in mV, a 4-scan (1 ms) mean,
 * the rest level removed, the table, the scale. */

#define NVT 0.028
#define ISR 0.300

/* log I0(x): the series below 20, the asymptotic expansion above. */
static double log_i0(double x)
{
    if (x < 20.0) {
        double term = 1.0, sum = 1.0;
        for (int k = 1; k < 200; ++k) {
            term *= (x * x) / (4.0 * k * k);
            sum += term;
            if (term < sum * 1e-17) break;
        }
        return log(sum);
    }
    const double s = 1.0 + 1.0 / (8.0 * x) + 9.0 / (128.0 * x * x) + 225.0 / (3072.0 * x * x * x);
    return x - 0.5 * log(2.0 * M_PI * x) + log(s);
}

static double model_port(double vdc)
{
    if (vdc <= 0.0) return 0.0;
    const double want = vdc / NVT + log1p(vdc / ISR);
    double lo = 0.0, hi = vdc / NVT + 50.0 + want;
    for (int n = 0; n < 200; ++n) {
        const double mid = 0.5 * (lo + hi);
        if (log_i0(mid) < want) lo = mid; else hi = mid;
    }
    return NVT * 0.5 * (lo + hi);
}

/* The detector's output for a port peak: the inverse, from the curve
 * tabulated once every 0.25 mV of output (the trip check runs millions). */
#define GRID_STEP_MV 0.25
#define GRID_N 12801U /* 0 .. 3200 mV */
static double grid_port[GRID_N];

static void build_grid(void)
{
    for (unsigned i = 0U; i < GRID_N; ++i) grid_port[i] = model_port(i * GRID_STEP_MV / 1000.0);
}

static double model_vdc(double vport)
{
    if (vport <= 0.0) return 0.0;
    unsigned lo = 0U, hi = GRID_N - 1U;
    if (vport >= grid_port[hi]) return (GRID_N - 1U) * GRID_STEP_MV / 1000.0;
    while (hi - lo > 1U) {
        const unsigned mid = (lo + hi) / 2U;
        if (grid_port[mid] <= vport) lo = mid; else hi = mid;
    }
    const double t = (vport - grid_port[lo]) / (grid_port[hi] - grid_port[lo]);
    return (lo + t) * GRID_STEP_MV / 1000.0;
}

static void check_table(void)
{
    const uint16_t *t = rf_meter_default_port_mv;
    assert(RF_MeterTableValid(t));
    for (unsigned i = 0U; i < RF_METER_POINTS; ++i) {
        const double want = 1000.0 * model_port(rf_meter_vdc_mv[i] / 1000.0);
        assert(fabs(t[i] - want) <= 0.51);
        if (i + 1U < RF_METER_POINTS)
            assert(fabsf(rf_meter_inv_width[i] * (float)(rf_meter_vdc_mv[i + 1U] - rf_meter_vdc_mv[i]) - 1.0f) < 1e-6f);
    }
    /* Interpolation: within 1 mV of the curve above 6 mV, continuous and
     * rising everywhere. */
    float last = -1.0f;
    for (float mv = 0.0f; mv <= 3000.0f; mv += 0.25f) {
        const float v = rf_meter_port_v(t, mv);
        assert(v >= last);
        last = v;
        if (mv >= 6.0f && mv <= 2900.0f) assert(fabs(1000.0 * v - 1000.0 * model_port(mv / 1000.0)) < 1.3);
    }
    assert(rf_meter_port_v(t, -5.0f) == 0.0f && rf_meter_port_v(t, NAN) == 0.0f);
    /* The interrupt's RAM linearizer gives the same volts, in every segment
     * and past the last breakpoint. */
    rf_meter_lin_t lin;
    RF_MeterLinBuild(t, &lin);
    for (float mv = -2.0f; mv <= 3200.0f; mv += 0.125f)
        assert(fabsf(rf_meter_lin_v(&lin, mv) - rf_meter_port_v(t, mv)) < 2e-5f);
    assert(rf_meter_lin_v(&lin, NAN) == 0.0f);
    /* A board table replaces the built-in one only when it holds one. */
    uint16_t board[RF_METER_POINTS] = {0};
    assert(RF_MeterTable(board) == rf_meter_default_port_mv);
    memcpy(board, t, sizeof(board));
    board[5] = board[4];
    assert(!RF_MeterTableValid(board));                 /* not rising */
    memcpy(board, t, sizeof(board));
    board[3] = 20U;
    assert(!RF_MeterTableValid(board));                 /* below the detector output */
    memcpy(board, t, sizeof(board));
    assert(RF_MeterTable(board) == board);
}

static void check_reading(void)
{
    rf_meter_acc_t a;
    rf_meter_reading_t r;
    /* 5 W forward: 22.36 V peak on the line, 2.236 V at the port. */
    const float f = sqrtf(5.0f);
    for (unsigned k = 0U; k < 4U; ++k) {
        static const float rho[4] = {0.0f, 0.08f, 1.0f / 3.0f, 0.5f};
        memset(&a, 0, sizeof(a));
        for (unsigned n = 0U; n < 400U; ++n) rf_meter_acc_add(&a, f, rho[k] * f, 7900.0f, 8000.0f, 0.0f);
        RF_MeterReading(&a, 1.0f, 0.0f, 0.0f, 0U, &r);
        assert(fabsf(r.pep_w - 5.0f) < 1e-3f && fabsf(r.avg_w - 5.0f) < 1e-3f);
        assert(fabsf(r.drain_v - 7.9f) < 1e-3f && fabsf(r.vin_v - 8.0f) < 1e-3f && r.scans == 400U && r.swr_valid);
        assert(fabsf(r.rho_raw - rho[k]) < 1e-5f && fabsf(r.rho - rho[k]) < 1e-5f);
        assert(fabsf(r.swr - (1.0f + rho[k]) / (1.0f - rho[k])) < 1e-3f);
        assert(fabsf(r.ref_w - 5.0f * rho[k] * rho[k]) < 1e-3f && fabsf(r.net_w + r.ref_w - 5.0f) < 1e-3f);
    }
    /* The bridge's own leakage: 0.08 read on a 50 ohm load (QMX reads about
     * 1.17 on 20 m) is SWR 1.00 once measured; a real 2:1 keeps its size
     * on average over the unknown phase. */
    memset(&a, 0, sizeof(a));
    for (unsigned n = 0U; n < 400U; ++n) rf_meter_acc_add(&a, f, 0.08f * f, 0.0f, 8000.0f, 0.0f);
    RF_MeterReading(&a, 1.0f, 0.08f, 0.0f, 0U, &r);
    assert(fabsf(r.swr - 1.0f) < 1e-3f && fabsf(r.swr_raw - 1.1739f) < 1e-3f);
    double mean = 0.0;
    for (unsigned p = 0U; p < 360U; ++p) {
        const double ph = p * M_PI / 180.0;
        const double re = 1.0 / 3.0 + 0.08 * cos(ph), im = 0.08 * sin(ph);
        memset(&a, 0, sizeof(a));
        for (unsigned n = 0U; n < 40U; ++n) rf_meter_acc_add(&a, f, (float)hypot(re, im) * f, 0.0f, 8000.0f, 0.0f);
        RF_MeterReading(&a, 1.0f, 0.08f, 0.0f, 0U, &r);
        mean += r.rho / 360.0;
    }
    assert(fabs(mean - 1.0 / 3.0) < 0.01);
    /* The two-part leakage, 20 m on the bench: |gamma| 0.1457 at 0.39 W,
     * 0.0955 at 2.56 W on a 50 ohm load. The fit splits it into 0.083 that
     * follows the drive and a 75 mV floor that does not; with both taken
     * out, the load reads 1.00 at either power, and 1 W close to it
     * (it measured 0.1138 there: 1.04 left). */
    float lk, fl;
    RF_MeterLeakFit(sqrtf(0.39f), 0.1457f, sqrtf(2.56f), 0.0955f, &lk, &fl);
    assert(fabsf(lk - 0.083f) < 0.002f && fabsf(fl - 0.0747f) < 0.002f);
    static const float pw[3] = {0.39f, 0.99f, 2.56f}, pr[3] = {0.1457f, 0.1138f, 0.0955f};
    for (unsigned k = 0U; k < 3U; ++k) {
        const float fv = sqrtf(pw[k]);
        memset(&a, 0, sizeof(a));
        for (unsigned n = 0U; n < 400U; ++n) rf_meter_acc_add(&a, fv, pr[k] * fv, 0.0f, 8000.0f, 0.0f);
        RF_MeterReading(&a, 1.0f, lk, fl, 0U, &r);
        assert(r.swr < 1.05f); /* 1.04 at 1 W: the model, not exactly the bench */
    }
    RF_MeterLeakFit(1.0f, 0.05f, 0.9f, 0.06f, &lk, &fl);     /* too close: the stronger alone */
    assert(lk == 0.05f && fl == 0.0f);
    RF_MeterLeakFit(1.0f, 0.05f, 0.0f, 0.0f, &lk, &fl);      /* one capture */
    assert(lk == 0.05f && fl == 0.0f);
    /* A per-band gain scales power, not SWR. Below 0.1 W there is no SWR. */
    memset(&a, 0, sizeof(a));
    for (unsigned n = 0U; n < 400U; ++n) rf_meter_acc_add(&a, f, 0.0f, 0.0f, 8000.0f, 0.0f);
    RF_MeterReading(&a, 1.2f, 0.0f, 0.0f, 0U, &r);
    assert(fabsf(r.pep_w - 6.0f) < 1e-3f);
    memset(&a, 0, sizeof(a));
    for (unsigned n = 0U; n < 400U; ++n) rf_meter_acc_add(&a, 0.25f, 0.2f, 0.0f, 8000.0f, 0.0f);
    RF_MeterReading(&a, 1.0f, 0.0f, 0.0f, 0U, &r);
    assert(!r.swr_valid && fabsf(r.pep_w - 0.0625f) < 1e-4f);
    /* Nothing summed: an empty reading, SWR 1. */
    memset(&a, 0, sizeof(a));
    RF_MeterReading(&a, 1.0f, 0.0f, 0.0f, 0U, &r);
    assert(r.scans == 0U && r.pep_w == 0.0f && r.swr == 1.0f && !r.swr_valid);
    assert(RF_MeterSwr(0.999f) == RF_METER_SWR_MAX && RF_MeterSwr(-1.0f) == 1.0f);
}

/* The ADC path of radio_analog.c for one scan, with its 1 ms mean. */
typedef struct { uint16_t hist[2][4]; uint32_t sum[2]; uint8_t k; } adc_path_t;

static uint16_t clamp_mv(double v)
{
    if (v < 0.0) return 0U;
    if (v > 2900.0) return 2900U;
    return (uint16_t)lround(v);
}

static double gauss(void)
{
    const double u = (rand() + 1.0) / (RAND_MAX + 2.0), w = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * w);
}

static void scan(adc_path_t *p, double fwd_v, double rev_v, double noise_mv, float *f, float *r)
{
    const uint16_t mv[2] = {clamp_mv(1000.0 * model_vdc(fwd_v) + noise_mv * gauss()),
                            clamp_mv(1000.0 * model_vdc(rev_v) + noise_mv * gauss())};
    for (unsigned d = 0U; d < 2U; ++d) {
        p->sum[d] += (uint32_t)mv[d] - p->hist[d][p->k];
        p->hist[d][p->k] = mv[d];
    }
    p->k = (uint8_t)((p->k + 1U) & 3U);
    static rf_meter_lin_t lin;
    static int built;
    if (!built) { RF_MeterLinBuild(rf_meter_default_port_mv, &lin); built = 1; }
    *f = rf_meter_lin_v(&lin, (float)p->sum[0] * 0.25f);
    *r = rf_meter_lin_v(&lin, (float)p->sum[1] * 0.25f);
}

static void check_adc_path(void)
{
    srand(12345);
    /* Power read through the detector and the table, with 27 mV rms of
     * ADC scatter per scan (what ADC_BIAS shows on the bench): PEP within 2 %
     * from 0.5 W to 8 W, SWR on a matched load near 1.0, a 2:1 near 2. */
    static const double watts[5] = {0.5, 1.0, 2.0, 5.0, 8.0};
    for (unsigned w = 0U; w < 5U; ++w) {
        for (unsigned k = 0U; k < 2U; ++k) {
            const double rho = k ? 1.0 / 3.0 : 0.0, vf = sqrt(watts[w]);
            adc_path_t p = {0};
            rf_meter_acc_t a;
            rf_meter_reading_t r;
            float f, rr, plateau = 0.0f;
            for (unsigned n = 0U; n < 4U; ++n) scan(&p, vf, rho * vf, 27.0, &f, &rr);
            for (unsigned interval = 0U; interval < 2U; ++interval) { /* the first sets the plateau */
                memset(&a, 0, sizeof(a));
                for (unsigned n = 0U; n < 400U; ++n) {
                    scan(&p, vf, rho * vf, 27.0, &f, &rr);
                    rf_meter_acc_add(&a, f, rr, 0.0f, 8000.0f, plateau);
                }
                plateau = RF_METER_PLATEAU * a.peak;
            }
            RF_MeterReading(&a, 1.0f, 0.0f, 0.0f, 0U, &r);
            assert(fabs(r.pep_w / watts[w] - 1.0) < 0.02 && r.pep_w == r.plateau_w);
            assert(fabs(r.avg_w / watts[w] - 1.0) < 0.02);
            /* The raw peak, what voice shows, keeps the noise's few sigma. */
            assert(r.peak_w >= r.plateau_w && r.peak_w / watts[w] < (watts[w] < 1.0 ? 1.15 : (watts[w] < 2.0 ? 1.11 : 1.07)));
            RF_MeterReading(&a, 1.0f, 0.0f, 0.0f, 1U, &r);
            assert(r.pep_w == r.peak_w);
            /* 27 mV of scatter rectified by a detector at rest reads as a
             * little reflection: SWR 1.10 at 0.5 W, 1.03 at 5 W on a matched
             * load (bench step 0 measures the real scatter). */
            if (k) assert(fabsf(r.swr - 2.0f) < (watts[w] < 1.0 ? 0.1f : 0.05f));
            else assert(r.swr < (watts[w] < 1.0 ? 1.12f : (watts[w] < 2.0 ? 1.08f : 1.05f)));
        }
    }
}

/* Keyed CW: dits of 60 ms with 5 ms raised-cosine ramps and the detector's
 * 10 ms decay after each, at 20 WPM. The plateau is the carrier's level;
 * the mean over every RF scan is not. */
static void check_keyed(void)
{
    srand(99);
    adc_path_t p = {0};
    rf_meter_acc_t a;
    rf_meter_reading_t r;
    float f, rr, plateau = 0.0f, det = 0.0f;
    const double vf = sqrt(4.0);
    for (unsigned interval = 0U; interval < 3U; ++interval) {
        memset(&a, 0, sizeof(a));
        for (unsigned n = 0U; n < 400U; ++n) {
            const unsigned t = (interval * 400U + n) % 480U; /* 120 ms: 60 on, 60 off, in scans */
            double env = 0.0;
            if (t < 240U) {
                const double ms = t * 0.25;
                env = ms < 5.0 ? 0.5 - 0.5 * cos(M_PI * ms / 5.0) : (ms > 55.0 ? 0.5 + 0.5 * cos(M_PI * (ms - 55.0) / 5.0) : 1.0);
            }
            /* The detector: instant attack, 10 ms decay (100 k, 0.1 uF). */
            const float v = (float)(env * vf);
            det = fmaxf(v, det * expf(-0.25f / 10.0f));
            scan(&p, det, 0.0, 27.0, &f, &rr);
            if (t < 240U) rf_meter_acc_add(&a, f, rr, 0.0f, 8000.0f, plateau); /* RF_EN up */
        }
        plateau = RF_METER_PLATEAU * a.peak;
    }
    RF_MeterReading(&a, 1.0f, 0.0f, 0.0f, 0U, &r);
    assert(fabs(r.pep_w / 4.0 - 1.0) < 0.015);
    assert(r.avg_w < 0.95 * 4.0);
}

/* The trip: SWR 3 is |gamma| 0.5. It fires for 3.1 and not for 2.9, never
 * below the 0.1 W gate, and (8 scans in a row, the handler's persistence)
 * not on 27 mV of scatter over a matched load at any power. */
static void check_trip(void)
{
    const float limit = (300.0f - 100.0f) / (300.0f + 100.0f);
    for (float f = RF_METER_GATE_V; f < 3.0f; f += 0.05f) {
        assert(rf_meter_swr_over(f, f * (3.1f - 1.0f) / (3.1f + 1.0f), limit));
        assert(!rf_meter_swr_over(f, f * (2.9f - 1.0f) / (2.9f + 1.0f), limit));
    }
    assert(!rf_meter_swr_over(0.30f, 0.30f, limit)); /* 0.09 W, all reflected */
    assert(rf_meter_swr_over(RF_METER_GATE_V, RF_METER_GATE_V, limit));
    srand(777);
    unsigned trips = 0U;
    static const double watts[6] = {0.1, 0.2, 0.5, 1.0, 3.0, 8.0};
    for (unsigned w = 0U; w < 6U; ++w) {
        adc_path_t p = {0};
        unsigned run = 0U;
        const double vf = sqrt(watts[w]);
        for (unsigned n = 0U; n < 400000U; ++n) { /* 100 s of scans per power */
            float f, r;
            scan(&p, vf, 0.08 * vf, 27.0, &f, &r);   /* QMX-like leakage, 50 ohm */
            run = rf_meter_swr_over(f, r, limit) ? run + 1U : 0U;
            if (run >= 8U) { ++trips; run = 0U; }
        }
    }
    assert(trips == 0U);
}

static void check_anchors(void)
{
    static const struct { uint32_t hz; uint8_t a; } cases[] = {
        {1840000UL, 0}, {3573000UL, 1}, {5357000UL, 2}, {7074000UL, 3}, {10136000UL, 4},
        {14074000UL, 5}, {18100000UL, 6}, {21074000UL, 7}, {24915000UL, 8}, {28074000UL, 9},
        {28200000UL, 9}, {500000UL, 0}, {30000000UL, 9},
    };
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert(RF_MeterAnchor(cases[i].hz) == cases[i].a);
    uint16_t gain[RF_METER_ANCHORS] = {0}, leak[RF_METER_ANCHORS] = {0};
    gain[5] = 1100U;
    leak[5] = 800U;
    assert(fabsf(RF_MeterGainAt(gain, 14074000UL) - 1.1f) < 1e-6f);
    assert(RF_MeterGainAt(gain, 7074000UL) == 1.0f);
    assert(fabsf(RF_MeterLeakAt(leak, 14074000UL) - 0.08f) < 1e-6f);
    assert(RF_MeterLeakAt(leak, 21074000UL) == 0.0f);
}

/* Settings v28: zero means the default for every meter field, so a v27
 * record reads back as before; anything else must be in range. */
static void check_config(void)
{
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    assert(RADIO_ConfigValid(&c));
    assert(RADIO_ConfigTxPanel(&c) && RADIO_ConfigMeterUpdateMs(&c) == 100U &&
           RADIO_ConfigMeterHangMs(&c) == 1000U && RADIO_ConfigTunePct(&c) == 50U);
    radio_config_t t = c;
    t.meter_flags = RADIO_METER_FLAG_PANEL_OFF;
    t.meter_update_10ms = 25U;
    t.meter_hang_100ms = 30U;
    t.tune_pct = 100U;
    assert(RADIO_ConfigValid(&t) && !RADIO_ConfigTxPanel(&t) && RADIO_ConfigMeterUpdateMs(&t) == 250U &&
           RADIO_ConfigMeterHangMs(&t) == 3000U && RADIO_ConfigTunePct(&t) == 100U);
    t = c; t.meter_flags = 0x02U; assert(!RADIO_ConfigValid(&t));
    t = c; t.meter_update_10ms = 4U; assert(!RADIO_ConfigValid(&t));
    t = c; t.meter_update_10ms = 51U; assert(!RADIO_ConfigValid(&t));
    t = c; t.meter_hang_100ms = 31U; assert(!RADIO_ConfigValid(&t));
    t = c; t.tune_pct = 9U; assert(!RADIO_ConfigValid(&t));
    t = c; t.tune_pct = 101U; assert(!RADIO_ConfigValid(&t));
    t = c; t.meter_gain_x1000[3] = 249U; assert(!RADIO_ConfigValid(&t));
    t = c; t.meter_gain_x1000[3] = 1100U; assert(RADIO_ConfigValid(&t));
    t = c; t.meter_leak_x10000[9] = 5001U; assert(!RADIO_ConfigValid(&t));
    t = c; t.meter_leak_x10000[9] = 1900U; assert(RADIO_ConfigValid(&t));
    t = c; t.meter_floor_100uv[5] = 5001U; assert(!RADIO_ConfigValid(&t));
    t = c; t.meter_floor_100uv[5] = 747U; assert(RADIO_ConfigValid(&t));
    t = c; memcpy(t.meter_port_mv, rf_meter_default_port_mv, sizeof(t.meter_port_mv)); assert(RADIO_ConfigValid(&t));
    t.meter_port_mv[11] = 0U; assert(!RADIO_ConfigValid(&t)); /* half a table */
    t = c; t.meter_port_mv[4] = 100U; assert(!RADIO_ConfigValid(&t));
}

int main(void)
{
    build_grid();
    check_table();
    check_reading();
    check_adc_path();
    check_keyed();
    check_trip();
    check_anchors();
    check_config();
    printf("REV1 power/SWR meter: diode table, reading, ADC path with noise, keyed CW, trip, anchors, settings v28: PASS\n");
    return 0;
}

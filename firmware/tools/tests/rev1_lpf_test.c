#include "radio_config.h"
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Netlist-derived model of the fitted QMX+ LPFs (REV1 designators, QMX+
 * values): antenna-side and PA-side PIN diodes, C-L-C-(L||C)-C, the 47 uH
 * bias return to the BSS123 at the middle node, and the reverse-biased
 * diodes of the four unselected filters on each rail. 50 ohm both ends. */
typedef struct { double ca, la, cm, lb, cn, cb; } lpf_t;
static const lpf_t lpf[5] = {
    {1200e-12, 4.42e-6, 2200e-12, 3.54e-6, 220e-12, 820e-12},
    {680e-12, 1.83e-6, 1200e-12, 1.55e-6, 100e-12, 680e-12},
    {390e-12, 1.03e-6, 680e-12, 866e-9, 43e-12, 390e-12},
    {180e-12, 518e-9, 330e-12, 435e-9, 27e-12, 180e-12},
    {120e-12, 313e-9, 180e-12, 268e-9, 15e-12, 82e-12},
};
typedef struct { double rd, ql, cj_off, cp, esl; } parasitics_t;
static const parasitics_t variants[] = {
    {1.0, 150.0, 10e-12, 1.5e-12, 1.5e-9}, /* nominal */
    {1.0, 150.0, 5e-12, 1.5e-12, 1.5e-9},
    {1.0, 150.0, 15e-12, 1.5e-12, 1.5e-9},
    {1.0, 150.0, 10e-12, 3e-12, 3e-9},
    {2.0, 80.0, 10e-12, 1.5e-12, 1.5e-9},
};

typedef struct { double complex a, b, c, d; } abcd_t;
static abcd_t mul(abcd_t x, abcd_t y)
{
    return (abcd_t){x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d};
}
static abcd_t series(double complex z) { return (abcd_t){1.0, z, 0.0, 1.0}; }
static abcd_t shunt(double complex y) { return (abcd_t){1.0, 0.0, y, 1.0}; }

/* Insertion loss in dB. */
static double loss_db(unsigned k, double hz, const parasitics_t *p)
{
    const lpf_t *f = &lpf[k];
    const double w = 2.0 * 3.141592653589793 * hz;
    #define IND(l) (1.0 / (1.0 / (I * w * (l) + w * (l) / p->ql) + I * w * p->cp))
    #define CAP(cap) (1.0 / (1.0 / (I * w * (cap)) + I * w * p->esl + 0.05))
    double complex choke = 1.0 / (1.0 / (I * w * 47e-6 + 8.0) + I * w * 3e-12);
    double complex diode = p->rd + I * w * 5e-9;
    double complex off = I * w * p->cj_off * 4.0;
    abcd_t m = shunt(off);
    m = mul(m, series(diode));
    m = mul(m, shunt(CAP(f->ca)));
    m = mul(m, series(IND(f->la)));
    m = mul(m, shunt(CAP(f->cm) + 1.0 / choke));
    m = mul(m, series(1.0 / (1.0 / IND(f->lb) + CAP(f->cn))));
    m = mul(m, shunt(CAP(f->cb)));
    m = mul(m, series(diode));
    m = mul(m, shunt(off));
    #undef IND
    #undef CAP
    double complex den = m.a + m.b / 50.0 + m.c * 50.0 + m.d;
    return 20.0 * log10(cabs(den) / 2.0);
}

/* Harmonic suppression the TX floors promise (see RADIO_ConfigLpfDefaults). */
static int harmonics_ok(unsigned k, double hz, const parasitics_t *p, double *r2, double *r3)
{
    double base = loss_db(k, hz, p);
    *r2 = loss_db(k, 2.0 * hz, p) - base;
    *r3 = loss_db(k, 3.0 * hz, p) - base;
    int ok = *r2 >= 10.0 && *r3 >= 40.0;
    for (unsigned n = 4U; n <= 9U; ++n) ok = ok && loss_db(k, n * hz, p) - base >= 50.0;
    return ok;
}

int main(void)
{
    radio_config_t c;
    memset(&c, 0, sizeof(c));
    RADIO_ConfigEncoderDefaults(&c);
    RADIO_ConfigLpfDefaults(&c);
    RADIO_ConfigBpfDefaults(&c);
    const parasitics_t *nominal = &variants[0];
    double r2, r3;

    /* The criterion is QMX+'s own worst case: the bottom of 80 m. */
    assert(harmonics_ok(1U, 3.5e6, nominal, &r2, &r3));
    assert(r2 < 13.0 && r3 < 45.0);

    /* Every transmit frequency the firmware allows meets it, for all
     * parasitic variants, from the floor up to the filter's upper edge. */
    for (unsigned v = 0U; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        c.confirmed = 1U; c.driver_a = c.driver_b = RADIO_DRIVER_UCC27524;
        c.drain_a = c.drain_b = RADIO_DRAIN_VIN; c.bias_ma = 30U; c.bias_limit_mv = 2800U;
        c.qualified_bands = 31U; c.signature = RADIO_CONFIG_SIGNATURE;
        c.vin_limit_mv = 14700U; c.pd_target_mv = 9000U; c.pd_current_ma = 1000U;
        c.pd_max_mv = 12000U;
        c.swr_limit_x100 = 300U; c.rx_i_gain = c.rx_q_gain = c.mic_gain = 1.0f;
        c.swr_gain[0] = c.swr_gain[1] = 1.0f;
        assert(RADIO_ConfigValid(&c));
        unsigned allowed = 0U;
        for (double hz = 1.0e6; hz <= RADIO_RX_MAX_HZ; hz *= 1.002) {
            uint8_t a, b;
            if (!RADIO_ConfigTxReady(&c, (uint32_t)hz, &a, &b)) continue;
            ++allowed;
            if (!harmonics_ok(a, hz, &variants[v], &r2, &r3)) {
                printf("LPF_%u at %.3f MHz: 2f %.1f dB, 3f %.1f dB (variant %u)\n", a, hz / 1e6, r2, r3, v);
                assert(0);
            }
        }
        assert(allowed > 700U);
    }

    /* The floors are tight: 3 % lower the nominal model already fails. */
    for (unsigned k = 0U; k < 5U; ++k) {
        double floor_hz = 1000.0 * c.lpf_tx_min_khz[k];
        assert(harmonics_ok(k, floor_hz, nominal, &r2, &r3));
        assert(!harmonics_ok(k, 0.97 * floor_hz, nominal, &r2, &r3));
    }

    /* Ham bands: QMX+ filter, TX allowed, and the fundamental passes. 15 m
     * sits at the top of LPF_3 as on QMX+ (1.6-2.5 dB in this model). */
    static const struct { double lo, hi; uint8_t slot; double max_loss; } band[] = {
        {1.8e6, 2.0e6, 0U, 0.6}, {3.5e6, 4.0e6, 1U, 0.6}, {5.25e6, 5.45e6, 1U, 0.6},
        {7.0e6, 7.3e6, 2U, 0.6}, {10.1e6, 10.15e6, 2U, 0.8}, {14.0e6, 14.35e6, 3U, 0.6},
        {18.068e6, 18.168e6, 3U, 0.8}, {21.0e6, 21.45e6, 3U, 2.8}, {24.89e6, 24.99e6, 4U, 1.0},
        {26.965e6, 27.405e6, 4U, 1.2}, {28.0e6, RADIO_RX_MAX_HZ, 4U, 1.3},
    };
    for (unsigned i = 0U; i < sizeof(band) / sizeof(band[0]); ++i)
        for (double hz = band[i].lo; hz <= band[i].hi; hz += (band[i].hi - band[i].lo) / 8.0) {
            uint8_t a, b;
            assert(RADIO_ConfigTxReady(&c, (uint32_t)hz, &a, &b) && a == band[i].slot);
            assert(loss_db(a, hz, nominal) <= band[i].max_loss);
        }

    /* Each crossover hands receive to a filter that passes it well. */
    for (unsigned k = 0U; k < 4U; ++k) {
        double edge = c.lpf_upper_hz[k];
        assert(loss_db(k + 1U, edge * 1.001, nominal) < 0.6);
        assert(loss_db(k, edge, nominal) < (k == 3U ? 2.8 : 1.0));
    }

    /* Gaps where no fitted filter transmits cleanly. */
    static const double gap[] = {1.0e6, 1.69e6, 2.6e6, 3.44e6, 5.7e6, 6.29e6, 10.4e6, 12.19e6};
    for (unsigned i = 0U; i < sizeof(gap) / sizeof(gap[0]); ++i) {
        assert(!RADIO_ConfigTxReady(&c, (uint32_t)gap[i], NULL, NULL));
        assert(!strcmp(RADIO_ConfigTxBlock(&c, (uint32_t)gap[i]), "BELOW LPF TX MIN"));
    }

    (void)harmonics_ok(1U, 3.5e6, nominal, &r2, &r3);
    printf("QMX+ LPF floors 1.70/3.45/6.30/12.2/20.5 MHz hold 2f>=10, 3f>=40, 4-9f>=50 dB over 5 parasitic sets; "
           "80 m bottom 2f %.1f 3f %.1f dB\n", r2, r3);
    printf("REV1 QMX+ LPF edges, TX harmonic floors and gaps: PASS\n");
    return 0;
}

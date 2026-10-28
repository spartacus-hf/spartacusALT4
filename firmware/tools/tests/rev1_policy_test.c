#include "radio_config.h"
#include "radio_pd_policy.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void config_test(void)
{
    radio_config_t c;
    uint8_t lp, bp;
    RADIO_ConfigDefaults(&c);
    assert(RADIO_ConfigValid(&c));
    assert(c.driver_a == RADIO_DRIVER_UCC27524 && c.driver_b == RADIO_DRIVER_UCC27524);
    assert(!c.confirmed && !RADIO_ConfigTxReady(&c, 7100000U, &lp, &bp));
    assert(c.pd_enable && !c.pd_internal_rd);
    assert(c.pd_target_mv == 9000U && c.pd_max_mv == 12000U);
    c.pd_max_mv = 4999U; assert(!RADIO_ConfigValid(&c));
    c.pd_max_mv = 14001U; assert(!RADIO_ConfigValid(&c));
    /* A ceiling under the target is legal; the selection clamps it. */
    c.pd_max_mv = 5000U; assert(RADIO_ConfigValid(&c));
    c.pd_max_mv = 12000U;
    assert(c.bias_ma == RADIO_BIAS_MA_DEFAULT && c.bias_ma == 30U); /* QMX PIN fwd bias default */
    c.bias_ma = RADIO_BIAS_MA_OFF - 1U; assert(!RADIO_ConfigValid(&c));
    c.bias_ma = RADIO_BIAS_MA_MAX + 1U; assert(!RADIO_ConfigValid(&c));
    c.bias_ma = RADIO_BIAS_MA_OFF; assert(RADIO_ConfigValid(&c));
    c.bias_ma = RADIO_BIAS_MA_DEFAULT;
    c.pd_internal_rd = 2U; assert(!RADIO_ConfigValid(&c));
    c.pd_internal_rd = 1U; assert(RADIO_ConfigValid(&c));
    c.pd_internal_rd = 0U;
    /* Receive BPF defaults are the fitted QMX 60-15m set: band -> U10 channel.
     * RADIO_ConfigFilters needs a covering LPF as well, so give this block the
     * full QMX+ set: which toroids happen to be wound is not what it tests. */
    {
        radio_config_t all = c;
        RADIO_ConfigLpfDefaults(&all);
        static const struct { uint32_t hz; uint8_t slot; } band[] = {
            {1810000U, 3U}, {3573000U, 3U}, {5357000U, 3U}, {7000000U, 3U}, {7300000U, 3U},
            {8500000U, 3U}, {8500001U, 2U}, {10100000U, 2U}, {10150000U, 2U}, {13500000U, 2U},
            {13500001U, 1U}, {14000000U, 1U}, {14350000U, 1U}, {18068000U, 1U}, {18168000U, 1U},
            {18500001U, 0U}, {21000000U, 0U}, {21450000U, 0U}, {24940000U, 0U}, {28333333U, 0U},
        };
        for (unsigned i = 0U; i < sizeof(band) / sizeof(band[0]); ++i)
            assert(RADIO_ConfigFilters(&all, band[i].hz, &lp, &bp) && bp == band[i].slot);
        assert(!RADIO_ConfigFilters(&all, 28333334U, &lp, &bp));
        radio_config_t d = c;
        memset(d.bpf_upper_hz, 0, sizeof(d.bpf_upper_hz));
        RADIO_ConfigBpfDefaults(&d);
        assert(!memcmp(d.bpf_upper_hz, c.bpf_upper_hz, sizeof(d.bpf_upper_hz)));
    }
    /* LPF defaults are the fitted QMX+ 160-10m set: band -> LPF_n, and TX
     * only above each filter's harmonic floor (model in suite rev1_lpf). */
    {
        static const struct { uint32_t hz; uint8_t slot; uint8_t tx; } band[] = {
            {1000000U, 0U, 0U}, {1699999U, 0U, 0U}, {1700000U, 0U, 1U}, {1810000U, 0U, 1U},
            {2000000U, 0U, 1U}, {2500000U, 0U, 1U}, {2500001U, 1U, 0U}, {3449999U, 1U, 0U},
            {3500000U, 1U, 1U}, {3800000U, 1U, 1U}, {5351500U, 1U, 1U}, {5450000U, 1U, 1U},
            {5600000U, 1U, 1U}, {5600001U, 2U, 0U}, {6299999U, 2U, 0U}, {7000000U, 2U, 1U},
            {7300000U, 2U, 1U}, {10100000U, 2U, 1U}, {10150000U, 2U, 1U}, {10300000U, 2U, 1U},
            {10300001U, 3U, 0U}, {12199999U, 3U, 0U}, {14000000U, 3U, 1U}, {14350000U, 3U, 1U},
            {18068000U, 3U, 1U}, {18168000U, 3U, 1U}, {21000000U, 3U, 1U}, {21450000U, 3U, 1U},
            {21450001U, 4U, 1U}, {24890000U, 4U, 1U}, {26965000U, 4U, 1U}, {27405000U, 4U, 1U},
            {28000000U, 4U, 1U}, {28333333U, 4U, 1U},
        };
        /* This table is the QMX+ design mapping, so it runs against the full
         * five-slot set regardless of which toroids are wound on this build;
         * lpf_fitted_test() covers what a fresh install actually gets. */
        radio_config_t all = c;
        RADIO_ConfigLpfDefaults(&all);
        radio_config_t t = all;
        t.confirmed = 1U; t.drain_a = t.drain_b = RADIO_DRAIN_VIN; t.qualified_bands = 31U;
        assert(RADIO_ConfigValid(&t));
        for (unsigned i = 0U; i < sizeof(band) / sizeof(band[0]); ++i) {
            assert(RADIO_ConfigFilters(&all, band[i].hz, &lp, &bp) && lp == band[i].slot);
            const char *why = RADIO_ConfigTxBlock(&t, band[i].hz);
            assert(RADIO_ConfigTxReady(&t, band[i].hz, &lp, &bp) == band[i].tx && (why == NULL) == band[i].tx);
            assert(band[i].tx || !strcmp(why, "BELOW LPF TX MIN"));
        }
        t.qualified_bands = 31U & ~(1U << 2U);
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "LPF UNTESTED"));
        t.qualified_bands = 31U; t.lpf_tx_min_khz[2] = 0U;
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "BELOW LPF TX MIN"));
        t.lpf_tx_min_khz[2] = 28334U;
        assert(!RADIO_ConfigValid(&t) && !strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "CONFIG INVALID"));
        t = all;
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "NOT CONFIRMED"));
        t.confirmed = 1U; t.drain_a = RADIO_DRAIN_VIN;
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "DRIVER/DRAIN UNSET"));
        t.drain_b = RADIO_DRAIN_VIN;
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "LPF UNTESTED"));
        /* The modeled bias voltage must stay under the trip limit even at the
         * lowest 5 V rail that permits TX; the buck-off setting needs none. */
        t.bias_limit_mv = RADIO_BiasTargetMv(30U, 4400U) + 49U;
        assert(RADIO_ConfigValid(&t) && !strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "BIAS OVER LIMIT"));
        t.bias_limit_mv += 1U;
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "LPF UNTESTED"));
        t.bias_limit_mv = 0U; t.bias_ma = RADIO_BIAS_MA_OFF;
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "LPF UNTESTED"));
        t.bias_ma = RADIO_BIAS_MA_MAX; t.bias_limit_mv = 2800U;
        assert(!strcmp(RADIO_ConfigTxBlock(&t, 7100000U), "LPF UNTESTED"));
        /* QMX+ LPF EDGES restores the full design set, which is the documented
         * way to bring slots back once their toroids are wound; the unfitted
         * mask on top reproduces the fresh-install defaults exactly, whatever
         * that mask happens to be on this build. */
        radio_config_t d = c;
        memset(d.lpf_upper_hz, 0, sizeof(d.lpf_upper_hz));
        memset(d.lpf_tx_min_khz, 0, sizeof(d.lpf_tx_min_khz));
        RADIO_ConfigLpfDefaults(&d);
        assert(!memcmp(&d, &all, sizeof(d)));
        RADIO_ConfigLpfUnfitted(&d);
        assert(!memcmp(&d, &c, sizeof(d)));
    }
    /* Generic slot rules below start from unconfigured filters. */
    memset(c.bpf_upper_hz, 0, sizeof(c.bpf_upper_hz));
    memset(c.lpf_upper_hz, 0, sizeof(c.lpf_upper_hz));
    memset(c.lpf_tx_min_khz, 0, sizeof(c.lpf_tx_min_khz));
    c.lpf_tx_min_khz[0] = 7000U;
    c.lpf_upper_hz[0] = 8000000U;
    c.bpf_upper_hz[0] = 10000000U;
    assert(RADIO_ConfigFilters(&c, 7100000U, &lp, &bp));
    assert(lp == 0U && bp == 0U);
    assert(!RADIO_ConfigTxReady(&c, 7100000U, &lp, &bp));
    c.confirmed = 1U; c.driver_a = RADIO_DRIVER_UCC27524; c.driver_b = RADIO_DRIVER_ACT541;
    c.drain_a = c.drain_b = RADIO_DRAIN_VIN;
    c.qualified_bands = 1U;
    assert(RADIO_ConfigTxReady(&c, 7100000U, &lp, &bp));
    c.envelope_mode = 1U;
    assert(!RADIO_ConfigTxReady(&c, 7100000U, &lp, &bp));
    c.drain_a = c.drain_b = RADIO_DRAIN_ENV;
    assert(!RADIO_ConfigTxReady(&c, 7100000U, &lp, &bp));
    c.envelope_dac_per_volt[0] = c.envelope_dac_per_volt[1] = 200.0f;
    assert(RADIO_ConfigTxReady(&c, 7100000U, &lp, &bp));
    {
        /* DRAIN MODULATION follows the jumpers: both on the follower is pure
         * envelope drive, either on VIN is outphasing, HYBRID is kept. */
        radio_config_t j = c;
        j.drain_a = j.drain_b = RADIO_DRAIN_UNKNOWN;
        j.envelope_mode = RADIO_DRAIN_MOD_OUTPHASING;
        RADIO_ConfigSetDrain(&j, 0U, RADIO_DRAIN_ENV);
        assert(j.drain_a == RADIO_DRAIN_ENV && j.envelope_mode == RADIO_DRAIN_MOD_OUTPHASING);
        RADIO_ConfigSetDrain(&j, 1U, RADIO_DRAIN_ENV);
        assert(j.drain_b == RADIO_DRAIN_ENV && j.envelope_mode == RADIO_DRAIN_MOD_ENVELOPE);
        assert(RADIO_ConfigTxReady(&j, 7100000U, &lp, &bp));
        j.envelope_mode = RADIO_DRAIN_MOD_OUTPHASING; /* by hand: kept */
        RADIO_ConfigSetDrain(&j, 0U, RADIO_DRAIN_ENV);
        assert(j.envelope_mode == RADIO_DRAIN_MOD_OUTPHASING);
        j.envelope_mode = RADIO_DRAIN_MOD_HYBRID;
        RADIO_ConfigSetDrain(&j, 1U, RADIO_DRAIN_VIN);
        assert(j.envelope_mode == RADIO_DRAIN_MOD_OUTPHASING);
        RADIO_ConfigSetDrain(&j, 0U, RADIO_DRAIN_VIN);
        assert(j.envelope_mode == RADIO_DRAIN_MOD_OUTPHASING);
        j.envelope_mode = RADIO_DRAIN_MOD_OUTPHASING;
        assert(RADIO_ConfigTxReady(&j, 7100000U, &lp, &bp));
        RADIO_ConfigSetDrain(&j, 0U, RADIO_DRAIN_ENV);
        RADIO_ConfigSetDrain(&j, 1U, RADIO_DRAIN_ENV);
        assert(j.envelope_mode == RADIO_DRAIN_MOD_ENVELOPE);
        j.envelope_mode = RADIO_DRAIN_MOD_HYBRID;
        RADIO_ConfigSetDrain(&j, 1U, RADIO_DRAIN_UNKNOWN);
        RADIO_ConfigSetDrain(&j, 1U, RADIO_DRAIN_ENV);
        assert(j.envelope_mode == RADIO_DRAIN_MOD_HYBRID);
    }
    assert(!RADIO_ConfigTxReady(&c, 6999999U, &lp, &bp)); /* LPF 0 harmonic floor */
    assert(!RADIO_ConfigTxReady(&c, 9000000U, &lp, &bp));
    assert(!RADIO_ConfigTxReady(&c, 30000000U, &lp, &bp));
    /* Slots follow the PCB, not frequency order: pick the tightest cover. */
    c.lpf_upper_hz[2] = 7200000U;
    assert(RADIO_ConfigValid(&c));
    assert(RADIO_ConfigFilters(&c, 7100000U, &lp, &bp) && lp == 2U && bp == 0U);
    assert(RADIO_ConfigFilters(&c, 7500000U, &lp, &bp) && lp == 0U);
    assert(!RADIO_ConfigFilters(&c, 8500000U, &lp, &bp) && lp == 255U);
    c.bpf_upper_hz[3] = 4000000U; c.bpf_upper_hz[1] = 30000000U;
    assert(!RADIO_ConfigValid(&c)); /* above the legal RF range */
    c.bpf_upper_hz[1] = 0U;
    assert(RADIO_ConfigFilters(&c, 3600000U, &lp, &bp) && bp == 3U && lp == 2U);
    c.bpf_upper_hz[3] = 0U;
    c.lpf_upper_hz[4] = 8000000U;
    assert(!RADIO_ConfigValid(&c)); /* duplicate edges are ambiguous */
    c.lpf_upper_hz[4] = 0U; c.lpf_upper_hz[2] = 12000000U;
    assert(RADIO_ConfigValid(&c));
    c.qualified_bands = 0x80000000U;
    assert(!RADIO_ConfigValid(&c));
    c.qualified_bands = 1U; c.rx_skew_samples = NAN;
    assert(!RADIO_ConfigValid(&c));
}

static uint32_t fixed(unsigned mv, unsigned ma) { return ((mv / 50U) << 10U) | (ma / 10U); }
static uint32_t pps(unsigned min_mv, unsigned max_mv, unsigned ma)
{ return (3UL << 30U) | ((max_mv / 100U) << 17U) | ((min_mv / 100U) << 8U) | (ma / 50U); }

static void pd_test(void)
{
    uint32_t pdo[] = {fixed(5000,3000), fixed(9000,2000), fixed(15000,3000), fixed(20000,3000), pps(3300,21000,3000)};
    radio_pd_request_t r;
    assert(RADIO_PdChoose(pdo, 5, 12000, 12000, 1100, 1, &r));
    assert(r.pps && r.mv == 12000 && r.ma == 1100 && r.position == 5);
    assert(((r.rdo >> 9) & 4095U) * 20U == 12000U);
    assert((r.rdo & 127U) * 50U == 1100U);
    assert(RADIO_PdChoose(pdo, 5, 9000, 9000, 3000, 1, &r));
    assert(!r.pps && r.mv == 9000 && r.ma == 2000); /* prefer fixed at equal voltage */
    assert(RADIO_PdChoose(pdo, 5, 12000, 12000, 1000, 0, &r));
    assert(!r.pps && r.mv == 9000);
    assert(!RADIO_PdChoose(pdo, 5, 14001, 14001, 1000, 1, &r) && !r.rdo);
    assert(!RADIO_PdChoose(pdo, 5, 9000, 9000, 4000, 1, &r));
    pdo[0] = fixed(9000,3000);
    assert(!RADIO_PdChoose(pdo, 5, 9000, 9000, 1000, 1, &r));
    pdo[0] = fixed(5000,0); /* malformed mandatory first object */
    assert(!RADIO_PdChoose(pdo, 5, 12000, 12000, 1000, 1, &r));
    pdo[0] = fixed(5000,3000);
    for (unsigned mv = 5000; mv <= 14000; mv += 7) {
        assert(RADIO_PdChoose(pdo, 5, mv, mv, 777, 1, &r));
        assert(r.mv <= mv && r.mv <= 14000 && r.ma <= 777 && r.ma <= 3000);
        assert((r.rdo & (1UL << 25U)) == 0U); /* power connector has no USB data */
    }
    /* The ceiling only comes into play when nothing reaches the target. A
     * source that does offer the target is unaffected by how high it is. */
    assert(RADIO_PdChoose(pdo, 5, 9000, 12000, 2000, 0, &r) && r.mv == 9000);

    /* A laptop charger whose ladder steps straight past 9 V. Taking the
     * LOWEST offer above the target matters: 12 V burns half again as much in
     * the linear regulator as 10 V would. */
    uint32_t skip[] = {fixed(5000,3000), fixed(12000,2500), fixed(20000,3250)};
    assert(RADIO_PdChoose(skip, 3, 9000, 12000, 2000, 1, &r));
    assert(!r.pps && r.mv == 12000 && r.position == 2);
    uint32_t two_up[] = {fixed(5000,3000), fixed(10000,2000), fixed(12000,2500)};
    assert(RADIO_PdChoose(two_up, 3, 9000, 12000, 2000, 1, &r) && r.mv == 10000);
    /* Above the ceiling nothing is taken and the mandatory 5 V is what is
     * left: 15 V and 20 V are over the board's VIN protection either way. */
    uint32_t too_high[] = {fixed(5000,3000), fixed(15000,3000), fixed(20000,3250)};
    assert(RADIO_PdChoose(too_high, 3, 9000, 12000, 2000, 1, &r));
    assert(r.mv == 5000 && r.position == 1);
    /* A ceiling below the target is not an error, it just means no headroom. */
    assert(RADIO_PdChoose(skip, 3, 9000, 5000, 2000, 1, &r) && r.mv == 5000);
    /* and one above the policy maximum is clamped to it, never obeyed. */
    uint32_t over[] = {fixed(5000,3000), fixed(15000,3000)};
    assert(RADIO_PdChoose(over, 2, 9000, 20000, 2000, 1, &r) && r.mv == 5000);
    /* A PPS range that starts above the target is reachable at its floor. */
    uint32_t high_pps[] = {fixed(5000,3000), pps(10000,11000,3000)};
    assert(RADIO_PdChoose(high_pps, 2, 9000, 12000, 2000, 1, &r));
    assert(r.pps && r.mv == 10000 && r.position == 2);
    assert(RADIO_PdChoose(high_pps, 2, 9000, 9500, 2000, 1, &r) && r.mv == 5000);
}
/* QRP Labs PIN bias model (QDX assembly 1.24): node = 0.6741 V + 12.1 ohm x I
 * per diode, plus (V5 - node)/1 k from R74/R77. Plants below may deviate
 * from it (slope, intercept) to bound the current error of the fixed model. */
typedef struct { double node_v, slope_ohm; } bias_diode_t;
/* Per-diode current for a TX_BIAS voltage v and 5 V rail r. */
static double diode_ma(const bias_diode_t *d, double v, double r)
{
    double i = ((v - d->node_v) / 33.0 + (r - d->node_v) / 1000.0) /
               (1.0 + d->slope_ohm / 33.0 + d->slope_ohm / 1000.0);
    return i > 0.0 ? i * 1000.0 : 0.0;
}

static void model_test(void)
{
    const bias_diode_t qdx = {0.6741, 12.1};
    /* QDX chart values: 30 mA per diode needs the node at 1.037 V. */
    assert(abs((int)RADIO_BiasTargetMv(30U, 5000U) - 1896) <= 1);
    assert(RADIO_BiasTargetMv(RADIO_BIAS_MA_OFF, 5000U) == 0U && RADIO_BiasTargetMv(0U, 5000U) == 0U);
    assert(RADIO_BiasTargetMv(30U, 0U) == RADIO_BiasTargetMv(30U, 5000U)); /* no rail reading */
    for (uint16_t ma = RADIO_BIAS_MA_OFF + 1U; ma <= RADIO_BIAS_MA_MAX; ++ma)
        for (uint16_t rail = 4400U; rail <= 5500U; rail += 100U) {
            const uint16_t mv = RADIO_BiasTargetMv(ma, rail);
            assert(fabs(diode_ma(&qdx, mv / 1000.0, rail / 1000.0) - ma) < 0.05);
            assert(fabsf(RADIO_BiasCurrentMa(mv, rail) - ma) < 0.05f);
            assert(mv + 50U <= 2800U); /* inside the default limit and ADC range */
        }
    /* Receive: no buck current, the node settles where R74/R77 feed ~4 mA
     * ("around 4 mA default bias current" with the SMPS off). */
    const double idle_v = 0.6741 + 12.1 * 4.274e-3;
    assert(fabs(diode_ma(&qdx, idle_v, 5.0) - 4.27) < 0.02);
    assert(fabsf(RADIO_BiasCurrentMa((uint16_t)(idle_v * 1000.0 + 0.5), 5000U) - 4.27f) < 0.1f);
}

/* Averaged buck: duty x (VIN - I x Ron) - (1 - duty) x V_SS14 into 330 uH with
 * DCR and 470 uF, no reverse inductor current, loaded by the two 33 ohm feeds
 * (or a short) and fed by R74/R77 below the node voltage. */
typedef struct { double i, v; } bias_plant_t;
static void plant_step(bias_plant_t *p, double duty, double vin, double dcr, const bias_diode_t *d, double short_ohm)
{
    const double dt = 1.0 / (4000.0 * 40.0), rail = 5.0;
    for (unsigned n = 0U; n < 40U; ++n) {
        double load;
        if (short_ohm > 0.0) load = p->v / short_ohm;
        else {
            const double ma = diode_ma(d, p->v, rail);
            const double node = d->node_v + d->slope_ohm * ma / 1000.0;
            load = 2.0 * (ma / 1000.0 - (rail - node) / 1000.0);
        }
        const double k = duty / 1024.0;
        double drive = k * (vin - p->i * 8.0) - (p->i > 0.0 ? (1.0 - k) * 0.35 : 0.0);
        p->i += (drive - p->v - p->i * dcr) / 330e-6 * dt;
        if (p->i < 0.0) p->i = 0.0;
        p->v = fmax(0.0, p->v + (p->i - load) / 470e-6 * dt);
    }
}

static uint16_t adc_mv(double v, unsigned k)
{
    double mv = v * 1000.0 + (double)((k * 7U) % 11U) - 5.0; /* +-5 mV */
    return (uint16_t)(mv < 0.0 ? 0.0 : (mv > 2900.0 ? 2900.0 : mv));
}

static void bias_test(void)
{
    model_test();
    const double dcrs[] = {0.5, 3.4, 7.0}; /* 330 uH axial chokes: about 3-7 ohm */
    const unsigned vins[] = {6500U, 9000U, 12000U, 14000U};
    const uint16_t currents[] = {10U, 30U, 45U};
    const bias_diode_t diodes[] = {{0.6741, 12.1}, {0.60, 8.0}, {0.75, 18.0}};
    const uint16_t limit = 2800U;
    double worst_error = 0.0;
    for (unsigned a = 0U; a < 3U; ++a) for (unsigned b = 0U; b < 4U; ++b)
    for (unsigned m = 0U; m < 3U; ++m) for (unsigned d = 0U; d < 3U; ++d) {
        if (currents[m] == 45U && vins[b] < 9000U) continue; /* needs > 50 % duty */
        const uint16_t target = RADIO_BiasTargetMv(currents[m], 5000U);
        bias_plant_t p = {0.0, 0.0};
        for (unsigned k = 0U; k < 8000U; ++k) plant_step(&p, 0.0, vins[b] / 1000.0, dcrs[a], &diodes[d], 0.0);
        radio_bias_loop_t loop;
        uint16_t start = adc_mv(p.v, 0U);
        RADIO_BiasLoopStart(&loop, start < target ? start : target);
        double peak = 0.0;
        unsigned settled_at = 0U, inside = 0U;
        for (unsigned k = 0U; k < 2000U; ++k) { /* 500 ms */
            const uint16_t measured = adc_mv(p.v, k);
            float duty = RADIO_BiasLoopStep(&loop, measured, target, limit, (uint16_t)vins[b]);
            assert(duty >= 0.0f && duty <= 512.0f); /* QMX maximum duty 50 % */
            assert(duty <= 1024.0f * (limit + 1500.0f) / vins[b] + 0.01f);
            assert(!RADIO_BiasEnvelopeFault(&loop, duty, measured, (uint16_t)vins[b]));
            plant_step(&p, duty, vins[b] / 1000.0, dcrs[a], &diodes[d], 0.0);
            if (p.v > peak) peak = p.v;
            if (fabs(p.v * 1000.0 - target) <= 30.0) { if (++inside == 80U && !settled_at) settled_at = k - 79U; }
            else inside = 0U;
        }
        assert(settled_at && settled_at < 4000U * 90U / 1000U);  /* within 90 ms */
        assert(peak * 1000.0 < target + 40.0 && peak * 1000.0 < limit); /* no LC overshoot */
        const double error = fabs(diode_ma(&diodes[d], p.v, 5.0) - currents[m]);
        if (d == 0U) assert(error < 1.0);
        if (currents[m] == 30U && error > worst_error) worst_error = error;
        assert(error < 0.2 * currents[m] || error < 3.0);
    }
    /* Fixed model against -4/+6 ohm slope and -74/+76 mV intercept spread:
     * the same order of accuracy QRP Labs accepted for QDX/QMX. */
    assert(worst_error < 5.5);
    printf("30 mA setting with diode/switch spread: within %.1f mA\n", worst_error);

    /* TX right after power-up, before R74/R77 have charged C73: no false
     * envelope trip while the buck charges it. */
    for (unsigned b = 0U; b < 4U; ++b) for (unsigned d = 0U; d < 3U; ++d) {
        bias_plant_t p = {0.0, 0.0};
        radio_bias_loop_t loop;
        const uint16_t target = RADIO_BiasTargetMv(45U, 5000U);
        RADIO_BiasLoopStart(&loop, 0U);
        for (unsigned k = 0U; k < 2000U; ++k) {
            const uint16_t measured = adc_mv(p.v, k);
            float duty = RADIO_BiasLoopStep(&loop, measured, target, limit, (uint16_t)vins[b]);
            assert(!RADIO_BiasEnvelopeFault(&loop, duty, measured, (uint16_t)vins[b]));
            plant_step(&p, duty, vins[b] / 1000.0, 7.0, &diodes[d], 0.0);
        }
        assert(vins[b] < 9000U || fabs(p.v * 1000.0 - target) < 30.0);
    }
    /* Shorted C73 or feed (QMX C504 failures): the envelope check stops the
     * buck within 15 ms, far below the BSS84's 0.52 A pulsed rating. */
    for (unsigned b = 0U; b < 4U; ++b) {
        bias_plant_t p = {0.0, 0.0};
        radio_bias_loop_t loop;
        RADIO_BiasLoopStart(&loop, 0U);
        unsigned k = 0U;
        for (; k < 400U; ++k) {
            const uint16_t measured = adc_mv(p.v, k);
            float duty = RADIO_BiasLoopStep(&loop, measured, RADIO_BiasTargetMv(30U, 5000U), limit, (uint16_t)vins[b]);
            if (RADIO_BiasEnvelopeFault(&loop, duty, measured, (uint16_t)vins[b])) break;
            plant_step(&p, duty, vins[b] / 1000.0, 3.4, &diodes[0], 0.05);
        }
        assert(k < 60U && p.i < 0.15);
    }
    /* Open or stale ADC_BIAS (reads 0 V): duty stops at the VIN ceiling and
     * the envelope check trips. */
    for (unsigned vin = 5000U; vin <= 14000U; vin += 1000U) {
        radio_bias_loop_t loop;
        RADIO_BiasLoopStart(&loop, 0U);
        float duty = 0.0f;
        uint8_t tripped = 0U;
        for (unsigned k = 0U; k < 4000U; ++k) {
            duty = RADIO_BiasLoopStep(&loop, 0U, 2800U, 2800U, (uint16_t)vin);
            tripped |= RADIO_BiasEnvelopeFault(&loop, duty, 0U, (uint16_t)vin);
        }
        float ceiling = 1024.0f * 4300.0f / (float)vin;
        assert(fabsf(duty - (ceiling < 512.0f ? ceiling : 512.0f)) < 0.01f && tripped);
    }
    /* C73 precharged above the limit (no LPF conducted): starting at the
     * target, the duty collapses before the 2 ms over-limit persistence. */
    for (unsigned b = 0U; b < 4U; ++b) {
        bias_plant_t p = {0.0, 4.9};
        radio_bias_loop_t loop;
        const uint16_t target = RADIO_BiasTargetMv(30U, 5000U);
        RADIO_BiasLoopStart(&loop, target);
        unsigned high = 0U, worst = 0U;
        for (unsigned k = 0U; k < 400U; ++k) {
            const uint16_t measured = adc_mv(p.v, k);
            float duty = RADIO_BiasLoopStep(&loop, measured, target, limit, (uint16_t)vins[b]);
            high = (duty > 0.5f && measured > limit) ? high + 1U : 0U;
            if (high > worst) worst = high;
            plant_step(&p, duty, vins[b] / 1000.0, 3.4, &diodes[0], 0.0);
        }
        assert(worst < 8U && fabs(p.v * 1000.0 - target) < 30.0);
    }
    /* Reference slews instead of stepping, and a lowered target winds down. */
    radio_bias_loop_t loop;
    RADIO_BiasLoopStart(&loop, 800U);
    (void)RADIO_BiasLoopStep(&loop, 800U, 2500U, 2800U, 12000U);
    assert(loop.ref_mv > 815.0f && loop.ref_mv < 817.0f);
    for (unsigned k = 0U; k < 400U; ++k) (void)RADIO_BiasLoopStep(&loop, 2500U, 1500U, 2800U, 12000U);
    assert(loop.ref_mv == 1500.0f);
}

/* Bench, 2026-09-27 (REV1, receive, LPF 3): single ADC_BIAS scans scatter
 * 27-29 mV rms (638-797 mV around a 721 mV node), white from one scan to the
 * next, while C73 keeps the node itself quiet. Deterministic Gaussian noise:
 * xorshift32 into a 12-uniform Irwin-Hall sum. */
static uint32_t noise_state = 0x2545F491UL;
static double noise_gauss(void)
{
    double s = 0.0;
    for (unsigned n = 0U; n < 12U; ++n) {
        noise_state ^= noise_state << 13; noise_state ^= noise_state >> 17; noise_state ^= noise_state << 5;
        s += (double)noise_state / 4294967296.0;
    }
    return s - 6.0;
}

static uint16_t noisy_mv(double v, double sigma)
{
    const double mv = v * 1000.0 + sigma * noise_gauss();
    return (uint16_t)(mv < 0.0 ? 0.0 : (mv > 2900.0 ? 2900.0 : mv));
}

/* No filter conducting (open toroid or diode): only R74/R77 feed the node,
 * which rises toward the 5 V rail; ADC_BIAS clips at the 2.9 V reference. */
static void open_plant_step(bias_plant_t *p, double duty, double vin)
{
    const double dt = 1.0 / (4000.0 * 40.0);
    for (unsigned n = 0U; n < 40U; ++n) {
        const double k = duty / 1024.0;
        double drive = k * (vin - p->i * 8.0) - (p->i > 0.0 ? (1.0 - k) * 0.35 : 0.0);
        p->i += (drive - p->v - p->i * 3.4) / 330e-6 * dt;
        if (p->i < 0.0) p->i = 0.0;
        p->v = fmax(0.0, p->v + (p->i + 2.0 * (5.0 - p->v) / 1000.0) / 470e-6 * dt);
    }
}

/* service_tx_entry as the keyed main loop runs it: it sees nearly every 4 kHz
 * scan, starts TX after 5 ms and 10 ms (40 scans) with the mean inside the
 * window, and trips FAULT_BIAS after 500 ms. The loop and the envelope trip
 * run on each scan, as in the ADC interrupt. The mean has run on the receive
 * level for 100 ms before the key. Returns the scan TX starts on, 0 for the
 * fault; *node_mv is the true TX_BIAS then. */
static unsigned tx_entry_scans(bias_plant_t *p, double sigma, uint16_t ma, unsigned vin, double dcr,
                               const bias_diode_t *d, uint16_t limit, uint8_t open, double *node_mv)
{
    const uint16_t target = RADIO_BiasTargetMv(ma, 5000U);
    radio_bias_avg_t avg = {0, 0U};
    uint16_t mean = 0U;
    for (unsigned k = 0U; k < 400U; ++k) {
        mean = RADIO_BiasAverageStep(&avg, noisy_mv(p->v, sigma));
        if (open) open_plant_step(p, 0.0, vin / 1000.0);
        else plant_step(p, 0.0, vin / 1000.0, dcr, d, 0.0);
    }
    radio_bias_loop_t loop;
    RADIO_BiasLoopStart(&loop, mean < target ? mean : target);
    unsigned ok_since = 0U;
    for (unsigned k = 0U; k <= 2000U; ++k) {
        const uint16_t scan = noisy_mv(p->v, sigma);
        mean = RADIO_BiasAverageStep(&avg, scan);
        const float duty = RADIO_BiasLoopStep(&loop, scan, target, limit, (uint16_t)vin);
        assert(!RADIO_BiasEnvelopeFault(&loop, duty, scan, (uint16_t)vin)); /* noise never trips it */
        if (open) open_plant_step(p, duty, vin / 1000.0);
        else plant_step(p, duty, vin / 1000.0, dcr, d, 0.0);
        if (!RADIO_BiasSettled(mean, target)) ok_since = k;
        if (k >= 20U && k - ok_since >= 40U) { *node_mv = p->v * 1000.0; return k; }
    }
    return 0U;
}

static void bias_noise_test(void)
{
    /* The mean: primed by its first sample, within 1 mV in steady state, no
     * overflow at the ADC limits, a 1 V step inside 30 mV within 16 ms. */
    radio_bias_avg_t a = {0, 0U};
    assert(RADIO_BiasAverageStep(&a, 723U) == 723U && a.primed);
    for (unsigned k = 0U; k < 200U; ++k) assert(abs((int)RADIO_BiasAverageStep(&a, 723U) - 723) <= 1);
    unsigned k = 0U;
    while (abs((int)RADIO_BiasAverageStep(&a, 1723U) - 1723) > 30) ++k;
    assert(k < 64U);
    for (k = 0U; k < 400U; ++k) (void)RADIO_BiasAverageStep(&a, 2900U);
    assert(RADIO_BiasAverageStep(&a, 2900U) == 2900U);
    for (k = 0U; k < 400U; ++k) (void)RADIO_BiasAverageStep(&a, 0U);
    assert(RADIO_BiasAverageStep(&a, 0U) <= 1U);
    a.primed = 0U;
    assert(RADIO_BiasAverageStep(&a, 1896U) == 1896U); /* restart forgets the past */
    /* It takes the bench scatter from 27 mV to well under the window. */
    double sum = 0.0, sq = 0.0;
    a.primed = 0U;
    for (k = 0U; k < 20000U; ++k) {
        const double m = RADIO_BiasAverageStep(&a, noisy_mv(1.896, 27.0));
        if (k >= 100U) { sum += m; sq += m * m; }
    }
    const double n = 19900.0, sd = sqrt(sq / n - (sum / n) * (sum / n));
    assert(fabs(sum / n - 1896.0) < 2.0 && sd < 7.0);

    /* The window: inclusive +-30 mV, and a zero target (buck off) is settled. */
    assert(RADIO_BiasSettled(0U, 0U) && RADIO_BiasSettled(2900U, 0U));
    assert(RADIO_BiasSettled(1866U, 1896U) && RADIO_BiasSettled(1926U, 1896U));
    assert(!RADIO_BiasSettled(1865U, 1896U) && !RADIO_BiasSettled(1927U, 1896U));
    assert(!RADIO_BiasSettled(0U, 1896U) && !RADIO_BiasSettled(65535U, 1896U));

    /* Keyed TX starts within 120 ms at the measured noise and at twice it,
     * with the true bias inside the window, over the whole plant spread. */
    const double sigmas[] = {0.0, 27.0, 60.0};
    const double dcrs[] = {0.5, 3.4, 7.0};
    const unsigned vins[] = {6500U, 9000U, 12000U, 14000U};
    const uint16_t currents[] = {10U, 30U, 45U};
    const bias_diode_t diodes[] = {{0.6741, 12.1}, {0.60, 8.0}, {0.75, 18.0}};
    unsigned worst_scan = 0U;
    double worst_error = 0.0;
    for (unsigned s = 0U; s < 3U; ++s) for (unsigned a2 = 0U; a2 < 3U; ++a2) for (unsigned b = 0U; b < 4U; ++b)
    for (unsigned m = 0U; m < 3U; ++m) for (unsigned d = 0U; d < 3U; ++d) {
        if (currents[m] == 45U && vins[b] < 9000U) continue; /* needs > 50 % duty */
        bias_plant_t p = {0.0, 0.0};
        for (k = 0U; k < 8000U; ++k) plant_step(&p, 0.0, vins[b] / 1000.0, dcrs[a2], &diodes[d], 0.0);
        double node = 0.0;
        const unsigned at = tx_entry_scans(&p, sigmas[s], currents[m], vins[b], dcrs[a2], &diodes[d], 2800U, 0U, &node);
        assert(at && at <= 4U * 120U);
        /* Up to the measured noise the node is quiet (about 2 mV rms). At twice
         * it, a scan 150 mV high now and then trips the loop's pulse skip and
         * the LC dips by up to ~70 mV (1.6 mA at 45 mA) for a moment. */
        const double error = fabs(node - RADIO_BiasTargetMv(currents[m], 5000U));
        assert(error < (sigmas[s] <= 27.0 ? 45.0 : 80.0));
        if (at > worst_scan) worst_scan = at;
        if (sigmas[s] <= 27.0 && error > worst_error) worst_error = error;
    }
    /* The mean does not loosen the check: with no filter conducting, or a buck
     * that cannot reach the target, TX never starts and FAULT_BIAS follows. */
    for (unsigned s = 0U; s < 3U; ++s) for (unsigned b = 0U; b < 4U; ++b) {
        double node = 0.0;
        bias_plant_t p = {0.0, 0.0};
        for (k = 0U; k < 8000U; ++k) open_plant_step(&p, 0.0, vins[b] / 1000.0);
        assert(!tx_entry_scans(&p, sigmas[s], 30U, vins[b], 3.4, &diodes[0], 2800U, 1U, &node));
        bias_plant_t w = {0.0, 0.0};
        for (k = 0U; k < 8000U; ++k) plant_step(&w, 0.0, 8.8, 3.4, &diodes[0], 0.0);
        /* Duty ceiling 1024 x (300 + 1500) / 8800: at most 1.8 V before losses. */
        assert(!tx_entry_scans(&w, sigmas[s], 30U, 8800U, 3.4, &diodes[0], 300U, 0U, &node));
    }
    printf("TX entry under 0/27/60 mV ADC_BIAS noise: worst %.1f ms; at <= 27 mV the true bias is within %.0f mV\n",
           worst_scan / 4.0, worst_error);
}


/* Nothing runs on a board whose assembly has not been declared, and the two
 * drain jumpers must agree before the PA can be keyed. */
static void gate_test(void)
{
    radio_config_t c;
    RADIO_ConfigDefaults(&c);

    /* A fresh install: receive is blocked, not just transmit. */
    assert(!c.confirmed);
    assert(RADIO_ConfigRxBlock(&c) && !strcmp(RADIO_ConfigRxBlock(&c), "NOT CONFIRMED"));
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "NOT CONFIRMED"));

    /* An invalid image blocks both, and says so before anything else. */
    radio_config_t bad = c;
    bad.signature = 0U;
    assert(!strcmp(RADIO_ConfigRxBlock(&bad), "CONFIG INVALID"));
    assert(!strcmp(RADIO_ConfigTxBlock(&bad, 7100000U), "CONFIG INVALID"));

    /* Confirming releases receive. Transmit still needs everything else. */
    c.confirmed = 1U;
    assert(RADIO_ConfigRxBlock(&c) == NULL);
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "DRIVER/DRAIN UNSET"));

    /* One jumper on VIN and the other on the envelope follower would run the
     * two Chireix branches at different drain voltages. */
    c.drain_a = RADIO_DRAIN_ENV;
    c.drain_b = RADIO_DRAIN_VIN;
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "DRAIN A/B DIFFER"));
    c.drain_a = RADIO_DRAIN_VIN;
    assert(strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "DRAIN A/B DIFFER"));
    c.drain_a = c.drain_b = RADIO_DRAIN_ENV;
    assert(strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "DRAIN A/B DIFFER"));

    /* Every pre-existing transmit condition still fires in order. */
    c.drain_a = c.drain_b = RADIO_DRAIN_VIN;
    c.envelope_mode = 1U;
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "ENVELOPE VS VIN DRAIN"));
    c.envelope_mode = 0U;
    c.drain_a = c.drain_b = RADIO_DRAIN_ENV;
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "ENVELOPE UNCALIBRATED"));
    c.envelope_dac_per_volt[0] = c.envelope_dac_per_volt[1] = 400.0f;
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 900000U), "NO FILTER"));
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 7100000U), "LPF UNTESTED"));
    c.qualified_bands = 31U;
    assert(!strcmp(RADIO_ConfigTxBlock(&c, 5800000U), "BELOW LPF TX MIN"));
    assert(RADIO_ConfigTxBlock(&c, 7100000U) == NULL);
    assert(RADIO_ConfigTxReady(&c, 7100000U, NULL, NULL));
}

/* Panel keys and the rotary encoder. */
static void controls_test(void)
{
    radio_config_t c;
    RADIO_ConfigDefaults(&c);

    /* Both spare panel keys do something out of the box. */
    assert(c.sw4 == RADIO_SW_PTT && c.sw5 == RADIO_SW_POWER);
    c.sw4 = RADIO_SW_LAST; assert(RADIO_ConfigValid(&c));
    c.sw4 = (uint8_t)(RADIO_SW_LAST + 1U); assert(!RADIO_ConfigValid(&c));
    c.sw4 = RADIO_SW_PTT;
    c.sw5 = (uint8_t)(RADIO_SW_LAST + 1U); assert(!RADIO_ConfigValid(&c));
    c.sw5 = RADIO_SW_POWER;

    /* Encoder defaults are JP20 pins 4/5/6, common to GND, 4 edges a step. */
    radio_config_t e;
    memset(&e, 0, sizeof(e));
    RADIO_ConfigEncoderDefaults(&e);
    assert(e.enc_a == RADIO_ENC_PIN_PC11 && e.enc_b == RADIO_ENC_PIN_PC12 &&
           e.enc_sw == RADIO_ENC_PIN_PC13);
    assert(e.enc_flags == 0U && e.enc_detent == 4U && e.enc_reserved == 0U);
    assert(c.enc_a == e.enc_a && c.enc_b == e.enc_b && c.enc_sw == e.enc_sw &&
           c.enc_flags == e.enc_flags && c.enc_detent == e.enc_detent);

    /* One pin may not serve two lines, but OFF may repeat. */
    c.enc_b = c.enc_a; assert(!RADIO_ConfigValid(&c));
    RADIO_ConfigDefaults(&c);
    c.enc_sw = c.enc_a; assert(!RADIO_ConfigValid(&c));
    RADIO_ConfigDefaults(&c);
    c.enc_a = c.enc_b = c.enc_sw = RADIO_ENC_PIN_OFF;
    assert(RADIO_ConfigValid(&c));

    /* Ranges. */
    RADIO_ConfigDefaults(&c);
    c.enc_a = RADIO_ENC_PIN_COUNT; assert(!RADIO_ConfigValid(&c));
    RADIO_ConfigDefaults(&c);
    c.enc_b = RADIO_ENC_PIN_COUNT; assert(!RADIO_ConfigValid(&c));
    RADIO_ConfigDefaults(&c);
    c.enc_sw = RADIO_ENC_PIN_COUNT; assert(!RADIO_ConfigValid(&c));
    RADIO_ConfigDefaults(&c);
    c.enc_detent = RADIO_ENC_DETENT_MIN - 1U; assert(!RADIO_ConfigValid(&c));
    c.enc_detent = RADIO_ENC_DETENT_MAX + 1U; assert(!RADIO_ConfigValid(&c));
    c.enc_detent = RADIO_ENC_DETENT_MAX; assert(RADIO_ConfigValid(&c));
    c.enc_flags = (uint8_t)(RADIO_ENC_FLAG_MASK + 1U); assert(!RADIO_ConfigValid(&c));
    c.enc_flags = RADIO_ENC_FLAG_MASK; assert(RADIO_ConfigValid(&c));
    c.enc_reserved = 1U; assert(!RADIO_ConfigValid(&c));
    /* FUNC key pin lives in boot_flags bits 2..5; a zero field is PC14 and
     * the other bits are untouched. */
    {
        radio_config_t f;
        RADIO_ConfigDefaults(&f);
        f.boot_flags = RADIO_BOOT_FLAG_DIAG_CONSOLE | RADIO_BOOT_FLAG_LCD_ROT270;
        assert(RADIO_ConfigFuncKeyPin(&f) == RADIO_ENC_PIN_PC14);
        for (uint8_t pin = 0U; pin < RADIO_ENC_PIN_COUNT; ++pin) {
            RADIO_ConfigSetFuncKeyPin(&f, pin);
            assert(RADIO_ConfigFuncKeyPin(&f) == pin);
            assert((f.boot_flags & 3U) == 3U && (f.boot_flags & ~0x3FU) == 0U);
            assert(RADIO_ConfigValid(&f));
        }
    }
}


/* Every toroid is wound on this build, so a fresh install must get all five
 * QMX+ filters. The slot-out-of-service mechanism still has to work, because
 * LPF n UPPER MHZ writes the same zero edge when an operator retires one
 * filter: such a slot must be invisible to both receive and transmit, and the
 * QMX+ restore action must bring it back. */
static void lpf_fitted_test(void)
{
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    assert(RADIO_ConfigValid(&c));

    /* Every toroid is wound on this build, so a fresh install gets the whole
     * QMX+ 160-10m set: edges and TX floors on all five slots. */
    assert(c.lpf_upper_hz[0] == 2500000UL && c.lpf_tx_min_khz[0] == 1700U);
    assert(c.lpf_upper_hz[1] == 5600000UL && c.lpf_tx_min_khz[1] == 3450U);
    assert(c.lpf_upper_hz[2] == 10300000UL && c.lpf_tx_min_khz[2] == 6300U);
    assert(c.lpf_upper_hz[3] == 21450000UL && c.lpf_tx_min_khz[3] == 12200U);
    assert(c.lpf_upper_hz[4] == RADIO_RX_MAX_HZ && c.lpf_tx_min_khz[4] == 20500U);

    /* The compiled unfitted mask is empty, asserted through the function that
     * applies it rather than by reading the macro: defaults survive it. */
    radio_config_t same = c;
    RADIO_ConfigLpfUnfitted(&same);
    assert(!memcmp(&same, &c, sizeof(same)));

    /* The whole receive range now resolves to a filter, including the bands
     * that had no RF path at all while LPF_3 and LPF_4 were out of service. */
    uint8_t lp = 9U, bp = 9U;
    static const struct { uint32_t hz; uint8_t slot; } covered[] = {
        {1000000U, 0U}, {1810000U, 0U}, {3573000U, 1U}, {5357000U, 1U},
        {7074000U, 2U}, {10136000U, 2U}, {14074000U, 3U}, {18100000U, 3U},
        {21074000U, 3U}, {24900000U, 4U}, {28000000U, 4U}, {RADIO_RX_MAX_HZ, 4U},
    };
    for (unsigned i = 0U; i < sizeof(covered) / sizeof(covered[0]); ++i)
        assert(RADIO_ConfigFilters(&c, covered[i].hz, &lp, &bp) && lp == covered[i].slot);
    assert(!RADIO_ConfigFilters(&c, RADIO_RX_MAX_HZ + 1UL, &lp, &bp) && lp == 255U);

    /* A commissioned board transmits on 20 m now, where it used to report
     * NO FILTER, and the harmonic floors still gate the gaps between slots. */
    radio_config_t tx = c;
    tx.confirmed = 1U;
    tx.drain_a = tx.drain_b = RADIO_DRAIN_VIN;
    tx.qualified_bands = 31U;
    assert(RADIO_ConfigValid(&tx));
    assert(RADIO_ConfigTxBlock(&tx, 7100000U) == NULL);
    assert(RADIO_ConfigTxBlock(&tx, 14074000U) == NULL);
    assert(RADIO_ConfigTxBlock(&tx, 28000000U) == NULL);
    assert(!strcmp(RADIO_ConfigTxBlock(&tx, 11000000U), "BELOW LPF TX MIN"));

    /* Taking a slot out of service by hand is the same encoding the unfitted
     * mask uses, so the dark-slot behaviour still has to hold: no receive
     * path, an explicit "no slot" rather than a stale index, and a transmit
     * refusal that names the filter even with every slot marked tested. */
    radio_config_t dark = tx;
    dark.lpf_upper_hz[3] = dark.lpf_upper_hz[4] = 0UL;
    dark.lpf_tx_min_khz[3] = dark.lpf_tx_min_khz[4] = 0U;
    assert(RADIO_ConfigValid(&dark));
    static const uint32_t unreachable[] = {10300001U, 14074000U, 18100000U, 21074000U, 24900000U, 28333333U};
    for (unsigned i = 0U; i < sizeof(unreachable) / sizeof(unreachable[0]); ++i) {
        assert(!RADIO_ConfigFilters(&dark, unreachable[i], &lp, &bp));
        assert(lp == 255U);
        assert(!strcmp(RADIO_ConfigTxBlock(&dark, unreachable[i]), "NO FILTER"));
        assert(!RADIO_ConfigTxReady(&dark, unreachable[i], &lp, &bp));
    }
    assert(RADIO_ConfigFilters(&dark, 7074000U, &lp, &bp) && lp == 2U);

    /* QMX+ LPF EDGES brings them back with no reflash, which is the path a
     * board whose settings were saved while a slot was dark has to take. */
    radio_config_t back = dark;
    RADIO_ConfigLpfDefaults(&back);
    assert(RADIO_ConfigValid(&back));
    assert(!memcmp(back.lpf_upper_hz, c.lpf_upper_hz, sizeof(back.lpf_upper_hz)));
    assert(!memcmp(back.lpf_tx_min_khz, c.lpf_tx_min_khz, sizeof(back.lpf_tx_min_khz)));
    assert(RADIO_ConfigFilters(&back, 14074000U, &lp, &bp) && lp == 3U);
    assert(RADIO_ConfigTxBlock(&back, 14074000U) == NULL);
}

int main(void)
{
    config_test(); lpf_fitted_test(); gate_test(); controls_test(); pd_test(); bias_test(); bias_noise_test();
    puts("REV1 configuration, UCC27524/PD defaults, CC termination, setup gating, panel keys and encoder, "
         "QRP Labs PIN bias model/loop/envelope, filter qualification and fixed/PPS limits: PASS");
    return 0;
}

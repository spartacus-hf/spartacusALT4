#define _GNU_SOURCE
/* REV1 receiver clocking: LO placement, balanced quadrature edges and the
 * HRTIM programming that produces them. The QSD model below is written from the
 * SN74CBT3253 truth table and the REV1 netlist, independently of the
 * production window formula it checks. */
#include "radio_clocks.h"
#include "radio_rx_front.h"
#include "radio.h"
#include "board_rev1.h"
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

uint32_t SystemCoreClock = 170000000U;
/* The exact clock tree (mod_band.c) at the nominal reference. */
uint64_t MOD_SysclkMicroHz(void) { return (uint64_t)SystemCoreClock * 1000000ULL; }
static radio_config_t test_config;
const radio_config_t *RADIO_Config(void) { return &test_config; }
radio_state_t RADIO_State(void) { return RADIO_RECEIVE; }
uint8_t BOARD_RfActive(void) { return 0U; }
uint8_t RADIO_CalActive(void) { return 0U; }
uint8_t BOARD_EmergencyLatched(void) { return 0U; }
void BOARD_SetRx(uint8_t active) { (void)active; }

#define TAU 6.283185307179586

/* Time-domain double-balanced QSD. U11 S1 = PB12, S0 = PB13, state 2*S1+S0
 * selects nB(state+1). Section 1 (1A) sees +RF, section 2 (2A) sees -RF.
 * REV1 ties 1Bk to 2B(5-k): node index k-1 is 1Bk. IN1 = 1B1 - 1B4 (U9),
 * IN2 = 1B3 - 1B2 (U12). Nodes are RC integrators with tau >> period. */
static double qsd_image_rejection_db(const radio_rx_geometry_t *g, double tau_periods, int sign,
                                     double *wanted_freq_sign)
{
    const unsigned p = g->period, h = g->half;
    const double fall = g->fall, rise = g->rise;
    const double tau = tau_periods * p, delta = sign / 1500.0; /* baseband = 1/1500 of LO */
    double node[4] = {0, 0, 0, 0};
    double edges[5] = {0.0, fall, h, h + rise, p};
    double complex plus = 0, minus = 0;
    const unsigned settle = 1500U, periods = 9000U;
    for (unsigned n = 0U; n < settle + periods; ++n) {
        for (unsigned seg = 0U; seg < 4U; ++seg) {
            double a = n * (double)p + edges[seg], b = n * (double)p + edges[seg + 1U];
            double mid = 0.5 * (edges[seg] + edges[seg + 1U]);
            unsigned s1 = mid < h, s0 = !(mid >= fall && mid < h + rise);
            unsigned state = 2U * s1 + s0;
            unsigned plus_node = state, minus_node = 3U - state;
            /* exact integral of cos(w t) over [a, b) with w = 2 pi (1+delta)/p */
            double w = TAU * (1.0 + delta) / p;
            double integral = (sin(w * b) - sin(w * a)) / w, len = b - a;
            node[plus_node] += (integral - node[plus_node] * len) / tau;
            node[minus_node] += (-integral - node[minus_node] * len) / tau;
        }
        if (n >= settle) {
            double complex z = (node[0] - node[3]) + I * (node[2] - node[1]); /* IN1 + j IN2 */
            double ph = TAU * fabs(delta) * n;
            plus += z * cexp(-I * ph);
            minus += z * cexp(I * ph);
        }
    }
    double wanted = sign > 0 ? cabs(plus) : cabs(minus);
    double image = sign > 0 ? cabs(minus) : cabs(plus);
    if (wanted_freq_sign) *wanted_freq_sign = cabs(plus) > cabs(minus) ? 1.0 : -1.0;
    return 20.0 * log10(wanted / (image + 1e-300));
}

static void quadrature_test(void)
{
    /* tau = 30 LO periods puts a 1/1500 IF at a quarter of the node RC corner,
     * like a 12 kHz IF with ~50 ohm driving the 33 nF QSD capacitors. */
    const unsigned periods[] = {192U, 193U, 194U, 195U, 389U, 1002U, 3023U};
    for (unsigned n = 0U; n < sizeof(periods) / sizeof(periods[0]); ++n) {
        const unsigned p = periods[n];
        radio_rx_geometry_t g, rounded;
        RADIO_RxClockEdges(p, &g);
        rounded = g;
        rounded.fall = rounded.rise = (p + 2U) / 4U;
        assert(g.half == p / 2U && g.fall + g.rise + (p - g.half) + g.half >= p);
        assert(g.half + g.rise < p && g.fall < g.half); /* windows ordered */
        for (int sign = -1; sign <= 1; sign += 2) {
            double orientation;
            double balanced = qsd_image_rejection_db(&g, 30.0, sign, &orientation);
            double plain = qsd_image_rejection_db(&rounded, 30.0, sign, NULL);
            /* A signal above the LO comes out at positive frequency (PB13
             * leading PB12 with REV1's cross-wired U11). */
            assert(orientation == (double)sign);
            assert(balanced > ((p & 1U) ? 58.0 : 63.0));
            /* Where one tick matters (upper HF), balancing clearly wins. */
            if (p < 400U && (p % 4U) != 0U && (p % 4U) != 3U) assert(balanced > plain + 4.0);
        }
    }
}

static void plan_test(void)
{
    const uint64_t hr = 170000000ULL * 32ULL;
    unsigned total = 0U, clean = 0U, any = 0U;
    for (unsigned mode = RADIO_RX_USB; mode <= RADIO_RX_FM; ++mode) {
        for (uint32_t hz = 1000000U; hz <= 28333333U; hz += 12347U) {
            uint32_t p = 0U;
            int32_t d = 0;
            uint8_t quality = RADIO_RxPlanLo(hr, hz, (radio_rx_mode_t)mode, &p, &d);
            ++total;
            if (!quality) continue;
            ++any;
            uint32_t lo = (uint32_t)((hr + p / 2U) / p);
            assert((int32_t)(hz - lo) == d && p >= 192U);
            static const int32_t edge[5][2] = {{300, 3000}, {-3000, -300}, {-350, 350}, {-5000, 5000}, {-10000, 10000}};
            assert(d + edge[mode][0] >= -50000 && d + edge[mode][1] <= 50000); /* channel inside flat span */
            if (quality == 2U) assert(d + edge[mode][0] > 600 || d + edge[mode][1] < -600); /* clear of DC */
            if (quality == 2U) ++clean;
            /* While the LO step is below the usable IF interval (about 14 MHz
             * for AM, 15.5 MHz for SSB/CW) a DC-free IF near 12 kHz exists. */
            if (hz < 14000000U && mode != RADIO_RX_FM) {
                assert(quality == 2U);
                uint32_t step = (uint32_t)(hz / p) + 1U;
                int32_t ad = d < 0 ? -d : d;
                /* The nearer LO step may be excluded for touching DC, so the
                 * chosen IF is within one LO step of 12 kHz. */
                assert((uint32_t)(ad > 12000 ? ad - 12000 : 12000 - ad) <= step + 100U + (mode == RADIO_RX_AM ? 6000U : 0U));
            }
        }
    }
    assert(any * 100U >= total * 80U && clean * 100U >= total * 70U);
    /* 40 m FT8 and 20 m SSB: the IF sits on 12 kHz within half an LO step
     * (9.2 kHz and 37 kHz per tick there). */
    uint32_t p; int32_t d;
    assert(RADIO_RxPlanLo(hr, 7074000U, RADIO_RX_USB, &p, &d) == 2U && abs(abs(d) - 12000) <= 4700);
    assert(RADIO_RxPlanLo(hr, 14200000U, RADIO_RX_USB, &p, &d) == 2U && abs(abs(d) - 12000) <= 18600);
    assert(RADIO_RxPlanLo(hr, 0U, RADIO_RX_USB, &p, &d) == 0U);
    /* Reference search over every period: best quality, then IF closest to
     * 12 kHz with a 4 kHz handicap for odd periods below 400 ticks. */
    static const int32_t chan[5][2] = {{300, 3000}, {-3000, -300}, {-350, 350}, {-5000, 5000}, {-10000, 10000}};
    unsigned ties = 0U;
    for (unsigned mode = RADIO_RX_USB; mode <= RADIO_RX_FM; ++mode) {
        for (uint32_t hz = 1800000U; hz <= 28333333U; hz += 99991U) {
            uint32_t ref_p = 0U; int32_t ref_d = 0; uint8_t ref_q = 0U; uint32_t ref_score = 0xFFFFFFFFU;
            for (uint32_t pp = 192U; pp <= 60000U; ++pp) {
                uint32_t lo = (uint32_t)((hr + pp / 2U) / pp);
                int64_t dd = (int64_t)hz - lo;
                if (dd < -60000 || dd > 60000) continue;
                int32_t a = (int32_t)dd + chan[mode][0], b = (int32_t)dd + chan[mode][1];
                if (a < -50000 || b > 50000) continue;
                uint8_t qq = (a > 600 || b < -600) ? 2U : 1U;
                uint32_t ad = (uint32_t)(dd < 0 ? -dd : dd);
                uint32_t sc = (ad > 12000U ? ad - 12000U : 12000U - ad) + (((pp & 1U) && pp < 400U) ? 4000U : 0U);
                if (qq > ref_q || (qq == ref_q && sc < ref_score)) { ref_q = qq; ref_score = sc; ref_p = pp; ref_d = (int32_t)dd; }
            }
            uint8_t quality = RADIO_RxPlanLo(hr, hz, (radio_rx_mode_t)mode, &p, &d);
            assert(quality == ref_q);
            if (quality) assert(p == ref_p && d == ref_d);
            if (quality && ref_p < 400U && !(ref_p & 1U)) ++ties;
        }
    }
    assert(ties > 0U);
}

static void hrtim_program_test(void)
{
    const uintptr_t regions[] = {0x40010000U, 0x48000000U};
    for (unsigned n = 0U; n < 2U; ++n)
        assert(mmap((void *)regions[n], 65536, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    for (uint32_t hz = 1800000U; hz <= 28300000U; hz += 1333333U) {
        uint32_t planned = 0U; int32_t unused;
        if (!RADIO_RxPlanLo((uint64_t)SystemCoreClock * 32ULL, hz, RADIO_RX_USB, &planned, &unused)) {
            assert(!RADIO_RxClockStart(hz, RADIO_RX_USB) && RADIO_RxClockHz() == 0U);
            continue;
        }
        assert(RADIO_RxClockStart(hz, RADIO_RX_USB));
        HRTIM_Timerx_TypeDef *b = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B];
        HRTIM_Timerx_TypeDef *c = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C];
        radio_rx_geometry_t g;
        RADIO_RxClockGeometry(&g);
        uint32_t p = g.period, h = g.half;
        assert(p == planned);
        radio_rx_geometry_t balanced;
        RADIO_RxClockEdges(p, &balanced);
        assert(g.fall == balanced.fall && g.rise == balanced.rise && h == balanced.half);
        assert(c->PERxR == p && c->CMP1xR == h && c->CMP2xR == h + g.rise);
        assert(c->SETx1R == HRTIM_OUTPUTSET_TIMPER && c->RSTx1R == HRTIM_OUTPUTRESET_TIMCMP1);
        assert(c->SETx2R == HRTIM_OUTPUTSET_TIMCMP2 && c->RSTx2R == HRTIM_OUTPUTRESET_TIMCEV3_TIMBCMP2);
        /* No cross-timer counter reset; B leads through a shortened first
         * period and the preloaded full period (last write). */
        assert(b->RSTxR == 0U && c->RSTxR == 0U && b->REPxR == 0U);
        assert((b->TIMxCR & (HRTIM_TIMCR_PREEN | HRTIM_TIMCR_TREPU | HRTIM_TIMCR_CONT)) ==
               (HRTIM_TIMCR_PREEN | HRTIM_TIMCR_TREPU | HRTIM_TIMCR_CONT));
        assert(b->PERxR == p && b->CMP2xR == h + g.fall);
        /* Legal high-resolution values (>= 3 coarse periods, <= 0xFFDF). */
        assert(p - h >= 96U && h >= 96U && h + g.rise < p && h + g.fall < p && p <= 0xFFDFU);
        assert(((GPIOB->AFR[1] >> 16U) & 0xFFU) == 0xDDU);
        uint32_t lo = (uint32_t)(((uint64_t)SystemCoreClock * 32ULL + p / 2U) / p);
        assert(RADIO_RxClockHz() == lo && RADIO_RxClockQuality() >= 1U);
    }
    /* BPF sweep: test clock (Timer D) at exactly the LO period, started in
     * the same MCR write, output left disabled for the chop gate. */
    for (uint32_t hz = 4000000U; hz <= RADIO_RX_MAX_HZ; hz += 1733333U) {
        HRTIM1->sCommonRegs.OENR = 0U;
        assert(RADIO_MeasureClocksStart(hz));
        HRTIM_Timerx_TypeDef *b = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B];
        HRTIM_Timerx_TypeDef *c = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C];
        HRTIM_Timerx_TypeDef *d = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D];
        const uint64_t clock = (uint64_t)SystemCoreClock * 32ULL;
        const uint32_t p = (uint32_t)((clock + hz / 2U) / hz);
        radio_rx_geometry_t g;
        RADIO_RxClockGeometry(&g);
        assert(g.period == p && c->PERxR == p && b->PERxR == p && d->PERxR == p);
        assert(d->CMP1xR == p / 2U && d->SETx1R == HRTIM_OUTPUTSET_TIMPER && d->RSTx1R == HRTIM_OUTPUTRESET_TIMCMP1);
        assert(d->CNTxR == 0U && c->CNTxR == 0U);
        const uint32_t timers = HRTIM_TIMERID_TIMER_B | HRTIM_TIMERID_TIMER_C | HRTIM_TIMERID_TIMER_D;
        assert((HRTIM1->sMasterRegs.MCR & timers) == timers);
        assert(HRTIM1->sCommonRegs.OENR == BOARD_RX_OUTPUTS);
        assert(((GPIOB->AFR[1] >> 24U) & 15U) == 13U && ((GPIOB->MODER >> 28U) & 3U) == 2U);
        assert(RADIO_RxClockQuality() == 0U);
        const uint32_t lo = (uint32_t)((clock + p / 2U) / p);
        assert(RADIO_RxClockHz() == lo && (lo > hz ? lo - hz : hz - lo) <= hz / p / 2U + 1U);
    }
    RADIO_TestOutput(1U);
    assert(HRTIM1->sCommonRegs.OENR == BOARD_TEST_OUTPUT);
    RADIO_TestOutput(0U);
    assert(HRTIM1->sCommonRegs.ODISR == BOARD_TEST_OUTPUT);
    assert(!RADIO_MeasureClocksStart(900000U) && !RADIO_MeasureClocksStart(RADIO_RX_MAX_HZ + 1U));
}


/* The premise mod_band.c's clock shopping rests on: above about 14 MHz one
 * HRTIM tick moves the LO by more than the IF the planner is aiming for, so
 * the fixed 170 MHz clock can leave the IF tens of kilohertz out while still
 * passing every check RADIO_RxPlanLo makes -- and another legal PLL setting in
 * the same narrow SYSCLK window lands much closer.
 *
 * This matters because REV1's residual I/Q error is a fixed skew between the
 * two channels, so the phase error it produces is proportional to the IF.
 * Measured on the board: 12.0 degrees at a 9.3 kHz offset and 69.9 degrees at
 * 36.0 kHz, which is 50 dB of corrected image rejection against 6 dB. */
static uint32_t if_error(uint32_t clk_hz, uint32_t rf_hz)
{
    int32_t d = 0;
    if (RADIO_RxPlanLo((uint64_t)clk_hz * 32ULL, rf_hz, RADIO_RX_USB, NULL, &d) < 2U)
        return 0xFFFFFFFFU;
    uint32_t a = (uint32_t)(d < 0 ? -d : d);
    return a > 12000U ? a - 12000U : 12000U - a;
}

static uint32_t best_clock_if_error(uint32_t rf_hz, uint32_t *at_clk)
{
    static const uint32_t r_values[] = {2U, 4U, 6U, 8U};
    uint32_t best = 0xFFFFFFFFU;
    for (uint32_t m = 1U; m <= 16U; ++m) {
        if (8000000ULL < 2660000ULL * m || 8000000ULL > 16000000ULL * m) continue;
        for (uint32_t n = 8U; n <= 127U; ++n) {
            uint64_t vco = 8000000ULL * n;
            if (vco < 64000000ULL * m || vco > 344000000ULL * m) continue;
            for (unsigned ri = 0U; ri < 4U; ++ri) {
                uint32_t den = m * r_values[ri];
                uint32_t clk = (uint32_t)((vco + den / 2U) / den);
                if (clk < 165000000U || clk > 170000000U) continue;
                uint32_t e = if_error(clk, rf_hz);
                if (e < best) { best = e; if (at_clk) *at_clk = clk; }
            }
        }
    }
    return best;
}

static void clock_shopping_test(void)
{
    /* Below 14 MHz the fixed clock already lands near the target, so nothing
     * should tempt the chooser away from it. */
    static const uint32_t settled[] = {1900000U, 3700000U, 7100000U};
    for (unsigned k = 0U; k < sizeof(settled) / sizeof(settled[0]); ++k)
        assert(if_error(170000000U, settled[k]) <= 8000U);

    /* On 20 m and 17 m it does not, and a better clock exists in the window.
     * These are the two cases the shopping was added for. */
    static const uint32_t stranded[] = {14020000U, 18100000U};
    for (unsigned k = 0U; k < sizeof(stranded) / sizeof(stranded[0]); ++k) {
        uint32_t clk = 0U;
        const uint32_t here = if_error(170000000U, stranded[k]);
        const uint32_t there = best_clock_if_error(stranded[k], &clk);
        assert(here > 8000U);                 /* the fixed clock is off target */
        assert(there + 4000U < here);         /* and another one is better by a margin */
        assert(clk >= 165000000U && clk <= 170000000U);
    }

    /* A frequency the fixed clock cannot plan at all, which the existing
     * quality-0 path already rescues. Both halves rely on the same window. */
    uint32_t clk = 0U;
    assert(RADIO_RxPlanLo(170000000ULL * 32ULL, 28100000U, RADIO_RX_USB, NULL, NULL) == 0U);
    assert(best_clock_if_error(28100000U, &clk) != 0xFFFFFFFFU);
}

int main(void)
{
    RADIO_ConfigDefaults(&test_config);
    quadrature_test();
    plan_test();
    clock_shopping_test();
    hrtim_program_test();
    puts("REV1 receiver balanced quadrature edges, 12 kHz IF planning and HRTIM clock programming: PASS");
    return 0;
}

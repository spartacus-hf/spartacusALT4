#include "clock_plan.h"
#include "mod_transport_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

static int64_t labs64(int64_t v) { return v < 0 ? -v : v; }

/* The 8 MHz reference allows PLL M of 1..3 only (input >= 2.66 MHz), so the
 * validated 165-170 MHz window holds exactly six system clocks. */
static void test_candidates(void)
{
    clock_pll_t c[CLOCK_PLL_CANDIDATES_MAX];
    const unsigned n = CLOCK_PllCandidates(CLOCK_SYSCLK_MIN_HZ, CLOCK_SYSCLK_MAX_HZ, c, CLOCK_PLL_CANDIDATES_MAX);
    static const uint32_t want[] = {165333333U, 166000000U, 166666667U, 168000000U, 169333333U, 170000000U};
    CHECK(n == 6U);
    for (unsigned i = 0U; i < n && i < 6U; ++i) {
        CHECK(CLOCK_PllNominalHz(c[i]) == want[i]);
        CHECK(CLOCK_PllLegal(c[i]));
        CHECK(c[i].m >= 1U && c[i].m <= 3U);
    }
    /* 168 MHz is reachable as 1/42, 2/84 and 3/126: the smallest M wins. */
    CHECK(n > 3U && c[3].m == 1U && c[3].n == 42U && c[3].r == 2U);
    const unsigned wide = CLOCK_PllCandidates(CLOCK_SYSCLK_AUTO_MIN_HZ, CLOCK_SYSCLK_MAX_HZ, c, CLOCK_PLL_CANDIDATES_MAX);
    CHECK(wide > 15U && wide <= CLOCK_PLL_CANDIDATES_MAX);
    for (unsigned i = 1U; i < wide; ++i) CHECK(CLOCK_PllNominalHz(c[i]) > CLOCK_PllNominalHz(c[i - 1U]));
    const clock_pll_t m4 = {4U, 85U, 2U}; /* 2 MHz input: below the PLL minimum */
    CHECK(!CLOCK_PllLegal(m4));
}

/* AUTO's real floor is the receive worker's cycle budget per PCM1821 frame:
 * 164 MHz keeps divider 15 like 165-170 MHz; WIDE adds the divider-14 clocks
 * (153.3-162.7 MHz) and nothing ever takes divider 13 (150-152 MHz), which
 * overran with USB audio on the bench. */
static void test_receive_budget(void)
{
    CHECK(CLOCK_RxFrameCycles(170000000U) == 960U);
    CHECK(CLOCK_RxFrameCycles(165333333U) == 960U);
    CHECK(CLOCK_RxFrameCycles(164000000U) == 960U);
    CHECK(CLOCK_RxFrameCycles(162666667U) == 896U);
    CHECK(CLOCK_RxFrameCycles(153333333U) == 896U);
    CHECK(CLOCK_RxFrameCycles(152000000U) == 832U);
    CHECK(CLOCK_RxFrameCycles(150000000U) == 832U);
    clock_pll_t c[CLOCK_PLL_CANDIDATES_MAX];
    const unsigned n = CLOCK_PllCandidates(CLOCK_SYSCLK_AUTO_MIN_HZ, CLOCK_SYSCLK_MAX_HZ, c, CLOCK_PLL_CANDIDATES_MAX);
    unsigned normal = 0U, wide = 0U;
    for (unsigned i = 0U; i < n; ++i) {
        const uint32_t hz = CLOCK_PllNominalHz(c[i]), cyc = CLOCK_RxFrameCycles(hz);
        if (cyc >= CLOCK_RX_FRAME_CYCLES_MIN) { ++normal; CHECK(hz >= 164000000U); }
        if (cyc >= CLOCK_RX_FRAME_CYCLES_WIDE_MIN) { ++wide; CHECK(hz > 153000000U); }
    }
    CHECK(normal == 7U);
    CHECK(wide == 18U);
}

/* The transmitter's budget (CLOCK_TxFits) gates WIDE's divider-14 clocks:
 * USB-audio SSB with the speech tools took 99.7 % of the CPU at 153.3 MHz
 * with TX PHASE 384K. Voice SSB at 384 kHz fits none of them, at 192 kHz
 * from 154.7 MHz; DIGI CLEAN SSB, AM, CW and FM all of them at either rate. */
static void test_tx_budget(void)
{
    CHECK(CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_SSB, 1U) > CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_SSB, 0U));
    CHECK(CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_SSB, 1U) > CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_SSB_CLEAN, 1U));
    CHECK(CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_SSB, 1U) > CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_AM, 1U));
    CHECK(CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_AM, 1U) > CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_FM, 1U));
    CHECK(CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_COUNT, 1U) == CLOCK_TxCyclesPerMs(CLOCK_TX_LOAD_SSB, 1U));
    /* The bench worst cases, with their margin, stay inside the budget at 170 MHz. */
    for (unsigned l = 0U; l < CLOCK_TX_LOAD_COUNT; ++l)
        for (unsigned p = 0U; p < 2U; ++p) CHECK(CLOCK_TxFits(170000000U, (clock_tx_load_t)l, (uint8_t)p));
    CHECK(!CLOCK_TxFits(153333333U, CLOCK_TX_LOAD_SSB, 1U));
    clock_pll_t c[CLOCK_PLL_CANDIDATES_MAX];
    const unsigned n = CLOCK_PllCandidates(CLOCK_SYSCLK_AUTO_MIN_HZ, CLOCK_SYSCLK_MAX_HZ, c, CLOCK_PLL_CANDIDATES_MAX);
    unsigned ssb192 = 0U;
    for (unsigned i = 0U; i < n; ++i) {
        const uint32_t hz = CLOCK_PllNominalHz(c[i]), cyc = CLOCK_RxFrameCycles(hz);
        if (cyc >= CLOCK_RX_FRAME_CYCLES_MIN || cyc < CLOCK_RX_FRAME_CYCLES_WIDE_MIN) continue;
        CHECK(!CLOCK_TxFits(hz, CLOCK_TX_LOAD_SSB, 1U));
        if (CLOCK_TxFits(hz, CLOCK_TX_LOAD_SSB, 0U)) { ++ssb192; CHECK(hz > 154500000U); }
        else CHECK(hz < 154500000U);
        for (unsigned p = 0U; p < 2U; ++p) {
            CHECK(CLOCK_TxFits(hz, CLOCK_TX_LOAD_SSB_CLEAN, (uint8_t)p));
            CHECK(CLOCK_TxFits(hz, CLOCK_TX_LOAD_AM, (uint8_t)p));
            CHECK(CLOCK_TxFits(hz, CLOCK_TX_LOAD_CW, (uint8_t)p));
            CHECK(CLOCK_TxFits(hz, CLOCK_TX_LOAD_FM, (uint8_t)p));
        }
    }
    CHECK(ssb192 == 9U);
}

static void test_exact_arithmetic(void)
{
    const clock_pll_t p170 = {2U, 85U, 2U}, p165 = {3U, 124U, 2U};
    CHECK(CLOCK_HseMicroHz(0) == 8000000000000ULL);
    CHECK(CLOCK_HseMicroHz(7190) == 8000000000000ULL + 57520000ULL); /* +7.19 ppm = +57.52 Hz */
    CHECK(CLOCK_PllMicroHz(p170, 0) == 170000000000000ULL);
    /* The CMSIS SystemCoreClockUpdate truncation this replaces gave 165333292. */
    CHECK(CLOCK_PllMicroHz(p165, 0) == 165333333333333ULL);
    CHECK(CLOCK_PllNominalHz(p165) == 165333333U);
    /* 10 MHz is exactly 544 ticks at 170 MHz; the error is then the ppm only. */
    const uint16_t p = CLOCK_CarrierPeriod(CLOCK_PllMicroHz(p170, 0), 10000000U);
    CHECK(p == 544U);
    CHECK(CLOCK_PeriodErrorMilliHz(CLOCK_PllMicroHz(p170, 0), p, 10000000U) == 0);
    CHECK(CLOCK_PeriodErrorMilliHz(CLOCK_PllMicroHz(p170, 7190), p, 10000000U) == 71900);
    /* 14.074 MHz at 170 MHz: 387 ticks, 14.056848 MHz, -17.152 kHz grid. */
    const uint16_t q = CLOCK_CarrierPeriod(CLOCK_PllMicroHz(p170, 0), 14074000U);
    CHECK(q == 387U);
    CHECK(labs64(CLOCK_PeriodErrorMilliHz(CLOCK_PllMicroHz(p170, 0), q, 14074000U) + 17152455) < 2);
    /* Period floor: 28.333 MHz needs 170 MHz; 165.3 MHz cannot reach it. */
    CHECK(CLOCK_CarrierPeriod(CLOCK_PllMicroHz(p165, 0), 28333000U) == 0U);
    CHECK(CLOCK_CarrierPeriod(CLOCK_PllMicroHz(p170, 0), 28333000U) == 192U);
}

static void test_image_estimate(void)
{
    /* ZOH images of the phase staircase: x/(1-x) with x = err/rate. */
    CHECK(CLOCK_ImageDbc(18000U, 192000U) == -20);
    CHECK(CLOCK_ImageDbc(1360U, 192000U) == -43);
    CHECK(CLOCK_ImageDbc(0U, 192000U) == -200);
    CHECK(CLOCK_ImageDbc(18000U, 384000U) == -26);
}

static int index_of(const clock_eval_t *e, unsigned n, uint32_t hz)
{
    for (unsigned i = 0U; i < n; ++i) if (e[i].sysclk_hz == hz) return (int)i;
    return -1;
}

/* What mod_band.c's AUTO sees: every setting from 150 MHz, those short of
 * the receive budget with no TX period so they are never chosen. */
static unsigned evaluate_tx(uint32_t dial, int32_t centre, radio_rx_mode_t mode, uint8_t wide,
                            clock_tx_load_t load, uint8_t phase384, clock_eval_t *e)
{
    clock_pll_t c[CLOCK_PLL_CANDIDATES_MAX];
    const unsigned n = CLOCK_PllCandidates(CLOCK_SYSCLK_AUTO_MIN_HZ, CLOCK_SYSCLK_MAX_HZ, c, CLOCK_PLL_CANDIDATES_MAX);
    /* mod_band.c's carrier_tx_shape, duty offset 0 */
    const clock_tx_shape_t shape = {phase384 ? 384000U : 192000U, (load == CLOCK_TX_LOAD_CW) ? 2U : 8U, 0};
    for (unsigned i = 0U; i < n; ++i) {
        CLOCK_Evaluate(c[i], 0, dial, centre, mode, &shape, &e[i]);
        /* mod_band.c's carrier_auto_clock_ok */
        const uint32_t cyc = CLOCK_RxFrameCycles(e[i].sysclk_hz);
        const uint8_t ok = (cyc >= CLOCK_RX_FRAME_CYCLES_MIN) ||
                           (wide && cyc >= CLOCK_RX_FRAME_CYCLES_WIDE_MIN &&
                            CLOCK_TxFits(e[i].sysclk_hz, load, phase384));
        if (!ok) e[i].period = 0U;
    }
    return n;
}

static unsigned evaluate_all(uint32_t dial, int32_t centre, radio_rx_mode_t mode, uint8_t wide, clock_eval_t *e)
{
    /* The FT8 case: USB audio under DIGI CLEAN at the 192 kHz phase rate. */
    return evaluate_tx(dial, centre, mode, wide, CLOCK_TX_LOAD_SSB_CLEAN, 0U, e);
}

static void test_auto_choice(void)
{
    clock_eval_t e[CLOCK_PLL_CANDIDATES_MAX];
    /* 14.074 MHz USB: with the fraction every clock that keeps the IF near
     * 12 kHz sits at or near its 184 ps floor, update_hz / P; 170 MHz has the
     * longest period and so the lowest floor, 497 Hz at k = +1. */
    unsigned n = evaluate_all(14074000U, 1500, RADIO_RX_USB, 0U, e);
    const int at170 = index_of(e, n, 170000000U), at166 = index_of(e, n, 166000000U);
    const int at164 = index_of(e, n, 164000000U), at154 = index_of(e, n, 154000000U);
    const int pick = CLOCK_ChooseAuto(e, n, -1);
    CHECK(pick >= 0 && pick == at170);
    CHECK(e[pick].period != 0U);
    CHECK(CLOCK_RxClass(&e[pick]) == 3U);
    CHECK(e[pick].extra != 0 && e[pick].tx_err_hz == e[pick].floor_hz);
    CHECK(CLOCK_TxCostSettled(&e[pick]));
    /* Whatever it picked, no candidate in its receive class beats it. */
    for (unsigned i = 0U; i < n; ++i) {
        if (!e[i].period || CLOCK_RxClass(&e[i]) != CLOCK_RxClass(&e[pick])) continue;
        CHECK(e[i].tx_err_hz >= e[pick].tx_err_hz);
    }
    /* A candidate never wins on TX alone at the cost of the receiver. */
    for (unsigned i = 0U; i < n; ++i) if (e[i].period) CHECK(CLOCK_RxClass(&e[i]) <= CLOCK_RxClass(&e[pick]));
    /* Already on 166 MHz, which needs k = +2 here (1019 Hz): 6 dB, worth the
     * retune; so is 164 MHz, at k = +4 (2065 Hz) now that k < 0 is gone. */
    CHECK(at166 >= 0 && !CLOCK_TxCostSettled(&e[at166]));
    CHECK(CLOCK_ChooseAuto(e, n, at166) == pick);
    CHECK(at164 >= 0 && !CLOCK_TxCostSettled(&e[at164]) && CLOCK_ChooseAuto(e, n, at164) == pick);
    /* WIDE gives the divider-14 clocks a TX period (154 MHz) but none beats
     * 170 MHz; 150 MHz never transmits. */
    CHECK(at154 >= 0 && !e[at154].period);
    n = evaluate_all(14074000U, 1500, RADIO_RX_USB, 1U, e);
    CHECK(e[index_of(e, n, 154000000U)].period != 0U);
    CHECK(CLOCK_ChooseAuto(e, n, -1) == index_of(e, n, 170000000U));
    CHECK(index_of(e, n, 150000000U) >= 0 && !e[index_of(e, n, 150000000U)].period);

    /* Hysteresis: a settled current clock stays. 170 MHz on its own grid at
     * P = 543 (odd: N = 3), costing only its floor. */
    n = evaluate_all(10018416U, 0, RADIO_RX_CW, 0U, e);
    const int ten = index_of(e, n, 170000000U);
    CHECK(ten >= 0 && e[ten].period == 543U && e[ten].cycles == 3U && e[ten].tx_err_hz == e[ten].floor_hz);
    CHECK(CLOCK_ChooseAuto(e, n, ten) == ten || CLOCK_RxClass(&e[CLOCK_ChooseAuto(e, n, ten)]) > CLOCK_RxClass(&e[ten]));

    /* Synthetic, floor 500 Hz (settled up to 707): same class, gains below
     * 3 dB or from a settled cost do not retune. */
    clock_eval_t s[2] = {{.pll = {2U, 85U, 2U}, .sysclk_hz = 170000000U, .period = 300U, .cycles = 2U,
                          .tx_err_hz = 1000U, .floor_hz = 500U, .rx_quality = 2U, .rx_if_err_hz = 1000U},
                         {.pll = {3U, 127U, 2U}, .sysclk_hz = 169333333U, .period = 301U, .cycles = 2U,
                          .tx_err_hz = 800U, .floor_hz = 500U, .rx_quality = 2U, .rx_if_err_hz = 1000U}};
    CHECK(!CLOCK_TxCostSettled(&s[0]));
    CHECK(CLOCK_ChooseAuto(s, 2U, 0) == 0);
    s[1].tx_err_hz = 600U;
    CHECK(CLOCK_ChooseAuto(s, 2U, 0) == 1);
    s[0].tx_err_hz = 707U; s[1].tx_err_hz = 10U;
    CHECK(CLOCK_TxCostSettled(&s[0]) && CLOCK_ChooseAuto(s, 2U, 0) == 0);
    s[0].tx_err_hz = 708U;
    CHECK(!CLOCK_TxCostSettled(&s[0]) && CLOCK_ChooseAuto(s, 2U, 0) == 1);
    /* Under CLOCK_TX_HOP_COST_HZ is settled whatever the floor. */
    s[0].tx_err_hz = CLOCK_TX_HOP_COST_HZ; s[0].floor_hz = 100U;
    CHECK(CLOCK_TxCostSettled(&s[0]) && CLOCK_ChooseAuto(s, 2U, 0) == 0);
    s[0].floor_hz = 500U;
    /* A better receive class always wins, even with a larger TX residual. */
    s[0].tx_err_hz = 100U; s[0].rx_if_err_hz = 20000U;
    s[1].tx_err_hz = 5000U; s[1].rx_if_err_hz = 1000U;
    CHECK(CLOCK_ChooseAuto(s, 2U, 0) == 1);
    /* No legal TX period is never chosen, nor settled. */
    s[1].period = 0U;
    CHECK(CLOCK_ChooseAuto(s, 2U, -1) == 0);
    CHECK(!CLOCK_TxCostSettled(&s[1]));
}

/* The key-down hop: only from an unsettled cost, only to a clean receive
 * plan, only for 6 dB. */
static void test_choose_tx(void)
{
    clock_eval_t s[4] = {
        {.sysclk_hz = 170000000U, .period = 259U, .cycles = 7U, .extra = 2, .tx_err_hz = 3000U,
         .floor_hz = 741U, .rx_quality = 2U, .rx_if_err_hz = 1000U},
        {.sysclk_hz = 166000000U, .period = 253U, .cycles = 2U, .tx_err_hz = 1600U,
         .floor_hz = 759U, .rx_quality = 2U, .rx_if_err_hz = 1000U},
        {.sysclk_hz = 164000000U, .period = 250U, .cycles = 2U, .tx_err_hz = 800U,
         .floor_hz = 768U, .rx_quality = 1U, .rx_if_err_hz = UINT32_MAX},
        {.sysclk_hz = 160000000U, .period = 244U, .cycles = 3U, .extra = -1, .tx_err_hz = 900U,
         .floor_hz = 787U, .rx_quality = 2U, .rx_if_err_hz = 9000U}};
    /* 3000 -> 900 Hz; 164 MHz is cleaner still but receives at DC. */
    CHECK(CLOCK_ChooseTx(s, 4U, 0) == 3);
    /* Not halved: 1600 -> 900 stays. */
    CHECK(CLOCK_ChooseTx(s, 4U, 1) == 1);
    /* Settled (within 3 dB of its floor) stays even with a better one. */
    s[0].tx_err_hz = 1000U;
    s[3].tx_err_hz = 300U;
    CHECK(CLOCK_TxCostSettled(&s[0]) && CLOCK_ChooseTx(s, 4U, 0) == 0);
    s[0].tx_err_hz = 3000U;
    /* A tie goes to the better receive class, then the higher clock. */
    s[1].tx_err_hz = 300U; s[1].rx_if_err_hz = 9000U; s[3].rx_if_err_hz = 1000U;
    CHECK(CLOCK_ChooseTx(s, 4U, 0) == 3);
    s[3].rx_if_err_hz = 9000U;
    CHECK(CLOCK_ChooseTx(s, 4U, 0) == 1);
    /* No current index, or none that can transmit. */
    CHECK(CLOCK_ChooseTx(s, 4U, -1) == -1);
    s[0].period = 0U;
    CHECK(CLOCK_ChooseTx(s, 4U, 0) == 1);
    /* Nothing with a clean receive plan: stay. */
    s[0].period = 259U;
    s[1].rx_quality = 0U; s[3].rx_quality = 1U;
    CHECK(CLOCK_ChooseTx(s, 4U, 0) == 0);
}

/* The best plan by brute force: every k and N the shape allows. */
static uint32_t carrier_brute_cost(uint64_t sysclk_uhz, uint32_t f, const clock_tx_shape_t *sh)
{
    const uint64_t hr = sysclk_uhz * CLOCK_HRTIM_MUL;
    const uint64_t master_max = hr / ((uint64_t)sh->update_hz * 1000000ULL) / sh->master_div;
    const uint32_t below = (uint32_t)(hr / ((uint64_t)f * 1000000ULL));
    uint32_t best = UINT32_MAX;
    for (uint32_t p = below - 1U; p <= below + 1U; ++p) {
        if ((p < CLOCK_MIN_PERIOD_TICKS) || (p > 65535U)) continue;
        for (int32_t k = 0; k <= CLOCK_CARRIER_EXTRA_MAX; ++k) {
            for (uint32_t n = ((uint32_t)k + 1U > 2U) ? (uint32_t)k + 1U : 2U; n <= CLOCK_CARRIER_CYCLES_MAX; ++n) {
                /* The whole period's master may span two periods past the cap. */
                if (((k != 0) || (n > 2U)) && ((int64_t)n * p + k > (int64_t)master_max)) break;
                const clock_carrier_t c = {(uint16_t)p, (uint8_t)n, (int8_t)k};
                const uint32_t cost = CLOCK_CarrierCostHz(sysclk_uhz, &c, f, sh->update_hz);
                if (cost < best) best = cost;
            }
        }
    }
    return best;
}

/* The branch timer under RM0440 28.3.4's rule, two counter resets in one
 * fHRTIM cycle keeping the last: the master resets it every N x P + k ticks
 * plus the NCO's step, `s` ticks an update (negative an advance), and a reset
 * landing before the timer's own roll-over in that roll-over's cycle is lost.
 * Returns the furthest the timer's phase strays from the resets, ticks. */
static uint32_t reset_model_stray(const clock_carrier_t *c, double s, double update_ticks, uint32_t masters)
{
    const int64_t p = c->period, m = (int64_t)c->cycles * c->period + c->extra;
    int64_t reset = 7, anchor = 7, shift = 0;
    double phase = 0.0, t = 0.0, next = update_ticks;
    uint32_t worst = 0U;
    for (uint32_t i = 0U; i < masters; ++i) {
        t += (double)m;
        int64_t step = 0;
        if (t >= next) {
            next += update_ticks;
            phase += s;
            const int64_t now = (int64_t)floor(phase + 0.5);
            step = now - shift;
            shift = now;
        }
        reset += m + step;
        const int64_t rollover = anchor + ((reset - anchor) / p + 1) * p;
        if (!((rollover - reset < 32) && (rollover / 32 == reset / 32))) anchor = reset;
        int64_t e = (anchor - reset) % p;
        if (e < 0) e += p;
        if (e > p / 2) e -= p;
        const uint32_t mag = (uint32_t)(e < 0 ? -e : e);
        if (mag > worst) worst = mag;
    }
    return worst;
}

/* CLOCK_CarrierPlan across 1.8-28.3 MHz at the fastest and slowest clocks,
 * for the CW, SSB and 384K shapes and the PA's duty offsets: the firmware's
 * own geometry takes every plan at any phase, the master stays inside its
 * cap, the plan is never costlier than the whole period, and it is the best
 * plan within its limits. */
static void test_carrier_plan(void)
{
    static const clock_tx_shape_t shapes[] = {
        {192000U, 2U, 0}, {192000U, 8U, 0}, {384000U, 8U, 0},
        {192000U, 8U, 150}, {192000U, 2U, -150}};
    static const uint64_t clocks[] = {170000000000000ULL, 153333333333333ULL};
    unsigned plans = 0U, fractional = 0U, wide_k = 0U;
    for (unsigned ci = 0U; ci < 2U; ++ci) for (unsigned si = 0U; si < sizeof(shapes) / sizeof(shapes[0]); ++si) {
        const clock_tx_shape_t *sh = &shapes[si];
        const uint64_t uhz = clocks[ci];
        const uint64_t master_max = uhz * CLOCK_HRTIM_MUL / ((uint64_t)sh->update_hz * 1000000ULL) / sh->master_div;
        for (uint32_t f = 1800000U; f <= 28333000U; f += 7919U) {
            clock_carrier_t c;
            if (!CLOCK_CarrierPlan(uhz, f, sh, &c)) {
                CHECK(c.period == 0U);
                continue;
            }
            ++plans;
            CHECK(c.cycles >= 2U && (int32_t)c.cycles > c.extra);
            CHECK(c.extra >= 0 && c.extra <= CLOCK_CARRIER_EXTRA_MAX);
            if (c.extra != 0) {
                ++fractional;
                if (c.extra > 2) ++wide_k;
                CHECK((uint64_t)((int64_t)c.cycles * c.period + c.extra) <= master_max);
            } else {
                CHECK(c.cycles == 2U || (c.cycles == 3U && 3U * c.period <= master_max));
            }
            const clock_carrier_t whole = {CLOCK_CarrierPeriod(uhz, f), 2U, 0};
            const uint32_t cost = CLOCK_CarrierCostHz(uhz, &c, f, sh->update_hz);
            CHECK(cost <= CLOCK_CarrierCostHz(uhz, &whole, f, sh->update_hz));
            CHECK(cost == carrier_brute_cost(uhz, f, sh));
            /* The plan's frequency is N carrier periods in N x P + k ticks. */
            const double exact = (double)uhz * CLOCK_HRTIM_MUL * c.cycles /
                                 ((double)c.cycles * c.period + c.extra);
            CHECK(fabs((double)CLOCK_CarrierMicroHz(uhz, &c) - exact) <= 1.0);
            /* Legal to the firmware at every phase, at the duty the PA asks
             * for (a duty illegal at this period is clamped before it runs). */
            const int16_t off = mod_duty_ticks_from_permille(c.period / 2U, sh->duty_permille, 1000);
            mod_hrtim_phase_geometry_t g;
            if (!mod_hrtim_phase_geometry(c.period, off, 0U, &g)) continue;
            CHECK(mod_hrtim_phase_geometry_frac(c.period, off, 0U, c.cycles, c.extra, &g) == 1U);
            CHECK(g.master_period_ticks == (uint32_t)((int32_t)c.cycles * c.period + c.extra));
            CHECK(mod_hrtim_phase_geometry_frac(c.period, off, (uint16_t)(c.period - 1U),
                                                c.cycles, c.extra, &g) == 1U);
        }
    }
    CHECK(plans > 15000U && fractional > plans / 2U && wide_k > 0U);
    /* No shape: the whole period, as before the fraction. */
    clock_carrier_t c;
    CHECK(CLOCK_CarrierPlan(170000000000000ULL, 14074000U, NULL, &c) == 1U);
    CHECK(c.extra == 0 && c.cycles == 2U && c.period == CLOCK_CarrierPeriod(170000000000000ULL, 14074000U));
    CHECK(CLOCK_CarrierPlan(170000000000000ULL, 30000000U, &shapes[0], &c) == 0U && c.period == 0U);

    /* A dial five sixths of a grid step past P = 192 (10 m at 170 MHz)
     * needs a large k: */
    const clock_tx_shape_t ssb = {192000U, 8U, 0};
    const uint32_t f10 = (uint32_t)(170000000ULL * 32U * 6U / (192U * 6U + 5U));
    CHECK(CLOCK_CarrierPlan(170000000000000ULL, f10, &ssb, &c) == 1U);
    CHECK(c.period == 192U && c.extra > 2);
    CHECK(CLOCK_ImageDbc(CLOCK_CarrierCostHz(170000000000000ULL, &c, f10, 192000U), 192000U) <= -27);

    /* The whole period at an exact grid dial: an odd P takes N = 3, which
     * moves its roll-over through the whole fHRTIM cycle; P = 384 never moves
     * it (2P and 3P both divide by 32), so the NCO's advances 1.5 kHz above
     * would be held up to 31 ticks: the plan steers clear of k = 0 there. */
    const clock_tx_shape_t cw = {192000U, 2U, 0};
    const uint64_t uhz170 = 170000000000000ULL;
    const uint32_t f383 = (uint32_t)((CLOCK_PeriodMicroHz(uhz170, 383U) + 500000U) / 1000000U);
    CHECK(CLOCK_CarrierPlan(uhz170, f383, &cw, &c) == 1U);
    CHECK(c.period == 383U && c.cycles == 3U && c.extra == 0);
    CHECK(CLOCK_CarrierCostHz(uhz170, &c, f383, 192000U) <= (192000U + 191U) / 383U);
    const clock_carrier_t held = {384U, 2U, 0};
    const uint32_t f384 = (uint32_t)(CLOCK_PeriodMicroHz(uhz170, 384U) / 1000000U) + 1500U;
    CHECK(CLOCK_CarrierCostHz(uhz170, &held, f384, 192000U) >= 32U * 192000U / 384U);
    CHECK(CLOCK_CarrierPlan(uhz170, f384, &cw, &c) == 1U);
    CHECK(!(c.period == 384U && c.extra == 0));
    CHECK(CLOCK_CarrierCostHz(uhz170, &c, f384, 192000U) < 16U * 192000U / 384U);

    /* Every plan 14-28.3 MHz, run through the reset rule with the residual's
     * steps and the reference trim's worst advance on top: the timer strays
     * no further than the plan's cost said, the steps themselves plus g when
     * it priced a held reset. A k = -1 plan, as the first fractional build's
     * HALF presets ran, strays far further. */
    for (unsigned si = 0U; si < 2U; ++si) {
        const clock_tx_shape_t *sh = &shapes[si];
        const double update_ticks = 170.0e6 * CLOCK_HRTIM_MUL / sh->update_hz;
        for (uint32_t f = 14000000U; f <= 28333000U; f += 21011U) {
            CHECK(CLOCK_CarrierPlan(uhz170, f, sh, &c) == 1U);
            const double err_hz = (double)CLOCK_CarrierErrorMilliHz(uhz170, &c, f) / 1000.0;
            const double trim_hz = (double)f * CLOCK_TX_TRIM_MARGIN_PPB * 1.0e-9;
            const double s = (err_hz - trim_hz) * c.period / sh->update_hz;
            const uint32_t cost = CLOCK_CarrierCostHz(uhz170, &c, f, sh->update_hz);
            const uint32_t cost_ticks = (uint32_t)(((uint64_t)cost * c.period + sh->update_hz - 1U) / sh->update_hz);
            CHECK(reset_model_stray(&c, s, update_ticks, 4000U) <= cost_ticks + (uint32_t)ceil(fabs(s)) + 1U);
        }
    }
    const clock_carrier_t v811 = {383U, 2U, -1};
    CHECK(reset_model_stray(&v811, 0.0, 170.0e6 * CLOCK_HRTIM_MUL / 192000.0, 4000U) >= 8U);
}

/* What the survey prices: the whole period's residual alone (as up to
 * v8.11), the planner's cost on AUTO's clock, or on the key-down hop's. */
enum { SURVEY_WHOLE, SURVEY_AUTO, SURVEY_HOP };

/* The cost at `f` on the clock AUTO (and with SURVEY_HOP the key-down hop)
 * picks among the clocks whose receive worker fits `min_cycles`.
 * UINT32_MAX if none transmits. */
static uint32_t survey_auto_cost(const clock_pll_t *pll, unsigned np, uint32_t min_cycles, uint32_t f,
                                 int32_t centre, radio_rx_mode_t mode, const clock_tx_shape_t *sh, int how)
{
    const int whole = (how == SURVEY_WHOLE);
    clock_eval_t ev[CLOCK_PLL_CANDIDATES_MAX];
    for (unsigned i = 0U; i < np; ++i) {
        memset(&ev[i], 0, sizeof(ev[i]));
        if (CLOCK_RxFrameCycles(CLOCK_PllNominalHz(pll[i])) < min_cycles) continue;
        CLOCK_Evaluate(pll[i], 0, f, centre, mode, sh, &ev[i]);
        if (whole && ev[i].period) {
            const uint64_t s = CLOCK_PllMicroHz(pll[i], 0);
            const uint32_t t = (uint32_t)((int32_t)f + centre);
            const int64_t e = CLOCK_PeriodErrorMilliHz(s, CLOCK_CarrierPeriod(s, t), t);
            ev[i].tx_err_hz = (uint32_t)(((e < 0 ? -e : e) + 500) / 1000);
        }
    }
    int a = CLOCK_ChooseAuto(ev, np, -1);
    if (how == SURVEY_HOP) a = CLOCK_ChooseTx(ev, np, a);
    return a < 0 ? UINT32_MAX : ev[a].tx_err_hz;
}

static int survey_cmp(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* docs/REV1.md's table and windows, printed with CLOCK_SURVEY=1: per band,
 * the cost every 1 kHz (USB, median / 90th percentile) on AUTO's clock with
 * whole periods, with the fraction, with WIDE CLOCK RANGE and after the
 * key-down hop; then the span every 100 Hz left above -40 dBc, CW and USB,
 * without and with the hop, and its worst. */
static void survey_bands(void)
{
    if (!getenv("CLOCK_SURVEY")) return;
    static const struct { const char *name; uint32_t lo, hi; } bands[] = {
        {"160", 1810000U, 2000000U}, {"80", 3500000U, 3800000U}, {"40", 7000000U, 7200000U},
        {"30", 10100000U, 10150000U}, {"20", 14000000U, 14350000U}, {"17", 18068000U, 18168000U},
        {"15", 21000000U, 21450000U}, {"12", 24890000U, 24990000U}, {"10", 28000000U, 28333000U}};
    clock_pll_t pll[CLOCK_PLL_CANDIDATES_MAX];
    const unsigned np = CLOCK_PllCandidates(CLOCK_SYSCLK_AUTO_MIN_HZ, CLOCK_SYSCLK_MAX_HZ, pll, CLOCK_PLL_CANDIDATES_MAX);
    const clock_tx_shape_t ssb = {192000U, 8U, 0}, cw = {192000U, 2U, 0};
    printf("USB every 1 kHz: band | whole period | fraction | fraction, WIDE | fraction, hop (median / 90th, dBc)\n");
    for (unsigned b = 0U; b < sizeof(bands) / sizeof(bands[0]); ++b) {
        static uint32_t c[4][1024];
        unsigned n = 0U;
        for (uint32_t f = bands[b].lo; (f <= bands[b].hi) && (n < 1024U); f += 1000U, ++n) {
            c[0][n] = survey_auto_cost(pll, np, CLOCK_RX_FRAME_CYCLES_MIN, f, 1500, RADIO_RX_USB, &ssb, SURVEY_WHOLE);
            c[1][n] = survey_auto_cost(pll, np, CLOCK_RX_FRAME_CYCLES_MIN, f, 1500, RADIO_RX_USB, &ssb, SURVEY_AUTO);
            c[2][n] = survey_auto_cost(pll, np, CLOCK_RX_FRAME_CYCLES_WIDE_MIN, f, 1500, RADIO_RX_USB, &ssb,
                                       SURVEY_AUTO);
            c[3][n] = survey_auto_cost(pll, np, CLOCK_RX_FRAME_CYCLES_MIN, f, 1500, RADIO_RX_USB, &ssb, SURVEY_HOP);
        }
        printf("  %4s m", bands[b].name);
        for (unsigned col = 0U; col < 4U; ++col) {
            qsort(c[col], n, sizeof(c[col][0]), survey_cmp);
            const uint32_t m = c[col][n / 2U], p9 = c[col][n * 9U / 10U];
            printf(" | %5lu / %5lu Hz (%ld / %ld)", (unsigned long)m, (unsigned long)p9,
                   (long)CLOCK_ImageDbc(m, 192000U), (long)CLOCK_ImageDbc(p9, 192000U));
        }
        printf("\n");
    }
    printf("Above -40 dBc every 100 Hz: band | CW span, worst | CW with hop | USB | USB with hop\n");
    for (unsigned b = 0U; b < sizeof(bands) / sizeof(bands[0]); ++b) {
        printf("  %4s m", bands[b].name);
        for (unsigned col = 0U; col < 4U; ++col) {
            const unsigned usb = col / 2U;
            uint32_t span = 0U, worst = 0U;
            for (uint32_t f = bands[b].lo; f <= bands[b].hi; f += 100U) {
                const uint32_t cost = survey_auto_cost(pll, np, CLOCK_RX_FRAME_CYCLES_MIN, f, usb ? 1500 : 0,
                                                       usb ? RADIO_RX_USB : RADIO_RX_CW, usb ? &ssb : &cw,
                                                       (col & 1U) ? SURVEY_HOP : SURVEY_AUTO);
                if (cost == UINT32_MAX) continue; /* 10 m's top: no period >= 192 ticks */
                if (CLOCK_ImageDbc(cost, 192000U) > -40) span += 100U;
                if (cost > worst) worst = cost;
            }
            printf(" | %3lu kHz, %ld dBc", (unsigned long)(span / 1000U), (long)CLOCK_ImageDbc(worst, 192000U));
        }
        printf("\n");
    }
}

/* Over every amateur band on a 1 kHz grid, with WIDE off and on, AUTO never
 * picks a clock without a TX period or short of the receive budget, and never
 * leaves the receiver worse than the best available. */
static void test_band_sweep_tx(void)
{
    /* 80 m LSB with WIDE and TX PHASE 384K ran at 153.3 MHz before the TX
     * gate (99.7 % CPU); now voice SSB at 384 kHz stays at 164-170 MHz on
     * every band, and CW there may still use the divider-14 clocks. */
    clock_eval_t e[CLOCK_PLL_CANDIDATES_MAX];
    unsigned cw_low = 0U;
    for (uint32_t f = 1800000U; f < 28000000U; f += 7919U) {
        unsigned n = evaluate_tx(f, -1500, RADIO_RX_LSB, 1U, CLOCK_TX_LOAD_SSB, 1U, e);
        int pick = CLOCK_ChooseAuto(e, n, -1);
        CHECK(pick >= 0 && e[pick].sysclk_hz >= 164000000U);
        n = evaluate_tx(f, 0, RADIO_RX_CW, 1U, CLOCK_TX_LOAD_CW, 1U, e);
        pick = CLOCK_ChooseAuto(e, n, -1);
        CHECK(pick >= 0 && e[pick].sysclk_hz > 153000000U);
        if (pick >= 0 && e[pick].sysclk_hz < 164000000U) ++cw_low;
    }
    CHECK(cw_low > 0U);
    const unsigned n80 = evaluate_tx(3750000U, -1500, RADIO_RX_LSB, 1U, CLOCK_TX_LOAD_SSB, 0U, e);
    const int p80 = CLOCK_ChooseAuto(e, n80, -1);
    CHECK(p80 >= 0 && e[p80].sysclk_hz > 154500000U);
}

static void test_band_sweep(void)
{
    static const uint32_t edges[][2] = {
        {1800000U, 2000000U}, {3500000U, 4000000U}, {7000000U, 7300000U}, {10100000U, 10150000U},
        {14000000U, 14350000U}, {18068000U, 18168000U}, {21000000U, 21450000U},
        {24890000U, 24990000U}, {28000000U, 28333000U}};
    clock_eval_t e[CLOCK_PLL_CANDIDATES_MAX];
    for (unsigned w = 0U; w < 2U; ++w) for (unsigned b = 0U; b < sizeof(edges) / sizeof(edges[0]); ++b) {
        for (uint32_t f = edges[b][0]; f < edges[b][1]; f += 1000U) {
            const unsigned n = evaluate_all(f, 1500, RADIO_RX_USB, (uint8_t)w, e);
            const int pick = CLOCK_ChooseAuto(e, n, -1);
            CHECK(pick >= 0 && e[pick].period != 0U);
            CHECK(e[pick].sysclk_hz >= (w ? 153000000U : 164000000U));
            uint8_t best = 0U;
            for (unsigned i = 0U; i < n; ++i)
                if (e[i].period && CLOCK_RxClass(&e[i]) > best) best = CLOCK_RxClass(&e[i]);
            CHECK(CLOCK_RxClass(&e[pick]) == best);
        }
    }
}

int main(void)
{
    test_candidates();
    test_receive_budget();
    test_tx_budget();
    test_exact_arithmetic();
    test_image_estimate();
    test_auto_choice();
    test_choose_tx();
    test_carrier_plan();
    test_band_sweep();
    test_band_sweep_tx();
    survey_bands();
    if (failures) { fprintf(stderr, "%u clock_plan checks failed\n", failures); return 1; }
    printf("clock_plan: candidates, receive and transmit budgets, exact PLL/HRTIM arithmetic, image estimate, AUTO and key-down choice, fractional carrier plan and band sweeps\n");
    return 0;
}

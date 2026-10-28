#include "mod_selftest_logic.h"
#include "card_text.h"
#include "radio_config.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Verdict rules of the REV1 self test, checked against the QRP Labs PIN bias
 * model that radio_config.c already ships (QDX assembly manual 1.24, "A new
 * bias circuit for PCB Rev 4"). The self test reads TX_BIAS with the bias buck
 * off, so every number below is the passive R74/R77 operating point rather
 * than a commanded current. */

#define NODE_MV   674.1
#define NODE_OHM   12.1
#define FEED_OHM   33.0
#define RAIL_OHM 1000.0

/* Both branches healthy: no current in the 33 ohm feeds, so TX_BIAS sits at
 * the diode node. `extra_ohm` is added path resistance (a cold joint, a
 * partly unwound toroid), which the model lumps with NODE_OHM. */
static double idle_node_mv(double rail_mv, double extra_ohm)
{
    const double k = NODE_OHM + extra_ohm;
    return (NODE_MV + k * rail_mv / RAIL_OHM) / (1.0 + k / RAIL_OHM);
}

/* One branch open, one healthy. The open branch's 1 k charges its own diode
 * node, which then feeds TX_BIAS through that branch's 33 ohm, lifting the
 * common node above the healthy value. Bisected on the branch-B node model. */
static double one_branch_open_mv(double rail_mv)
{
    double lo = NODE_MV, hi = rail_mv;
    for (unsigned n = 0U; n < 200U; ++n) {
        const double v = 0.5 * (lo + hi);
        const double na = (FEED_OHM * rail_mv + RAIL_OHM * v) / (FEED_OHM + RAIL_OHM);
        const double into_v = (na - v) / FEED_OHM;          /* mA out of the open branch */
        const double nb = v - FEED_OHM * into_v;            /* healthy branch node */
        const double ib = into_v + (rail_mv - nb) / RAIL_OHM;
        const double residual = nb - (NODE_MV + NODE_OHM * ib);
        if (residual > 0.0) hi = v; else lo = v;
    }
    return 0.5 * (lo + hi);
}

static void check_table(void)
{
    uint8_t seen[ST_GROUP_COUNT] = {0};
    for (unsigned id = 0U; id < SELFTEST_COUNT; ++id) {
        const char *name = MOD_SelfTestName((uint8_t)id);
        assert(name && name[0] && strcmp(name, "?"));
        assert(strlen(name) <= 20U); /* the 240 px small font fits 40 columns */
        assert(MOD_SelfTestUnit((uint8_t)id) != NULL);
        const uint8_t group = MOD_SelfTestGroup((uint8_t)id);
        assert(group && (group & (group - 1U)) == 0U && group <= ST_GROUP_ANALOG);
        assert(strcmp(MOD_SelfTestGroupName(group), "?"));
        for (unsigned b = 0U; b < ST_GROUP_COUNT; ++b)
            if (group == (1U << b)) seen[b] = 1U;
    }
    for (unsigned b = 0U; b < ST_GROUP_COUNT; ++b) assert(seen[b]); /* no dead group */

    /* Limits live in flash rather than in every result row, so they have to be
     * coherent on their own: a real window, or one of the two sentinels. */
    unsigned dynamic = 0U;
    for (unsigned id = 0U; id < SELFTEST_COUNT; ++id) {
        int32_t low = 0, high = 0;
        MOD_SelfTestLimits((uint8_t)id, &low, &high);
        if (low == ST_LIMIT_DYNAMIC || high == ST_LIMIT_DYNAMIC) { ++dynamic; continue; }
        assert(low <= high);
        assert(low != ST_LIMIT_NONE); /* the open end is always the high one */
    }
    /* Only the VIN ceiling and the PIN bias window depend on live config. */
    assert(dynamic == 2U);
    {
        int32_t low = 0, high = 0;
        MOD_SelfTestLimits(ST_VIN, &low, &high);
        assert(low == 6000 && high == ST_LIMIT_DYNAMIC);
        MOD_SelfTestLimits(ST_BIAS_BUCK, &low, &high);
        assert(low == ST_LIMIT_DYNAMIC && high == ST_LIMIT_DYNAMIC);
        MOD_SelfTestLimits(ST_LPF0, &low, &high);
        assert(low == 0 && high == ST_LPF_CONDUCT_MV); /* the literal tracks the constant */
        MOD_SelfTestLimits(SELFTEST_COUNT, &low, &high);
        assert(low == 0 && high == 0);
    }
    /* A row carries only what a run produced -- everything else is indexed by
     * position or read from the flash tables. Compared against a reference
     * struct rather than a byte count so the check holds on a 64-bit host as
     * well as on the 32-bit target, where this is 12 bytes and the whole
     * results array is 648. Putting the limits back in a row would cost this
     * board 432 bytes of SRAM it does not have. */
    struct reference_row { int32_t value; const char *detail; uint8_t verdict, flags; uint16_t text; };
    assert(sizeof(selftest_result_t) == sizeof(struct reference_row));
    assert(MOD_SelfTestName(SELFTEST_COUNT)[0] == '?');
    assert(MOD_SelfTestGroup(SELFTEST_COUNT) == 0U);

    /* Group words the console accepts, and what the default run covers. */
    assert(MOD_SelfTestGroupFromName("power") == ST_GROUP_POWER);
    assert(MOD_SelfTestGroupFromName("POWER") == ST_GROUP_POWER);
    assert(MOD_SelfTestGroupFromName("Analog") == ST_GROUP_ANALOG);
    assert(MOD_SelfTestGroupFromName("all") == ST_GROUP_ALL);
    assert(MOD_SelfTestGroupFromName("powers") == 0U);
    assert(MOD_SelfTestGroupFromName("") == 0U);
    assert(MOD_SelfTestGroupFromName(NULL) == 0U);
    /* The unattended run must not wait on a human or energise the PA path. */
    assert(!(ST_GROUP_DEFAULT & ST_GROUP_CONTROLS));
    assert(!(ST_GROUP_DEFAULT & ST_GROUP_ANALOG));
    assert((ST_GROUP_DEFAULT & ST_GROUP_FILTERS) && (ST_GROUP_DEFAULT & ST_GROUP_POWER));
    assert(ST_GROUP_ALL == (ST_GROUP_DEFAULT | ST_GROUP_CONTROLS | ST_GROUP_ANALOG));
}

static void check_ranges(void)
{
    assert(MOD_SelfTestRange(10, 10, 20) == ST_PASS);   /* inclusive at both ends */
    assert(MOD_SelfTestRange(20, 10, 20) == ST_PASS);
    assert(MOD_SelfTestRange(9, 10, 20) == ST_FAIL);
    assert(MOD_SelfTestRange(21, 10, 20) == ST_FAIL);
    assert(MOD_SelfTestRange(-5, -10, -1) == ST_PASS);
    assert(MOD_SelfTestRangeWarn(21, 10, 20) == ST_WARN);
    assert(MOD_SelfTestRangeWarn(15, 10, 20) == ST_PASS);
    /* An inverted window is a programming error and must never pass. */
    assert(MOD_SelfTestRange(15, 20, 10) == ST_FAIL);
    assert(MOD_SelfTestRangeWarn(15, 20, 10) == ST_FAIL);
}

static void check_lpf_continuity(void)
{
    /* A healthy filter reads as conducting over every rail the board can
     * present, including the 3.7 V USB-C cold start where TX is inhibited
     * but the continuity test still has to work. */
    double lowest = 1e9, highest = -1e9, worst_ma = 1e9, best_ma = -1e9;
    for (double rail = 3700.0; rail <= 5500.0; rail += 25.0) {
        const double v = idle_node_mv(rail, 0.0);
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
        assert(MOD_SelfTestLpfState((int32_t)(v + 0.5)) == ST_LPF_CONDUCTING);
        assert(MOD_SelfTestLpfVerdict((int32_t)(v + 0.5), 1U, 1U) == ST_PASS);
        /* The shipped model must agree that this is the passive receive bias
         * QRP Labs describe, not a commanded current. */
        const float ma = RADIO_BiasCurrentMa((uint16_t)(v + 0.5), (uint16_t)rail);
        if (ma < worst_ma) worst_ma = ma;
        if (ma > best_ma) best_ma = ma;
        assert(ma > 2.5f && ma < 5.5f);
        /* Nothing selected: TX_BIAS charges to the rail. */
        assert(MOD_SelfTestLpfVerdict((int32_t)rail, 0U, 1U) == ST_PASS);
        assert(MOD_SelfTestLpfVerdict((int32_t)rail, 1U, 1U) == ST_FAIL);
        assert(MOD_SelfTestLpfVerdict((int32_t)(v + 0.5), 0U, 1U) == ST_FAIL);
    }
    assert(lowest > 700.0 && highest < 740.0); /* nearly rail independent */

    /* A shorted bias node fails whichever state was expected. */
    for (int32_t mv = 0; mv < ST_LPF_SHORT_MV; mv += 37) {
        assert(MOD_SelfTestLpfState(mv) == ST_LPF_SHORTED);
        assert(MOD_SelfTestLpfVerdict(mv, 1U, 1U) == ST_FAIL);
        assert(MOD_SelfTestLpfVerdict(mv, 0U, 1U) == ST_FAIL);
    }

    /* An unfitted slot is a build choice, never a defect. */
    assert(MOD_SelfTestLpfVerdict(5000, 1U, 0U) == ST_SKIP);
    assert(MOD_SelfTestLpfVerdict(726, 1U, 0U) == ST_SKIP);
    assert(MOD_SelfTestLpfVerdict(0, 0U, 0U) == ST_SKIP);

    /* One branch open must not read as healthy. This is the whole reason
     * ST_LPF_CONDUCT_MV is 900 mV: widening it back to the 1100 mV that a
     * both-branches-only analysis would allow lets this case pass. */
    const double single = one_branch_open_mv(5000.0);
    assert(single > 880.0 && single < 930.0);
    assert(MOD_SelfTestLpfState((int32_t)single) == ST_LPF_MARGINAL);
    assert(MOD_SelfTestLpfVerdict((int32_t)single, 1U, 1U) == ST_WARN);
    assert((int32_t)single > ST_LPF_CONDUCT_MV);

    /* Added path resistance: healthy through about 40 ohm, flagged past 50. */
    assert(MOD_SelfTestLpfVerdict((int32_t)idle_node_mv(5000.0, 40.0), 1U, 1U) == ST_PASS);
    assert(MOD_SelfTestLpfVerdict((int32_t)idle_node_mv(5000.0, 50.0), 1U, 1U) != ST_PASS);
    assert(MOD_SelfTestLpfVerdict((int32_t)idle_node_mv(5000.0, 100.0), 1U, 1U) == ST_WARN);

    /* Defence in depth: at any bias the operator can command from 15 mA up,
     * a running buck reads above the conducting window, so a self test that
     * somehow left the buck on reports a fault rather than a pass. Below
     * 15 mA the commanded bias approaches the passive one by construction,
     * which is why the test switches the buck off instead of relying on the
     * level (QDX/QMX disable the buck at 5 mA for the same reason). */
    for (uint16_t ma = 15U; ma <= RADIO_BIAS_MA_MAX; ++ma)
        for (uint16_t rail = 4400U; rail <= 5500U; rail += 50U)
            assert(RADIO_BiasTargetMv(ma, rail) > ST_LPF_CONDUCT_MV);

    /* Every state has a detail of its own on the card (card_text.def). */
    for (unsigned fitted = 0U; fitted <= 1U; ++fitted)
        for (unsigned expect = 0U; expect <= 1U; ++expect)
            for (int32_t mv = 0; mv <= 5000; mv += 250) {
                const uint16_t d = MOD_SelfTestLpfDetail(mv, (uint8_t)expect, (uint8_t)fitted);
                assert(d >= TXT_ST_LPF_NOT_FITTED && d <= TXT_ST_LPF_OPEN_EXPECTED);
            }
    printf("LPF continuity: healthy %.0f-%.0f mV (%.2f-%.2f mA passive), "
           "one branch open %.0f mV, limit %d mV\n",
           lowest, highest, worst_ma, best_ma, single, ST_LPF_CONDUCT_MV);
}

static void check_one_hot(void)
{
    for (uint8_t slot = 0U; slot < 5U; ++slot) {
        assert(MOD_SelfTestOneHot(1UL << slot, slot) == ST_PASS);
        for (uint8_t other = 0U; other < 5U; ++other)
            if (other != slot) assert(MOD_SelfTestOneHot(1UL << other, slot) == ST_FAIL);
        /* Two select transistors on at once shorts two filters together. */
        assert(MOD_SelfTestOneHot((1UL << slot) | (1UL << ((slot + 1U) % 5U)), slot) == ST_FAIL);
        assert(MOD_SelfTestOneHot(0UL, slot) == ST_FAIL);
        assert(MOD_SelfTestOneHot(31UL, slot) == ST_FAIL);
    }
    /* Deselected: every line must be low. */
    assert(MOD_SelfTestOneHot(0UL, 255U) == ST_PASS);
    for (uint8_t slot = 0U; slot < 5U; ++slot)
        assert(MOD_SelfTestOneHot(1UL << slot, 255U) == ST_FAIL);
    /* Bits above PE6 belong to other peripherals and must be ignored. */
    assert(MOD_SelfTestOneHot(0xFFE0UL | 4UL, 2U) == ST_PASS);
}

static void check_envelope_fit(void)
{
    uint16_t code[16], mv[16];
    selftest_fit_t fit;

    /* A clean transfer: 1500 counts per volt with a small offset. */
    for (unsigned i = 0U; i < 16U; ++i) {
        code[i] = (uint16_t)(i * 200U);
        mv[i] = (uint16_t)(40.0 + code[i] * 1000.0 / 1500.0);
    }
    MOD_SelfTestFit(code, mv, 16U, &fit);
    assert(fit.valid && fit.r2_ppt >= ST_ENV_R2_MIN_PPT);
    assert(fabsf(fit.counts_per_volt - 1500.0f) < 5.0f);
    assert(fit.intercept_mv > 35 && fit.intercept_mv < 45);
    assert(fit.counts_per_volt > (float)ST_ENV_CPV_MIN && fit.counts_per_volt < (float)ST_ENV_CPV_MAX);

    /* A couple of ADC counts of noise must not disqualify a good board. */
    for (unsigned i = 0U; i < 16U; ++i)
        mv[i] = (uint16_t)(40.0 + code[i] * 1000.0 / 1500.0 + ((i % 3U) ? 4 : -4));
    MOD_SelfTestFit(code, mv, 16U, &fit);
    assert(fit.valid && fit.r2_ppt >= ST_ENV_R2_MIN_PPT);

    /* A follower that saturates part way is not a straight line, and must be
     * rejected rather than fitted through the knee. */
    for (unsigned i = 0U; i < 16U; ++i) {
        double ideal = 40.0 + code[i] * 1000.0 / 1500.0;
        mv[i] = (uint16_t)(ideal > 1200.0 ? 1200.0 : ideal);
    }
    MOD_SelfTestFit(code, mv, 16U, &fit);
    assert(fit.valid && fit.r2_ppt < ST_ENV_R2_MIN_PPT);

    /* Degenerate inputs never produce a slope to apply. */
    for (unsigned i = 0U; i < 16U; ++i) mv[i] = 900U;              /* flat */
    MOD_SelfTestFit(code, mv, 16U, &fit);
    assert(!fit.valid);
    for (unsigned i = 0U; i < 16U; ++i) mv[i] = (uint16_t)(3000U - code[i] / 4U); /* falling */
    MOD_SelfTestFit(code, mv, 16U, &fit);
    assert(!fit.valid);
    for (unsigned i = 0U; i < 16U; ++i) { code[i] = 512U; mv[i] = (uint16_t)(700U + i); }
    MOD_SelfTestFit(code, mv, 16U, &fit);
    assert(!fit.valid);                                            /* one code only */
    MOD_SelfTestFit(code, mv, 2U, &fit);
    assert(!fit.valid);                                            /* too few points */
    MOD_SelfTestFit(NULL, mv, 16U, &fit);
    assert(!fit.valid);
    MOD_SelfTestFit(code, NULL, 16U, &fit);
    assert(!fit.valid);
}

static selftest_result_t row(uint8_t verdict)
{
    selftest_result_t r;
    memset(&r, 0, sizeof(r));
    r.verdict = verdict;
    return r;
}

static void check_rollup(void)
{
    selftest_result_t r[5];
    unsigned pass, warn, fail, skip;

    r[0] = row(ST_PASS); r[1] = row(ST_PASS); r[2] = row(ST_SKIP);
    r[3] = row(ST_PASS); r[4] = row(ST_PASS);
    assert(MOD_SelfTestRollup(r, 5U) == ST_PASS);
    r[2] = row(ST_WARN);
    assert(MOD_SelfTestRollup(r, 5U) == ST_WARN);
    r[4] = row(ST_FAIL);
    assert(MOD_SelfTestRollup(r, 5U) == ST_FAIL);   /* a fail outranks a warn */
    r[2] = row(ST_PASS);
    assert(MOD_SelfTestRollup(r, 5U) == ST_FAIL);

    /* A run where nothing was reached is not a pass. Letting SKIP or PENDING
     * roll up as PASS would report a healthy board for a run that never ran. */
    for (unsigned i = 0U; i < 5U; ++i) r[i] = row(ST_SKIP);
    assert(MOD_SelfTestRollup(r, 5U) == ST_SKIP);
    for (unsigned i = 0U; i < 5U; ++i) r[i] = row(ST_PENDING);
    assert(MOD_SelfTestRollup(r, 5U) == ST_SKIP);
    r[3] = row(ST_RUNNING);
    assert(MOD_SelfTestRollup(r, 5U) == ST_SKIP);
    assert(MOD_SelfTestRollup(r, 0U) == ST_SKIP);
    assert(MOD_SelfTestRollup(NULL, 5U) == ST_SKIP);

    r[0] = row(ST_PASS); r[1] = row(ST_WARN); r[2] = row(ST_FAIL);
    r[3] = row(ST_SKIP); r[4] = row(ST_PENDING);
    MOD_SelfTestTally(r, 5U, &pass, &warn, &fail, &skip);
    assert(pass == 1U && warn == 1U && fail == 1U && skip == 1U);
    MOD_SelfTestTally(r, 5U, NULL, NULL, NULL, NULL); /* all-NULL is legal */
    MOD_SelfTestTally(NULL, 5U, &pass, &warn, &fail, &skip);
    assert(!pass && !warn && !fail && !skip);

    for (uint8_t v = ST_PENDING; v <= ST_SKIP; ++v)
        assert(strcmp(MOD_SelfTestVerdictName(v), "?"));
}

int main(void)
{
    check_table();
    check_ranges();
    check_lpf_continuity();
    check_one_hot();
    check_envelope_fit();
    check_rollup();
    printf("REV1 self-test verdict rules, LPF continuity model, envelope fit and roll-up: PASS\n");
    return 0;
}

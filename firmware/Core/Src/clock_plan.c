#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "clock_plan.h"
#include "radio_dsp.h"
#include "radio_rx_front.h"
#include <math.h>

uint64_t CLOCK_HseMicroHz(int32_t ppb)
{
    if (ppb > CLOCK_REF_PPB_LIMIT) ppb = CLOCK_REF_PPB_LIMIT;
    if (ppb < -CLOCK_REF_PPB_LIMIT) ppb = -CLOCK_REF_PPB_LIMIT;
    /* HSE x 1e6 uHz, plus HSE x 1e6 uHz x ppb x 1e-9: HSE / 1000 uHz per ppb
     * (8000 at 8 MHz). */
    return (uint64_t)((int64_t)CLOCK_HSE_NOMINAL_HZ * 1000000LL +
                      (int64_t)(CLOCK_HSE_NOMINAL_HZ / 1000UL) * (int64_t)ppb);
}

uint64_t CLOCK_PllMicroHz(clock_pll_t pll, int32_t ppb)
{
    const uint64_t den = (uint64_t)pll.m * pll.r;
    if (!den) return 0U;
    return (CLOCK_HseMicroHz(ppb) * pll.n + den / 2U) / den;
}

uint32_t CLOCK_PllNominalHz(clock_pll_t pll)
{
    const uint32_t den = (uint32_t)pll.m * pll.r;
    if (!den) return 0U;
    return (uint32_t)(((uint64_t)CLOCK_HSE_NOMINAL_HZ * pll.n + den / 2U) / den);
}

uint8_t CLOCK_PllLegal(clock_pll_t pll)
{
    if (pll.m < 1U || pll.m > 16U || pll.n < 8U || pll.n > 127U) return 0U;
    if (pll.r != 2U && pll.r != 4U && pll.r != 6U && pll.r != 8U) return 0U;
    const uint64_t in = CLOCK_HSE_NOMINAL_HZ;
    if (in < 2660000ULL * pll.m || in > 16000000ULL * pll.m) return 0U;
    const uint64_t vco = in * pll.n;
    if (vco < 96000000ULL * pll.m || vco > 344000000ULL * pll.m) return 0U;
    return 1U;
}

unsigned CLOCK_PllCandidates(uint32_t min_hz, uint32_t max_hz, clock_pll_t *out, unsigned cap)
{
    unsigned count = 0U;
    static const uint8_t r_values[] = {2U, 4U, 6U, 8U};
    /* M in steps of CLOCK_REF_SCALE: a 16 MHz reference gets exactly the
     * settings 8 MHz has (M doubled), not new ones nobody measured. */
    for (uint8_t m = CLOCK_REF_SCALE; m <= 16U; m = (uint8_t)(m + CLOCK_REF_SCALE)) {
        for (unsigned ri = 0U; ri < sizeof(r_values); ++ri) {
            const uint32_t r = r_values[ri];
            /* SYSCLK = HSE x n / (m x r): the n that can land in range. */
            const uint64_t scale = (uint64_t)m * r;
            uint64_t n_lo = ((uint64_t)min_hz * scale + CLOCK_HSE_NOMINAL_HZ - 1U) / CLOCK_HSE_NOMINAL_HZ;
            uint64_t n_hi = ((uint64_t)max_hz * scale) / CLOCK_HSE_NOMINAL_HZ;
            if (n_lo < 8U) n_lo = 8U;
            if (n_hi > 127U) n_hi = 127U;
            for (uint64_t n = n_lo; n <= n_hi; ++n) {
                const clock_pll_t p = {m, (uint8_t)n, (uint8_t)r};
                if (!CLOCK_PllLegal(p)) continue;
                const uint32_t hz = CLOCK_PllNominalHz(p);
                if (hz < min_hz || hz > max_hz) continue;
                /* Two settings with the same ratio make the same clock; the
                 * first found has the smallest M. Keep the list ascending. */
                unsigned at = 0U;
                uint8_t duplicate = 0U;
                for (; at < count; ++at) {
                    if (n * out[at].m * out[at].r == (uint64_t)out[at].n * m * r) {
                        duplicate = 1U;
                        break;
                    }
                    if (CLOCK_PllNominalHz(out[at]) > hz) break;
                }
                if (duplicate || count >= cap) continue;
                for (unsigned k = count; k > at; --k) out[k] = out[k - 1U];
                out[at] = p;
                ++count;
            }
        }
    }
    return count;
}

uint32_t CLOCK_RxFrameCycles(uint32_t sysclk_hz)
{
    return 64UL * RADIO_I2sDivider(sysclk_hz, 0U);
}

uint32_t CLOCK_TxCyclesPerMs(clock_tx_load_t load, uint8_t phase384)
{
    /* {192 kHz, 384 kHz} phase rate: the bench worst case plus ~3 %. */
    static const uint32_t cycles[CLOCK_TX_LOAD_COUNT][2] = {
        [CLOCK_TX_LOAD_SSB]       = {142000U, 152000U},
        [CLOCK_TX_LOAD_SSB_CLEAN] = {122000U, 132000U},
        [CLOCK_TX_LOAD_AM]        = {115000U, 124000U},
        [CLOCK_TX_LOAD_CW]        = {110000U, 120000U},
        [CLOCK_TX_LOAD_FM]        = { 90000U, 100000U},
    };
    if ((unsigned)load >= (unsigned)CLOCK_TX_LOAD_COUNT) load = CLOCK_TX_LOAD_SSB;
    return cycles[load][phase384 ? 1U : 0U];
}

uint8_t CLOCK_TxFits(uint32_t sysclk_hz, clock_tx_load_t load, uint8_t phase384)
{
    return (uint64_t)CLOCK_TxCyclesPerMs(load, phase384) * 100U <=
           (uint64_t)(sysclk_hz / 1000U) * CLOCK_TX_BUDGET_PCT;
}

uint16_t CLOCK_CarrierPeriod(uint64_t sysclk_uhz, uint32_t target_hz)
{
    if (!target_hz) return 0U;
    const uint64_t hr_uhz = sysclk_uhz * CLOCK_HRTIM_MUL;
    const uint64_t t_uhz = (uint64_t)target_hz * 1000000ULL;
    const uint64_t p = (hr_uhz + t_uhz / 2U) / t_uhz;
    if (p < CLOCK_MIN_PERIOD_TICKS || p > 65535ULL) return 0U;
    return (uint16_t)p;
}

uint64_t CLOCK_PeriodMicroHz(uint64_t sysclk_uhz, uint32_t period)
{
    if (!period) return 0U;
    return (sysclk_uhz * CLOCK_HRTIM_MUL + period / 2U) / period;
}

int64_t CLOCK_PeriodErrorMilliHz(uint64_t sysclk_uhz, uint32_t period, uint32_t target_hz)
{
    if (!period) return 0;
    const int64_t f_uhz = (int64_t)CLOCK_PeriodMicroHz(sysclk_uhz, period);
    const int64_t diff = f_uhz - (int64_t)target_hz * 1000000LL;
    return diff >= 0 ? (diff + 500) / 1000 : (diff - 500) / 1000;
}

uint64_t CLOCK_CarrierMicroHz(uint64_t sysclk_uhz, const clock_carrier_t *c)
{
    if ((c == NULL) || !c->period) return 0U;
    if (c->extra == 0) return CLOCK_PeriodMicroHz(sysclk_uhz, c->period);
    /* N carrier periods in N x P + k ticks. sysclk x 32 x 255 < 2^61. */
    const int64_t master = (int64_t)c->cycles * c->period + c->extra;
    if (master <= 0) return 0U;
    return (sysclk_uhz * CLOCK_HRTIM_MUL * c->cycles + (uint64_t)master / 2U) / (uint64_t)master;
}

int64_t CLOCK_CarrierErrorMilliHz(uint64_t sysclk_uhz, const clock_carrier_t *c, uint32_t target_hz)
{
    const uint64_t f = CLOCK_CarrierMicroHz(sysclk_uhz, c);
    if (!f) return 0;
    const int64_t diff = (int64_t)f - (int64_t)target_hz * 1000000LL;
    return diff >= 0 ? (diff + 500) / 1000 : (diff - 500) / 1000;
}

uint32_t CLOCK_CarrierCostHz(uint64_t sysclk_uhz, const clock_carrier_t *c, uint32_t target_hz,
                             uint32_t update_hz)
{
    if ((c == NULL) || !c->period) return UINT32_MAX;
    const int64_t err = CLOCK_CarrierErrorMilliHz(sysclk_uhz, c, target_hz);
    const uint64_t mag = (uint64_t)(err < 0 ? -err : err);
    uint32_t cost = (mag / 1000U >= UINT32_MAX) ? UINT32_MAX : (uint32_t)((mag + 500U) / 1000U);
    const uint32_t k = (uint32_t)(c->extra < 0 ? -c->extra : c->extra);
    if (k != 0U) {
        /* The nudge: a k-tick phase step once per master period, a sawtooth of
         * k/P of a turn -- sidebands at k/P of the carrier, as an NCO residual
         * of k/P x the update rate leaves. */
        const uint32_t nudge = (uint32_t)(((uint64_t)k * update_hz + c->period / 2U) / c->period);
        if (nudge > cost) cost = nudge;
    }
    /* A master reset landing just before the branch timer's own roll-over,
     * in the same fHRTIM cycle, is lost: of two counter resets in one cycle
     * the timer keeps the last (RM0440 28.3.4). The NCO's advances land
     * there -- the residual's when the grid is under the dial, the reference
     * trim's when it drifts slow. The nudge catches a lost reset up, k ticks a
     * master period; while that falls short of the advances, the timer's
     * phase waits until its roll-over, moving N x P ticks through the cycle
     * each master period, comes round below the reset: up to g ticks late, g
     * the largest power of two dividing N x P, 32 when that never moves. */
    const uint64_t master = (uint64_t)c->cycles * c->period + k;
    const int64_t advance_mhz = (int64_t)((uint64_t)target_hz * CLOCK_TX_TRIM_MARGIN_PPB / 1000000U) - err;
    const uint64_t heal_mhz = (uint64_t)k * sysclk_uhz * CLOCK_HRTIM_MUL / 1000U / (master * c->period);
    if ((advance_mhz > 0) && (heal_mhz <= (uint64_t)advance_mhz)) {
        const uint32_t turn = ((uint32_t)c->cycles * c->period) & 31U;
        const uint32_t g = turn ? (turn & (0U - turn)) : 32U;
        const uint32_t late = (uint32_t)(((uint64_t)g * update_hz + c->period / 2U) / c->period);
        if (late > cost) cost = late;
    }
    return cost;
}

typedef struct {
    uint64_t sysclk_uhz;
    uint32_t target_hz, update_hz;
    clock_carrier_t best;
    uint32_t best_cost;
} carrier_search_t;

static void carrier_try(carrier_search_t *s, uint64_t period, uint64_t cycles, int32_t extra)
{
    const clock_carrier_t c = {(uint16_t)period, (uint8_t)cycles, (int8_t)extra};
    const uint32_t cost = CLOCK_CarrierCostHz(s->sysclk_uhz, &c, s->target_hz, s->update_hz);
    if (cost < s->best_cost) {
        s->best = c;
        s->best_cost = cost;
    }
}

/* The fractions over whole period `base` (under the exact one, hr/t ticks):
 * P + k/N = hr/t at N = k t / (hr - P t), the N either side of that, or past
 * the master's cap the longest master allowed, the nearest fraction it reaches. */
static void carrier_try_fractions(carrier_search_t *s, uint64_t hr, uint64_t t, uint64_t base,
                                  uint64_t master_max)
{
    if ((base < CLOCK_MIN_PERIOD_TICKS) || (base > 65535U) || (hr <= base * t)) return;
    const uint64_t gap = hr - base * t;
    for (uint64_t k = 1U; k <= CLOCK_CARRIER_EXTRA_MAX; ++k) {
        const uint64_t n0 = k * t / gap;
        const uint64_t n_lo = (k + 1U > 2U) ? k + 1U : 2U;
        uint64_t n_hi = (master_max > k) ? (master_max - k) / base : 0U;
        if (n_hi > CLOCK_CARRIER_CYCLES_MAX) n_hi = CLOCK_CARRIER_CYCLES_MAX;
        if (n_hi < n_lo) continue;
        const uint64_t first = (n0 < n_lo) ? n_lo : ((n0 > n_hi) ? n_hi : n0);
        const uint64_t last = (n0 + 1U > n_hi) ? n_hi : ((n0 + 1U < n_lo) ? n_lo : n0 + 1U);
        for (uint64_t n = first; n <= last; ++n) carrier_try(s, base, n, (int32_t)k);
    }
}

uint8_t CLOCK_CarrierPlan(uint64_t sysclk_uhz, uint32_t target_hz, const clock_tx_shape_t *shape,
                          clock_carrier_t *out)
{
    const clock_carrier_t none = {0U, 0U, 0};
    if (out == NULL) return 0U;
    *out = none;
    const uint16_t whole = CLOCK_CarrierPeriod(sysclk_uhz, target_hz);
    if (!whole) return 0U;
    carrier_search_t s = {sysclk_uhz, target_hz,
                          ((shape != NULL) && shape->update_hz) ? shape->update_hz : CLOCK_TX_UPDATE_HZ,
                          {whole, 2U, 0}, UINT32_MAX};
    if ((shape == NULL) || !shape->master_div) {
        *out = s.best;
        return 1U;
    }
    const uint64_t hr = sysclk_uhz * CLOCK_HRTIM_MUL;
    const uint64_t t = (uint64_t)target_hz * 1000000ULL;
    uint64_t master_max = hr / ((uint64_t)s.update_hz * 1000000ULL) / shape->master_div;
    if (master_max > CLOCK_HRTIM_PERIOD_MAX) master_max = CLOCK_HRTIM_PERIOD_MAX;
    const uint64_t p = hr / t; /* the whole period under the exact one */
    /* The whole period either side, the master at three periods or two,
     * three first to win a tie: an odd N x P moves the roll-over through
     * every place in its fHRTIM cycle, so a reset an NCO advance lost
     * lands again within a tick. */
    for (uint64_t q = p; q <= p + 1U; ++q) {
        if ((q < CLOCK_MIN_PERIOD_TICKS) || (q > 65535U)) continue;
        if (3U * q <= master_max) carrier_try(&s, q, 3U, 0);
        carrier_try(&s, q, 2U, 0);
    }
    /* Only k > 0, lengthening the period under the exact one. A negative
     * k would land every master reset just before the roll-over, where it
     * is lost (CLOCK_CarrierCostHz): the timer's phase slid until a reset
     * landed a cycle earlier, up to 32 ticks of jitter, the smear the
     * bench saw at every k = -1. The period below that too, at its largest
     * fractions: on an exact period that divides by 32 every plan over it
     * holds its resets. */
    carrier_try_fractions(&s, hr, t, p, master_max);
    if (p > 0U) carrier_try_fractions(&s, hr, t, p - 1U, master_max);
    *out = s.best;
    return 1U;
}

int32_t CLOCK_ImageDbc(uint32_t err_hz, uint32_t update_hz)
{
    if (!update_hz || err_hz >= update_hz) return 0;
    if (!err_hz) return -200;
    const float x = (float)err_hz / (float)update_hz;
    const float db = 20.0f * log10f(x / (1.0f - x));
    return (int32_t)(db - 0.5f);
}

void CLOCK_Evaluate(clock_pll_t pll, int32_t ppb, uint32_t dial_hz, int32_t tx_centre_hz,
                    radio_rx_mode_t rx_mode, const clock_tx_shape_t *shape, clock_eval_t *e)
{
    e->pll = pll;
    e->sysclk_hz = CLOCK_PllNominalHz(pll);
    const uint64_t sysclk_uhz = CLOCK_PllMicroHz(pll, ppb);
    const int64_t centre = (int64_t)dial_hz + tx_centre_hz;
    const uint32_t target = centre > 0 ? (uint32_t)centre : dial_hz;
    clock_carrier_t c;
    (void)CLOCK_CarrierPlan(sysclk_uhz, target, shape, &c);
    e->period = c.period;
    e->cycles = c.cycles;
    e->extra = c.extra;
    const uint32_t update_hz = ((shape != NULL) && shape->update_hz) ? shape->update_hz
                                                                      : CLOCK_TX_UPDATE_HZ;
    e->tx_err_hz = CLOCK_CarrierCostHz(sysclk_uhz, &c, target, update_hz);
    e->floor_hz = c.period ? (update_hz + c.period / 2U) / c.period : UINT32_MAX;
    int32_t offset = 0;
    const uint64_t hr_hz = (sysclk_uhz * CLOCK_HRTIM_MUL + 500000U) / 1000000U;
    e->rx_quality = RADIO_RxPlanLo(hr_hz, dial_hz, rx_mode, NULL, &offset);
    if (e->rx_quality < 2U) {
        e->rx_if_err_hz = UINT32_MAX;
    } else {
        const uint32_t a = (uint32_t)(offset < 0 ? -offset : offset);
        e->rx_if_err_hz = a > CLOCK_RX_IF_TARGET_HZ ? a - CLOCK_RX_IF_TARGET_HZ : CLOCK_RX_IF_TARGET_HZ - a;
    }
}

uint8_t CLOCK_RxClass(const clock_eval_t *e)
{
    if (e->rx_quality >= 2U) return e->rx_if_err_hz <= CLOCK_RX_IF_TOLERANCE_HZ ? 3U : 2U;
    return e->rx_quality;
}

/* Strictly better for AUTO: receive class, then TX residual, then the
 * receive IF, then the higher SYSCLK (more HRTIM ticks per carrier cycle). */
static uint8_t auto_better(const clock_eval_t *a, const clock_eval_t *b)
{
    const uint8_t ca = CLOCK_RxClass(a), cb = CLOCK_RxClass(b);
    if (ca != cb) return ca > cb;
    if (a->tx_err_hz != b->tx_err_hz) return a->tx_err_hz < b->tx_err_hz;
    if (a->rx_if_err_hz != b->rx_if_err_hz) return a->rx_if_err_hz < b->rx_if_err_hz;
    return a->sysclk_hz > b->sysclk_hz;
}

uint8_t CLOCK_TxCostSettled(const clock_eval_t *e)
{
    if (!e->period) return 0U;
    if (e->tx_err_hz <= CLOCK_TX_HOP_COST_HZ) return 1U;
    return (uint8_t)((uint64_t)e->tx_err_hz * 1000U <= (uint64_t)e->floor_hz * 1414U);
}

int CLOCK_ChooseAuto(const clock_eval_t *e, unsigned n, int current)
{
    int best = -1;
    for (unsigned i = 0U; i < n; ++i) {
        if (!e[i].period) continue;
        if (best < 0 || auto_better(&e[i], &e[best])) best = (int)i;
    }
    if (best < 0 || current < 0 || (unsigned)current >= n || current == best) return best;
    const clock_eval_t *c = &e[current], *b = &e[best];
    if (!c->period || CLOCK_RxClass(c) != CLOCK_RxClass(b)) return best;
    /* Same receive class: only a clearly smaller TX residual pays for a PLL
     * retune (a ~10 ms receive gap). sqrt(2) is 3 dB of image level. */
    if (CLOCK_TxCostSettled(c)) return current;
    if ((uint64_t)b->tx_err_hz * 1414U >= (uint64_t)c->tx_err_hz * 1000U) return current;
    return best;
}

int CLOCK_ChooseTx(const clock_eval_t *e, unsigned n, int current)
{
    if ((current < 0) || ((unsigned)current >= n)) return current;
    const clock_eval_t *c = &e[current];
    if (c->period && CLOCK_TxCostSettled(c)) return current;
    int best = -1;
    for (unsigned i = 0U; i < n; ++i) {
        if (!e[i].period || (e[i].rx_quality < 2U)) continue;
        if (best >= 0) {
            const clock_eval_t *b = &e[best];
            if (e[i].tx_err_hz > b->tx_err_hz) continue;
            if (e[i].tx_err_hz == b->tx_err_hz) {
                const uint8_t ci = CLOCK_RxClass(&e[i]), cb = CLOCK_RxClass(b);
                if ((ci < cb) || ((ci == cb) && (e[i].sysclk_hz <= b->sysclk_hz))) continue;
            }
        }
        best = (int)i;
    }
    if ((best < 0) || (best == current)) return current;
    if (c->period && ((uint64_t)e[best].tx_err_hz * 2U > c->tx_err_hz)) return current;
    return best;
}

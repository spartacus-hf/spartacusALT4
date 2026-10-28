#include "radio_rx_front.h"
#include <math.h>
#include <string.h>

/* 96-tap / 32-phase Kaiser-windowed sinc as float, input 171--190 kHz
 * (PCM1821 at SYSCLK/(64 x 14 or 15) for a 165--170 MHz SYSCLK).
 * The window is designed for 72 dB of stopband with the edge at 25 kHz, so the
 * passband is flat to about +-18.7 kHz (measured -0.31 dB at 18 kHz, -3.4 at
 * 20 kHz, -39 at 24 kHz, -83 at 25 kHz, -86 at 42 kHz) instead of the +-12 kHz
 * the previous 64-tap Blackman table allowed. The 42 kHz figure is itself a
 * reason the rows are float: their Q15 rounding measured -76 dB there.
 * The 25 kHz edge, not 24, keeps the QSD image (-(2*IF + delta)) and the first
 * fold of the 48 kHz output out of the widened passband.
 * Rows hold the designed coefficients unquantised and sum to unity DC gain;
 * see tools/gen_rx_coeff.py for the design and the row order.
 *
 * The window is taken at each row's own fractional position, so every row is
 * a slice of one symmetric prototype and row P - p is row p reversed one tap
 * along (its last tap, past the window, zero). Flash keeps rows 0..P/2 only,
 * 6.5 KB instead of 12; RADIO_RxFrontCoefficientsTo writes the rest out into
 * the receiver's RAM copy, the only table the FIR reads. */
#include "radio_rx_coeff.inc"

/* The table and the tap count cannot drift apart (a regeneration that changes
 * the branch length without the define is a compile error, not a quiet bug),
 * and the history must stay a power of two >= the tap count or the wrap mask
 * and the doubled write are both wrong. */
_Static_assert(sizeof(rx_coeff_half[0]) / sizeof(rx_coeff_half[0][0]) == RADIO_RX_FILTER_TAPS,
               "radio_rx_coeff.inc has a different branch length than RADIO_RX_FILTER_TAPS");
_Static_assert((RADIO_RX_HISTORY & (RADIO_RX_HISTORY - 1U)) == 0U &&
               RADIO_RX_HISTORY >= RADIO_RX_FILTER_TAPS,
               "RADIO_RX_HISTORY must be a power of two >= RADIO_RX_FILTER_TAPS");
_Static_assert((RADIO_RX_FILTER_TAPS % 4U) == 0U,
               "the tap loop consumes four taps per iteration and would drop the rest");
_Static_assert(sizeof(rx_coeff_half) == (RADIO_RX_PHASES / 2U + 1U) * RADIO_RX_FILTER_TAPS * sizeof(float),
               "radio_rx_coeff.inc has a different phase count than RADIO_RX_PHASES");

/* One output of the polyphase FIR: M taps over the interleaved (I,Q) window
 * at h. Four taps per iteration into two accumulators per channel, summed
 * in exactly this order: I0 += h0*c0, Q0 += h1*c0, I1 += h2*c1, Q1 += h3*c1,
 * I0 += h4*c2, Q0 += h5*c2, I1 += h6*c3, Q1 += h7*c3, then I0 + I1, Q0 + Q1.
 *
 * On the Cortex-M4F the compiler's version of this loop cost ~55 cycles per
 * four taps: twelve single VLDRs, each 2 cycles because a multiply-add sits
 * between them, and eight VFMAs at 3 cycles each. Loading the eight history
 * floats and four coefficients with two VLDMs (1 + N cycles) and splitting
 * every multiply-add into VMUL + VADD (1 + 1) makes it ~34, the largest
 * single saving on the receive deadline. The products are rounded before
 * they are added, as on a host without FMA, so this matches the portable
 * loop below except where a fused multiply-add would have rounded once.
 *
 * The loop head sits on a 64-bit flash word (.p2align 3). Left to the layout
 * around it, it landed on a halfword boundary whenever code before it moved
 * by two bytes, and the receive worker took ~6k cycles more per call (bench
 * 2026-10-03: 37.8k -> 44.3k, every receive mode). */
static inline void rx_fir(const float *h, const float *tap, float *out_i, float *out_q)
{
#if defined(__arm__) && defined(__ARM_FP) && !defined(RADIO_RX_FIR_PORTABLE)
    unsigned count = RADIO_RX_FILTER_TAPS / 4U;
    float oi, oq;
    __asm volatile(
        "vmov      s12, %[zero]              \n"
        "vmov      s13, %[zero]              \n"
        "vmov      s14, %[zero]              \n"
        "vmov      s15, %[zero]              \n"
        ".p2align  3                         \n"
        "1:                                  \n"
        "vldmia    %[h]!, {s0-s7}            \n"
        "vldmia    %[c]!, {s8-s11}           \n"
        "vmul.f32  s0, s0, s8                \n"
        "vmul.f32  s1, s1, s8                \n"
        "vmul.f32  s2, s2, s9                \n"
        "vmul.f32  s3, s3, s9                \n"
        "vmul.f32  s4, s4, s10               \n"
        "vmul.f32  s5, s5, s10               \n"
        "vmul.f32  s6, s6, s11               \n"
        "vmul.f32  s7, s7, s11               \n"
        "vadd.f32  s12, s12, s0              \n"
        "vadd.f32  s13, s13, s1              \n"
        "vadd.f32  s14, s14, s2              \n"
        "vadd.f32  s15, s15, s3              \n"
        "vadd.f32  s12, s12, s4              \n"
        "vadd.f32  s13, s13, s5              \n"
        "vadd.f32  s14, s14, s6              \n"
        "vadd.f32  s15, s15, s7              \n"
        "subs      %[n], %[n], #1            \n"
        "bne       1b                        \n"
        "vadd.f32  %[oi], s12, s14           \n"
        "vadd.f32  %[oq], s13, s15           \n"
        : [h] "+r"(h), [c] "+r"(tap), [n] "+r"(count), [oi] "=t"(oi), [oq] "=t"(oq)
        : [zero] "r"(0U)
        : "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
          "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15", "cc", "memory");
    *out_i = oi;
    *out_q = oq;
#else
    float oi0 = 0.0f, oi1 = 0.0f, oq0 = 0.0f, oq1 = 0.0f;
    for (unsigned t = 0U; t < (RADIO_RX_FILTER_TAPS / 4U); ++t) {
        const float c0 = tap[0], c1 = tap[1], c2 = tap[2], c3 = tap[3];
        oi0 += h[0] * c0; oq0 += h[1] * c0;
        oi1 += h[2] * c1; oq1 += h[3] * c1;
        oi0 += h[4] * c2; oq0 += h[5] * c2;
        oi1 += h[6] * c3; oq1 += h[7] * c3;
        tap += 4U;
        h += 8U;
    }
    *out_i = oi0 + oi1;
    *out_q = oq0 + oq1;
#endif
}

/* Timing correction. The polyphase FIR evaluates each 48 kHz output at the
 * nearest of RADIO_RX_PHASES fractional positions, not at its true instant:
 * up to half a phase step, 1/64 of an input sample, early or late, in a
 * pattern set by the rate ratio. A tone f from the dial comes out phase
 * modulated by 2 pi f times that error, which spreads it into a spurious
 * floor: simulated with this table at 170.8 kHz, -47 dBc at 17 kHz and
 * -51 dBc at 9 kHz, single lines -52 to -62 dBc -- a strong signal 10-20 kHz
 * off the dial raising the floor under a weak one, which QMX (an integer
 * 48 kHz ADC) does not do. The error of every output is known, so each is
 * moved to its true instant to first order, y + d y', with y' from a
 * least-squares derivative across the 48 kHz stream (+-7 outputs, flat to
 * 0.40 fS): -89 to -94 dBc from 3 to 17 kHz in the host suite
 * (rev1_front_filter, the production float code), for ~70 cycles an output and
 * 0.15 ms of latency. d is kept as a byte (1/256 of a step): its rounding
 * sits ~48 dB under the error it corrects. */
#define RX_TC_DELAY 7U
#define RX_TC_RING  16U
_Static_assert(RX_TC_RING >= 2U * RX_TC_DELAY + 1U && (RX_TC_RING & (RX_TC_RING - 1U)) == 0U,
               "the timing-correction ring holds the whole derivative window");
static const float rx_tc_coeff[RX_TC_DELAY] = {
    9.618694427e-01f, -4.271848670e-01f, 2.321656420e-01f, -1.286966948e-01f,
    6.742897478e-02f, -3.112709667e-02f, 1.110179323e-02f,
};

void RADIO_RxClockEdges(uint32_t period, radio_rx_geometry_t *g)
{
    const uint32_t m = period / 4U;
    g->period = period;
    g->half = period / 2U;
    switch (period % 4U) {
    case 0U: g->fall = m; g->rise = m; break;          /* exact quarters */
    case 2U: g->fall = m; g->rise = m + 1U; break;     /* S0 low window centred on p/2 */
    case 1U: g->fall = m; g->rise = m + 1U; break;     /* S0+S3 = (p+1)/2 */
    default: g->fall = m + 1U; g->rise = m + 1U; break; /* 4m+3: S0+S3 = (p+1)/2 */
    }
}

/* The receive channel per mode the planner works with: the defaults until
 * radio.c hands over the configured widths (RADIO_ConfigChannelSpans). */
static int16_t rx_span[5][2] = RADIO_RX_CHANNEL_HZ;

void RADIO_RxSetChannelSpans(const int16_t spans[5][2])
{
    memcpy(rx_span, spans, sizeof(rx_span));
}

void RADIO_RxChannelSpan(radio_rx_mode_t mode, int32_t *lo, int32_t *hi)
{
    const unsigned m = (unsigned)mode <= (unsigned)RADIO_RX_FM ? (unsigned)mode : 0U;
    *lo = rx_span[m][0];
    *hi = rx_span[m][1];
}

/* The receive ADC's rate. REV1's PCM1820 (strapped I2S slave, linear phase,
 * DRE off) repeats two of every thirteen samples at 162-190 kHz -- each
 * channel's sample from two frames before, I and Q three frames apart; at
 * every rate tried in that range, which is what showed as the "fS/312 comb":
 * 24 x 13 = 312. Every received signal carried copies at k x fS/312, the
 * first at -4 dBc. At 48-104 kHz it is clean (112-153 kHz it does not run at
 * all). Measured 2026-10-02 from raw `iq` snapshots with the I2S divider set
 * live; on 20 m FT8 the ~95 kHz rate decoded three times as many signals,
 * ~10 dB stronger, than 171 kHz.
 *
 * At ~95 kHz the ADC is flat to +-43 kHz, so the plan takes a +-40 kHz IF
 * window. One HRTIM tick moves the LO by rf^2 / (SYSCLK x 32) -- 37 kHz at
 * 14 MHz, 81 kHz at 21 MHz, 144 kHz at 28 MHz -- so up to about 20 MHz every
 * dial has an LO in that window; above it, where none fits, the plan falls
 * back to the old +-50 kHz with the ADC at 170-190 kHz. `clock adc96` can pin
 * either for tests. */
static uint8_t rx_adc_policy;      /* RADIO_RX_ADC_AUTO, _96K, _HIGH */
static uint8_t rx_adc_running96;   /* the running plan's (RADIO_RxKeepLo) */
void RADIO_RxAdcPolicySet(uint8_t policy) { rx_adc_policy = policy <= RADIO_RX_ADC_HIGH ? policy : RADIO_RX_ADC_AUTO; }
uint8_t RADIO_RxAdcPolicy(void) { return rx_adc_policy; }
void RADIO_RxAdcRunning96(uint8_t on) { rx_adc_running96 = on ? 1U : 0U; }

static uint8_t plan_lo(uint64_t hr_clock_hz, uint32_t rf_hz, radio_rx_mode_t mode, int32_t flat,
                       uint32_t *period, int32_t *offset_hz)
{
    const int16_t (*span)[2] = rx_span;
    const int32_t dc = 600, target = 12000;
    unsigned m = (unsigned)mode <= (unsigned)RADIO_RX_FM ? (unsigned)mode : 0U;
    uint8_t best_quality = 0U;
    uint32_t best_period = 0U, best_score = 0xFFFFFFFFUL;
    int32_t best_offset = 0;
    if (!rf_hz || hr_clock_hz < (uint64_t)rf_hz * 192ULL) return 0U;
    uint32_t p0 = (uint32_t)((hr_clock_hz + rf_hz / 2U) / rf_hz);
    /* Ticks covering +-45 kHz around the nearest period (LO step ~ rf/p). */
    uint32_t reach = (uint32_t)(((uint64_t)flat * p0 + rf_hz - 1U) / rf_hz) + 1U;
    if (reach > 256U) reach = 256U;
    for (uint32_t p = p0 > reach ? p0 - reach : 1U; p <= p0 + reach; ++p) {
        if (p < 192U || p > 0xFFDCU) continue;
        uint32_t lo = (uint32_t)((hr_clock_hz + p / 2U) / p);
        int32_t d = (int32_t)(rf_hz - lo);
        int32_t a = d + span[m][0], b = d + span[m][1];
        if (a < -flat || b > flat) continue;
        uint8_t quality = (a > dc || b < -dc) ? 2U : 1U;
        int32_t ad = d < 0 ? -d : d;
        /* Odd periods cannot balance the QSD windows. Below ~400 ticks (above
         * 13.6 MHz) that costs about 5 dB of image rejection, so take one only
         * if it is 4 kHz closer to the 12 kHz IF; longer periods don't care. */
        uint32_t score = (uint32_t)(ad > target ? ad - target : target - ad) +
                         (((p & 1U) && p < 400U) ? 4000U : 0U);
        if (quality > best_quality || (quality == best_quality && score < best_score)) {
            best_quality = quality; best_score = score; best_period = p; best_offset = d;
        }
    }
    if (best_quality) {
        if (period) *period = best_period;
        if (offset_hz) *offset_hz = best_offset;
    }
    return best_quality;
}

__attribute__((cold)) uint8_t RADIO_RxPlanLoAdc(uint64_t hr_clock_hz, uint32_t rf_hz, radio_rx_mode_t mode,
                                                uint32_t *period, int32_t *offset_hz, uint8_t *adc96)
{
    uint32_t p96 = 0U, pw = 0U;
    int32_t o96 = 0, ow = 0;
    uint8_t q96 = 0U, qw = 0U;
    if (rx_adc_policy != RADIO_RX_ADC_HIGH) q96 = plan_lo(hr_clock_hz, rf_hz, mode, 40000, &p96, &o96);
    /* A clean plan at ~96 kHz, or any when pinned there; the wide window
     * only when it does better. */
    if (q96 < 2U && rx_adc_policy != RADIO_RX_ADC_96K) qw = plan_lo(hr_clock_hz, rf_hz, mode, 50000, &pw, &ow);
    const uint8_t narrow = (uint8_t)(q96 >= qw && q96);
    const uint8_t q = narrow ? q96 : qw;
    if (q) {
        if (period) *period = narrow ? p96 : pw;
        if (offset_hz) *offset_hz = narrow ? o96 : ow;
    }
    if (adc96) *adc96 = narrow;
    return q;
}

__attribute__((cold)) uint8_t RADIO_RxPlanLo(uint64_t hr_clock_hz, uint32_t rf_hz, radio_rx_mode_t mode,
                                             uint32_t *period, int32_t *offset_hz)
{
    return RADIO_RxPlanLoAdc(hr_clock_hz, rf_hz, mode, period, offset_hz, NULL);
}

__attribute__((cold)) uint8_t RADIO_RxKeepLo(int32_t offset_hz, int32_t span_lo, int32_t span_hi)
{
    const int32_t flat = rx_adc_running96 ? 38000 : 45000, dc = 600;
    const int32_t a = offset_hz + span_lo, b = offset_hz + span_hi;
    const int32_t m = offset_hz < 0 ? -offset_hz : offset_hz;
    if (a < -flat || b > flat) return 0U;             /* off the flat span */
    if (!(a > dc || b < -dc)) return 0U;               /* channel touches DC */
    return (uint8_t)(m >= RADIO_RX_IF_KEEP_MIN_HZ && m <= RADIO_RX_IF_KEEP_MAX_HZ);
}

__attribute__((cold)) void RADIO_RxFrontIfTerms(radio_rx_if_terms_t *t, float rate, float offset_hz, float skew_samples)
{
    if (!isfinite(offset_hz)) offset_hz = 0.0f;
    const float angle = 6.28318530718f * offset_hz / rate;
    t->step_i = cosf(angle); t->step_q = -sinf(angle);
    t->delay_i_scale = t->delay_q_scale = 1.0f;
    t->delay_trim = 0.0f;
    radio_iq_delay_t d;
    RADIO_IqDelayInit(&d, skew_samples);
    if (d.active) {
        float hi, hq;
        RADIO_IqDelayResponse(&d, offset_hz / rate, &hi, &hq);
        const float magnitude = sqrtf(hi * hi + hq * hq);
        const float physical = angle * skew_samples;
        const float residual = d.delay_q
                             ? physical + atan2f(hq, hi)
                             : physical - atan2f(hq, hi);
        const float cosine = cosf(residual);
        if (magnitude > 0.1f && fabsf(cosine) > 0.1f) {
            if (d.delay_q) t->delay_q_scale = 1.0f / (magnitude * cosine);
            else {
                t->delay_i_scale = 1.0f / magnitude;
                t->delay_q_scale = 1.0f / cosine;
            }
            t->delay_trim = sinf(residual) / cosine; /* tan without tanf */
        }
    }
}

static void front_take_terms(radio_rx_front_t *s, const radio_rx_if_terms_t *t)
{
    s->step_i = t->step_i; s->step_q = t->step_q;
    s->delay_i_scale = t->delay_i_scale; s->delay_q_scale = t->delay_q_scale;
    s->delay_trim = t->delay_trim;
}

__attribute__((cold)) void RADIO_RxFrontInit(radio_rx_front_t *s, float rate, float offset_hz, float skew_samples)
{
    memset(s, 0, sizeof(*s));
    if (!isfinite(rate) || rate < 80000.0f || rate > 192000.0f) rate = 177083.0f;
    s->rate = rate; s->osc_i = 1.0f; s->trim_i = 1.0f;
    s->coeff = NULL; /* RADIO_RxFrontCoefficientsTo gives it the whole table */
    RADIO_IqDelayInit(&s->iq_delay, skew_samples);
    radio_rx_if_terms_t t;
    RADIO_RxFrontIfTerms(&t, rate, offset_hz, skew_samples);
    front_take_terms(s, &t);
}

__attribute__((cold)) void RADIO_RxFrontCoefficientsTo(radio_rx_front_t *s, float *ram)
{
    const unsigned half = RADIO_RX_PHASES / 2U, taps = RADIO_RX_FILTER_TAPS;
    memcpy(ram, rx_coeff_half, sizeof(rx_coeff_half));
    for (unsigned row = half + 1U; row < RADIO_RX_PHASES; ++row) {
        const float *mirror = rx_coeff_half[RADIO_RX_PHASES - row];
        float *dst = &ram[row * taps];
        for (unsigned n = 0U; n + 1U < taps; ++n) dst[n] = mirror[taps - 2U - n];
        dst[taps - 1U] = 0.0f;
    }
    s->coeff = ram;
}

unsigned RADIO_RxFrontBlock(radio_rx_front_t *s, const radio_config_t *c,
                            const float *in_i, const float *in_q, unsigned n,
                            float *out_i, float *out_q)
{
    if (s->coeff == NULL) return 0U; /* no table yet: nothing to filter with */
    /* A seamless retune handed over new IF terms: take them at a block
     * boundary. The oscillator itself runs on, so the phase is continuous. */
    if (s->retune_seq != s->retune_applied) {
        s->retune_applied = s->retune_seq;
        front_take_terms(s, &s->next);
    }
    /* Everything the loop reads that cannot change during the run. */
    const uint8_t invert = c->rx_iq_invert;
    const float i_gain = c->rx_i_gain * s->delay_i_scale;
    const float q_gain = c->rx_q_gain * s->delay_q_scale;
    const float trim = s->delay_trim;
    const float rate = s->rate;
    const float step_i = s->step_i * s->trim_i - s->step_q * s->trim_q;
    const float step_q = s->step_i * s->trim_q + s->step_q * s->trim_i;
    /* Output samples per phase step, negated: a late instant (fraction above
     * one half) needs the output moved back. */
    const float tc_scale = -(48000.0f / (float)RADIO_RX_PHASES) / rate;
    float dc_i = s->dc_i, dc_q = s->dc_q, osc_i = s->osc_i, osc_q = s->osc_q;
    float phase = s->phase;
    unsigned index = s->index, normalize = s->normalize, produced = 0U;
    for (unsigned k = 0U; k < n; ++k) {
        float i = in_i[k], q = in_q[k];
        if (!isfinite(i) || !isfinite(q)) i = q = 0.0f;
        if (invert) q = -q;
        dc_i += 0.00005f * (i - dc_i);
        dc_q += 0.00005f * (q - dc_q);
        RADIO_IqDelaySample(&s->iq_delay, i - dc_i, q - dc_q, &i, &q);
        i *= i_gain;
        q = q * q_gain - i * trim;
        index = (index + 1U) & (RADIO_RX_HISTORY - 1U);
        const float hi_new = i * osc_i - q * osc_q;
        const float hq_new = i * osc_q + q * osc_i;
        const unsigned at = 2U * index;
        const unsigned wrap = at + 2U * RADIO_RX_HISTORY;
        s->history[at] = hi_new;
        s->history[at + 1U] = hq_new;
        s->history[wrap] = hi_new;
        s->history[wrap + 1U] = hq_new;
        const float next_i = osc_i * step_i - osc_q * step_q;
        osc_q = osc_i * step_q + osc_q * step_i;
        osc_i = next_i;
        /* Once in 256 samples: kept out of the sample loop's straight line
         * (with -fno-math-errno the inline sqrt no longer marks it cold). */
        if (__builtin_expect(++normalize == 256U, 0)) {
            const float norm = 1.0f / sqrtf(osc_i * osc_i + osc_q * osc_q);
            osc_i *= norm; osc_q *= norm; normalize = 0U;
        }
        phase += 48000.0f;
        if (phase < rate) continue;
        phase -= rate;
        const float position = phase * ((float)RADIO_RX_PHASES / 48000.0f);
        unsigned tap_phase = (unsigned)position;
        if (tap_phase > RADIO_RX_PHASES - 1U) tap_phase = RADIO_RX_PHASES - 1U;
        float frac = position - (float)tap_phase;
        if (frac > 0.996f) frac = 0.996f;
        /* This runs at the 48 kHz output rate and is most of the receive DSP:
         * 2 x M multiply-accumulates per output, 2767 times a second. Written
         * naively it cost about 318 us per 361 us I2S half on REV1 -- 88 % of
         * the core for receive alone -- and the receiver could never meet its
         * DMA deadline. What pays for it, none of it a change to the filter:
         *   - the interleaved doubled history above turns the window into one
         *     ascending run of (I,Q) pairs, so eight floats arrive with the
         *     pointer walking itself, and one pointer serves both channels
         *     instead of two walking down;
         *   - the rows are float, not Q15, so a tap costs one load and one
         *     multiply-accumulate and no int-to-float convert at all. At 96
         *     taps that is ~340,000 converts a second less, and the
         *     coefficients are the designed ones rather than their Q15
         *     rounding (see tools/gen_rx_coeff.py);
         *   - four taps per iteration, two accumulators per channel: a
         *     multiply-add has three cycles of latency, and a single running
         *     sum serialises on it;
         *   - rx_fir() loads each four-tap group with two VLDMs and does not
         *     fuse the multiply-adds, and the rows can be read from a RAM
         *     copy (RADIO_RxFrontCoefficientsTo) instead of flash.
         * Same taps and same data, so only the summation order differs. */
        /* The oldest of the M samples just written: `index` advanced above, so
         * the window ends on it and starts M writes earlier, and the doubled
         * copy makes that a contiguous run with no wrap. */
        const float *h = &s->history[2U * (index + RADIO_RX_HISTORY + 1U
                                           - RADIO_RX_FILTER_TAPS)];
        const unsigned slot = s->tc_at;
        rx_fir(h, &s->coeff[tap_phase * RADIO_RX_FILTER_TAPS], &s->tc_i[slot], &s->tc_q[slot]);
        s->tc_frac[slot] = (uint8_t)(frac * 256.0f);
        s->tc_at = (slot + 1U) & (RX_TC_RING - 1U);
        /* Emit the output RX_TC_DELAY back, moved to its true instant. */
        const unsigned m = (slot - RX_TC_DELAY) & (RX_TC_RING - 1U);
        float di = 0.0f, dq = 0.0f;
        for (unsigned j = 1U; j <= RX_TC_DELAY; ++j) {
            const unsigned a = (m + j) & (RX_TC_RING - 1U), b = (m - j) & (RX_TC_RING - 1U);
            di += rx_tc_coeff[j - 1U] * (s->tc_i[a] - s->tc_i[b]);
            dq += rx_tc_coeff[j - 1U] * (s->tc_q[a] - s->tc_q[b]);
        }
        const float d = ((float)s->tc_frac[m] * (1.0f / 256.0f) + (0.5f / 256.0f) - 0.5f) * tc_scale;
        out_i[produced] = s->tc_i[m] + d * di;
        out_q[produced] = s->tc_q[m] + d * dq;
        ++produced;
    }
    s->dc_i = dc_i; s->dc_q = dc_q;
    s->osc_i = osc_i; s->osc_q = osc_q;
    s->phase = phase;
    s->index = index; s->normalize = normalize;
    return produced;
}

uint8_t RADIO_RxFrontSample(radio_rx_front_t *s, const radio_config_t *c,
                           float i, float q, float *out_i, float *out_q)
{
    return (uint8_t)RADIO_RxFrontBlock(s, c, &i, &q, 1U, out_i, out_q);
}

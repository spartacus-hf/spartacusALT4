#include "radio_iq_fit.h"
#include <math.h>
#include <string.h>

#define IQ_TWO_PI 6.28318530718f

/* The fractional-delay response at the measurement bin and the scaling that
 * undoes it; see iq_correction below. */
typedef struct {
    float i, q;
    float i_scale, q_scale, trim;
    uint8_t delay_q;
} iq_correction_t;

static void oscillate(float *i, float *q, float di, float dq)
{
    float ni = *i * di - *q * dq;
    *q = *i * dq + *q * di;
    *i = ni;
}

static void renormalize(float *i, float *q)
{
    float m = *i * *i + *q * *q;
    if (m > 1e-12f) {
        float n = 1.0f / sqrtf(m);
        *i *= n; *q *= n;
    } else {
        *i = 1.0f; *q = 0.0f;
    }
}

void RADIO_IqCorrInit(radio_iq_corr_t *c, float cycles_per_sample, uint32_t samples)
{
    memset(c, 0, sizeof(*c));
    if (!isfinite(cycles_per_sample)) cycles_per_sample = 0.0f;
    if (samples < 256U) samples = 256U;
    c->samples = samples;
    c->cycles_per_sample = cycles_per_sample;
    c->q_gain = 1.0f;
    c->osc_i = 1.0f;
    float angle = IQ_TWO_PI * cycles_per_sample;
    c->step_i = cosf(angle); c->step_q = sinf(angle);
    /* Hann over the run: w(n) = 0.5 - 0.5 cos(2 pi (n + 0.5) / N), carried by
     * a second oscillator so there is no cosine call per sample. */
    float half = 3.14159265359f / (float)samples;
    c->win_i = cosf(half); c->win_q = sinf(half);
    c->win_step_i = cosf(2.0f * half); c->win_step_q = sinf(2.0f * half);
}

void RADIO_IqCorrCorrection(radio_iq_corr_t *c, uint8_t invert, float q_gain,
                            float skew_samples)
{
    c->invert = invert ? 1U : 0U;
    c->q_gain = isfinite(q_gain) ? q_gain : 1.0f;
    c->skew_samples = isfinite(skew_samples) ? skew_samples : 0.0f;
}

/* The correction is a function of q_gain, the skew and the bin alone, and the
 * solve is the only place that wants it. Deriving it there rather than caching
 * it keeps the correlator the audio half-block feeds per sample -- the only
 * part of this that has to live in CCM -- at its original size. */
static void iq_correction(const radio_iq_corr_t *c, iq_correction_t *k)
{
    k->i = 1.0f;
    k->q = 0.0f;
    k->i_scale = 1.0f;
    k->q_scale = c->q_gain;
    k->trim = 0.0f;
    radio_iq_delay_t delay;
    RADIO_IqDelayInit(&delay, c->skew_samples);
    k->delay_q = delay.delay_q;
    if (!delay.active) return;
    const float w = IQ_TWO_PI * c->cycles_per_sample;
    RADIO_IqDelayResponse(&delay, c->cycles_per_sample, &k->i, &k->q);
    const float magnitude = sqrtf(k->i * k->i + k->q * k->q);
    const float physical = w * c->skew_samples;
    const float residual = delay.delay_q ? physical + atan2f(k->q, k->i)
                                         : physical - atan2f(k->q, k->i);
    const float cosine = cosf(residual);
    if (magnitude > 0.1f && fabsf(cosine) > 0.1f) {
        if (delay.delay_q) k->q_scale = c->q_gain / (magnitude * cosine);
        else {
            k->i_scale = 1.0f / magnitude;
            k->q_scale = c->q_gain / cosine;
        }
        k->trim = sinf(residual) / cosine; /* tan without tanf */
    }
}

uint8_t RADIO_IqCorrDone(const radio_iq_corr_t *c) { return c->count >= c->samples; }

void RADIO_IqCorrAdd(radio_iq_corr_t *c, float i, float q)
{
    if (c->count >= c->samples) return;
    if (!isfinite(i) || !isfinite(q)) i = q = 0.0f;
    float a = fabsf(i), b = fabsf(q);
    if (a > c->peak) c->peak = a;
    if (b > c->peak) c->peak = b;
    /* Corrections are applied to the accumulated complex bin in Solve. A
     * linear filter at one frequency is exactly one complex multiply, and
     * doing it there avoids another all-pass at the 177 kHz input rate. */
    const float w = 0.5f - 0.5f * c->win_i;
    const float wi = w * i, wq = w * q;
    c->x_i += wi * c->osc_i;
    c->x_q -= wi * c->osc_q;
    c->y_i += wq * c->osc_i;
    c->y_q -= wq * c->osc_q;
    c->dc_i += wi;
    c->dc_q += wq;
    c->weight += w;
    oscillate(&c->osc_i, &c->osc_q, c->step_i, c->step_q);
    oscillate(&c->win_i, &c->win_q, c->win_step_i, c->win_step_q);
    if (++c->normalize >= 256U) {
        renormalize(&c->osc_i, &c->osc_q);
        renormalize(&c->win_i, &c->win_q);
        c->normalize = 0U;
    }
    ++c->count;
}

/* An int64 as float from its two halves: the compiler's own conversion is
 * newlib's soft-float __aeabi_l2f on this FPU. Two roundings, so at most
 * one unit in the last place off the exact conversion. */
static float i64_float(int64_t v)
{
    return (float)(int32_t)(v >> 32) * 4294967296.0f + (float)(uint32_t)v;
}

static float amplitude_dbfs(float amplitude)
{
    return amplitude > 1e-9f ? 20.0f * log10f(amplitude) : -180.0f;
}

__attribute__((cold)) uint8_t RADIO_IqSolve(const radio_iq_corr_t *c, radio_iq_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->i_gain = 1.0f; out->q_gain = 1.0f;
    if (!RADIO_IqCorrDone(c) || !(c->weight > 0.0f)) return 0U;
    const float scale = 1.0f / c->weight;
    out->dc_i = c->dc_i * scale;
    out->dc_q = c->dc_q * scale;
    out->clipped = c->peak > 0.98f;
    float xi = c->x_i, xq = c->x_q, yi = c->y_i, yq = c->y_q;
    if (c->invert) { yi = -yi; yq = -yq; }
    iq_correction_t k;
    iq_correction(c, &k);
    if (c->skew_samples != 0.0f) {
        float *ai = k.delay_q ? &yi : &xi;
        float *aq = k.delay_q ? &yq : &xq;
        const float ni = *ai * k.i - *aq * k.q;
        *aq = *ai * k.q + *aq * k.i;
        *ai = ni;
    }
    xi *= k.i_scale;
    xq *= k.i_scale;
    yi = yi * k.q_scale - xi * k.trim;
    yq = yq * k.q_scale - xq * k.trim;
    /* The two sideband levels, reported whether or not the rest solves:
     * S+ = X + jY and S- = conj(X - jY). The floor pass has no carrier and
     * needs exactly these. */
    const float sp_i = xi - yq, sp_q = xq + yi;
    const float sm_i = xi + yq, sm_q = xq - yi;
    const float sp = sqrtf(sp_i * sp_i + sp_q * sp_q) * scale;
    const float sm = sqrtf(sm_i * sm_i + sm_q * sm_q) * scale;
    const float den = xi * xi + xq * xq;
    float k_i = 0.0f, k_q = 0.0f;
    if (den > 0.0f) {
        /* k = (1 + g) e^(j theta) = j Y / X. */
        const float ratio_i = (yi * xi + yq * xq) / den;
        const float ratio_q = (yq * xi - yi * xq) / den;
        k_i = -ratio_q; k_q = ratio_i;
        /* Re(k) < 0 is |1 - conj(k)| > |1 + k|: the carrier is in the negative
         * bin, which is a swapped I/Q pair rather than an imbalance. RX IQ
         * INVERT negates Q, which conjugates the stream and exchanges the two
         * bins, so solve for the stream that setting would produce. */
        if (k_i < 0.0f) { out->inverted = 1U; k_i = -k_i; k_q = -k_q; }
    }
    const float wanted = out->inverted ? sm : sp;
    const float image = out->inverted ? sp : sm;
    out->wanted_dbfs = amplitude_dbfs(wanted);
    out->image_dbfs = amplitude_dbfs(image);
    out->image_db = out->wanted_dbfs - out->image_dbfs;
    if (!(den > 0.0f)) return 0U;
    if (!(wanted > 1e-7f)) return 0U; /* no carrier: nothing was injected */
    const float magnitude = sqrtf(k_i * k_i + k_q * k_q);
    const float theta = atan2f(k_q, k_i);
    if (!(magnitude > 1e-3f) || !isfinite(theta)) return 0U;
    const float cosine = cosf(theta);
    if (fabsf(cosine) < 1e-3f) return 0U;
    out->amp_db = 20.0f * log10f(magnitude);
    out->phase_deg = theta * (180.0f / 3.14159265359f);
    /* A channel time skew produces theta = 2 pi f skew.  Store the delay in
     * input samples so the receive front end corrects every frequency by the
     * corresponding angle instead of reusing this one measured angle. */
    out->i_gain = 1.0f;
    out->q_gain = 1.0f / magnitude;
    if (fabsf(c->cycles_per_sample) < 1.0e-6f) return 0U;
    out->skew_samples = theta / (IQ_TWO_PI * c->cycles_per_sample);
    if (!isfinite(out->q_gain) || !isfinite(out->skew_samples) ||
        fabsf(out->skew_samples) > RADIO_IQ_SKEW_MAX) return 0U;
    out->valid = 1U;
    return 1U;
}

float RADIO_IqDeltaCycles(uint32_t p_lo, uint32_t p_test, uint32_t i2s_div)
{
    if (!p_lo || !p_test || !i2s_div) return 0.0f;
    /* Divide term by term: the product of two 16-bit periods is past the range
     * where a float counts integers exactly. */
    float n = 2048.0f * (float)i2s_div * ((float)p_lo - (float)p_test);
    return n / (float)p_lo / (float)p_test;
}

uint8_t RADIO_IqChoosePeriod(uint32_t p_lo, uint32_t i2s_div, float rate_hz,
                             float min_hz, float max_hz, uint32_t *p_test)
{
    if (p_lo < RADIO_IQ_PERIOD_MIN || p_lo > RADIO_IQ_PERIOD_MAX ||
        !(rate_hz > 0.0f) || !(min_hz > 0.0f) || !(max_hz > min_hz)) return 0U;
    /* delta grows with the offset, so the first offset that clears min_hz is
     * also the smallest delta available. Above the LO first, so the carrier
     * lands in the positive bin the receiver normally uses. */
    for (uint32_t k = 1U; k <= 256U; ++k) {
        const uint32_t candidate[2] = {p_lo - k, p_lo + k};
        for (unsigned side = 0U; side < 2U; ++side) {
            const uint32_t p = candidate[side];
            if (p_lo <= k || p < RADIO_IQ_PERIOD_MIN || p > RADIO_IQ_PERIOD_MAX) continue;
            const float delta = fabsf(RADIO_IqDeltaCycles(p_lo, p, i2s_div)) * rate_hz;
            if (delta < min_hz) continue;
            if (delta > max_hz) return 0U;
            if (p_test) *p_test = p;
            return 1U;
        }
    }
    return 0U;
}

__attribute__((cold)) void RADIO_IqSnapshotStats(const int32_t *pairs, unsigned n, radio_iq_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!pairs || !n) return;
    out->i_max = out->i_min = pairs[0];
    out->q_max = out->q_min = pairs[1];
    /* Exact sums of the 24-bit samples (the PCM1821 word is left-justified)
     * in 64 bits -- room for 2^17 pairs -- then one scale back to +-1 full
     * scale, 2^-46. More exact than the double sums this replaces, and no
     * software double arithmetic on a single-precision FPU. */
    int64_t sii = 0, sqq = 0, siq = 0;
    for (unsigned k = 0U; k < n; ++k) {
        const int32_t i = pairs[2U * k], q = pairs[2U * k + 1U];
        if (i > out->i_max) out->i_max = i;
        if (i < out->i_min) out->i_min = i;
        if (q > out->q_max) out->q_max = q;
        if (q < out->q_min) out->q_min = q;
        const int32_t a = i >> 8, b = q >> 8;
        sii += (int64_t)a * a; sqq += (int64_t)b * b; siq += (int64_t)a * b;
    }
    const float scale = 1.0f / 70368744177664.0f;
    const float pii = i64_float(sii) * scale, pqq = i64_float(sqq) * scale, piq = i64_float(siq) * scale;
    out->i_rms = sqrtf(pii / (float)n);
    out->q_rms = sqrtf(pqq / (float)n);
    out->clipped = (uint8_t)(out->i_max > 2080374784 || out->i_min < -2080374784 ||
                             out->q_max > 2080374784 || out->q_min < -2080374784);
    /* Below about -70 dBFS on either channel the estimate is noise, not a
     * measurement, and saying so is more use than printing a number. */
    if (!(pii > 1e-8f) || !(pqq > 1e-8f)) return;
    out->amp_db = 10.0f * log10f(pqq / pii);
    float sine = piq / sqrtf(pii * pqq);
    if (sine > 1.0f) sine = 1.0f;
    if (sine < -1.0f) sine = -1.0f;
    out->phase_deg = asinf(sine) * (180.0f / 3.14159265358979323846f);
    out->valid = 1U;
}

#ifndef RADIO_SPECTRUM_DSP_H
#define RADIO_SPECTRUM_DSP_H

/* The arithmetic of the waterfall, header-only so the host suite runs the
 * very code the firmware does (tools/tests/spectrum_dsp_test.c).
 *
 * FFT: 512-point complex, in place, radix-2^2 decimation in frequency -- the
 * multiplication count of radix-4 with radix-2's output order -- so the
 * result is bit-reversed and is read through spec_bitrev9 rather than
 * permuted: the waterfall only ever reads the ~300 bins it draws. Twiddles
 * come from one 129-float quarter-wave cosine table in flash (516 bytes);
 * the window is a 257-entry Q15 half table the engine rebuilds in the
 * receiver arena, so nothing here costs static RAM.
 *
 * The half-band decimator is the one piece the receive worker runs: it lets
 * the zoomed spans capture at 24 or 12 kHz, so zooming in buys resolution
 * (47 and 23 Hz bins) rather than bigger pixels. */

#include <math.h>
#include <stdint.h>
#include <string.h>

#define SPEC_FFT_N 512U

/* cos(2 pi n / 512), n = 0..128. */
static const float spec_cos_q[129] = {
    1.000000000f, 0.999924702f, 0.999698819f, 0.999322385f,
    0.998795456f, 0.998118113f, 0.997290457f, 0.996312612f,
    0.995184727f, 0.993906970f, 0.992479535f, 0.990902635f,
    0.989176510f, 0.987301418f, 0.985277642f, 0.983105487f,
    0.980785280f, 0.978317371f, 0.975702130f, 0.972939952f,
    0.970031253f, 0.966976471f, 0.963776066f, 0.960430519f,
    0.956940336f, 0.953306040f, 0.949528181f, 0.945607325f,
    0.941544065f, 0.937339012f, 0.932992799f, 0.928506080f,
    0.923879533f, 0.919113852f, 0.914209756f, 0.909167983f,
    0.903989293f, 0.898674466f, 0.893224301f, 0.887639620f,
    0.881921264f, 0.876070094f, 0.870086991f, 0.863972856f,
    0.857728610f, 0.851355193f, 0.844853565f, 0.838224706f,
    0.831469612f, 0.824589303f, 0.817584813f, 0.810457198f,
    0.803207531f, 0.795836905f, 0.788346428f, 0.780737229f,
    0.773010453f, 0.765167266f, 0.757208847f, 0.749136395f,
    0.740951125f, 0.732654272f, 0.724247083f, 0.715730825f,
    0.707106781f, 0.698376249f, 0.689540545f, 0.680600998f,
    0.671558955f, 0.662415778f, 0.653172843f, 0.643831543f,
    0.634393284f, 0.624859488f, 0.615231591f, 0.605511041f,
    0.595699304f, 0.585797857f, 0.575808191f, 0.565731811f,
    0.555570233f, 0.545324988f, 0.534997620f, 0.524589683f,
    0.514102744f, 0.503538384f, 0.492898192f, 0.482183772f,
    0.471396737f, 0.460538711f, 0.449611330f, 0.438616239f,
    0.427555093f, 0.416429560f, 0.405241314f, 0.393992040f,
    0.382683432f, 0.371317194f, 0.359895037f, 0.348418680f,
    0.336889853f, 0.325310292f, 0.313681740f, 0.302005949f,
    0.290284677f, 0.278519689f, 0.266712757f, 0.254865660f,
    0.242980180f, 0.231058108f, 0.219101240f, 0.207111376f,
    0.195090322f, 0.183039888f, 0.170961889f, 0.158858143f,
    0.146730474f, 0.134580709f, 0.122410675f, 0.110222207f,
    0.098017140f, 0.085797312f, 0.073564564f, 0.061320736f,
    0.049067674f, 0.036807223f, 0.024541229f, 0.012271538f,
    0.000000000f,
};

/* cos(2 pi n / 512) for any n. */
static inline float spec_cos512(unsigned n)
{
    n &= 511U;
    if (n <= 128U) return spec_cos_q[n];
    if (n <= 256U) return -spec_cos_q[256U - n];
    if (n <= 384U) return -spec_cos_q[n - 256U];
    return spec_cos_q[512U - n];
}

/* Position of bin k (taken mod 512) in the transformed buffer. */
static inline unsigned spec_bitrev9(unsigned k)
{
#if defined(__arm__) && defined(__ARM_ARCH_7EM__)
    uint32_t r;
    __asm ("rbit %0, %1" : "=r"(r) : "r"(k));
    return r >> 23;
#else
    unsigned r = 0U;
    for (unsigned b = 0U; b < 9U; ++b) r = (r << 1) | ((k >> b) & 1U);
    return r;
#endif
}

/* x: 512 interleaved (re, im) floats; X[k] ends at spec_bitrev9(k). About
 * 25,000 cycles on the Cortex-M4F. */
static inline void spec_fft512(float *x)
{
    for (unsigned m = SPEC_FFT_N; m >= 8U; m >>= 2) {
        const unsigned q = m >> 2, tstep = SPEC_FFT_N / m;
        for (unsigned j = 0U; j < q; ++j) {
            const unsigned t = j * tstep;
            /* W^j, W^2j, W^3j with W = exp(-2 pi i / m). */
            const float w1r = spec_cos512(t), w1i = -spec_cos512(t + 384U);
            const float w2r = spec_cos512(2U * t), w2i = -spec_cos512(2U * t + 384U);
            const float w3r = spec_cos512(3U * t), w3i = -spec_cos512(3U * t + 384U);
            for (unsigned b = j; b < SPEC_FFT_N; b += m) {
                float *p0 = &x[2U * b], *p1 = &x[2U * (b + q)];
                float *p2 = &x[2U * (b + 2U * q)], *p3 = &x[2U * (b + 3U * q)];
                const float ar = p0[0] + p2[0], ai = p0[1] + p2[1];
                const float cr = p0[0] - p2[0], ci = p0[1] - p2[1];
                const float br = p1[0] + p3[0], bi = p1[1] + p3[1];
                const float dr = p1[1] - p3[1], di = p3[0] - p1[0]; /* (x1 - x3)(-i) */
                const float er = ar - br, ei = ai - bi;
                const float fr = cr + dr, fi = ci + di;
                const float gr = cr - dr, gi = ci - di;
                p0[0] = ar + br;              p0[1] = ai + bi;
                p1[0] = er * w2r - ei * w2i;  p1[1] = er * w2i + ei * w2r;
                p2[0] = fr * w1r - fi * w1i;  p2[1] = fr * w1i + fi * w1r;
                p3[0] = gr * w3r - gi * w3i;  p3[1] = gr * w3i + gi * w3r;
            }
        }
    }
    for (unsigned b = 0U; b < 2U * SPEC_FFT_N; b += 4U) {
        const float ar = x[b], ai = x[b + 1U], br = x[b + 2U], bi = x[b + 3U];
        x[b] = ar + br; x[b + 1U] = ai + bi;
        x[b + 2U] = ar - br; x[b + 3U] = ai - bi;
    }
}

/* Generalised cosine windows, w = a0 - a1 c1 + a2 c2 - a3 c3 + a4 c4 with
 * ck = cos(2 pi k n / N): Hann, 4-term Blackman-Harris (-92 dB sidelobes),
 * 5-term flat top, rectangular. `db8` is 8 x 20 log10(N a0): a full-scale
 * complex tone's bin, which is 0 dBFS. */
typedef struct { float a[5]; int16_t db8; } spec_wdef_t;
static const spec_wdef_t spec_windows[4] = {
    {{0.5f, 0.5f, 0.0f, 0.0f, 0.0f}, 385},
    {{0.35875f, 0.48829f, 0.14128f, 0.01168f, 0.0f}, 362},
    {{0.21557895f, 0.41663158f, 0.277263158f, 0.083578947f, 0.006947368f}, 327},
    {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 433},
};

/* The window is symmetric, w[512 - n] = w[n], so a table of n = 0..256 is all
 * of it. Q15 (x 32767): the error that rounding leaves is ~-119 dB under a
 * full-scale tone, far below any window's own sidelobes. */
#define SPEC_WIN_HALF 257U
static inline void spec_window_table(const spec_wdef_t *w, int16_t *out)
{
    for (unsigned n = 0U; n < SPEC_WIN_HALF; ++n) {
        const float c1 = spec_cos512(n);
        const float c2 = 2.0f * c1 * c1 - 1.0f;
        const float c3 = 2.0f * c1 * c2 - c1;
        const float c4 = 2.0f * c1 * c3 - c2;
        const float v = w->a[0] - w->a[1] * c1 + w->a[2] * c2 - w->a[3] * c3 + w->a[4] * c4;
        const float s = v * 32767.0f;
        out[n] = (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
    }
}

/* Receive I/Q correction for the raw stream, the same model the front end
 * applies: Q negated when RX IQ INVERT is on, per-channel gain, and the
 * leading channel delayed by a fractional sample. The front end delays with
 * a two-tap interpolator (radio_iq_delay.h) and trims its residual at the
 * planned IF; across the WIDE display that interpolator drooped -- the
 * image of a strong signal stood 30 dB under it 20 kHz from the LO and 13 dB
 * at 50 kHz -- so here the delay is applied after the transform, exactly
 * at every bin (spec_iq_delay_bins). spec_prepare does the rest. */
typedef struct {
    float i_gain, q_gain, q_sign, skew;
    uint8_t delay, delay_q;
} spec_iq_t;

/* The window at sample n of the block, 0 past its end. */
static inline float spec_win_at(const int16_t *win, unsigned n)
{
    if (n >= SPEC_FFT_N) return 0.0f;
    return (float)win[n < SPEC_WIN_HALF ? n : SPEC_FFT_N - n] * (1.0f / 32767.0f);
}

/* Condition one block in place: raw streams lose their mean (the LO's own DC
 * and op-amp offsets) and get the I/Q gains and sign; every block is
 * windowed. The delay waits for spec_iq_delay_bins. */
static inline void spec_prepare(float *x, const spec_iq_t *iq, const int16_t *win)
{
    float mi = 0.0f, mq = 0.0f;
    if (iq) {
        for (unsigned n = 0U; n < SPEC_FFT_N; ++n) { mi += x[2U * n]; mq += x[2U * n + 1U]; }
        mi *= 1.0f / (float)SPEC_FFT_N;
        mq *= 1.0f / (float)SPEC_FFT_N;
    }
    /* The channel spec_iq_delay_bins delays is shifted in the block with
     * its window; windowed here with the window that far ahead, it ends up
     * under the same window as the other (linear interpolation of a window
     * this smooth errs by under 1e-4). */
    const uint8_t shift = (uint8_t)(iq && iq->delay);
    const float frac = shift ? iq->skew - (float)(unsigned)iq->skew : 0.0f;
    const unsigned whole = shift ? (unsigned)iq->skew : 0U;
    for (unsigned n = 0U; n < SPEC_FFT_N; ++n) {
        float i = x[2U * n], q = x[2U * n + 1U];
        if (iq) {
            i = (i - mi) * iq->i_gain;
            q = (q - mq) * iq->q_sign * iq->q_gain;
        }
        const float g = spec_win_at(win, n);
        float gd = g;
        if (shift) gd = (1.0f - frac) * spec_win_at(win, n + whole) + frac * spec_win_at(win, n + whole + 1U);
        x[2U * n] = i * ((shift && !iq->delay_q) ? gd : g);
        x[2U * n + 1U] = q * ((shift && iq->delay_q) ? gd : g);
    }
}

/* The leading channel's fractional delay, on a transformed raw block: each
 * bin pair k, -k is split into the I and Q spectra -- I = (X[k] + X*[-k])/2,
 * Q = (X[k] - X*[-k])/2i, both real signals -- the delayed one turned by
 * exp(-i 2 pi k skew / N), and the pair put back together. That is the
 * exact delay at every frequency, where an interpolator's droops; the
 * window under the delayed channel was set ahead for it (spec_prepare).
 * ~8,000 cycles a block, a third of the transform. DC has no delay; the
 * Nyquist bin is not drawn. */
static inline void spec_iq_delay_bins(float *x, const spec_iq_t *iq)
{
    if (!iq || !iq->delay) return;
    const float step = 6.28318530718f * iq->skew / (float)SPEC_FFT_N;
    const float sr = cosf(step), si = -sinf(step);
    float cr = 1.0f, ci = 0.0f; /* exp(-i k step), by rotation */
    for (unsigned k = 1U; k < SPEC_FFT_N / 2U; ++k) {
        const float nr = cr * sr - ci * si;
        ci = cr * si + ci * sr;
        cr = nr;
        float *a = &x[2U * spec_bitrev9(k)], *b = &x[2U * spec_bitrev9(SPEC_FFT_N - k)];
        const float ar = a[0], ai = a[1], br = b[0], bi = -b[1]; /* X[k], X*[-k] */
        float ir = 0.5f * (ar + br), ii = 0.5f * (ai + bi);
        float qr = 0.5f * (ai - bi), qi = -0.5f * (ar - br);
        if (iq->delay_q) {
            const float t = qr * cr - qi * ci;
            qi = qr * ci + qi * cr;
            qr = t;
        } else {
            const float t = ir * cr - ii * ci;
            ii = ir * ci + ii * cr;
            ir = t;
        }
        a[0] = ir - qi; a[1] = ii + qr;   /* I + iQ */
        b[0] = ir + qi; b[1] = qr - ii;   /* I* + iQ*, the mirror bin */
    }
}

/* |X[k]|^2 of a transformed block, k signed. */
static inline float spec_power(const float *x, int k)
{
    const unsigned p = spec_bitrev9((unsigned)k & (SPEC_FFT_N - 1U));
    const float re = x[2U * p], im = x[2U * p + 1U];
    return re * re + im * im;
}

/* 10 log10(p) in eighths of a dB, floored at -200 dB: exponent plus a cubic
 * for the mantissa's log2 (least squares on [1, 2), under 0.005 dB). */
#define SPEC_DB8_MIN (-1600)
static inline int16_t spec_db8(float p)
{
    if (!(p > 1.0e-20f)) return (int16_t)SPEC_DB8_MIN;
    uint32_t bits;
    memcpy(&bits, &p, 4U);
    const int32_t e = (int32_t)(bits >> 23) - 127;
    bits = (bits & 0x007FFFFFUL) | 0x3F800000UL;
    float m;
    memcpy(&m, &bits, 4U);
    const float l2 = (float)e + ((0.15391353f * m - 1.02949275f) * m + 3.01073029f) * m - 2.13381654f;
    const float d = l2 * 24.0823997f; /* 8 x 10 log10(2) */
    return d < (float)SPEC_DB8_MIN ? (int16_t)SPEC_DB8_MIN : (int16_t)d;
}

/* One of the 16 colour levels: `db8` placed on a scale `span8` eighths of a
 * dB tall starting at `bottom8`. */
static inline uint8_t spec_level(int32_t db8, int32_t bottom8, int32_t span8)
{
    const int32_t v = ((db8 - bottom8) * 16) / (span8 > 0 ? span8 : 1);
    return (uint8_t)(v < 0 ? 0 : (v > 15 ? 15 : v));
}

/* Half-band decimator by two, 31 taps (8 symmetric pairs around a 0.5 centre
 * tap; every other tap is zero). Equiripple: flat to 0.198 fS within 0.02 dB
 * and 59 dB down from 0.302 fS, where the passband edge folds back -- so a
 * 48 kHz stream decimated to 24 kHz is clean to +-9.5 kHz, and the same taps
 * a stage later to 12 kHz are clean to +-4.75 kHz. DC gain 0.9989.
 *
 * Polyphase: the pairs only ever meet odd-phase inputs and the centre only
 * even-phase ones, so the state is 16 odd and 8 even samples, and an output
 * costs 8 pair sums and 8 multiplies a channel. */
#define SPEC_HB_PAIRS 8U
typedef struct {
    float odd[2U * SPEC_HB_PAIRS][2];
    float even[SPEC_HB_PAIRS][2];
    uint8_t oi, ei, phase;
} spec_hb_t;

/* Push one sample; 1 with an output in *oi, *oq every second call. */
static inline uint8_t spec_hb_push(spec_hb_t *s, float i, float q, float *oi, float *oq)
{
    static const float g[SPEC_HB_PAIRS] = {
        3.155540824e-01f, -9.806861728e-02f, 5.100845918e-02f, -2.918700129e-02f,
        1.662125252e-02f, -8.922859095e-03f, 4.276326392e-03f, -1.823169063e-03f,
    };
    if (!s->phase) {
        s->phase = 1U;
        s->even[s->ei][0] = i;
        s->even[s->ei][1] = q;
        s->ei = (uint8_t)((s->ei + 1U == SPEC_HB_PAIRS) ? 0U : s->ei + 1U);
        return 0U;
    }
    s->phase = 0U;
    const unsigned n = 2U * SPEC_HB_PAIRS, p = s->oi;
    s->odd[p][0] = i;
    s->odd[p][1] = q;
    s->oi = (uint8_t)((p + 1U == n) ? 0U : p + 1U);
    /* Pair j is the odd samples 7 - j and 8 + j back from the newest (p):
     * the first walks forward, the second back, from the middle outwards. */
    unsigned a = (p >= SPEC_HB_PAIRS - 1U) ? p - (SPEC_HB_PAIRS - 1U) : p + SPEC_HB_PAIRS + 1U;
    unsigned b = a ? a - 1U : n - 1U;
    /* The centre tap is the oldest even sample: the one `ei` writes next. */
    float ri = 0.5f * s->even[s->ei][0], rq = 0.5f * s->even[s->ei][1];
    for (unsigned j = 0U; j < SPEC_HB_PAIRS; ++j) {
        ri += g[j] * (s->odd[a][0] + s->odd[b][0]);
        rq += g[j] * (s->odd[a][1] + s->odd[b][1]);
        a = (a + 1U == n) ? 0U : a + 1U;
        b = b ? b - 1U : n - 1U;
    }
    *oi = ri;
    *oq = rq;
    return 1U;
}

#endif

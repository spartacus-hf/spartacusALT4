#ifndef MOD_ANALYTIC_LOGIC_H
#define MOD_ANALYTIC_LOGIC_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
#define MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE 1
#endif

#if (MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE != 0) && \
    (MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE != 1)
#error "MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE must be 0 or 1"
#endif

#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
#define MOD_ANALYTIC12_COMPILED 1

/*
 * Production 48 -> 12 -> 48 ksps analytic-signal generator.
 *
 * The 79-tap low-pass is a unity-gain, 4.5 kHz Kaiser-window FIR at
 * 48 ksps.  It is used both before /4 decimation and, with the standard
 * x4 gain, as the polyphase reconstruction filter.  The 255-tap Type-III
 * Hilbert transformer runs at 12 ksps; antisymmetry reduces it to 64 MACs.
 * Coefficients are float32 design outputs and are shared verbatim with the
 * host spectral validation.
 */
#define MOD_ANALYTIC12_DECIMATION       4U
#define MOD_ANALYTIC12_LPF_TAPS         79U
#define MOD_ANALYTIC12_HILBERT_TAPS     255U
#define MOD_ANALYTIC12_HILBERT_DELAY    127U
#define MOD_ANALYTIC12_HILBERT_PAIRS    64U
#define MOD_ANALYTIC12_INTERP_HISTORY   20U

/* The decimator's and the interpolator's histories are written twice, at k
 * and k + their length, so their newest values always lie in one run (the
 * Hilbert pair's would cost another kilobyte). out_i/out_q: the block form's
 * four output phases, computed together when a low-rate sample arrives (a
 * state is run by one form only). */
typedef struct {
    float decim_hist[2U * MOD_ANALYTIC12_LPF_TAPS];
    float hilbert_hist[MOD_ANALYTIC12_HILBERT_TAPS];
    float interp_i_hist[2U * MOD_ANALYTIC12_INTERP_HISTORY];
    float interp_q_hist[2U * MOD_ANALYTIC12_INTERP_HISTORY];
    float out_i[MOD_ANALYTIC12_DECIMATION];
    float out_q[MOD_ANALYTIC12_DECIMATION];
    uint32_t decim_wr;
    uint32_t hilbert_wr;
    uint32_t interp_wr;
    uint8_t decim_phase;
} mod_analytic12_state_t;

static const float mod_analytic12_lpf[MOD_ANALYTIC12_LPF_TAPS] = {
    -4.024726150e-05f, -3.275992276e-05f,  2.641653555e-05f,  1.415902370e-04f,  2.773533924e-04f,
     3.566152009e-04f,  2.850551391e-04f, -4.905042997e-19f, -4.739895230e-04f, -9.922561003e-04f,
    -1.308862702e-03f, -1.159224659e-03f, -3.889702784e-04f,  9.196904721e-04f,  2.389507601e-03f,
     3.412101883e-03f,  3.346946556e-03f,  1.806896646e-03f, -1.074979431e-03f, -4.526412580e-03f,
    -7.264874876e-03f, -7.891870104e-03f, -5.457439926e-03f,  4.140990637e-18f,  7.173841354e-03f,
     1.365524344e-02f,  1.659264043e-02f,  1.370525360e-02f,  4.341143649e-03f, -9.810824879e-03f,
    -2.469126321e-02f, -3.466797993e-02f, -3.403167054e-02f, -1.879336312e-02f,  1.177201793e-02f,
     5.437340587e-02f,  1.020735726e-01f,  1.457739025e-01f,  1.764453799e-01f,  1.874768138e-01f,
     1.764453799e-01f,  1.457739025e-01f,  1.020735726e-01f,  5.437340587e-02f,  1.177201793e-02f,
    -1.879336312e-02f, -3.403167054e-02f, -3.466797993e-02f, -2.469126321e-02f, -9.810824879e-03f,
     4.341143649e-03f,  1.370525360e-02f,  1.659264043e-02f,  1.365524344e-02f,  7.173841354e-03f,
     4.140990637e-18f, -5.457439926e-03f, -7.891870104e-03f, -7.264874876e-03f, -4.526412580e-03f,
    -1.074979431e-03f,  1.806896646e-03f,  3.346946556e-03f,  3.412101883e-03f,  2.389507601e-03f,
     9.196904721e-04f, -3.889702784e-04f, -1.159224659e-03f, -1.308862702e-03f, -9.922561003e-04f,
    -4.739895230e-04f, -4.905042997e-19f,  2.850551391e-04f,  3.566152009e-04f,  2.773533924e-04f,
     1.415902370e-04f,  2.641653555e-05f, -3.275992276e-05f, -4.024726150e-05f
};

/* Non-zero coefficients h[0], h[2], ... h[126].  h[254-n] = -h[n]. */
static const float mod_analytic12_hilbert_half[MOD_ANALYTIC12_HILBERT_PAIRS] = {
    -7.455638115e-05f, -9.859786951e-05f, -1.263135200e-04f, -1.580257813e-04f, -1.940719085e-04f,
    -2.348042472e-04f, -2.805904369e-04f, -3.318137606e-04f, -3.888734500e-04f, -4.521851079e-04f,
    -5.221813335e-04f, -5.993121886e-04f, -6.840460701e-04f, -7.768705836e-04f, -8.782937075e-04f,
    -9.888450149e-04f, -1.109077246e-03f, -1.239568461e-03f, -1.380923786e-03f, -1.533778501e-03f,
    -1.698801061e-03f, -1.876696711e-03f, -2.068212023e-03f, -2.274139784e-03f, -2.495325869e-03f,
    -2.732675523e-03f, -2.987162676e-03f, -3.259839490e-03f, -3.551848931e-03f, -3.864438273e-03f,
    -4.198977258e-03f, -4.556977190e-03f, -4.940115381e-03f, -5.350266118e-03f, -5.789533723e-03f,
    -6.260296796e-03f, -6.765259895e-03f, -7.307520602e-03f, -7.890645415e-03f, -8.518773131e-03f,
    -9.196738712e-03f, -9.930229746e-03f, -1.072599273e-02f, -1.159209386e-02f, -1.253826451e-02f,
    -1.357635017e-02f, -1.472092699e-02f, -1.599013060e-02f, -1.740681566e-02f, -1.900017634e-02f,
    -2.080811933e-02f, -2.288074046e-02f, -2.528563701e-02f, -2.811624855e-02f, -3.150547296e-02f,
    -3.564886376e-02f, -4.084644094e-02f, -4.758303612e-02f, -5.669630319e-02f, -6.976879388e-02f,
    -9.019206464e-02f, -1.267847419e-01f, -2.118826956e-01f, -6.365117431e-01f
};

/* The tables as the block form reads them: a copy in RAM, where the flash's
 * wait states do not stall every tap. */
typedef struct {
    float lpf[MOD_ANALYTIC12_LPF_TAPS];
    float hilbert_half[MOD_ANALYTIC12_HILBERT_PAIRS];
} mod_analytic12_coeffs_t;

static inline void mod_analytic12_coeffs_load(mod_analytic12_coeffs_t *c)
{
    memcpy(c->lpf, mod_analytic12_lpf, sizeof(c->lpf));
    memcpy(c->hilbert_half, mod_analytic12_hilbert_half, sizeof(c->hilbert_half));
}

static inline uint32_t mod_analytic12_lag(uint32_t newest, uint32_t lag, uint32_t length)
{
    return (newest >= lag) ? (newest - lag) : (newest + length - lag);
}

static inline void mod_analytic12_reset(mod_analytic12_state_t *st)
{
    if (st != NULL) memset(st, 0, sizeof(*st));
}

static inline float mod_analytic12_decimate(mod_analytic12_state_t *st, uint32_t newest)
{
    float y = mod_analytic12_lpf[MOD_ANALYTIC12_LPF_TAPS / 2U]
            * st->decim_hist[mod_analytic12_lag(newest, MOD_ANALYTIC12_LPF_TAPS / 2U,
                                                MOD_ANALYTIC12_LPF_TAPS)];
    for (uint32_t n = 0U; n < MOD_ANALYTIC12_LPF_TAPS / 2U; n++) {
        float pair = st->decim_hist[mod_analytic12_lag(newest, n, MOD_ANALYTIC12_LPF_TAPS)]
                   + st->decim_hist[mod_analytic12_lag(newest,
                                                       MOD_ANALYTIC12_LPF_TAPS - 1U - n,
                                                       MOD_ANALYTIC12_LPF_TAPS)];
        y += mod_analytic12_lpf[n] * pair;
    }
    return y;
}

static inline void mod_analytic12_lowrate(mod_analytic12_state_t *st, float x,
                                           float sideband_sign)
{
    uint32_t newest = st->hilbert_wr;
    st->hilbert_hist[newest] = x;
    st->hilbert_wr++;
    if (st->hilbert_wr >= MOD_ANALYTIC12_HILBERT_TAPS) st->hilbert_wr = 0U;

    float q = 0.0f;
    for (uint32_t k = 0U; k < MOD_ANALYTIC12_HILBERT_PAIRS; k++) {
        uint32_t near_lag = 2U * k;
        uint32_t far_lag = MOD_ANALYTIC12_HILBERT_TAPS - 1U - near_lag;
        float pair = st->hilbert_hist[mod_analytic12_lag(newest, near_lag,
                                                        MOD_ANALYTIC12_HILBERT_TAPS)]
                   - st->hilbert_hist[mod_analytic12_lag(newest, far_lag,
                                                        MOD_ANALYTIC12_HILBERT_TAPS)];
        q += mod_analytic12_hilbert_half[k] * pair;
    }
    float d = st->hilbert_hist[mod_analytic12_lag(newest, MOD_ANALYTIC12_HILBERT_DELAY,
                                                  MOD_ANALYTIC12_HILBERT_TAPS)];
    float q_side = sideband_sign * q;
    float i_low = 0.7071067811865475244f * (d - q_side);
    float q_low = 0.7071067811865475244f * (d + q_side);

    newest = st->interp_wr;
    st->interp_i_hist[newest] = i_low;
    st->interp_q_hist[newest] = q_low;
    st->interp_i_hist[newest + MOD_ANALYTIC12_INTERP_HISTORY] = i_low;
    st->interp_q_hist[newest + MOD_ANALYTIC12_INTERP_HISTORY] = q_low;
    st->interp_wr++;
    if (st->interp_wr >= MOD_ANALYTIC12_INTERP_HISTORY) st->interp_wr = 0U;
}

static inline float mod_analytic12_interpolate(const float *hist, uint32_t newest,
                                                uint8_t phase)
{
    float y = 0.0f;
    uint32_t lag = 0U;
    for (uint32_t tap = phase; tap < MOD_ANALYTIC12_LPF_TAPS;
         tap += MOD_ANALYTIC12_DECIMATION) {
        y += mod_analytic12_lpf[tap]
           * hist[mod_analytic12_lag(newest, lag, MOD_ANALYTIC12_INTERP_HISTORY)];
        lag++;
    }
    return (float)MOD_ANALYTIC12_DECIMATION * y;
}

#if defined(__GNUC__)
#define MOD_ANALYTIC12_NOINLINE __attribute__((noinline, unused))
#else
#define MOD_ANALYTIC12_NOINLINE
#endif

/* One sample at a time: the reference form, which the host tests measure and
 * mod_analytic12_block must match bit for bit. The normal image runs only the
 * block form, hence `unused'. */
static MOD_ANALYTIC12_NOINLINE void mod_analytic12_process(mod_analytic12_state_t *st, float x,
                                                            float sideband_sign,
                                                            float *i_sig, float *q_sig)
{
    if (!isfinite(x)) x = 0.0f;
    if (!isfinite(sideband_sign)) sideband_sign = 1.0f;
    else sideband_sign = (sideband_sign < 0.0f) ? -1.0f : 1.0f;

    uint8_t phase = st->decim_phase;
    uint32_t newest = st->decim_wr;
    st->decim_hist[newest] = x;
    st->decim_hist[newest + MOD_ANALYTIC12_LPF_TAPS] = x;
    st->decim_wr++;
    if (st->decim_wr >= MOD_ANALYTIC12_LPF_TAPS) st->decim_wr = 0U;

    if (phase == 0U) {
        float low = mod_analytic12_decimate(st, newest);
        mod_analytic12_lowrate(st, low, sideband_sign);
    }

    newest = (st->interp_wr == 0U) ? (MOD_ANALYTIC12_INTERP_HISTORY - 1U)
                                   : (st->interp_wr - 1U);
    *i_sig = mod_analytic12_interpolate(st->interp_i_hist, newest, phase);
    *q_sig = mod_analytic12_interpolate(st->interp_q_hist, newest, phase);

    phase++;
    st->decim_phase = (phase < MOD_ANALYTIC12_DECIMATION) ? phase : 0U;
}

/*
 * The block form. Every output is the same sum, over the same terms in the
 * same order, as in mod_analytic12_process, so the two agree bit for bit;
 * only the circular histories' wrap is taken out of the inner loops, which
 * the per-sample form pays a compare a tap for: the mirrored histories are
 * read in one run, the Hilbert pair's in the stretches between wraps.
 * (2026-10-03: the per-sample form took ~1.6k cycles a sample on the radio,
 * a third of the CPU.)
 */
#if defined(__GNUC__) && !defined(__clang__)
#define MOD_ANALYTIC12_UNROLL _Pragma("GCC unroll 4")
#else
#define MOD_ANALYTIC12_UNROLL
#endif

static inline float mod_analytic12_decimate_run(const mod_analytic12_state_t *st,
                                                const mod_analytic12_coeffs_t *coeffs,
                                                uint32_t newest)
{
    /* Lag k of the newest is at newest + 79 - k in the mirrored history. */
    const float *h = &st->decim_hist[newest + MOD_ANALYTIC12_LPF_TAPS];
    const float *far = h - (MOD_ANALYTIC12_LPF_TAPS - 1U);
    float y = coeffs->lpf[MOD_ANALYTIC12_LPF_TAPS / 2U] * h[-(int32_t)(MOD_ANALYTIC12_LPF_TAPS / 2U)];
    MOD_ANALYTIC12_UNROLL
    for (uint32_t n = 0U; n < MOD_ANALYTIC12_LPF_TAPS / 2U; n++) {
        float pair = h[-(int32_t)n] + far[n];
        y += coeffs->lpf[n] * pair;
    }
    return y;
}

static inline void mod_analytic12_lowrate_run(mod_analytic12_state_t *st,
                                              const mod_analytic12_coeffs_t *coeffs,
                                              float x, float sideband_sign)
{
    uint32_t newest = st->hilbert_wr;
    st->hilbert_hist[newest] = x;
    st->hilbert_wr++;
    if (st->hilbert_wr >= MOD_ANALYTIC12_HILBERT_TAPS) st->hilbert_wr = 0U;

    /* Pair k: lag 2k (near) and lag 254 - 2k, at newest + 1 + 2k modulo 255
     * (far). Each run is the stretch before either index wraps. */
    const float *h = st->hilbert_hist;
    uint32_t near = newest;
    uint32_t far = (newest + 1U < MOD_ANALYTIC12_HILBERT_TAPS) ? (newest + 1U) : 0U;
    float q = 0.0f;
    uint32_t k = 0U;
    while (k < MOD_ANALYTIC12_HILBERT_PAIRS) {
        uint32_t run = MOD_ANALYTIC12_HILBERT_PAIRS - k;
        uint32_t near_run = near / 2U + 1U;
        uint32_t far_run = (MOD_ANALYTIC12_HILBERT_TAPS - 1U - far) / 2U + 1U;
        if (near_run < run) run = near_run;
        if (far_run < run) run = far_run;
        const float *hn = &h[near];
        const float *hf = &h[far];
        const float *c = &coeffs->hilbert_half[k];
        MOD_ANALYTIC12_UNROLL
        for (uint32_t j = 0U; j < run; j++) {
            float pair = hn[-(int32_t)(2U * j)] - hf[2U * j];
            q += c[j] * pair;
        }
        k += run;
        near = (near >= 2U * run) ? (near - 2U * run)
                                  : (near + MOD_ANALYTIC12_HILBERT_TAPS - 2U * run);
        far += 2U * run;
        if (far >= MOD_ANALYTIC12_HILBERT_TAPS) far -= MOD_ANALYTIC12_HILBERT_TAPS;
    }
    float d = h[mod_analytic12_lag(newest, MOD_ANALYTIC12_HILBERT_DELAY,
                                   MOD_ANALYTIC12_HILBERT_TAPS)];
    float q_side = sideband_sign * q;
    float i_low = 0.7071067811865475244f * (d - q_side);
    float q_low = 0.7071067811865475244f * (d + q_side);

    newest = st->interp_wr;
    st->interp_i_hist[newest] = i_low;
    st->interp_q_hist[newest] = q_low;
    st->interp_i_hist[newest + MOD_ANALYTIC12_INTERP_HISTORY] = i_low;
    st->interp_q_hist[newest + MOD_ANALYTIC12_INTERP_HISTORY] = q_low;
    st->interp_wr++;
    if (st->interp_wr >= MOD_ANALYTIC12_INTERP_HISTORY) st->interp_wr = 0U;
}

/* mod_analytic12_interpolate for all four phases of I and Q, which share the
 * history until the next low-rate sample: each history value is loaded once
 * for eight sums, each sum still taken lag by lag. Lag j of the newest is at
 * newest + 20 - j in the mirrored histories, and its coefficients in phases
 * 0-3 are lpf[4j] to lpf[4j + 3]; phase 3 has 19 taps. */
static inline void mod_analytic12_interpolate_phases(mod_analytic12_state_t *st,
                                                     const mod_analytic12_coeffs_t *coeffs,
                                                     uint32_t newest)
{
    const float *hi = &st->interp_i_hist[newest + MOD_ANALYTIC12_INTERP_HISTORY];
    const float *hq = &st->interp_q_hist[newest + MOD_ANALYTIC12_INTERP_HISTORY];
    float i0 = 0.0f, i1 = 0.0f, i2 = 0.0f, i3 = 0.0f;
    float q0 = 0.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;
    uint32_t j = 0U;
    for (; j < MOD_ANALYTIC12_INTERP_HISTORY - 1U; j++) {
        const float *c = &coeffs->lpf[MOD_ANALYTIC12_DECIMATION * j];
        const float a = hi[-(int32_t)j];
        const float b = hq[-(int32_t)j];
        i0 += c[0] * a; q0 += c[0] * b;
        i1 += c[1] * a; q1 += c[1] * b;
        i2 += c[2] * a; q2 += c[2] * b;
        i3 += c[3] * a; q3 += c[3] * b;
    }
    {
        const float *c = &coeffs->lpf[MOD_ANALYTIC12_DECIMATION * j];
        const float a = hi[-(int32_t)j];
        const float b = hq[-(int32_t)j];
        i0 += c[0] * a; q0 += c[0] * b;
        i1 += c[1] * a; q1 += c[1] * b;
        i2 += c[2] * a; q2 += c[2] * b;
    }
    const float g = (float)MOD_ANALYTIC12_DECIMATION;
    st->out_i[0] = g * i0; st->out_q[0] = g * q0;
    st->out_i[1] = g * i1; st->out_q[1] = g * q1;
    st->out_i[2] = g * i2; st->out_q[2] = g * q2;
    st->out_i[3] = g * i3; st->out_q[3] = g * q3;
}

/* n samples of x[] to i_sig[] and q_sig[]. x may be i_sig: each input is
 * read before its output is written. coeffs: mod_analytic12_coeffs_load. */
static inline void mod_analytic12_block(mod_analytic12_state_t *st,
                                        const mod_analytic12_coeffs_t *coeffs,
                                        const float *x, float *i_sig, float *q_sig,
                                        uint32_t n, float sideband_sign)
{
    if (!isfinite(sideband_sign)) sideband_sign = 1.0f;
    else sideband_sign = (sideband_sign < 0.0f) ? -1.0f : 1.0f;

    for (uint32_t s = 0U; s < n; s++) {
        float v = x[s];
        if (!isfinite(v)) v = 0.0f;

        uint8_t phase = st->decim_phase;
        uint32_t newest = st->decim_wr;
        st->decim_hist[newest] = v;
        st->decim_hist[newest + MOD_ANALYTIC12_LPF_TAPS] = v;
        st->decim_wr++;
        if (st->decim_wr >= MOD_ANALYTIC12_LPF_TAPS) st->decim_wr = 0U;

        if (phase == 0U) {
            float low = mod_analytic12_decimate_run(st, coeffs, newest);
            mod_analytic12_lowrate_run(st, coeffs, low, sideband_sign);
            newest = (st->interp_wr == 0U) ? (MOD_ANALYTIC12_INTERP_HISTORY - 1U)
                                           : (st->interp_wr - 1U);
            mod_analytic12_interpolate_phases(st, coeffs, newest);
        }
        i_sig[s] = st->out_i[phase];
        q_sig[s] = st->out_q[phase];

        phase++;
        st->decim_phase = (phase < MOD_ANALYTIC12_DECIMATION) ? phase : 0U;
    }
}

#undef MOD_ANALYTIC12_NOINLINE
#undef MOD_ANALYTIC12_UNROLL

#else
#define MOD_ANALYTIC12_COMPILED 0
#endif

#endif /* MOD_ANALYTIC_LOGIC_H */

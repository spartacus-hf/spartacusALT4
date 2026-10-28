#ifndef RADIO_DSP_H
#define RADIO_DSP_H
#include <math.h>
#include "radio_config.h"

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } radio_biquad_t;
/* CW AGC look-ahead: the audio is delayed this many 48 kHz samples behind the
 * envelope that sets its gain, so the gain has already come down when a
 * strong element's leading edge reaches the headphones (QMX: 1.3 ms). */
#define RADIO_DSP_CW_LOOKAHEAD 48U
typedef struct {
    radio_biquad_t lp[2][4];
    float dc_i, dc_q, agc, am_dc, fm_last_i, fm_last_q, fm_lp;
    float level;      /* channel power after filtering, before AGC (S-meter) */
    float last_power; /* the same power for the newest sample, un-averaged */
    /* The channel-filter output of the newest sample, before the BFO and the
     * AGC: complex baseband with the dial at DC. The CW decoder reads it. */
    float last_i, last_q;
    /* The newest demodulated sample before the AGC (and its clamp): the
     * USB capture's source in DIGI receive (RADIO_DSP_DIGI_USB_GAIN). */
    float raw;
    float tune_i, tune_q, tune_di, tune_dq;
    float beat_i, beat_q, beat_di, beat_dq;
    float rate;
    uint32_t normalize;
    uint8_t mode;
    /* Hang AGC (RADIO_DspSetCw, and RADIO_DspSetVoice for USB/LSB/AM): on
     * when hang_agc is set; fixed_gain replaces it when the voice AGC is OFF. */
    uint8_t hang_agc, fixed_agc;
    uint8_t digi;     /* USB/LSB with the DIGI passband (RADIO_DspSetVoice) */
    /* FM squelch (RADIO_DspSetVoice): channel power to open and to close,
     * and the audio gain slewing between them; open_power 0 = no squelch. */
    float sql_open, sql_close, sql_gain;
    float offset_hz;  /* the DSP NCO's offset from the dial (RADIO_DspInit) */
    uint8_t cw_qualified;
    uint16_t cw_delay_at;
    uint32_t cw_over, cw_hang, cw_hang_samples, cw_noise_samples;
    float cw_recover, cw_impulse_recover;
    float cw_delay[RADIO_DSP_CW_LOOKAHEAD];
} radio_dsp_t;

/* AGC: a peak follower that holds the demodulated audio at TARGET of full
 * scale, with at most MAX_GAIN, so a signal below TARGET / MAX_GAIN stays
 * linear. The maximum was 100 (40 dB): REV1's own channel noise sits near
 * -83 dBFS with no antenna, so the floor and every signal weaker than
 * -49 dBFS came out 26 dB or more under the AGC level and were barely
 * audible at full volume. 316 (50 dB) puts that floor ~20 dB under TARGET,
 * where an antenna's band noise brings the AGC into action. */
#define RADIO_DSP_AGC_TARGET 0.35f
#define RADIO_DSP_AGC_MAX_GAIN 316.0f
/* DIGI receive's USB audio: the demodulated signal at this fixed gain, no
 * AGC, as QMX's Digi mode sends it (its AGC works in CW only; Band config RF
 * gain 54 dB). On a 40 m EFHW the AGC's maximum gain put the band noise at
 * the USB output ~30 dB over QMX+'s and clipped 1.3 % of the samples with AGC
 * OFF; with the AGC it rode the gain on every strong station, the waterfall's
 * floor dropping 6 dB under the slot's signals and back in the gap, and
 * decoded 25 a slot to QMX+'s 30-32 (bench, 2026-10-02). 10 is 30 dB under
 * the maximum: the floor where QMX+ puts it, its ~40 dB of headroom above. */
#define RADIO_DSP_DIGI_USB_GAIN 10.0f
void RADIO_DspInit(radio_dsp_t *s, float rate, float offset_hz, radio_rx_mode_t mode);
/* CW only (no effect in other modes): the beat note in Hz, the channel filter
 * as an index into RADIO_CW_BW_TABLE, and RADIO_CW_AGC_SLOW/FAST. Call after
 * RADIO_DspInit, which sets the defaults. Out-of-range values take them too. */
void RADIO_DspSetCw(radio_dsp_t *s, uint16_t pitch_hz, uint8_t bw_index, uint8_t agc);
/* Voice modes: the SSB and AM receive widths as indices into
 * RADIO_SSB_BW_TABLE / RADIO_AM_BW_TABLE (RADIO_SSB_BW_DIGI: the DIGI
 * passband), the voice AGC (RADIO_RX_AGC_*) for USB/LSB/AM, and the FM
 * squelch level (0 off). No effect in CW. */
void RADIO_DspSetVoice(radio_dsp_t *s, uint8_t ssb_index, uint8_t am_index, uint8_t agc, uint8_t squelch);
/* Power at the channel-filter output for the sample RADIO_DspDemodulate just
 * produced, before the AGC and the BFO. A filter sweep has to measure this
 * rather than the demodulated audio: the AGC would flatten the very curve the
 * sweep is plotting. RADIO_DspLevelDbfs() is the ~10 ms average of it. */
static inline float RADIO_DspLastPower(const radio_dsp_t *s) { return s->last_power; }
float RADIO_DspDemodulate(radio_dsp_t *s, float i, float q);
/* Channel power relative to ADC full scale, floored at -150 dBFS. */
float RADIO_DspLevelDbfs(const radio_dsp_t *s);
float RADIO_DspSample(radio_dsp_t *s, const radio_config_t *c, float i, float q);
/* Inline: the receive worker calls it for every capture sample, and the
 * call cost more than the saturation (bench, v8.10). */
static inline int16_t RADIO_Saturate16(float x)
{
    if (!isfinite(x)) return 0;
    if (x >= 1.0f) return 32767;
    if (x <= -1.0f) return -32768;
    /* To nearest: a cast truncates toward zero, a dead zone two codes wide
     * around silence. VCVTR rounds by FPSCR (nearest); newlib's lrintf is
     * a call. */
#if defined(__arm__) && defined(__ARM_FP)
    float r;
    int32_t v;
    __asm__("vcvtr.s32.f32 %0, %1" : "=t"(r) : "t"(x * 32767.0f));
    __asm__("vmov %0, %1" : "=r"(v) : "t"(r));
    return (int16_t)v;
#else
    return (int16_t)lrintf(x * 32767.0f);
#endif
}
/* Headphone PWM renderer: 48 kHz audio to TIM2 compare codes, one per PWM
 * period (~295-380 kHz), linearly interpolated and quantized by a
 * second-order error-feedback shaper, y = x + (1 - z^-1)^2 e.
 *
 * The work is split by rate. RADIO_PhoneLevel runs once per 48 kHz sample:
 * the clamp (and its NaN test) and the scaling to codes. RADIO_PhoneRender
 * runs per PWM period and has no float compare in it at all -- on the
 * Cortex-M4F each one is a VCMP plus a VMRS that stalls the pipeline, and the
 * previous per-period quantizer paid four of them plus a clamp, about 68
 * cycles a period and ~17,500 per DMA half, enough to push the receive half
 * past its deadline whenever the two landed together:
 *   - the position between 48 kHz samples is a 0.32 fixed-point phase, so
 *     "a new sample is due" is the carry of an integer add;
 *   - the interpolated level advances by a per-period delta, recomputed
 *     exactly at every new sample;
 *   - the quantizer truncates. Its error then lies in (-1, 0] rather than
 *     [-0.5, 0.5]; the constant -1/2 mean is DC, which (1 - z^-1)^2 removes
 *     exactly, so the output mean is the input mean. With |e| < 1 the loop
 *     is bounded by construction, so the old overload reset is gone, and
 *     because every level is inside mid +- 0.9 * scale (0.08..0.92 of the
 *     period) and the shaper moves u by at most (-1, +2) codes, the code
 *     never reaches 0 or top - 1 and needs no clamp either. */
typedef struct { float mid, scale; } radio_shape_t;
static inline radio_shape_t RADIO_NoiseShapeScale(uint32_t top)
{
    radio_shape_t k;
    k.mid = 0.5f * (float)top;
    k.scale = (float)top * (120.0f / 256.0f);
    return k;
}
/* One 48 kHz sample (full scale +-1) as a PWM level in codes. Clamped to
 * +-0.90; NaN fails both tests and becomes silence. */
static inline float RADIO_PhoneLevel(float sample, const radio_shape_t *k)
{
    if (!(sample >= -0.90f)) sample = sample < 0.0f ? -0.90f : 0.0f;
    if (sample > 0.90f) sample = 0.90f;
    return k->mid + sample * k->scale;
}
typedef struct {
    float value, delta, last, e1, e2; /* level now, its per-period step, newest sample, shaper errors */
    uint32_t phase, step;             /* 0.32 position between 48 kHz samples, advance per period */
} radio_phone_t;
/* 48 kHz / PWM rate as 0.32 fixed point; timer_hz / top is the PWM rate. */
static inline uint32_t RADIO_PhoneStep(uint32_t top, uint32_t timer_hz)
{
    return timer_hz ? (uint32_t)((((uint64_t)48000U * top) << 32) / timer_hz) : 0U;
}
/* Idle at mid-scale: the state a silent renderer holds, and where it starts. */
static inline void RADIO_PhoneReset(radio_phone_t *p, const radio_shape_t *k)
{
    p->value = p->last = k->mid;
    p->delta = p->e1 = p->e2 = 0.0f;
}
/* How many 48 kHz levels the next n periods consume: one per phase carry. */
static inline unsigned RADIO_PhoneNeeds(const radio_phone_t *p, unsigned n)
{
    return (unsigned)(((uint64_t)p->phase + (uint64_t)n * p->step) >> 32);
}
/* Render n periods into `out`, both channels per word (left low, right
 * high), consuming exactly RADIO_PhoneNeeds(p, n) levels from `in`. */
static inline void RADIO_PhoneRender(radio_phone_t *p, const float *in, uint32_t *out, unsigned n)
{
    const uint32_t step = p->step;
    const float to_unit = 1.0f / 4294967296.0f;
    const float step_f = (float)step * to_unit;
    uint32_t phase = p->phase;
    float value = p->value, delta = p->delta, last = p->last, e1 = p->e1, e2 = p->e2;
    unsigned k = 0U;
    while (k < n) {
        /* Periods whose phase advance does not carry: one 32-bit divide per
         * 48 kHz sample instead of a carry test in every period. */
        unsigned steady = step ? (unsigned)((0xFFFFFFFFUL - phase) / step) : n;
        if (steady > n - k) steady = n - k;
        for (unsigned j = 0U; j < steady; ++j) {
            value += delta;
            const float u = value + e2 - e1 - e1;
            const int32_t code = (int32_t)u;
            e2 = e1;
            e1 = (float)code - u;
            out[k + j] = (uint32_t)code * 0x00010001UL;
        }
        k += steady;
        phase += steady * step;
        if (k == n) break;
        /* The period that carries: a new 48 kHz sample. */
        phase += step;
        const float level = *in++;
        const float slope = level - last;
        value = last + slope * ((float)phase * to_unit);
        delta = slope * step_f;
        last = level;
        const float u = value + e2 - e1 - e1;
        const int32_t code = (int32_t)u;
        e2 = e1;
        e1 = (float)code - u;
        out[k++] = (uint32_t)code * 0x00010001UL;
    }
    p->phase = phase;
    p->value = value; p->delta = delta; p->last = last;
    p->e1 = e1; p->e2 = e2;
}
/* Headphone TIM2 reload (448..575, ~295-380 kHz at 170 MHz) whose nearest
 * harmonic is farthest from rf_hz; REV1 has no RC reconstruction filter ahead
 * of the NE5532, so the PWM carrier reaches the receiver. */
uint32_t RADIO_PhonePwmReload(uint32_t timer_hz, uint32_t rf_hz);
/* Timer reload in [arr_min, arr_max] whose nearest PWM harmonic is farthest
 * from rf_hz (QRP Labs style switching-noise relocation); fallback if rf_hz
 * is unknown. */
uint32_t RADIO_PwmReloadAway(uint32_t timer_hz, uint32_t rf_hz, uint32_t arr_min,
                             uint32_t arr_max, uint32_t fallback);
/* I2S3 divider D for PCM1821 FSYNC = clock / (64 D), BCLK = 64 FSYNC. Slave
 * mode only accepts the listed 176.4/192 kHz family rates, which SYSCLK cannot
 * hit exactly: alternate = 0 gives the closest match, 1 the other family. */
uint32_t RADIO_I2sDivider(uint32_t clock_hz, uint8_t alternate);
uint32_t RADIO_I2sDivider96(uint32_t clock_hz, uint8_t alternate);
float RADIO_SineTurn(float turn);
/* RADIO_SineTurn's body, for the RF interrupt's per-sample envelope decode
 * (RADIO_EnvelopeFill), where the call itself cost as much as the
 * polynomial. Bounded argument oscillator: no large-argument libm reducer on
 * the realtime stack. Odd degree-9 polynomial on [-pi/2, pi/2]. */
static inline float RADIO_SineTurnInline(float turn)
{
    /* Inside (0, 1) floorf is 0 and the reduction changes nothing; skipping
     * it keeps a libm call off the RF envelope's per-sample path
     * (RADIO_EnvelopeFill, 0 to 0.5). Zero takes the full path, which
     * turns -0.0 into +0.0 exactly as before. */
    if (!((turn > 0.0f) && (turn < 1.0f))) {
        if (!isfinite(turn)) return 0.0f;
        turn -= floorf(turn);
    }
    float x = turn < 0.25f ? turn : turn < 0.75f ? 0.5f - turn : turn - 1.0f;
    x *= 6.28318530718f;
    float z = x * x;
    float result = x * (1.0f + z * (-1.0f/6.0f + z * (1.0f/120.0f + z * (-1.0f/5040.0f + z/362880.0f))));
    return result > 1.0f ? 1.0f : (result < -1.0f ? -1.0f : result);
}
#endif

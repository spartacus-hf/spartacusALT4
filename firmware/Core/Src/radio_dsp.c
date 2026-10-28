#include "radio_dsp.h"
#include "math_pos.h"
#include <math.h>
#include <string.h>

static float biquad(radio_biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

#include "radio_rx_channel.inc"

/* The same real sections on I and Q, state cleared. */
static void load_sections(radio_dsp_t *s, const float (*rows)[5])
{
    for (unsigned n = 0U; n < 4U; ++n) {
        const float *k = rows[n];
        for (unsigned ch = 0U; ch < 2U; ++ch) {
            radio_biquad_t *f = &s->lp[ch][n];
            f->b0 = k[0]; f->b1 = k[1]; f->b2 = k[2]; f->a1 = k[3]; f->a2 = k[4];
            f->z1 = f->z2 = 0.0f;
        }
    }
}

__attribute__((cold)) void RADIO_DspInit(radio_dsp_t *s, float rate, float offset_hz, radio_rx_mode_t mode)
{
    memset(s, 0, sizeof(*s));
    if (!isfinite(rate) || rate < 32000.0f || rate > 96000.0f) rate = 48000.0f;
    if (!isfinite(offset_hz)) offset_hz = 0.0f;
    s->rate = rate; s->mode = mode; s->agc = 1.0f; s->offset_hz = offset_hz;
    float center = mode == RADIO_RX_USB ? 1650.0f : mode == RADIO_RX_LSB ? -1650.0f : 0.0f;
    float beat = mode == RADIO_RX_CW ? (float)RADIO_CW_PITCH_DEFAULT_HZ : center;
    float mix = 6.28318530718f * (offset_hz + center) / rate;
    s->tune_i = s->beat_i = 1.0f;
    s->tune_di = cosf(mix); s->tune_dq = -sinf(mix);
    s->beat_di = cosf(6.28318530718f * beat / rate);
    s->beat_dq = sinf(6.28318530718f * beat / rate);
    /* 8th-order elliptic channel filter (0.1 dB ripple, 80 dB stop band) on
     * I and Q: the opposite sideband 300 Hz away is >= 80 dB down, versus
     * about 60 dB in QMX and 19 dB for the previous 6th-order Butterworth.
     * Designed for 48 kHz, the only rate the receiver produces. */
    unsigned table = (unsigned)mode <= (unsigned)RADIO_RX_FM ? (unsigned)mode : (unsigned)RADIO_RX_USB;
    load_sections(s, rx_channel_sos[table]);
    if (mode == RADIO_RX_CW) RADIO_DspSetCw(s, RADIO_CW_PITCH_DEFAULT_HZ, 0xFFU, RADIO_CW_AGC_SLOW);
    else RADIO_DspSetVoice(s, 0xFFU, 0xFFU, RADIO_RX_AGC_DEFAULT, 0U);
}

/* CW hang AGC timing, per mode. SLOW holds the gain 250 ms -- through the
 * character gaps of anything from 15 WPM up -- and then recovers at 15 dB/s,
 * so a word gap lifts band noise by a few dB rather than the ~17 dB per dit
 * gap the linear-domain release gave. FAST holds 80 ms (inside a character
 * at 15 WPM) and recovers at 60 dB/s, for a weak reply after a strong call.
 * A peak shorter than 4 ms (static, a click) is let through the look-ahead
 * limiter without starting the hang, and is undone at 300 dB/s. These sit in
 * the ranges QMX operators settled on (hang 40-110 ms, 10-60 dB/s; QRP Labs
 * forum, "Best QMX AGC settings?", 2025-01). */
#define CW_AGC_SLOW_HANG_MS      250.0f
#define CW_AGC_SLOW_RECOVER_DBS  15.0f
#define CW_AGC_FAST_HANG_MS      80.0f
#define CW_AGC_FAST_RECOVER_DBS  60.0f
#define CW_AGC_NOISE_MS          4.0f
#define CW_AGC_IMPULSE_DBS       300.0f
/* About 0.4 ms: the look-ahead is 1 ms, so the gain has settled before the
 * edge it reacts to is heard. */
#define CW_AGC_ATTACK            0.05f

__attribute__((cold)) void RADIO_DspSetCw(radio_dsp_t *s, uint16_t pitch_hz, uint8_t bw_index, uint8_t agc)
{
    static const uint16_t bandwidths[RADIO_CW_BW_COUNT] = RADIO_CW_BW_TABLE;
    if (s->mode != RADIO_RX_CW) return;
    if (pitch_hz < RADIO_CW_PITCH_MIN_HZ || pitch_hz > RADIO_CW_PITCH_MAX_HZ) pitch_hz = RADIO_CW_PITCH_DEFAULT_HZ;
    if (bw_index >= RADIO_CW_BW_COUNT) {
        bw_index = 0U;
        while (bw_index + 1U < RADIO_CW_BW_COUNT && bandwidths[bw_index] != RADIO_CW_BW_DEFAULT_HZ) ++bw_index;
    }
    s->beat_di = cosf(6.28318530718f * (float)pitch_hz / s->rate);
    s->beat_dq = sinf(6.28318530718f * (float)pitch_hz / s->rate);
    load_sections(s, rx_cw_sos[bw_index]);
    const uint8_t fast = agc == RADIO_CW_AGC_FAST;
    const float ms = s->rate * 0.001f;
    s->cw_hang_samples = (uint32_t)((fast ? CW_AGC_FAST_HANG_MS : CW_AGC_SLOW_HANG_MS) * ms);
    s->cw_noise_samples = (uint32_t)(CW_AGC_NOISE_MS * ms);
    s->cw_recover = powf_pos(10.0f, (fast ? CW_AGC_FAST_RECOVER_DBS : CW_AGC_SLOW_RECOVER_DBS) / (20.0f * s->rate));
    s->cw_impulse_recover = powf_pos(10.0f, CW_AGC_IMPULSE_DBS / (20.0f * s->rate));
    s->hang_agc = 1U;
}

/* Voice AGC speeds: hang ms and recovery dB/s, for SLOW, MED and FAST. The
 * peak follower it replaces released exponentially over ~260 ms with no hang,
 * so band noise pumped up between syllables and words; QMX operators run
 * hang 40-110 ms in CW and longer in SSB (QRP Labs forum, 2025-01). */
static const float voice_agc[3][2] = { {500.0f, 10.0f}, {250.0f, 20.0f}, {100.0f, 40.0f} };

__attribute__((cold)) void RADIO_DspSetVoice(radio_dsp_t *s, uint8_t ssb_index, uint8_t am_index,
                                             uint8_t agc, uint8_t squelch)
{
    if (s->mode == RADIO_RX_CW) return;
    if (s->mode == RADIO_RX_USB || s->mode == RADIO_RX_LSB) {
        const uint8_t digi = (uint8_t)(ssb_index == RADIO_SSB_BW_DIGI);
        if (!digi && ssb_index >= RADIO_SSB_BW_COUNT) ssb_index = 3U; /* 2700 Hz */
        const float width = digi ? (float)(RADIO_DIGI_RX_HIGH_HZ - RADIO_DIGI_RX_LOW_HZ)
                                 : (float)RADIO_SsbBandwidthAt(ssb_index);
        const float centre = (digi ? (float)RADIO_DIGI_RX_LOW_HZ : (float)RADIO_SSB_LOW_CUT_HZ) + 0.5f * width;
        const float mix = 6.28318530718f * (s->offset_hz + (s->mode == RADIO_RX_USB ? centre : -centre)) / s->rate;
        s->tune_di = cosf(mix); s->tune_dq = -sinf(mix);
        s->beat_di = cosf(6.28318530718f * (s->mode == RADIO_RX_USB ? centre : -centre) / s->rate);
        s->beat_dq = sinf(6.28318530718f * (s->mode == RADIO_RX_USB ? centre : -centre) / s->rate);
        load_sections(s, digi ? rx_digi_sos : rx_ssb_sos[ssb_index]);
        s->digi = digi;
    } else if (s->mode == RADIO_RX_AM) {
        load_sections(s, rx_am_sos[am_index < RADIO_AM_BW_COUNT ? am_index : RADIO_AM_BW_COUNT - 1U]);
    }
    s->hang_agc = 0U;
    s->fixed_agc = 0U;
    if (s->mode != RADIO_RX_FM) {
        if (agc == RADIO_RX_AGC_OFF) {
            s->fixed_agc = 1U;
        } else {
            const unsigned k = (agc == RADIO_RX_AGC_SLOW) ? 0U : (agc == RADIO_RX_AGC_FAST) ? 2U : 1U;
            const float ms = s->rate * 0.001f;
            s->cw_hang_samples = (uint32_t)(voice_agc[k][0] * ms);
            s->cw_noise_samples = (uint32_t)(CW_AGC_NOISE_MS * ms);
            s->cw_recover = powf_pos(10.0f, voice_agc[k][1] / (20.0f * s->rate));
            s->cw_impulse_recover = powf_pos(10.0f, CW_AGC_IMPULSE_DBS / (20.0f * s->rate));
            s->hang_agc = 1U;
        }
    }
    s->sql_open = s->sql_close = 0.0f;
    s->sql_gain = 1.0f;
    if (s->mode == RADIO_RX_FM && squelch > 0U && squelch <= RADIO_FM_SQUELCH_MAX) {
        s->sql_open = powf_pos(10.0f, RADIO_FM_SQUELCH_DB(squelch) / 10.0f);
        s->sql_close = 0.5f * s->sql_open; /* 3 dB of hysteresis */
        s->sql_gain = 0.0f;                /* closed until a carrier opens it */
    }
}

/* CW AGC: a hang AGC on the channel envelope. The peak follower the other
 * modes use tracks the rectified audio and releases exponentially in the
 * linear domain, which from a strong signal's gain climbs back ~17 dB inside a
 * 60 ms dit gap -- band noise swelling between every element. Here the level
 * is the complex envelope |i + jq| (no audio-rate ripple), the audio runs
 * RADIO_DSP_CW_LOOKAHEAD samples behind it, the gain holds for the hang time
 * after the signal last set it, then recovers at a fixed dB rate. */
static float cw_agc(radio_dsp_t *s, float audio, float power)
{
    const float envelope = sqrtf(power);
    const float floor = RADIO_DSP_AGC_TARGET / RADIO_DSP_AGC_MAX_GAIN;
    float agc = s->agc;
    const float limit = RADIO_DSP_AGC_TARGET / (envelope > floor ? envelope : floor);
    if (limit < agc) agc += CW_AGC_ATTACK * (limit - agc);
    /* The signal is what sets the gain while it stays within 2 dB of it. */
    if (envelope * agc > 0.8f * RADIO_DSP_AGC_TARGET) {
        if (s->cw_over < s->cw_noise_samples) ++s->cw_over;
        else { s->cw_hang = s->cw_hang_samples; s->cw_qualified = 1U; }
    } else {
        s->cw_over = 0U;
        if (s->cw_hang) --s->cw_hang;
        else if (agc < limit) {
            agc *= s->cw_qualified ? s->cw_recover : s->cw_impulse_recover;
            if (agc >= limit) { agc = limit; s->cw_qualified = 0U; }
        }
    }
    s->agc = agc;
    const unsigned at = s->cw_delay_at;
    const float delayed = s->cw_delay[at];
    s->cw_delay[at] = audio;
    s->cw_delay_at = (uint16_t)(at + 1U < RADIO_DSP_CW_LOOKAHEAD ? at + 1U : 0U);
    const float out = delayed * agc;
    return out > 1.0f ? 1.0f : (out < -1.0f ? -1.0f : out);
}

/* FM squelch on the channel power's 10 ms average: opens above sql_open,
 * closes below sql_close, and slews the audio over ~5 ms either way so it
 * neither clicks nor chops the first syllable. */
static float fm_squelch(radio_dsp_t *s, float out)
{
    if (s->level >= s->sql_open) { if (s->sql_gain < 1.0f) s->sql_gain += 1.0f / 240.0f; }
    else if (s->level < s->sql_close && s->sql_gain > 0.0f) s->sql_gain -= 1.0f / 240.0f;
    if (s->sql_gain < 0.0f) s->sql_gain = 0.0f;
    if (s->sql_gain > 1.0f) s->sql_gain = 1.0f;
    return out * s->sql_gain;
}

/* atan2 for the FM discriminator: Abramowitz & Stegun 4.4.49 on the octant
 * (|error| <= 1e-5 rad), one division, no library call. newlib's atan2f, one
 * per 48 kHz output, cost ~5k cycles of a ~62k receive half; with v8.08's
 * receive worker (~44k) and the headphones (~9.3k) FM ran out of time and
 * missed 25-75 halves a second, each a receiver restart (bench, 2026-10-02;
 * USB 0). The caller has ruled out (0, 0). */
static inline float fm_atan2(float y, float x)
{
    const float ax = fabsf(x), ay = fabsf(y);
    const uint8_t steep = (uint8_t)(ay > ax);
    const float t = steep ? ax / ay : ay / ax, t2 = t * t;
    float r = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
    if (steep) r = 1.57079633f - r;
    if (x < 0.0f) r = 3.14159265f - r;
    return (y < 0.0f) ? -r : r;
}

static void oscillator(float *i, float *q, float di, float dq)
{
    float ni = *i * di - *q * dq;
    *q = *i * dq + *q * di;
    *i = ni;
}

float RADIO_DspSample(radio_dsp_t *s, const radio_config_t *c, float i, float q)
{
    if (!isfinite(i) || !isfinite(q)) return 0.0f;
    s->dc_i += 0.0002f * (i - s->dc_i);
    s->dc_q += 0.0002f * (q - s->dc_q);
    i = (i - s->dc_i) * c->rx_i_gain;
    q = (q - s->dc_q) * c->rx_q_gain;
    return RADIO_DspDemodulate(s, i, q);
}

float RADIO_DspDemodulate(radio_dsp_t *s, float i, float q)
{
    if (!isfinite(i) || !isfinite(q)) { s->raw = 0.0f; return 0.0f; }
    float ri = i * s->tune_i - q * s->tune_q;
    q = i * s->tune_q + q * s->tune_i;
    i = ri;
    oscillator(&s->tune_i, &s->tune_q, s->tune_di, s->tune_dq);
    for (unsigned n = 0U; n < 4U; ++n) {
        i = biquad(&s->lp[0][n], i);
        q = biquad(&s->lp[1][n], q);
    }
    const float power = i * i + q * q;
    s->last_power = power;
    s->last_i = i;
    s->last_q = q;
    s->level += 0.002f * (power - s->level); /* ~10 ms at 48 kHz */
    float out;
    if (s->mode == RADIO_RX_AM) {
        float magnitude = sqrtf(i * i + q * q);
        s->am_dc += 0.0002f * (magnitude - s->am_dc);
        out = magnitude - s->am_dc;
    } else if (s->mode == RADIO_RX_FM) {
        float cross = q * s->fm_last_i - i * s->fm_last_q;
        float dot = i * s->fm_last_i + q * s->fm_last_q;
        out = (dot * dot + cross * cross > 1e-16f) ? fm_atan2(cross, dot) * (s->rate / 31415.92654f) : 0.0f;
        s->fm_last_i = i; s->fm_last_q = q;
        s->fm_lp += (1.0f / (1.0f + 0.000075f * s->rate)) * (out - s->fm_lp);
        out = s->fm_lp;
    } else {
        out = i * s->beat_i - q * s->beat_q;
    }
    oscillator(&s->beat_i, &s->beat_q, s->beat_di, s->beat_dq);
    if (s->sql_open > 0.0f) out = fm_squelch(s, out);
    s->raw = out;
    /* Once in 256 samples: kept out of the per-sample straight line (with
     * -fno-math-errno the inline sqrt no longer marks it cold). */
    if (__builtin_expect(++s->normalize >= 256U, 0)) {
        float norm = 1.0f / sqrtf(s->tune_i * s->tune_i + s->tune_q * s->tune_q);
        s->tune_i *= norm; s->tune_q *= norm;
        norm = 1.0f / sqrtf(s->beat_i * s->beat_i + s->beat_q * s->beat_q);
        s->beat_i *= norm; s->beat_q *= norm; s->normalize = 0U;
    }
    if (s->hang_agc) return cw_agc(s, out, power);
    if (s->fixed_agc) {
        /* AGC OFF: the whole receive gain, the clamp the only limit. */
        out *= RADIO_DSP_AGC_MAX_GAIN;
        return out > 1.0f ? 1.0f : (out < -1.0f ? -1.0f : out);
    }
    /* Plain comparisons, not fminf/fmaxf: Cortex-M4 has no VMAXNM, so those
     * are library calls, and four of them per 48 kHz output sample cost more
     * than the AGC itself. The inputs are NaN-checked on entry, so the only
     * difference between these and the libm versions is speed. */
    float magnitude = fabsf(out);
    if (magnitude < RADIO_DSP_AGC_TARGET / RADIO_DSP_AGC_MAX_GAIN)
        magnitude = RADIO_DSP_AGC_TARGET / RADIO_DSP_AGC_MAX_GAIN;
    const float target = RADIO_DSP_AGC_TARGET / magnitude; /* <= the maximum gain */
    float speed = target < s->agc ? 0.02f : 0.00008f;
    s->agc += speed * (target - s->agc);
    out *= s->agc;
    return out > 1.0f ? 1.0f : (out < -1.0f ? -1.0f : out);
}

float RADIO_DspLevelDbfs(const radio_dsp_t *s)
{
    float p = s->level;
    return (isfinite(p) && p > 1e-15f) ? 10.0f * log10f(p) : -150.0f;
}

uint32_t RADIO_PwmReloadAway(uint32_t timer_hz, uint32_t rf_hz, uint32_t arr_min,
                             uint32_t arr_max, uint32_t fallback)
{
    uint32_t best = fallback, best_gap = 0U;
    if (!timer_hz || !rf_hz) return best;
    for (uint32_t arr = arr_min; arr <= arr_max; ++arr) {
        uint32_t pwm = timer_hz / (arr + 1U);
        uint32_t harmonic = (rf_hz + pwm / 2U) / pwm * pwm;
        uint32_t gap = harmonic > rf_hz ? harmonic - rf_hz : rf_hz - harmonic;
        if (gap > best_gap) { best_gap = gap; best = arr; }
    }
    return best;
}

__attribute__((cold)) uint32_t RADIO_PhonePwmReload(uint32_t timer_hz, uint32_t rf_hz)
{
    return RADIO_PwmReloadAway(timer_hz, rf_hz, 448U, 575U, 511U);
}

/* The PCM1820/1821 near 96 kHz (radio_rx_front.c: it repeats samples at
 * 162-190 kHz and is clean here); `alternate` takes the 88.2 kHz family. */
uint32_t RADIO_I2sDivider96(uint32_t clock_hz, uint8_t alternate)
{
    const uint32_t f = alternate ? 88200UL : 96000UL;
    return (clock_hz + 32UL * f) / (64UL * f);
}

__attribute__((cold)) uint32_t RADIO_I2sDivider(uint32_t clock_hz, uint8_t alternate)
{
    if (clock_hz < 64UL * 4UL * 176400UL) return 4U;
    /* 176.4 kHz family: nearest divider. 192 kHz family: never above the
     * PCM1821 192 kHz maximum, so round the divider up. */
    uint32_t d176 = (clock_hz + 64UL * 88200UL) / (64UL * 176400UL);
    uint32_t d192 = (clock_hz + 64UL * 192000UL - 1UL) / (64UL * 192000UL);
    if (d192 < 4U) d192 = 4U;
    if (d176 < d192) d176 = d192;
    uint32_t f176 = clock_hz / (64UL * d176), f192 = clock_hz / (64UL * d192);
    uint32_t e176 = f176 > 176400UL ? f176 - 176400UL : 176400UL - f176;
    uint32_t e192 = f192 > 192000UL ? f192 - 192000UL : 192000UL - f192;
    /* Compare relative errors: e176 / 176400 against e192 / 192000. */
    uint8_t prefer176 = (uint64_t)e176 * 192000ULL <= (uint64_t)e192 * 176400ULL;
    return (prefer176 != (alternate != 0U)) ? d176 : d192;
}

/* Headphone oscillator; the body is RADIO_SineTurnInline (radio_dsp.h). */
float RADIO_SineTurn(float turn)
{
    return RADIO_SineTurnInline(turn);
}

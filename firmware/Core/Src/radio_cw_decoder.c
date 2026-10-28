/* Main loop, one 1 ms sample at a time (~0.3 % of the core): built for
 * size. */
#pragma GCC optimize ("Os")

#include "radio_cw_decoder.h"
#include "mod_morse_tables.h"
#include <math.h>
#include <string.h>

/* See radio_cw_decoder.h for the structure. Every constant below was chosen
 * against host suite cw_decoder (tools/tests/cw_decoder_test.c), which runs
 * this file on keyed signals through the production CW channel filter. */

#define TWO_PI          6.28318530718f
#define RATE_HZ         1000.0f
#define BIN_STEP_HZ     10.0f
#define HOP_MS          32U
#define SPEC_SMOOTH     0.15f   /* per hop: ~210 ms */
/* Hops averaged before a peak may lock: the first few smoothed spectra of
 * noise alone often have one bin 6 dB over the median. */
#define SPEC_WARMUP     8U
#define LOCK_ON         4.0f    /* smoothed peak / median bin: 6 dB */
#define LOCK_OFF        2.5f
/* The lock outlasts the smoothed peak by this much: through a Farnsworth
 * word gap (3.7 s at 18/5 WPM, W1AW's slowest practice) and a short pause. */
#define LOCK_HOLD_MS    5000U
/* A lock regained this close to the last one, this soon, is the same
 * station after a pause (a Farnsworth word gap, a deep fade): it keeps its
 * speed and its place in the text. */
#define RELOCK_HZ       15.0f
#define RELOCK_MS       10000U
/* Silence before the keyed level starts to relax toward the noise. */
#ifndef FADE_RELAX
#define FADE_RELAX      1
#endif
#ifndef FADE_DITS
#define FADE_DITS       4.0f
#endif
#ifndef SIG_HOLD_MS
#define SIG_HOLD_MS     4000U
#endif
/* Spectral peak over the median that still counts as the station being there. */
#ifndef SIG_CARRIER
#define SIG_CARRIER     LOCK_OFF
#endif
/* 30 WPM until the first marks say otherwise. The integrator and the
 * debounce scale with the dit, and windows sized for a slow sender swallow a
 * fast one's gaps whole -- its elements merge into long marks and it reads
 * as slower still. Short windows see fast code, and slow code still splits
 * into dits and dahs, so the estimate starts fast and grows. */
#define DIT_START_MS    40.0f
#define DIT_MIN_MS      15.0f   /* 80 WPM */
#define DIT_MAX_MS      300.0f  /* 4 WPM */
/* Of the way from the unkeyed level to the keyed plateau. The integrator
 * turns an element into a trapezoid whose ramps last the integration time;
 * only crossings at the same height on both ramps -- half way -- give back
 * the element's true length, so the hysteresis sits symmetrically on 0.5. */
#ifndef THRESH_ON
#define THRESH_ON       0.50f
#endif
#ifndef THRESH_OFF
#define THRESH_OFF      0.40f
#endif
#ifndef SQUELCH_RATIO
#define SQUELCH_RATIO   1.8f    /* keyed / unkeyed envelope: 5 dB */
#endif
/* Integration of 0.9 dit is close to a matched filter for a dit and still
 * leaves a one-dit gap visible; debounce 0.3 dit. With the thresholds above
 * these gave the lowest error rates of the host sweep (MA 0.5-1.1, debounce
 * 0.2-0.45, thresholds 0.55/0.45 and 0.5/0.4; tools/tests/cw_decoder_test.c
 * --score). Both count in units (unit_ms), a dit for a keyer: sized to a
 * weighted fist's longer dits they bridged its shorter spaces, elements ran
 * together and the speed read ever slower. */
#ifndef MA_DITS
#define MA_DITS         0.9f    /* coherent integration, in units */
#endif
#ifndef PERSIST_DITS
#define PERSIST_DITS    0.3f    /* debounce, in units */
#endif
/* The keyed level takes this share of each mark's plateau: quickly up, so
 * a new stronger station is followed at once, and almost as quickly down,
 * so it keeps up with QSB (+-6 dB at 0.3 Hz fades about 11 dB/s). */
#ifndef SIG_RISE
#define SIG_RISE        0.6f
#endif
#ifndef SIG_FALL
#define SIG_FALL        0.5f
#endif
/* Silence after a mark's (half-height) end before the envelope one
 * integration length back is noise alone: the rest of the falling ramp, the
 * delay itself and 4 ms for the channel filter's ringing. */
#ifndef NOISE_CLEAR
#define NOISE_CLEAR(len) ((uint32_t)(len) + (uint32_t)(len) / 2U + 4U)
#endif

/* The history and the envelope record are kept as bfloat16: the receive
 * worker hands the samples over that way already (radio_audio.c cwdec_bf16),
 * so the history loses nothing, and the envelope's 8-bit mantissa is far
 * finer than any level decision made from it. */
static inline uint16_t to_bf16(float x)
{
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    u += 0x7FFFU + ((u >> 16) & 1U); /* to nearest, ties to even */
    return (uint16_t)(u >> 16);
}

static inline float from_bf16(uint32_t h)
{
    const uint32_t u = h << 16;
    float x;
    memcpy(&x, &u, sizeof(x));
    return x;
}

static uint8_t ma_length(float unit)
{
    float len = unit * MA_DITS;
    if (len < 4.0f) len = 4.0f;
    if (len > (float)RADIO_CWDEC_MA_MAX) len = (float)RADIO_CWDEC_MA_MAX;
    return (uint8_t)(len + 0.5f);
}

/* The unit gaps are counted in: half a dit and the space after it. Weighting
 * lengthens every mark and shortens every space by the same amount, so the
 * character gap of a heavy fist (3 dits less its weight) can be under two of
 * its dits while it is still well over two units. */
static float unit_ms(const radio_cwdec_t *d) { return 0.5f * (d->dit_ms + d->space_ms); }

/* Marks end where the falling ramp crosses THRESH_OFF, not half way: every
 * mark reads (THRESH_ON - THRESH_OFF) of an integration length long and every
 * gap as much short. Timing takes it back; the edges themselves stay where
 * they were seen, for the level estimates that are dated from them. */
static float ramp_ms(const radio_cwdec_t *d) { return (THRESH_ON - THRESH_OFF) * (float)d->ma_len; }

static void log_interval(radio_cwdec_t *d, uint8_t mark, float ms);

static void set_nco(radio_cwdec_t *d)
{
    const float w = -TWO_PI * d->freq_hz / RATE_HZ;
    d->nco_di = cosf(w);
    d->nco_dq = sinf(w);
}

static void detector_reset(radio_cwdec_t *d)
{
    memset(d->ma, 0, sizeof(d->ma));
    d->ma_i = d->ma_q = 0.0f;
    d->ma_at = d->ma_fill = 0U;
    d->ma_len = ma_length(d->dit_ms);
}

/* The integrator keeps the last RADIO_CWDEC_MA_MAX samples whatever its
 * length, so a new speed resizes it by summing the newest `len` again:
 * no gap in the envelope while the speed settles. */
static void detector_resize(radio_cwdec_t *d, uint8_t len)
{
    float si = 0.0f, sq = 0.0f;
    unsigned at = d->ma_at;
    for (unsigned n = 0U; n < len; ++n) {
        at = at ? at - 1U : RADIO_CWDEC_MA_MAX - 1U;
        si += d->ma[at][0];
        sq += d->ma[at][1];
    }
    d->ma_i = si;
    d->ma_q = sq;
    d->ma_len = len;
}

/* A new station may send at another speed and spacing: find them again,
 * from fast, and print nothing until its speed is confirmed. */
static void forget_station(radio_cwdec_t *d)
{
    d->speed_ok = 0U;
    d->lock_mark = 0U;
    d->log_n = 0U;
    d->mark_count = d->mark_at = 0U;
    d->gap_count = d->gap_at = 0U;
    d->gap_stretch = 0.0f;
    d->last_mark_ok = 0U;
    d->dit_ms = DIT_START_MS;
    d->dah_split_ms = 2.0f * DIT_START_MS;
    d->space_ms = DIT_START_MS;
    detector_resize(d, ma_length(d->dit_ms));
}

__attribute__((cold)) void RADIO_CwDecoderInit(radio_cwdec_t *d)
{
    memset(d, 0, sizeof(*d));
    d->span = RADIO_CWDEC_NEAR;
    RADIO_CwDecoderRetune(d);
}

__attribute__((cold)) void RADIO_CwDecoderSetSpan(radio_cwdec_t *d, uint16_t filter_hz)
{
    unsigned half = filter_hz / (2U * (unsigned)BIN_STEP_HZ);
    if (half < RADIO_CWDEC_NEAR) half = RADIO_CWDEC_NEAR;
    if (half > (RADIO_CWDEC_BINS - 1U) / 2U) half = (RADIO_CWDEC_BINS - 1U) / 2U;
    if (half == d->span) return;
    d->span = (uint8_t)half;
    RADIO_CwDecoderRetune(d);
}

/* A hole of up to 3 units is passed over as if it had not been there: the
 * interval it fell in reads that much short, which the dit/dah split and
 * the character gap mostly absorb (host stress bench: fewer errors than
 * dropping the character). A longer one would merge characters and words,
 * so its time passes -- every interval after it is measured from the right
 * place -- and the character it fell in is dropped rather than read with
 * elements missing: decoding picks up at the next character gap. */
void RADIO_CwDecoderLost(radio_cwdec_t *d, uint32_t ms)
{
    if ((float)ms <= 3.0f * unit_ms(d)) return;
    d->now_ms += ms;
    d->run = 0U;
    d->sym_bits = d->sym_len = 0U;
    d->synced = 0U;
    if (!d->speed_ok) d->log_n = 0U; /* the log is read as one stretch of time */
}

__attribute__((cold)) void RADIO_CwDecoderRetune(radio_cwdec_t *d)
{
    memset(d->hist, 0, sizeof(d->hist));
    memset(d->spec, 0, sizeof(d->spec));
    d->hist_at = d->hop = d->spec_n = 0U;
    d->freq_hz = d->peak_ratio = 0.0f;
    d->locked = 0U;
    d->lock_hold = 0U;
    d->nco_i = 1.0f; d->nco_q = 0.0f; d->nco_norm = 0U;
    set_nco(d);
    d->env = d->sig = d->noise = 0.0f;
    d->raw = d->keyed = d->primed = 0U;
    d->run = d->prime_n = 0U;
    d->prime_sum = 0.0f;
    d->edge_ms = d->now_ms;
    d->sym_bits = d->sym_len = 0U;
    d->word_done = 1U;
    d->open = 0U;
    d->char_peak = 0.0f;
    forget_station(d);
    detector_reset(d);
}

static void emit(radio_cwdec_t *d, char c)
{
    if (d->text_len >= RADIO_CWDEC_TEXT) {
        memmove(d->text, d->text + 1, RADIO_CWDEC_TEXT - 1U);
        d->text_len = RADIO_CWDEC_TEXT - 1U;
    }
    d->text[d->text_len++] = c;
    d->text[d->text_len] = '\0';
    ++d->emitted;
    if (c != ' ') ++d->chars;
}

/* The bin nearest `hz` from the pitch. */
static unsigned bin_of(const radio_cwdec_t *d, float hz)
{
    const float kf = hz / BIN_STEP_HZ + (float)d->span + 0.5f;
    const unsigned last = 2U * d->span;
    return kf < 0.0f ? 0U : kf >= (float)last ? last : (unsigned)kf;
}

/* Smoothed power across the span over the last 64 ms. A peak that stands
 * LOCK_ON above the median bin is a carrier; its interpolated position
 * becomes the detector frequency. Which peak: once locked, the strongest
 * within NEAR bins of the station; unlocked, one within NEAR bins of the
 * pitch before any further out -- the operator tunes the station wanted to
 * the pitch, and a wide filter holds others. A station appearing at the
 * pitch also takes over from one locked further out. */
static void spectrum(radio_cwdec_t *d)
{
    const unsigned half = d->span, nb = 2U * half + 1U;
    /* The window, oldest sample first, unpacked once for all the bins. */
    float z[RADIO_CWDEC_WINDOW][2];
    unsigned at = d->hist_at;
    for (unsigned n = 0U; n < RADIO_CWDEC_WINDOW; ++n) {
        z[n][0] = from_bf16(d->hist[at] >> 16);
        z[n][1] = from_bf16(d->hist[at] & 0xFFFFU);
        if (++at == RADIO_CWDEC_WINDOW) at = 0U;
    }
    /* Per-sample phase step of the lowest bin, and the rotation from one bin
     * to the next: two libm pairs a spectrum instead of one per bin, and no
     * table in RAM. */
    const float w0 = TWO_PI * (float)half * BIN_STEP_HZ / RATE_HZ;
    const float wd = TWO_PI * BIN_STEP_HZ / RATE_HZ;
    const float rot_i = cosf(wd), rot_q = -sinf(wd);
    float si = cosf(w0), sq = sinf(w0);
    float sorted[2U * RADIO_CWDEC_NEAR + 1U];
    const unsigned tracked = bin_of(d, d->freq_hz);
    float tracked_power = 0.0f;
    for (unsigned k = 0U; k < nb; ++k) {
        if (k) {
            const float ri = si * rot_i - sq * rot_q;
            sq = si * rot_q + sq * rot_i;
            si = ri;
        }
        float pi = 1.0f, pq = 0.0f, ai = 0.0f, aq = 0.0f;
        for (unsigned n = 0U; n < RADIO_CWDEC_WINDOW; ++n) {
            const float zi = z[n][0], zq = z[n][1];
            ai += zi * pi - zq * pq;
            aq += zi * pq + zq * pi;
            const float ni = pi * si - pq * sq;
            pq = pi * sq + pq * si;
            pi = ni;
        }
        const float p = ai * ai + aq * aq;
        if (k == tracked) tracked_power = p;
        d->spec[k] += SPEC_SMOOTH * (p - d->spec[k]);
    }
    /* The noise floor: the median, by insertion sort, of the bins within
     * NEAR of the pitch, where the channel filter is flat. Taken across a
     * wide span it took in the filter's skirts, read low, and let noise and
     * the first instants of an element lock. */
    const unsigned m0 = half - RADIO_CWDEC_NEAR, mn = 2U * RADIO_CWDEC_NEAR + 1U;
    for (unsigned k = 0U; k < mn; ++k) {
        const float v = d->spec[m0 + k];
        unsigned j = k;
        while (j && sorted[j - 1U] > v) { sorted[j] = sorted[j - 1U]; --j; }
        sorted[j] = v;
    }
    const float median = sorted[RADIO_CWDEC_NEAR];
    /* Peaks are local maxima: a strong carrier's skirt slopes away from it
     * and never makes one, so it cannot pass for a second station. */
    unsigned near = nb, track = nb, wide = nb;
    for (unsigned k = 0U; k < nb; ++k) {
        const float v = d->spec[k];
        if ((k && d->spec[k - 1U] >= v) || (k + 1U < nb && d->spec[k + 1U] > v)) continue;
        if (wide == nb || v > d->spec[wide]) wide = k;
        if (k + RADIO_CWDEC_NEAR >= half && k <= half + RADIO_CWDEC_NEAR && (near == nb || v > d->spec[near])) near = k;
        if (k + RADIO_CWDEC_NEAR >= tracked && k <= tracked + RADIO_CWDEC_NEAR && (track == nb || v > d->spec[track])) track = k;
    }
    const uint8_t near_lock = (uint8_t)(near != nb && median > 0.0f && d->spec[near] >= LOCK_ON * median);
    unsigned best = d->locked ? track : (near_lock ? near : wide);
    if (best == nb) best = tracked;
    d->peak_ratio = median > 0.0f ? d->spec[best] / median : 0.0f;
    d->now_ratio = median > 0.0f ? tracked_power / median : 0.0f;
    if (d->spec_n < SPEC_WARMUP) { ++d->spec_n; return; }
    /* Locked off the pitch, and a station has come up at it: that one. */
    const unsigned off = tracked > half ? tracked - half : half - tracked;
    if (d->locked && near_lock && off > RADIO_CWDEC_NEAR &&
        (near > tracked ? near - tracked : tracked - near) > RADIO_CWDEC_NEAR) {
        d->locked = 0U;
        best = near;
        d->peak_ratio = d->spec[near] / median;
    }
    if (d->peak_ratio > d->char_peak) d->char_peak = d->peak_ratio;
    if (d->peak_ratio >= LOCK_ON) {
        float delta = 0.0f;
        if (best > 0U && best + 1U < nb) {
            const float a = d->spec[best - 1U], b = d->spec[best], c = d->spec[best + 1U];
            const float den = a - 2.0f * b + c;
            if (den < 0.0f) delta = 0.5f * (a - c) / den;
            if (delta > 0.5f) delta = 0.5f;
            if (delta < -0.5f) delta = -0.5f;
        }
        const float target = ((float)best - (float)half + delta) * BIN_STEP_HZ;
        const uint8_t same = (uint8_t)(!d->locked && d->speed_ok && d->unlock_ms &&
                                       fabsf(target - d->freq_hz) < RELOCK_HZ &&
                                       d->now_ms - d->unlock_ms < RELOCK_MS);
        /* Jump on acquisition, follow smoothly once locked. */
        d->freq_hz = d->locked ? d->freq_hz + 0.5f * (target - d->freq_hz) : target;
        set_nco(d);
        if (!d->locked && !same) {
            /* Whatever was being assembled before the lock is not trusted:
             * it began on the wrong frequency, or on noise. Decoding starts
             * at the next character gap, not in the middle of a letter, and
             * the speed of whatever was locked before is not this one's. */
            d->lock_ms = d->now_ms;
            d->sym_bits = d->sym_len = 0U;
            d->synced = 0U;
            forget_station(d);
            /* ...unless a mark is under way: most often the one whose
             * carrier raised the peak, the first element of the first
             * letter. Decoding starts with it. It was heard partly before
             * the detector moved to this frequency, so it goes to the log
             * for replay() but not to the speed search; the gap before it
             * is logged too, and replay() starts at the next character gap
             * if that says the mark was inside a letter. */
            if (d->keyed) {
                d->lock_ms = d->edge_ms;
                d->synced = 1U;
                d->lock_mark = 1U;
                log_interval(d, 0U, (float)d->gap_ms);
            }
        }
        d->locked = 1U;
        d->lock_hold = LOCK_HOLD_MS;
    } else if (d->locked && d->peak_ratio < LOCK_OFF) {
        d->lock_hold = d->lock_hold > HOP_MS ? (uint16_t)(d->lock_hold - HOP_MS) : 0U;
        if (!d->lock_hold) { d->locked = 0U; d->unlock_ms = d->now_ms ? d->now_ms : 1U; }
    }
}

/* Dits and dahs by two-medians of the last RADIO_CWDEC_MARKS marks, seeded
 * from the quartiles: a sender far from the current estimate is found within
 * a character, and a stray long mark (noise joined to an element) cannot drag
 * the estimate the way a mean would. */
static float median_of(const float *v, unsigned n) { return n ? v[n / 2U] : 0.0f; }

static void track_speed(radio_cwdec_t *d, float mark)
{
    d->marks[d->mark_at] = mark;
    d->mark_at = (uint8_t)((d->mark_at + 1U) % RADIO_CWDEC_MARKS);
    if (d->mark_count < RADIO_CWDEC_MARKS) ++d->mark_count;
    float sorted[RADIO_CWDEC_MARKS];
    const unsigned n = d->mark_count;
    for (unsigned k = 0U; k < n; ++k) {
        const float v = d->marks[k];
        unsigned j = k;
        while (j && sorted[j - 1U] > v) { sorted[j] = sorted[j - 1U]; --j; }
        sorted[j] = v;
    }
    float c1 = sorted[n / 4U], c2 = sorted[(3U * n) / 4U];
    float dit = d->dit_ms;
    uint8_t clean = 0U;
    if (c2 >= 2.0f * c1) {
        unsigned split = 0U;
        for (unsigned iter = 0U; iter < 3U; ++iter) {
            const float at = sqrtf(c1 * c2);
            split = 0U;
            while (split < n && sorted[split] < at) ++split;
            if (!split || split == n) break;
            c1 = median_of(sorted, split);
            c2 = median_of(sorted + split, n - split);
        }
        if (split && split < n && c2 >= 2.1f * c1 && c2 <= 4.8f * c1) {
            dit = ((float)split * c1 + (float)(n - split) * c2 / 3.0f) / (float)n;
            d->dah_split_ms = sqrtf(c1 * c2);
            /* Dahs of 2.2 to 4.5 dits: a keyer's 3, a light keyer
             * weighting's 2.3, a bug's or a hand's long dahs. */
            clean = (uint8_t)(split >= 3U && n - split >= 2U && c2 >= 2.2f * c1 && c2 <= 4.5f * c1);
            /* ...and tight: nearly every mark close to one of the two. Noise
             * and the fragments of a first element seen at the wrong speed
             * make loose pairs that can still sit near 1:3. */
            if (clean) {
                unsigned near = 0U;
                for (unsigned k = 0U; k < n; ++k) {
                    const float m = sorted[k];
                    if ((m > 0.65f * c1 && m < 1.5f * c1) || (m > 0.65f * c2 && m < 1.5f * c2)) ++near;
                }
                if (n < 5U || near * 100U < n * 85U) clean = 0U;
            }
            /* The speed is known: characters may be printed from here on. */
            if (clean && d->peak_ratio >= LOCK_ON) d->speed_ok = 1U;
        }
    } else {
        /* One kind only: they can be taken for dits when well under the
         * estimate (a faster sender), never for dahs -- a run of merged
         * elements would otherwise read as a slower one. */
        const float mid = median_of(sorted, n);
        if (mid < 0.6f * d->dit_ms) { dit = mid; d->dah_split_ms = 2.0f * dit; }
    }
    /* Slowing down is where noise pushes (gaps fill in, elements merge, and a
     * longer dit widens the integrator that merges them), so the estimate
     * grows by at most 15 % a mark -- unless dits and dahs form two clean
     * clusters near 1:3, which merged noise does not make: that speed is
     * taken as it stands, so a slow sender is found in two characters and
     * replay() reads the start with the right dit. */
    if (!clean && dit > 1.15f * d->dit_ms) dit = 1.15f * d->dit_ms;
    if (dit < DIT_MIN_MS) dit = DIT_MIN_MS;
    if (dit > DIT_MAX_MS) dit = DIT_MAX_MS;
    d->dit_ms += (clean ? 1.0f : 0.5f) * (dit - d->dit_ms);
    if (d->dah_split_ms < 1.5f * d->dit_ms || d->dah_split_ms > 2.5f * d->dit_ms)
        d->dah_split_ms = 2.0f * d->dit_ms;
    /* Until the speed is known the space is taken to be a dit: gaps heard
     * at a wrong speed include merged elements' character gaps, and a unit
     * grown from them widens the integrator that merged them. */
    if (!d->speed_ok) d->space_ms = d->dit_ms;
    const uint8_t len = ma_length(unit_ms(d));
    if (len != d->ma_len) detector_resize(d, len);
}

static void log_interval(radio_cwdec_t *d, uint8_t mark, float ms)
{
    if (d->log_n >= RADIO_CWDEC_LOG) return; /* past this, the start is lost */
    const uint32_t v = ms > 32767.0f ? 32767U : (uint32_t)ms;
    d->log[d->log_n++] = (uint16_t)(v | (mark ? 0x8000U : 0U));
}

/* A gap that ended a character or a word (over 2 units), and not a pause
 * between overs (30 units and more); kept in units, so a change of speed
 * does not move the threshold it sets.
 *
 * Silence that ends a word: standard spacing puts 3 units between characters
 * and 7 between words, split at 5. Farnsworth timing stretches both -- at 18
 * WPM characters and 10 WPM overall a character gap is 9 units -- so the
 * split follows the character gaps actually heard: the lower quartile of the
 * recent gaps (words are rarer than characters; the shortest while fewer than
 * four are known) times sqrt(7/3), never under 5 units (word_gap_ms). With
 * standard spacing that is 4.6 units, so the 5 stands. The quartile only
 * changes here, so it is taken here rather than every silent millisecond. */
static void note_gap(radio_cwdec_t *d, float gap)
{
    gap /= unit_ms(d);
    if (gap <= 2.0f || gap >= 30.0f) return;
    d->gaps[d->gap_at] = gap;
    d->gap_at = (uint8_t)((d->gap_at + 1U) % RADIO_CWDEC_GAPS);
    if (d->gap_count < RADIO_CWDEC_GAPS) ++d->gap_count;
    float sorted[RADIO_CWDEC_GAPS];
    const unsigned n = d->gap_count;
    for (unsigned k = 0U; k < n; ++k) {
        const float v = d->gaps[k];
        unsigned j = k;
        while (j && sorted[j - 1U] > v) { sorted[j] = sorted[j - 1U]; --j; }
        sorted[j] = v;
    }
    d->gap_stretch = 1.53f * sorted[n / 4U];
}

static float word_gap_ms(const radio_cwdec_t *d)
{
    return (d->gap_stretch > 5.0f ? d->gap_stretch : 5.0f) * unit_ms(d);
}

static char finish_character(radio_cwdec_t *d);

/* The speed has just been confirmed: read what was heard since the lock
 * again with it, so the first words print instead of being lost to the
 * search. The character still being sent stays open for the live path. */
static void replay(radio_cwdec_t *d)
{
    d->sym_bits = d->sym_len = 0U;
    /* The space inside characters, from the logged gaps under two units,
     * before anything is split with it. */
    d->space_ms = d->dit_ms;
    for (unsigned k = 1U; k < d->log_n; ++k)
        if (!(d->log[k] & 0x8000U) && (d->log[k - 1U] & 0x8000U) && (float)d->log[k] < 2.0f * unit_ms(d))
            d->space_ms += 0.25f * ((float)d->log[k] - d->space_ms);
    /* The logged gaps set the word threshold before the words are split. */
    for (unsigned k = 1U; k < d->log_n; ++k)
        if (!(d->log[k] & 0x8000U) && (d->log[k - 1U] & 0x8000U)) note_gap(d, (float)d->log[k]);
    /* Only from the last mark that does not fit the speed found -- a dit or
     * a dah of it -- on: a search that took long logged noise and fragments,
     * and re-read with a speed they did not confirm they printed as a burst
     * of guesses. (Re-reading only the word under way cut the bursts too,
     * but lost the first word of every clean signal: host stress bench.) */
    unsigned k = 0U;
    for (unsigned j = k; j < d->log_n; ++j) {
        if (!(d->log[j] & 0x8000U)) continue;
        const float ms = (float)(d->log[j] & 0x7FFFU);
        if (!((ms > 0.5f * d->dit_ms && ms < 1.7f * d->dit_ms) || (ms > 1.9f * d->dit_ms && ms < 5.0f * d->dit_ms)))
            k = j + 1U;
    }
    /* Start at a character gap: decoding may have synced on a long gap
     * inside a slow sender's letter, or the cut above fell inside one. */
    while (k < d->log_n && ((d->log[k] & 0x8000U) || (float)(d->log[k] & 0x7FFFU) <= 2.0f * unit_ms(d))) ++k;
    /* Nothing to re-read, or the character under way is partly cut away:
     * the live path waits for the next character gap rather than finish it. */
    if (k >= d->log_n) d->synced = 0U;
    /* The spectrum that confirmed the speed vouches for what is re-read. */
    const float evidence = d->char_peak > d->peak_ratio ? d->char_peak : d->peak_ratio;
    for (; k < d->log_n; ++k) {
        const float ms = (float)(d->log[k] & 0x7FFFU);
        d->char_peak = evidence;
        if (d->log[k] & 0x8000U) {
            d->sym_bits = (uint8_t)((d->sym_bits << 1) | (ms > d->dah_split_ms ? 1U : 0U));
            if (d->sym_len < 7U) ++d->sym_len;
            d->word_done = 0U;
        } else {
            if (ms > 2.0f * unit_ms(d)) (void)finish_character(d);
            if (ms > word_gap_ms(d) && d->text_len && d->text[d->text_len - 1U] != ' ') emit(d, ' ');
        }
    }
    d->char_peak = evidence; /* and for the character still being sent */
    d->log_n = 0U;
}

/* Longer than this is a carrier (tuning, a beacon's key-down), not Morse:
 * ten dits, but never under a 5 WPM dah, so the speed can be found from a
 * slower sender's dahs while the estimate is still fast. */
static float longest_mark(const radio_cwdec_t *d)
{
    const float ten = 10.0f * d->dit_ms;
    return ten > 1000.0f ? ten : 1000.0f;
}

static char finish_character(radio_cwdec_t *d)
{
    char c = '\0';
    if (d->sym_len) {
        c = d->sym_len <= 6U ? MTRAIN_DecodeSymbol(d->sym_bits, d->sym_len) : '*';
        /* The table's '?' is also its answer for an unknown pattern. */
        if (c == '?' && !(d->sym_len == 6U && d->sym_bits == 0x0CU)) c = '*';
        /* Nothing is printed until the speed has been confirmed by a clean
         * dit/dah split: before that, dits and dahs are guesses, and a slow
         * sender read at the 30 WPM start is all T's. Nor unless the
         * spectrum showed a carrier while the character was sent: noise
         * whose chance peak locked the detector makes marks too, and the
         * keyed/unkeyed level ratio of noise alone is 12 dB. */
        if (d->locked && d->open && d->speed_ok && d->char_peak >= LOCK_ON) emit(d, c);
        else c = '\0';
    }
    d->sym_bits = d->sym_len = 0U;
    d->char_peak = 0.0f;
    return c;
}

/* The spectrum's history, the tracked carrier mixed to DC and the coherent
 * integrator. Returns 0 until the integrator has filled once. The stages
 * of RADIO_CwDecoderStep are out of line to keep each within the
 * complexity budget; the decoder runs in the main loop, 1000 times a
 * second, where the calls cost nothing that matters. */
static __attribute__((noinline)) uint8_t detect(radio_cwdec_t *d, float i, float q)
{
    d->hist[d->hist_at] = ((uint32_t)to_bf16(i) << 16) | to_bf16(q);
    if (++d->hist_at == RADIO_CWDEC_WINDOW) d->hist_at = 0U;
    if (++d->hop == HOP_MS) { d->hop = 0U; spectrum(d); }

    /* Mix the tracked carrier to DC and integrate over ma_len samples. */
    const float yi = i * d->nco_i - q * d->nco_q;
    const float yq = i * d->nco_q + q * d->nco_i;
    const float ni = d->nco_i * d->nco_di - d->nco_q * d->nco_dq;
    d->nco_q = d->nco_i * d->nco_dq + d->nco_q * d->nco_di;
    d->nco_i = ni;
    if (++d->nco_norm == 256U) {
        const float g = 1.0f / sqrtf(d->nco_i * d->nco_i + d->nco_q * d->nco_q);
        d->nco_i *= g; d->nco_q *= g; d->nco_norm = 0U;
    }
    const unsigned out = (d->ma_at + RADIO_CWDEC_MA_MAX - d->ma_len) % RADIO_CWDEC_MA_MAX;
    d->ma_i += yi - d->ma[out][0];
    d->ma_q += yq - d->ma[out][1];
    d->ma[d->ma_at][0] = yi;
    d->ma[d->ma_at][1] = yq;
    if (++d->ma_at == RADIO_CWDEC_MA_MAX) {
        d->ma_at = 0U;
        detector_resize(d, d->ma_len); /* re-sum: no float drift in the running sum */
    }
    if (d->ma_fill < RADIO_CWDEC_MA_MAX) { ++d->ma_fill; return 0U; }
    d->env = sqrtf(d->ma_i * d->ma_i + d->ma_q * d->ma_q);
    return 1U;
}

/* Both levels and the squelch from this sample's envelope; returns the
 * thresholded (not yet debounced) decision. */
static __attribute__((noinline)) uint8_t levels(radio_cwdec_t *d)
{
    const float span = d->sig - d->noise;
    const float on = d->noise + THRESH_ON * span, off = d->noise + THRESH_OFF * span;
    const uint8_t raw = d->raw ? (d->env > off) : (d->env > on);
    d->raw = raw;
    /* The keyed level is the mean envelope over each mark's plateau: from half
     * an integration length after its (half-height) start to half an
     * integration length before its end. The mean over the whole mark would
     * include the ramps and put the threshold low, lengthening every element;
     * its peak would be pushed up by noise and break weak elements apart.
     * The unkeyed level learns only while both the state and the decision
     * say unkeyed, so a noise spike does not train either toward the other. */
    d->env_hist[d->env_at] = to_bf16(d->env);
    d->env_at = (uint8_t)((d->env_at + 1U) % RADIO_CWDEC_ENV_HIST);
    if (d->keyed) {
        if (d->now_ms - d->edge_ms >= (uint32_t)(d->ma_len / 2U)) {
            d->plateau_sum += from_bf16(d->env_hist[(d->env_at + RADIO_CWDEC_ENV_HIST - 1U) % RADIO_CWDEC_ENV_HIST]);
            ++d->plateau_n;
        }
        /* A mark far above the keyed level -- tuning onto a station after
         * the decoder had only heard noise, or a stronger one taking over --
         * lifts it at once. Waiting for the mark's end let a strong signal
         * hold the envelope over the off threshold through every gap: the
         * elements of a whole letter ran into one mark. */
        if (d->env > 2.0f * d->sig) {
            /* 12 dB over everything heard so far, before the speed is known:
             * the marks collected until now were noise or another station,
             * and would confirm a wrong speed together with this one's. */
            if (!d->speed_ok && d->env > 4.0f * d->sig) {
                d->mark_count = d->mark_at = 0U;
                d->log_n = 0U;
                d->sym_bits = d->sym_len = 0U;
            }
            d->sig = 0.5f * d->env;
        }
    } else if (!raw && d->now_ms - d->edge_ms >= NOISE_CLEAR(d->ma_len)) {
        /* ...from the envelope one integration length ago, and only once that
         * sample is clear of the last mark's falling ramp. The next mark's
         * rising ramp starts half an integration length before its
         * half-height edge, which has not come yet, so the delayed sample is
         * clear of it too. Learning from the present instead took in the
         * start of every next element's ramp: the noise level settled at a
         * tenth of the keyed one, and a fade then left the threshold above
         * the faded elements. */
        const float past = from_bf16(d->env_hist[(d->env_at + RADIO_CWDEC_ENV_HIST - 1U - d->ma_len) % RADIO_CWDEC_ENV_HIST]);
        d->noise += (past - d->noise) * (past < d->noise ? (1.0f / 32.0f) : (1.0f / 128.0f));
    }
    /* The keyed level relaxes toward the noise over ~2 s while the station is
     * there -- keyed, or its carrier still standing out of the spectrum: a
     * fade can take its elements under the threshold, and then no mark would
     * bring the level down to them. In true silence it holds for SIG_HOLD_MS
     * -- Farnsworth code (W1AW practice, most trainers) pauses seconds
     * between characters, and a level that sagged there stretched every
     * element -- and only then relaxes, so one strong station does not
     * deafen the decoder to a weaker one after it. */
    if (d->keyed || d->peak_ratio >= SIG_CARRIER || d->now_ms - d->edge_ms > SIG_HOLD_MS)
        d->sig += (d->noise * 3.16f - d->sig) * (1.0f / 2000.0f);
#if FADE_RELAX
    /* The latest 64 ms show the station keying, yet no mark has begun for
     * over 2 dits: a fade has taken its elements under the threshold. Bring
     * the keyed level down over about FADE_DITS dits instead of 2 s. In an
     * ordinary gap the window has emptied by then. */
    if (!d->keyed && d->now_ratio >= LOCK_ON && (float)(d->now_ms - d->edge_ms) > 2.0f * d->dit_ms)
        d->sig += (d->noise * 3.16f - d->sig) / (FADE_DITS * d->dit_ms);
#endif
    d->open = (d->sig > SQUELCH_RATIO * d->noise) ? 1U : 0U;
    return raw;
}

/* A mark has started at `edge`: the gap before it is over. */
static __attribute__((noinline)) void mark_start(radio_cwdec_t *d, uint32_t edge)
{
    const float gap = (float)(edge - d->edge_ms) + ramp_ms(d);
    /* The gap before the first logged mark is logged too: replay()
     * needs it to know whether the log starts on a letter. */
    if (d->locked && d->synced && !d->speed_ok) log_interval(d, 0U, gap);
    /* Only a gap between two decoded marks says anything about the spacing:
     * the word threshold from the longer ones, the unit from the shorter. */
    if (d->last_mark_ok && d->locked && d->synced && d->speed_ok) note_gap(d, gap);
    if (d->speed_ok && d->last_mark_ok && gap < 2.0f * unit_ms(d)) {
        d->space_ms += 0.25f * (gap - d->space_ms);
        const uint8_t len = ma_length(unit_ms(d));
        if (len != d->ma_len) detector_resize(d, len);
    }
    d->gap_ms = gap > 65535.0f ? 65535U : (uint16_t)gap;
    d->edge_ms = edge; /* a mark starts; the gap before it is over */
    /* Samples since the back-dated edge that are already past the
     * rising ramp count toward the plateau. */
    d->plateau_sum = 0.0f;
    d->plateau_n = 0U;
}

/* A mark has ended at `edge`: learn the keyed level from its plateau and,
 * if it is a real element, add it to the character. */
static __attribute__((noinline)) void mark_end(radio_cwdec_t *d, uint32_t edge)
{
    const float mark = (float)(edge - d->edge_ms) - ramp_ms(d);
    d->edge_ms = edge;
    /* Take back what was summed after the plateau ended: the last
     * (now - edge) + ma_len/2 samples are the falling ramp. */
    unsigned tail = (d->now_ms - edge) + d->ma_len / 2U + 1U;
    unsigned at = d->env_at;
    for (unsigned k = 0U; k < tail && k < RADIO_CWDEC_ENV_HIST && d->plateau_n; ++k) {
        at = at ? at - 1U : RADIO_CWDEC_ENV_HIST - 1U;
        d->plateau_sum -= from_bf16(d->env_hist[at]);
        --d->plateau_n;
    }
    if (d->plateau_n >= 2U) {
        const float plateau = d->plateau_sum / (float)d->plateau_n;
        d->sig += (plateau > d->sig ? SIG_RISE : SIG_FALL) * (plateau - d->sig);
    }
    d->last_mark_ok = 0U;
    if (mark >= 0.45f * d->dit_ms && mark < longest_mark(d) && d->locked && d->open &&
        d->synced && edge - (uint32_t)mark >= d->lock_ms) {
        d->last_mark_ok = 1U;
        const uint8_t known = d->speed_ok;
        if (!known) log_interval(d, 1U, mark);
        if (!d->lock_mark) track_speed(d, mark);
        d->lock_mark = 0U;
        if (!known && d->speed_ok) {
            replay(d); /* this mark was the last entry in the log */
        } else {
            d->sym_bits = (uint8_t)((d->sym_bits << 1) | (mark > d->dah_split_ms ? 1U : 0U));
            if (d->sym_len < 7U) ++d->sym_len;
        }
        d->word_done = 0U;
    }
}

/* Silence (or a key-down) running on: ends characters and words, and drops
 * a half character under a steady carrier. */
static __attribute__((noinline)) char silence_step(radio_cwdec_t *d)
{
    if (!d->keyed) {
        const float silence = (float)(d->now_ms - d->edge_ms) + ramp_ms(d);
        if (d->locked && !d->synced && silence > 2.0f * unit_ms(d)) d->synced = 1U;
        if (d->sym_len && silence > 2.0f * unit_ms(d)) {
            char c = finish_character(d);
            if (c) return c;
        }
        if (!d->word_done && silence > word_gap_ms(d)) {
            d->word_done = 1U;
            if (d->text_len && d->text[d->text_len - 1U] != ' ') { emit(d, ' '); return ' '; }
        }
    } else if ((float)(d->now_ms - d->edge_ms) > longest_mark(d)) {
        /* A carrier, not Morse: drop the half character. */
        d->sym_bits = d->sym_len = 0U;
    }
    return '\0';
}

char RADIO_CwDecoderStep(radio_cwdec_t *d, float i, float q)
{
    if (!isfinite(i) || !isfinite(q)) i = q = 0.0f;
    ++d->now_ms;
    if (!detect(d, i, q)) return '\0';

    /* Two levels: the envelope's mean while keyed and while not. The first
     * full window seeds both; the keyed level starts 10 dB up so a carrier
     * that appears later is still recognised as one. */
    if (!d->primed) {
        d->prime_sum += d->env;
        if (++d->prime_n < 64U) return '\0';
        d->noise = d->prime_sum / 64.0f;
        d->sig = 3.16f * d->noise;
        d->primed = 1U;
    }
    const uint8_t raw = levels(d);

    /* Debounce by PERSIST_DITS: a state holds until the raw decision has
     * disagreed with it that long; the edge is dated where it began. */
    uint16_t persist = (uint16_t)(unit_ms(d) * PERSIST_DITS);
    if (persist < 4U) persist = 4U;
    if (raw == d->keyed) {
        d->run = 0U;
    } else if (++d->run >= persist) {
        const uint32_t edge = d->now_ms - d->run + 1U;
        d->run = 0U;
        d->keyed = raw;
        if (raw) mark_start(d, edge);
        else mark_end(d, edge);
    }
    return silence_step(d);
}

uint8_t RADIO_CwDecoderWpm(const radio_cwdec_t *d)
{
    /* The tracked dit has the ramp taken back already (ramp_ms). */
    const float dit = d->dit_ms;
    return (d->locked && dit > 0.0f) ? (uint8_t)(1200.0f / dit + 0.5f) : 0U;
}

float RADIO_CwDecoderOffsetHz(const radio_cwdec_t *d) { return d->freq_hz; }

float RADIO_CwDecoderSnrDb(const radio_cwdec_t *d)
{
    return (d->noise > 0.0f && d->sig > 0.0f) ? 20.0f * log10f(d->sig / d->noise) : 0.0f;
}

uint8_t RADIO_CwDecoderLocked(const radio_cwdec_t *d) { return d->locked; }

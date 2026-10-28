#ifndef MOD_DIGI_TONE_LOGIC_H
#define MOD_DIGI_TONE_LOGIC_H

/* DIGI TONE: USB audio under DIGI CLEAN transmitted the way QMX transmits
 * it (render_digi_tone, mod_tx_pipeline.c). The audio is measured, not
 * modulated (QDX manual 1.24, "Audio frequency analysis"): each rising zero
 * crossing is timed to 1/65536 of a sample by linear interpolation, the tone
 * is the mean period over the last >= 10 ms of crossings (QMX's 480 minimum
 * samples), updated every cycle, and the carrier is a constant-envelope,
 * phase-continuous tone at that offset from the dial, at full drive.
 * WSJT-X's Pwr slider and the sound server's volume do not set the power,
 * and where QMX keys only above 80 % of full scale (its "two dots": a PC
 * level a little low gives no RF at all), any tone above
 * MOD_DIGI_TONE_FLOOR keys here.
 *
 * The carrier rises and falls on a 10 ms smoothstep where QMX switches: it
 * starts once MOD_DIGI_TONE_ACQUIRE consistent cycles have been timed, and
 * falls when the cycles drop under half the over's loudest (QMX's fall
 * threshold, but relative: WSJT-X's 20 ms closing ramp is half way down, so
 * the two end together), after MOD_DIGI_TONE_HOLD samples and a period and
 * a half without a cycle above the floor (a cut stream -- the DC block's
 * tail has no crossings -- but not a dropped USB frame or two), or after
 * MOD_DIGI_TONE_LOST samples without a tone. A cycle more than an eighth off
 * the mean is not part of a tone (speech, noise): timing restarts from it
 * while the carrier holds its frequency. Cycles from before a step of more
 * than 1/32 leave the mean, so an FSK shift settles in MOD_DIGI_TONE_ACQUIRE
 * cycles. A level lowered by more than half mid-over is taken as the new
 * level after MOD_DIGI_TONE_RELEVEL samples.
 *
 * 48 kHz samples in [-1, 1]; phase in units of pi. All zeros is idle. */

#include <math.h>
#include <stdint.h>

#define MOD_DIGI_TONE_RING     64U          /* crossings; 10 ms of 5 kHz is 50 */
#define MOD_DIGI_TONE_FLOOR    0.00316f     /* -50 dBFS */
#define MOD_DIGI_TONE_WINDOW   (480UL << 16) /* Q16 samples */
#define MOD_DIGI_TONE_ACQUIRE  4U
#define MOD_DIGI_TONE_RAMP     (1.0f / 480.0f)
#define MOD_DIGI_TONE_LOST     2400U
#define MOD_DIGI_TONE_HOLD     192U
#define MOD_DIGI_TONE_RELEVEL  960U
#define MOD_DIGI_TONE_STEP_MIN (2.0f * 100.0f / 48000.0f)  /* 100 Hz */
#define MOD_DIGI_TONE_STEP_MAX (2.0f * 5000.0f / 48000.0f) /* 5 kHz */
_Static_assert((MOD_DIGI_TONE_RING & (MOD_DIGI_TONE_RING - 1U)) == 0U, "the ring index is masked");
_Static_assert(MOD_DIGI_TONE_RING <= 256U, "cycles is a byte");

typedef struct {
    uint32_t cross[MOD_DIGI_TONE_RING]; /* rising zero crossings, Q16 samples */
    uint32_t n;          /* samples measured: the crossings' clock */
    uint32_t seq;        /* the renderer's: pcm_reset_seq of the stream */
    uint32_t since_tone; /* samples since the last tone measurement */
    float prev;          /* the sample before */
    float peak;          /* |x| since the last crossing */
    float phase;         /* the carrier's offset phase, units of pi */
    float step;          /* its advance a sample, units of pi; 0: none yet */
    float ramp;          /* 0..1 along the key-down smoothstep */
    float level;         /* the loudest cycle of the over */
    uint16_t quiet;      /* samples since a crossing closed a cycle above the floor */
    uint16_t low;        /* samples of cycles under half the level */
    uint8_t head;        /* the newest crossing */
    uint8_t cycles;      /* whole cycles between the oldest kept and head */
    uint8_t primed;      /* cross[head] starts a cycle */
    uint8_t keyed;
} mod_digi_tone_state_t;

static inline void mod_digi_tone_restart(mod_digi_tone_state_t *s, uint32_t t)
{
    s->head = 0U;
    s->cross[0] = t;
    s->cycles = 0U;
    s->primed = 1U;
}

/* A rising zero crossing at t (Q16 samples) closes the cycle s->peak was
 * taken over. Once a cycle: out of line. */
static __attribute__((noinline, unused)) void mod_digi_tone_crossing(mod_digi_tone_state_t *s, uint32_t t)
{
    const uint32_t mask = MOD_DIGI_TONE_RING - 1U;
    const float pk = s->peak;
    s->peak = 0.0f;
    if (pk >= MOD_DIGI_TONE_FLOOR) s->quiet = 0U;
    if ((pk < MOD_DIGI_TONE_FLOOR) || (s->primed == 0U)) {
        mod_digi_tone_restart(s, t);
        return;
    }
    if (pk > s->level) s->level = pk;
    const uint32_t period = t - s->cross[s->head];
    if (pk < 0.5f * s->level) {
        s->keyed = 0U;
        if ((uint32_t)s->low + (period >> 16) >= MOD_DIGI_TONE_RELEVEL) {
            s->level = pk;
            s->low = 0U;
        } else {
            s->low = (uint16_t)(s->low + (period >> 16));
        }
    } else {
        s->low = 0U;
    }
    if (s->cycles != 0U) {
        const uint32_t span = s->cross[s->head] - s->cross[(s->head - s->cycles) & mask];
        const float mean = (float)span / (float)s->cycles;
        if (fabsf((float)period - mean) * 8.0f > mean) {
            mod_digi_tone_restart(s, t);
            return;
        }
    }
    s->head = (uint8_t)((s->head + 1U) & mask);
    s->cross[s->head] = t;
    if (s->cycles < mask) s->cycles++;
    /* The fewest cycles that still reach across the window, none from
     * before a step. */
    while (s->cycles > MOD_DIGI_TONE_ACQUIRE) {
        const uint32_t second = s->cross[(s->head - (s->cycles - 1U)) & mask];
        const float oldest = (float)(second - s->cross[(s->head - s->cycles) & mask]);
        if (((t - second) < MOD_DIGI_TONE_WINDOW) &&
            (fabsf(oldest - (float)period) * 32.0f <= (float)period)) {
            break;
        }
        s->cycles--;
    }
    if (s->cycles < MOD_DIGI_TONE_ACQUIRE) return;
    const float step = (2.0f * 65536.0f) * (float)s->cycles
                     / (float)(t - s->cross[(s->head - s->cycles) & mask]);
    if ((step < MOD_DIGI_TONE_STEP_MIN) || (step > MOD_DIGI_TONE_STEP_MAX)) return;
    s->step = step;
    s->since_tone = 0U;
    if (s->low == 0U) s->keyed = 1U;
}

/* One sample: measure, key, ramp, and advance the carrier's phase. */
static inline void mod_digi_tone_sample(mod_digi_tone_state_t *s, float x)
{
    const float ax = fabsf(x);
    if (ax > s->peak) s->peak = ax;
    if (s->quiet != 0xFFFFU) s->quiet++;
    if ((s->prev < 0.0f) && (x >= 0.0f)) {
        const float frac = s->prev / (s->prev - x); /* (0, 1] after prev */
        mod_digi_tone_crossing(s, ((s->n - 1U) << 16) + (uint32_t)(frac * 65536.0f));
    }
    s->prev = x;
    s->n++;
    if (s->since_tone != 0xFFFFFFFFU) s->since_tone++;
    if (((s->quiet > MOD_DIGI_TONE_HOLD) && ((float)s->quiet * s->step > 3.0f)) ||
        (s->quiet == 0xFFFFU)) {
        /* Silence: the over, or this stretch of it, is over. */
        s->keyed = 0U;
        s->primed = 0U;
        s->level = 0.0f;
        s->low = 0U;
    } else if ((s->keyed != 0U) && (s->since_tone > MOD_DIGI_TONE_LOST)) {
        s->keyed = 0U;
    }
    float ramp = s->ramp + ((s->keyed != 0U) ? MOD_DIGI_TONE_RAMP : -MOD_DIGI_TONE_RAMP);
    if (ramp < 0.0f) ramp = 0.0f;
    else if (ramp > 1.0f) ramp = 1.0f;
    s->ramp = ramp;
    float ph = s->phase + s->step;
    if (ph >= 1.0f) ph -= 2.0f;
    s->phase = ph;
}

/* The carrier's envelope, 0..1: the ramp's smoothstep. */
static inline float mod_digi_tone_envelope(const mod_digi_tone_state_t *s)
{
    const float r = s->ramp;
    return r * r * (3.0f - 2.0f * r);
}

/* The carrier's offset from the dial, Hz (0 before a tone). */
static inline float mod_digi_tone_hz(const mod_digi_tone_state_t *s)
{
    return s->step * 24000.0f;
}

#endif /* MOD_DIGI_TONE_LOGIC_H */

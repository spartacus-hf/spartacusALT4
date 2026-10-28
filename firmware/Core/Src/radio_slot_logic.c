#include "radio_slot_logic.h"
#include <string.h>

/* Signals start 0.5 s after the boundary and stop at 13.14 s, so the quiet
 * stretch is centred ~0.68 s before the boundary: the boundary is about
 * seven bins after the centre of the quietest 2 s window. Received DT
 * (latency, other stations' clocks) moves the observed gap a few tenths
 * later; RADIO_SlotQuiet works from the same estimate, so what it calls
 * quiet is the gap as observed. */
#define SLOT_BOUNDARY_AFTER_GAP_CENTRE 7U
#define SLOT_QUIET_FROM_MS 13400
#define SLOT_QUIET_TO_MS   14700
/* A host key and the band agree within this many bins (1 s): WSJT-X keys at
 * the boundary, a Tune press or a late start anywhere. */
#define SLOT_AGREE_BINS    10U

static uint16_t bin_distance(uint16_t a, uint16_t b)
{
    const uint16_t d = (uint16_t)((a + RADIO_SLOT_BINS - b) % RADIO_SLOT_BINS);
    return (d > RADIO_SLOT_BINS / 2U) ? (uint16_t)(RADIO_SLOT_BINS - d) : d;
}

__attribute__((cold)) void RADIO_SlotInit(radio_slot_t *s, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->origin_ms = s->bin_start_ms = now_ms;
}

/* The median of the fold without sorting it: the value with as many bins
 * below as above, found by counting (150 x 150 compares, once a bin). */
static float fold_median(const float *f, unsigned n)
{
    float best = f[0];
    unsigned best_gap = n;
    for (unsigned i = 0U; i < n; ++i) {
        unsigned below = 0U, above = 0U;
        for (unsigned j = 0U; j < n; ++j) {
            if (f[j] < f[i]) ++below;
            else if (f[j] > f[i]) ++above;
        }
        const unsigned gap = below > above ? below - above : above - below;
        if (gap < best_gap) { best_gap = gap; best = f[i]; }
    }
    return best;
}

static void slot_estimate(radio_slot_t *s)
{
    if (s->slots < RADIO_SLOT_LOCK_SLOTS) return;
    /* Quietest GAP_BINS-long window, circularly. */
    float sum = 0.0f;
    for (unsigned k = 0U; k < RADIO_SLOT_GAP_BINS; ++k) sum += s->fold[k];
    float best = sum;
    unsigned start = 0U;
    for (unsigned g = 1U; g < RADIO_SLOT_BINS; ++g) {
        sum += s->fold[(g + RADIO_SLOT_GAP_BINS - 1U) % RADIO_SLOT_BINS] - s->fold[g - 1U];
        if (sum < best) { best = sum; start = g; }
    }
    const float gap = best / (float)RADIO_SLOT_GAP_BINS;
    const float busy = fold_median(s->fold, RADIO_SLOT_BINS);
    /* 10 log10 without libm: ratio to dB through the float's exponent and a
     * short polynomial in the mantissa (0.02 dB) -- enough for a threshold. */
    float depth = -100.0f;
    if (gap > 0.0f && busy > 0.0f) {
        const float r = busy / gap;
        union { float f; uint32_t u; } v = { r };
        const int32_t e = (int32_t)((v.u >> 23) & 0xFFU) - 127;
        v.u = (v.u & 0x007FFFFFU) | 0x3F800000U;  /* mantissa in [1, 2) */
        const float m = v.f - 1.0f;
        const float log2m = m * (1.4425f + m * (-0.7213f + m * (0.4809f + m * -0.2014f)));
        depth = 3.0103f * ((float)e + log2m);
    }
    s->depth_db = depth;
    if (depth >= RADIO_SLOT_LOCK_DB) {
        const uint16_t band = (uint16_t)((start + RADIO_SLOT_GAP_BINS / 2U + SLOT_BOUNDARY_AFTER_GAP_CENTRE)
                                         % RADIO_SLOT_BINS);
        /* A host key that agrees with the band is the finer phase; one that
         * does not (a Tune press set it while the band was still learning)
         * gives way to the band. */
        if (s->locked == 2U && bin_distance(band, s->boundary_bin) <= SLOT_AGREE_BINS) return;
        s->boundary_bin = band;
        s->locked = 1U;
    } else if (s->locked != 2U) {
        s->locked = 0U;  /* a dead band keeps a host phase */
    }
}

uint8_t RADIO_SlotAdd(radio_slot_t *s, uint32_t now_ms, float power)
{
    uint8_t done = 0U;
    /* A main loop held off for more than a slot: start over. */
    if ((uint32_t)(now_ms - s->bin_start_ms) >= RADIO_SLOT_MS) {
        const uint8_t locked = s->locked;
        const uint16_t boundary = s->boundary_bin;
        const uint32_t origin = s->origin_ms;
        RADIO_SlotInit(s, now_ms);
        if (locked == 2U) {
            /* Keep a host-given phase: rebase it onto the new origin. */
            const uint32_t at = (origin + (uint32_t)boundary * RADIO_SLOT_BIN_MS);
            const uint32_t since = (uint32_t)(now_ms - at) % RADIO_SLOT_MS;
            s->boundary_bin = (uint16_t)(((RADIO_SLOT_MS - since) % RADIO_SLOT_MS) / RADIO_SLOT_BIN_MS);
            s->locked = 2U;
        }
    }
    while ((uint32_t)(now_ms - s->bin_start_ms) >= RADIO_SLOT_BIN_MS) {
        if (s->acc_n) {
            const float mean = s->acc / (float)s->acc_n;
            float *f = &s->fold[s->bin];
            if (s->slots == 0U) *f = mean;
            else *f += 0.25f * (mean - *f);
        }
        s->acc = 0.0f;
        s->acc_n = 0U;
        s->bin_start_ms += RADIO_SLOT_BIN_MS;
        if (++s->bin >= RADIO_SLOT_BINS) {
            s->bin = 0U;
            s->origin_ms = s->bin_start_ms;
            if (s->slots < 0xFFFFU) ++s->slots;
        }
        done = 1U;
    }
    if (power >= 0.0f) {  /* NaN fails this too */
        s->acc += power;
        ++s->acc_n;
    }
    if (done) slot_estimate(s);
    return done;
}

uint8_t RADIO_SlotSync(radio_slot_t *s, uint32_t now_ms, uint32_t lead_ms)
{
    const uint32_t at = now_ms + lead_ms;  /* the boundary */
    const uint32_t since_origin = (uint32_t)(at - s->origin_ms) % RADIO_SLOT_MS;
    const uint16_t key = (uint16_t)((since_origin + RADIO_SLOT_BIN_MS / 2U) / RADIO_SLOT_BIN_MS % RADIO_SLOT_BINS);
    if (s->locked == 1U && bin_distance(key, s->boundary_bin) > SLOT_AGREE_BINS) return 0U;
    s->boundary_bin = key;
    s->locked = 2U;
    return 1U;
}

int32_t RADIO_SlotPhaseMs(const radio_slot_t *s, uint32_t now_ms)
{
    if (!s->locked) return -1;
    const uint32_t boundary = s->origin_ms + (uint32_t)s->boundary_bin * RADIO_SLOT_BIN_MS;
    return (int32_t)((uint32_t)(now_ms - boundary + 4U * RADIO_SLOT_MS) % RADIO_SLOT_MS);
}

uint8_t RADIO_SlotQuiet(const radio_slot_t *s, uint32_t now_ms)
{
    const int32_t phase = RADIO_SlotPhaseMs(s, now_ms);
    return (uint8_t)(phase >= SLOT_QUIET_FROM_MS && phase <= SLOT_QUIET_TO_MS);
}

void RADIO_GapInit(radio_gap_judge_t *g, uint32_t bad)
{
    g->bad = bad;
    g->sent = 0U;
    g->open = 0U;
}

uint8_t RADIO_GapStep(radio_gap_judge_t *g, uint8_t live, uint32_t bad, uint32_t sent)
{
    const uint8_t lost = (uint8_t)(bad != g->bad);
    g->bad = bad;
    if (!live) { g->open = 0U; return 0U; } /* the host went: what it left was no gap */
    if (lost && !g->open) { g->open = 1U; g->sent = sent; }
    if (g->open && (uint32_t)(sent - g->sent) >= RADIO_GAP_HEARD_FRAMES) { g->open = 0U; return 1U; }
    return 0U;
}

/* Host suite for Core/Src/radio_slot_logic.c: the FT8 slot clock learnt from
 * the passband power. A synthetic band: noise all the time, FT8 signals from
 * 0.5 s to 13.14 s of every 15 s slot (plus a per-slot random level), sampled
 * at main-loop pace with jitter. Checked: no lock before three slots or on a
 * band without the cadence, the boundary within 0.3 s of the truth (the
 * observed DT shifts it, and the test feeds a DT of 0.3 s), the quiet window
 * inside the gap, a host key refining the phase only when it agrees with
 * the band, a stalled loop restarting the fold, and the tick wrapping. */
#include "radio_slot_logic.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t rng = 12345U;
static float frand(void)
{
    rng = rng * 1664525U + 1013904223U;
    return (float)(rng >> 8) / 16777216.0f;
}

/* Passband power at absolute time t_ms (boundary at multiples of 15000 + b). */
static float band_power(uint32_t t_ms, uint32_t boundary_ms, float busy_db, float dt_s, int cadence)
{
    float p = 1.0f * (0.7f + 0.6f * frand()); /* noise */
    if (!cadence) return p * (1.0f + 3.0f * frand());
    const uint32_t ph = (t_ms - boundary_ms) % 15000U;
    const float s = (float)ph * 0.001f;
    if (s >= 0.5f + dt_s && s <= 13.14f + dt_s) p += powf(10.0f, busy_db / 10.0f) * (0.5f + frand());
    return p;
}

static int32_t circ_err(int32_t a, int32_t b)
{
    int32_t d = (a - b) % 15000;
    if (d > 7500) d -= 15000;
    if (d < -7500) d += 15000;
    return d;
}

static void run(uint32_t start, uint32_t boundary, float busy_db, float dt_s, int cadence,
                uint32_t seconds, radio_slot_t *s)
{
    RADIO_SlotInit(s, start);
    uint32_t t = start;
    for (uint32_t end = start + seconds * 1000U; (int32_t)(end - t) > 0;) {
        (void)RADIO_SlotAdd(s, t, band_power(t, boundary, busy_db, dt_s, cadence));
        t += 5U + (uint32_t)(frand() * 25.0f); /* main loop: 5-30 ms a pass */
    }
}

/* The gap judge: one host pulling 48 frames a millisecond, one pass a ms. */
static void gap_judge(void)
{
    radio_gap_judge_t g;
    uint32_t bad = 7U, sent = 1000U, heard = 0U;
    RADIO_GapInit(&g, bad);
    /* Clean streaming: nothing. */
    for (int ms = 0; ms < 100; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    assert(heard == 0U);
    /* A real gap: a run of losses over 5 ms, the host pulling on: one gap,
     * once 20 ms have been pulled past its start. */
    for (int ms = 0; ms < 5; ++ms) { sent += 48U; bad += 3U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    for (int ms = 0; ms < 14; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    assert(heard == 0U);
    for (int ms = 0; ms < 2; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    assert(heard == 1U);
    for (int ms = 0; ms < 100; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    assert(heard == 1U);
    /* Sustained trouble (a loss every 5 ms for 200 ms) keeps counting: about
     * one gap per 20 ms pulled. */
    heard = 0U;
    for (int ms = 0; ms < 200; ++ms) {
        sent += 48U;
        if (ms % 5 == 0) bad++;
        heard += RADIO_GapStep(&g, 1U, bad, sent);
    }
    assert(heard >= 8U && heard <= 10U);
    for (int ms = 0; ms < 30; ++ms) { sent += 48U; (void)RADIO_GapStep(&g, 1U, bad, sent); }
    /* The host stops pulling (PipeWire suspends the source): the ring fills
     * and overflows for 200 ms until "live" drops. No gap. */
    heard = 0U;
    for (int ms = 0; ms < 3; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    for (int ms = 0; ms < 200; ++ms) { bad += 48U; heard += RADIO_GapStep(&g, ms < 180 ? 1U : 0U, bad, sent); }
    assert(heard == 0U);
    /* The next stream starts: the old overflow is not carried into it. */
    for (int ms = 0; ms < 100; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    assert(heard == 0U);
    /* Losses while nobody is live (the start's settling second) never count. */
    for (int ms = 0; ms < 50; ++ms) { bad++; heard += RADIO_GapStep(&g, 0U, bad, sent); }
    for (int ms = 0; ms < 100; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, bad, sent); }
    assert(heard == 0U);
    /* The counters wrap. */
    RADIO_GapInit(&g, 0xFFFFFFFFU);
    sent = 0xFFFFFF00U;
    heard = RADIO_GapStep(&g, 1U, 0U, sent);
    for (int ms = 0; ms < 25; ++ms) { sent += 48U; heard += RADIO_GapStep(&g, 1U, 0U, sent); }
    assert(heard == 1U);
}

int main(void)
{
    radio_slot_t s;
    /* Not before three slots. */
    run(1000U, 7000U, 6.0f, 0.3f, 1, 40U, &s);
    assert(!s.locked);
    assert(RADIO_SlotPhaseMs(&s, 41000U) == -1);
    assert(!RADIO_SlotQuiet(&s, 41000U));

    /* Locks after a few slots, boundary within 0.3 s plus the DT. */
    for (uint32_t b = 0U; b < 15000U; b += 1700U) {
        run(1000U, b, 6.0f, 0.3f, 1, 75U, &s);
        assert(s.locked == 1U);
        assert(s.depth_db > 4.0f);
        const uint32_t now = 1000U + 75000U;
        const int32_t phase = RADIO_SlotPhaseMs(&s, now);
        const int32_t truth = (int32_t)((now - b) % 15000U);
        const int32_t err = circ_err(phase, truth);
        /* The estimate is late by about the DT (the observed gap is late). */
        if (err > 100 || err < -650) {
            printf("boundary %u: phase %d truth %d err %d\n", (unsigned)b, (int)phase, (int)truth, (int)err);
            return 1;
        }
        /* Quiet only inside the gap. */
        uint32_t quiet = 0U;
        for (uint32_t t = now; t < now + 15000U; t += 10U) {
            if (!RADIO_SlotQuiet(&s, t)) continue;
            ++quiet;
            float sec = (float)((t - b) % 15000U) * 0.001f;
            if (sec < 0.6f) sec += 15.0f; /* just past the boundary: still the gap */
            if (!(sec > 13.14f + 0.3f - 0.05f && sec < 15.0f + 0.5f)) { printf("b %u quiet at %.2f s, err %d\n", (unsigned)b, (double)sec, (int)err); return 1; }
        }
        assert(quiet > 100U); /* > 1 s of it */
    }

    /* A weak band (1 dB busy) does not lock; no cadence does not lock. */
    run(1000U, 3000U, -6.0f, 0.3f, 1, 90U, &s);
    assert(!s.locked);
    run(1000U, 3000U, 6.0f, 0.3f, 0, 90U, &s);
    assert(!s.locked);

    /* A host key that agrees with the band (WSJT-X at the boundary) refines
     * the phase and band learning keeps it; one 3.8 s off (Tune) is ignored. */
    run(1000U, 5000U, 6.0f, 0.3f, 1, 75U, &s);
    assert(s.locked == 1U);
    assert(!RADIO_SlotSync(&s, 76200U, 0U)); /* 1200 mod 15000 */
    assert(s.locked == 1U);
    assert(RADIO_SlotSync(&s, 79900U, 200U)); /* boundary 80.1 s: 5100 mod 15000 */
    assert(s.locked == 2U);
    assert(abs(circ_err(RADIO_SlotPhaseMs(&s, 80100U), 0)) <= 50);
    for (uint32_t t = 80000U; t < 125000U; t += 20U) (void)RADIO_SlotAdd(&s, t, band_power(t, 5000U, 6.0f, 0.3f, 1));
    assert(s.locked == 2U);
    assert(abs(circ_err(RADIO_SlotPhaseMs(&s, 125100U), 0)) <= 50);
    /* A stalled main loop (20 s) restarts the fold but keeps the host phase. */
    (void)RADIO_SlotAdd(&s, 145000U, 1.0f);
    assert(s.locked == 2U);
    assert(abs(circ_err(RADIO_SlotPhaseMs(&s, 155100U), 0)) <= 100);

    /* A key while still learning (a Tune press) is taken; the band, once it
     * locks 2.8 s away, takes over. */
    RADIO_SlotInit(&s, 1000U);
    assert(RADIO_SlotSync(&s, 2200U, 0U));
    assert(s.locked == 2U);
    for (uint32_t t = 1000U; t < 76000U; t += 20U) (void)RADIO_SlotAdd(&s, t, band_power(t, 5000U, 6.0f, 0.3f, 1));
    assert(s.locked == 1U);
    {
        const int32_t err = circ_err(RADIO_SlotPhaseMs(&s, 76000U), (int32_t)((76000U - 5000U) % 15000U));
        assert(err <= 100 && err >= -650);
    }
    /* A band without the cadence keeps a host phase. */
    RADIO_SlotInit(&s, 1000U);
    assert(RADIO_SlotSync(&s, 2200U, 0U));
    for (uint32_t t = 1000U; t < 76000U; t += 20U) (void)RADIO_SlotAdd(&s, t, band_power(t, 5000U, 6.0f, 0.3f, 0));
    assert(s.locked == 2U);
    assert(abs(circ_err(RADIO_SlotPhaseMs(&s, 77200U), 0)) <= 50);

    /* The tick wraps. */
    const uint32_t start = 0xFFFFFFFFU - 40000U;
    const uint32_t b = start + 9000U;
    run(start, b, 6.0f, 0.3f, 1, 90U, &s);
    assert(s.locked == 1U);
    const uint32_t now = start + 90000U; /* past the wrap */
    const int32_t err = circ_err(RADIO_SlotPhaseMs(&s, now), (int32_t)((uint32_t)(now - b) % 15000U));
    assert(err <= 100 && err >= -650);

    gap_judge();
    printf("slot_logic: PASS\n");
    return 0;
}

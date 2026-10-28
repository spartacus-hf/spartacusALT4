#ifndef RADIO_GUARD_LOGIC_H
#define RADIO_GUARD_LOGIC_H

/* The protection trips that need nothing but one ADC scan (4 kHz,
 * DMA2_Channel1_IRQHandler in radio_analog.c), beside the limits the
 * interrupt already holds (VIN over, the 5 V rail, bias, envelope, SWR over
 * the limit):
 *
 * - VIN collapsing while TRANSMIT. The PD contract normally sags from 9 V to
 *   7-8 V at full drain (bench, 2026-10-02). Under max(6.3 V, 70 % of VIN
 *   with TX off) for 2 ms the source is folding back or the PA draws what it
 *   must not -- REV1 has no current sense, so this is the overcurrent trip
 *   too -- and the LM1084-5 is about to let the 5 V rail go.
 * - |gamma| jumping within an over, under the SWR limit: a wire breaking, an
 *   arc, a connector letting go. Learnt from the over's first 16 ms of strong
 *   forward RF (0.5 W up, where the bridge leakage's constant floor moves
 *   |gamma| by under 0.09), then 0.25 over it for 2 ms of strong scans
 *   trips. TX TEST is where a manual tuner is turned with the carrier up:
 *   the limit alone there.
 * - RF on the bridge with TX not permitted: 0.1 W forward for 2 ms, outside
 *   the detectors' 100 ms hold after RF, is a driver that did not stop or a
 *   transmitter close by. Either way the trip takes the receiver off too.
 *
 * Each returns 1 when its trip is due. Every count needs GUARD_SCANS scans in
 * a row: one noisy scan, a key edge's detector skew or a load step never
 * trips. A few compares a scan, one division a scan for the first 16 ms of
 * an over. */

#include <stdint.h>
#include "radio_rf_meter.h"

#define RADIO_GUARD_VIN_FLOOR_MV  6300U   /* the LM1084-5's input for a 5 V rail */
#define RADIO_GUARD_VIN_SHARE_X64 45U     /* 45/64: 70 % of VIN with TX off */
#define RADIO_GUARD_SWR_STRONG_V  0.707f  /* 0.5 W forward (QMX's gate) */
#define RADIO_GUARD_SWR_LEARN     64U     /* strong scans an over's |gamma| is learnt from */
#define RADIO_GUARD_SWR_JUMP      0.25f   /* |gamma| over the learnt one: SWR 1.2 -> 2.0 */
#define RADIO_GUARD_SCANS         8U      /* in a row: 2 ms, as the SWR limit trip */

typedef struct {
    uint8_t vin_idle;     /* VIN with TX not permitted, 64 mV units */
    uint8_t swr_base_q7;  /* the over's learnt |gamma|, Q7 */
    uint8_t swr_n;        /* strong scans learnt so far */
    uint8_t vin_low : 4;  /* TX: scans of VIN under the floor */
    uint8_t run : 4;      /* TX: scans of a |gamma| jump; off: scans of stray RF */
} radio_guard_t;
_Static_assert(RADIO_GUARD_SCANS < 16U, "the guard counts are four bits");
_Static_assert(RADIO_GUARD_SWR_LEARN < 256U, "swr_n is a byte");

/* TX not permitted: VIN learnt, the over's state cleared. `judge` is 0
 * while RF is active or the detectors still hold it; `f` is the forward
 * port volts, read only when judged. */
static inline uint8_t radio_guard_idle(radio_guard_t *g, uint16_t vin_mv, uint8_t judge, float f)
{
    const uint32_t units = (uint32_t)vin_mv >> 6;
    g->vin_idle = (uint8_t)(units > 255U ? 255U : units);
    g->vin_low = 0U;
    g->swr_n = 0U;
    if (!judge || !(f >= RF_METER_GATE_V)) { g->run = 0U; return 0U; }
    if (g->run < RADIO_GUARD_SCANS) ++g->run;
    return (uint8_t)(g->run >= RADIO_GUARD_SCANS);
}

/* The VIN floor this over: 70 % of VIN with TX off, 6.3 V at least. */
static inline uint32_t radio_guard_vin_floor_mv(const radio_guard_t *g)
{
    const uint32_t floor = (uint32_t)g->vin_idle * RADIO_GUARD_VIN_SHARE_X64;
    return floor > RADIO_GUARD_VIN_FLOOR_MV ? floor : RADIO_GUARD_VIN_FLOOR_MV;
}

/* TX permitted, every scan. */
static inline uint8_t radio_guard_tx_vin(radio_guard_t *g, uint16_t vin_mv)
{
    if (g->swr_n < RADIO_GUARD_SWR_LEARN) g->run = 0U; /* no stray-RF count carried into TX */
    if ((uint32_t)vin_mv >= radio_guard_vin_floor_mv(g)) { g->vin_low = 0U; return 0U; }
    if (g->vin_low < RADIO_GUARD_SCANS) ++g->vin_low;
    return (uint8_t)(g->vin_low >= RADIO_GUARD_SCANS);
}

/* TX with RF active: the 1 ms means of forward and reflected port volts
 * (bridge leakage left in, as the limit trip has it). `tune` is TX TEST. */
static inline uint8_t radio_guard_swr(radio_guard_t *g, float f, float r, uint8_t tune)
{
    if (!(f >= RADIO_GUARD_SWR_STRONG_V)) { g->run = 0U; return 0U; }
    float base = (float)g->swr_base_q7 * (1.0f / 128.0f);
    if (g->swr_n < RADIO_GUARD_SWR_LEARN) {
        float gamma = r / f;
        if (!(gamma >= 0.0f)) gamma = 0.0f;
        if (gamma > 1.99f) gamma = 1.99f; /* the limit trip has long gone by then */
        base += (gamma - base) / (float)(g->swr_n + 1U);
        g->swr_base_q7 = (uint8_t)(base * 128.0f + 0.5f);
        ++g->swr_n;
        return 0U;
    }
    if (tune || !(r > f * (base + RADIO_GUARD_SWR_JUMP))) { g->run = 0U; return 0U; }
    if (g->run < RADIO_GUARD_SCANS) ++g->run;
    return (uint8_t)(g->run >= RADIO_GUARD_SCANS);
}

#endif

#ifndef RADIO_SLOT_LOGIC_H
#define RADIO_SLOT_LOGIC_H
#include <stdint.h>

/* The FT8 slot clock, learnt from the band itself. FT8 transmits from 0.5 s to
 * 13.14 s of every 15 s UTC slot, so the passband's power drops for ~2.4 s
 * before each boundary. The receiver's channel power, summed in 100 ms bins
 * and folded modulo 15 s on the radio's millisecond tick, shows that gap
 * after a few slots; its position is the slot phase. A transmission keyed
 * by the host (CAT PTT, which WSJT-X raises at the boundary) refines it.
 * The radio needs no UTC: only "where in the slot are we", so that a
 * receiver restart, an autosave or a card write can wait for the gap
 * instead of tearing through a slot's signals.
 *
 * Pure logic (host suite slot_logic): the caller feeds power and time. */
#define RADIO_SLOT_MS        15000U
#define RADIO_SLOT_BIN_MS    100U
#define RADIO_SLOT_BINS      (RADIO_SLOT_MS / RADIO_SLOT_BIN_MS)
/* The quiet stretch searched for, and the part of it the scheduler uses. */
#define RADIO_SLOT_GAP_BINS  20U   /* 2.0 s */
#define RADIO_SLOT_LOCK_DB   2.0f  /* busy part this much over the gap */
#define RADIO_SLOT_LOCK_SLOTS 3U   /* folded slots before a lock */

typedef struct {
    float fold[RADIO_SLOT_BINS]; /* mean power per bin, exponential over slots */
    float acc;                   /* the bin being summed */
    uint32_t acc_n;
    uint32_t origin_ms;          /* tick of fold bin 0 */
    uint32_t bin_start_ms;
    uint16_t bin;                /* fold index of the bin being summed */
    uint16_t slots;              /* complete passes over the fold */
    uint16_t boundary_bin;       /* the slot boundary, as a fold bin */
    uint8_t locked;              /* 1 band-learnt, 2 set by a host TX */
    float depth_db;              /* busy median over the gap, dB */
} radio_slot_t;

void RADIO_SlotInit(radio_slot_t *s, uint32_t now_ms);
/* One power sample (linear, any unit) at now_ms. Called from the main loop as
 * often as it likes; samples are averaged per bin. Returns 1 when a bin
 * completed (and the estimate may have moved). */
uint8_t RADIO_SlotAdd(radio_slot_t *s, uint32_t now_ms, float power);
/* A host key (CAT PTT) at now_ms with the boundary lead_ms ahead. WSJT-X
 * raises PTT at the boundary from a 100 ms GUI timer and starts FT8 audio at
 * 0.5 s, so lead 0 puts the phase within ~0.1 s. Taken while learning, or
 * when it agrees with the band's phase within 1 s; a key elsewhere (Tune, a
 * late start) is ignored. Returns 1 when taken. */
uint8_t RADIO_SlotSync(radio_slot_t *s, uint32_t now_ms, uint32_t lead_ms);
/* Milliseconds since the last slot boundary, or -1 with no lock. */
int32_t RADIO_SlotPhaseMs(const radio_slot_t *s, uint32_t now_ms);
/* 1 inside the quiet part of the gap (13.4 s .. 14.7 s), where a ~10 ms
 * receiver restart or a card write costs no signal; 0 otherwise or unlocked. */
uint8_t RADIO_SlotQuiet(const radio_slot_t *s, uint32_t now_ms);

/* Gaps the host heard. The stream loses audio when silence is padded in, the
 * ring overflows or the receiver misses a half. A host that stops pulling
 * (PipeWire suspending the source, a program closing) leaves the ring to
 * fill and overflow until the interface closes: audio nobody receives, the
 * end of a stream, not a gap. So a loss counts only once the host has gone
 * on pulling RADIO_GAP_HEARD_FRAMES after it, and losses before that are
 * one gap with it. Pure logic (host suite slot_logic). */
#define RADIO_GAP_HEARD_FRAMES 960U /* 20 ms at 48 kHz */
typedef struct {
    uint32_t bad;   /* the loss counter as last seen */
    uint32_t sent;  /* frames the host had taken when the open gap began */
    uint8_t open;   /* a loss waits for the host to pull past it */
} radio_gap_judge_t;
void RADIO_GapInit(radio_gap_judge_t *g, uint32_t bad);
/* Once per pass. live: the host is pulling (and settled); bad: the loss
 * counter; sent: frames the host has taken. Returns 1 when a gap is heard. */
uint8_t RADIO_GapStep(radio_gap_judge_t *g, uint8_t live, uint32_t bad, uint32_t sent);
#endif

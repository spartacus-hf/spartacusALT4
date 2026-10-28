#ifndef RADIO_DIGI_H
#define RADIO_DIGI_H
#include <stdint.h>

/* DIGI awareness, main loop only: what the operator and the scheduler need to
 * know about a digital-mode session, gathered once per pass (RADIO_DigiService)
 *   - the FT8 slot clock learnt from the passband (radio_slot_logic.h);
 *   - the host capture stream's health (RADIO_DigiStats): whether the host
 *     is pulling it, gaps in the last ten minutes and in the slot now;
 *   - the reference: following the host or local, its ppm, the tracker's
 *     state and its re-acquisitions (each a frequency step) in ten minutes;
 *   - the receive worker's share of the core. */
typedef struct {
    uint8_t host_live;    /* the host pulled capture frames in the last 0.2 s */
    int32_t slot_ms;      /* ms since the slot boundary, -1 unlocked */
    uint8_t slot_locked;  /* 0 learning, 1 from the band, 2 from a host TX */
    float   slot_depth_db;
    uint32_t gaps_10min;  /* gaps the host heard (RADIO_GapStep), last 10 min */
    uint32_t gaps_slot;   /* in the slot running now */
    uint8_t follow;       /* 1 = REF SOURCE USB HOST */
    uint8_t ref_state;    /* 0 no host, 1 acquiring, 2 locked */
    float   ref_ppm;      /* the tracker's trim, mean over 10 s */
    uint32_t reacq_10min; /* tracker re-acquisitions, last 10 min */
    uint8_t load_pct;     /* receive worker, % of the core over ~1 s */
} radio_digi_status_t;

void RADIO_DigiService(uint32_t now_ms);
void RADIO_DigiStatus(radio_digi_status_t *out, uint32_t now_ms);
/* USB bus reset (interrupt context): the tracker's next acquisitions belong
 * to a new host, not steps. */
void RADIO_DigiUsbReset(void);
/* CAT PTT raised: WSJT-X keys at the slot boundary (RADIO_SlotSync). */
void RADIO_DigiHostKey(uint32_t now_ms);
/* 1 while a deferrable disruption (a receiver restart, a card write) costs
 * no FT8 signal: in the slot gap, or no lock and no stream to protect. */
uint8_t RADIO_DigiQuiet(uint32_t now_ms);
#endif

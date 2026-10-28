#ifndef RADIO_MIC_MONITOR_H
#define RADIO_MIC_MONITOR_H
#include <stdint.h>

/* Rules for the console's microphone monitor (radio_audio.h), kept pure so
 * they can be exercised on a host. The monitor routes a microphone into the
 * USB capture endpoint without transmitting, so the things that must end it
 * are the ones that would make that either wrong or unsafe: a PTT press,
 * which wants the microphone for the modulator; leaving RECEIVE, which takes
 * away the receiver workspace holding the capture ring; storage mode; and the
 * window's own deadline, so a console session cannot leave the ADC running.
 *
 * Deadlines are compared as a signed tick difference, so the rule survives
 * the HAL_GetTick() wrap. */
typedef enum {
    RADIO_MIC_MONITOR_IDLE = 0, /* not monitoring: leave the PTT path alone */
    RADIO_MIC_MONITOR_HOLD,     /* monitoring, ADC already on the right source */
    RADIO_MIC_MONITOR_START,    /* monitoring, put the ADC on `requested` */
    RADIO_MIC_MONITOR_END       /* stop monitoring and release the ADC */
} radio_mic_monitor_action_t;

typedef struct {
    uint8_t  requested;  /* RADIO_SOURCE_MICn asked for, 0 when off */
    uint8_t  running;    /* microphone source the ADC is on, 0 when none */
    uint8_t  receiving;  /* RADIO_State() == RADIO_RECEIVE */
    uint8_t  ptt;
    uint8_t  storage;
    uint32_t now_ms;
    uint32_t until_ms;
} radio_mic_monitor_state_t;

static inline radio_mic_monitor_action_t
RADIO_MicMonitorDecide(const radio_mic_monitor_state_t *s)
{
    if (!s->requested) return RADIO_MIC_MONITOR_IDLE;
    if (!s->receiving || s->ptt || s->storage) return RADIO_MIC_MONITOR_END;
    if ((int32_t)(s->now_ms - s->until_ms) >= 0) return RADIO_MIC_MONITOR_END;
    return (s->running == s->requested) ? RADIO_MIC_MONITOR_HOLD
                                        : RADIO_MIC_MONITOR_START;
}

#endif

#include "radio_mic_monitor.h"
#include "radio_config.h"
#include <assert.h>
#include <stdio.h>

/* The console's microphone monitor routes a microphone into the USB capture
 * endpoint without transmitting. These are the rules that keep it from
 * outliving the conditions that make it valid. */

static radio_mic_monitor_state_t running_monitor(void)
{
    radio_mic_monitor_state_t s = {
        .requested = RADIO_SOURCE_MIC2, .running = RADIO_SOURCE_MIC2,
        .receiving = 1U, .ptt = 0U, .storage = 0U,
        .now_ms = 1000U, .until_ms = 31000U,
    };
    return s;
}

static void idle_when_not_asked_for(void)
{
    radio_mic_monitor_state_t s = running_monitor();
    s.requested = 0U;
    /* Nothing requested: the PTT microphone path must be left alone even when
     * every other field would otherwise end a monitor. */
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_IDLE);
    s.ptt = 1U; s.receiving = 0U; s.storage = 1U; s.now_ms = s.until_ms + 1U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_IDLE);
}

static void starts_and_holds(void)
{
    radio_mic_monitor_state_t s = running_monitor();
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_HOLD);
    s.running = 0U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_START);
    /* The PTT path may have left the ADC on another microphone. */
    s.running = RADIO_SOURCE_MIC1;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_START);
    s.running = RADIO_SOURCE_MIC3;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_START);
    /* Every source the monitor accepts behaves the same way. */
    for (uint8_t src = RADIO_SOURCE_MIC1; src <= RADIO_SOURCE_MIC3; ++src) {
        s.requested = src; s.running = src;
        assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_HOLD);
    }
}

static void ends_on_every_disqualifier(void)
{
    radio_mic_monitor_state_t s;
    /* PTT: the modulator wants the same ADC, and the monitor never transmits. */
    s = running_monitor(); s.ptt = 1U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_END);
    /* Leaving RECEIVE takes away the workspace holding the capture ring. */
    s = running_monitor(); s.receiving = 0U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_END);
    s = running_monitor(); s.storage = 1U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_END);
    /* The deadline ends it whether or not the ADC ever started. */
    s = running_monitor(); s.now_ms = s.until_ms;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_END);
    s = running_monitor(); s.now_ms = s.until_ms + 1U; s.running = 0U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_END);
    /* One tick short of it does not. */
    s = running_monitor(); s.now_ms = s.until_ms - 1U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_HOLD);
}

static void deadline_survives_the_tick_wrap(void)
{
    radio_mic_monitor_state_t s = running_monitor();
    /* A window opened just before HAL_GetTick() wraps: 30 s still means 30 s. */
    s.now_ms = 0xFFFFF000UL;
    s.until_ms = s.now_ms + 30000U;        /* wraps to 0x00006C2F */
    assert(s.until_ms < s.now_ms);
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_HOLD);
    s.now_ms = 0xFFFFFFFFUL;               /* one tick before the wrap */
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_HOLD);
    s.now_ms = 0x00000000UL;               /* just after it */
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_HOLD);
    s.now_ms = s.until_ms - 1U;
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_HOLD);
    s.now_ms = s.until_ms;                 /* the deadline, on the far side */
    assert(RADIO_MicMonitorDecide(&s) == RADIO_MIC_MONITOR_END);
}

int main(void)
{
    idle_when_not_asked_for();
    starts_and_holds();
    ends_on_every_disqualifier();
    deadline_survives_the_tick_wrap();
    printf("REV1 microphone monitor: idle, start/hold, every disqualifier and the tick wrap: PASS\n");
    return 0;
}

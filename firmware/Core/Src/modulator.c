#include "radio.h"
/**
 * @file    modulator.c
 * @brief   Chireix outphasing multi-mode modulator orchestration.
 */

#include "mod_priv.h"
#include "radio_config.h"
#include "mod_morse_trainer.h"

static uint32_t sat_inc_u32(uint32_t v)
{
    return (v == 0xFFFFFFFFUL) ? v : (v + 1UL);
}

#if MOD_DEEP_DIAG_ENABLE
void mod_diag_capture(uint32_t branch_a, uint32_t branch_c,
                      float envelope, float theta, float phi)
{
    if (cfg_runtime_diag_enable == 0U) return;
    diag_branch_a = branch_a;
    diag_branch_c = branch_c;
    diag_envelope = envelope;
    diag_theta_deg = theta * 180.0f;
    diag_phi_deg = phi * 180.0f;
}
#endif

/* Packed storage: stage is 0..7 and severity 0..3, so they share a byte.
 * Plain SRAM rather than CCM -- the deep-diagnostic flavour has the least CCM
 * left of the three, and this is touched once per event, not per sample. */
typedef struct {
    uint32_t value;
    uint16_t tick_ms;        /* low 16 bits of HAL_GetTick */
    uint8_t  stage_severity; /* stage in bits 0..3, severity in bits 4..7 */
    uint8_t  reason;
} pipeline_log_entry_t;
_Static_assert(sizeof(pipeline_log_entry_t) == 8U, "pipeline log entry must stay 8 bytes");
_Static_assert(MOD_PIPELINE_STAGE_COUNT <= 16U && MOD_PIPELINE_REASON_COUNT <= 256U,
               "stage must fit a nibble and reason a byte");
static pipeline_log_entry_t pipeline_log[MOD_PIPELINE_LOG_DEPTH];
static uint8_t pipeline_log_next;   /* next slot to write */
static uint8_t pipeline_log_count;  /* entries held, saturating at the depth */
static uint8_t pipeline_log_seq;    /* entries ever logged, wrapping: what is new to a reader */

static mod_pipeline_severity_t pipeline_default_severity(mod_pipeline_reason_t reason)
{
    switch (reason) {
        case MOD_PIPELINE_REASON_NONE:
        case MOD_PIPELINE_REASON_SCHED_IO_DEFERRED:
        case MOD_PIPELINE_REASON_SCHED_UI_DEFERRED:
        case MOD_PIPELINE_REASON_CONTROLLED_STREAM_RESET:
            return MOD_PIPELINE_SEVERITY_NONE;
        case MOD_PIPELINE_REASON_PCM_RESET_DISCARD:
        case MOD_PIPELINE_REASON_RENDER_RESET_DISCARD:
            /* A frame in flight when its ring was flushed. Every flush is
             * deliberate -- the host closing or changing the stream, a
             * transport reset or resync, a retune, a released TX TEST paddle
             * -- and those that matter are reported themselves; the frame
             * belonged to what was flushed. As HARD, a host ending an over
             * while the renderer was mid-pop latched a lockout and left the
             * radio in PROTECTION (FAULT_DMA) until faults were cleared
             * (bench, 2026-09-28: one of four natural USB-audio overs).
             * Counted, not latched. */
            return MOD_PIPELINE_SEVERITY_NONE;
        case MOD_PIPELINE_REASON_USB_FEEDBACK_TX_FAILED:
            /* A feedback packet the host did not collect is a rate hint that
             * was superseded before it was read, not lost audio and not a
             * pipeline fault: the next SOF queues a fresher value. Latching a
             * sticky full-screen notice for it blanked the UI the first time
             * any host opened the stream.
             *
             * It stays counted in pipeline_health.reason_events, which the
             * console reports under `pipeline`. If the synch endpoint really
             * is dead, the host's sample rate drifts and that surfaces as
             * PCM overflow or render underrun -- which are concealed or hard
             * and do latch, because those are the conditions that cost audio. */
            return MOD_PIPELINE_SEVERITY_NONE;
        case MOD_PIPELINE_REASON_USB_MISSING_PACKET:
        case MOD_PIPELINE_REASON_USB_BAD_PACKET_SIZE:
        case MOD_PIPELINE_REASON_USB_ZERO_PACKET:
            return MOD_PIPELINE_SEVERITY_CONCEALED;
        default:
            return MOD_PIPELINE_SEVERITY_HARD;
    }
}

void mod_pipeline_note_severity(mod_pipeline_stage_t stage, mod_pipeline_reason_t reason,
                                mod_pipeline_severity_t severity, uint32_t value)
{
    uint8_t enter_hard_lockout = 0U;
    if ((uint32_t)stage >= (uint32_t)MOD_PIPELINE_STAGE_COUNT) stage = MOD_PIPELINE_STAGE_NONE;
    if ((uint32_t)reason >= (uint32_t)MOD_PIPELINE_REASON_COUNT) reason = MOD_PIPELINE_REASON_NONE;
    if ((uint32_t)severity > (uint32_t)MOD_PIPELINE_SEVERITY_HARD) {
        severity = MOD_PIPELINE_SEVERITY_HARD;
    }

    uint32_t tick = HAL_GetTick();
    uint32_t pm = enter_critical();
    pipeline_health.total_events = sat_inc_u32(pipeline_health.total_events);
    pipeline_health.stage_events[stage] = sat_inc_u32(pipeline_health.stage_events[stage]);
    pipeline_health.reason_events[reason] = sat_inc_u32(pipeline_health.reason_events[reason]);
    if ((pipeline_health.sticky_latched == 0U) ||
        ((severity != MOD_PIPELINE_SEVERITY_NONE) &&
         ((uint8_t)severity >= pipeline_health.last_severity))) {
        pipeline_health.last_tick_ms = tick;
        pipeline_health.last_value = value;
        pipeline_health.last_stage = (uint8_t)stage;
        pipeline_health.last_reason = (uint8_t)reason;
        pipeline_health.last_severity = (uint8_t)severity;
    }
    if (reason == MOD_PIPELINE_REASON_CONTROLLED_STREAM_RESET) {
        pipeline_health.controlled_reset_events = sat_inc_u32(pipeline_health.controlled_reset_events);
    }
    /* Already inside the critical section, so the ring needs no lock of its
     * own. Scheduler deferrals are routine and would evict real events. */
    if ((reason != MOD_PIPELINE_REASON_SCHED_IO_DEFERRED) &&
        (reason != MOD_PIPELINE_REASON_SCHED_UI_DEFERRED)) {
        pipeline_log_entry_t *slot = &pipeline_log[pipeline_log_next];
        slot->tick_ms = (uint16_t)tick;
        slot->value = value;
        slot->stage_severity = (uint8_t)(((uint8_t)stage & 0x0FU) |
                                         (uint8_t)(((uint8_t)severity & 0x0FU) << 4));
        slot->reason = (uint8_t)reason;
        pipeline_log_next = (uint8_t)((pipeline_log_next + 1U) % MOD_PIPELINE_LOG_DEPTH);
        if (pipeline_log_count < MOD_PIPELINE_LOG_DEPTH) pipeline_log_count++;
        pipeline_log_seq++;
    }
    if (severity != MOD_PIPELINE_SEVERITY_NONE) {
        pipeline_health.sticky_stage_mask |= (1UL << (uint32_t)stage);
        if (pipeline_health.sticky_latched == 0U) {
            pipeline_health.first_tick_ms = tick;
            pipeline_health.first_value = value;
            pipeline_health.first_stage = (uint8_t)stage;
            pipeline_health.first_reason = (uint8_t)reason;
            pipeline_health.first_severity = (uint8_t)severity;
        }
        pipeline_health.sticky_latched = 1U;
    }
    if ((severity == MOD_PIPELINE_SEVERITY_CONCEALED) ||
        (severity == MOD_PIPELINE_SEVERITY_HARD)) {
        pipeline_health.soft_fault_events = sat_inc_u32(pipeline_health.soft_fault_events);
        pipeline_health.soft_fault_latched = 1U;
    }
    if (severity == MOD_PIPELINE_SEVERITY_HARD) {
        enter_hard_lockout = (pipeline_hard_lockout == 0U) ? 1U : 0U;
        pipeline_hard_lockout = 1U;
        pipeline_health.hard_lockout = 1U;
    }
    exit_critical(pm);

    if (severity != MOD_PIPELINE_SEVERITY_NONE) {
        LED_GPIO_Port->BSRR = LED_Pin;
    }
    if (enter_hard_lockout != 0U) {
        mod_pipeline_emergency_stop();
    }
    /* Into the SD event log: every hard event, a concealed one at most once
     * a second (a stuttering USB stream would otherwise fill it). */
    static uint32_t concealed_logged_at;
    if ((severity == MOD_PIPELINE_SEVERITY_HARD) ||
        ((severity == MOD_PIPELINE_SEVERITY_CONCEALED) && (uint32_t)(tick - concealed_logged_at) >= 1000U)) {
        if (severity == MOD_PIPELINE_SEVERITY_CONCEALED) concealed_logged_at = tick;
        MOD_EventLog(MOD_EVENT_PIPELINE, (uint8_t)reason, (uint16_t)(((uint16_t)stage << 8) | (uint16_t)severity),
                     value, (uint32_t)active_config);
    }
}

void mod_pipeline_note(mod_pipeline_stage_t stage, mod_pipeline_reason_t reason, uint32_t value)
{
    mod_pipeline_note_severity(stage, reason, pipeline_default_severity(reason), value);
}

void mod_pipeline_note_controlled_reset(void)
{
    mod_pipeline_note(MOD_PIPELINE_STAGE_CONTROL,
                      MOD_PIPELINE_REASON_CONTROLLED_STREAM_RESET,
                      (uint32_t)active_config);
}

__attribute__((cold)) void MOD_ClearPipelineHealth(void)
{
    MOD_RequestPipelineFaultAcknowledge();
}

uint8_t MOD_PipelineEventSeq(void) { return pipeline_log_seq; }

uint8_t MOD_GetPipelineEvent(uint8_t index, mod_pipeline_event_t *out)
{
    pipeline_log_entry_t entry;
    uint8_t ok = 0U;
    if (out == NULL) return 0U;
    const uint16_t now = (uint16_t)HAL_GetTick();
    uint32_t pm = enter_critical();
    if (index < pipeline_log_count) {
        const uint8_t slot = (uint8_t)((pipeline_log_next + MOD_PIPELINE_LOG_DEPTH - 1U - index)
                                       % MOD_PIPELINE_LOG_DEPTH);
        entry = pipeline_log[slot];
        ok = 1U;
    }
    exit_critical(pm);
    if (!ok) return 0U;
    out->value = entry.value;
    out->age_ms = (uint16_t)(now - entry.tick_ms);
    out->stage = (uint8_t)(entry.stage_severity & 0x0FU);
    out->severity = (uint8_t)(entry.stage_severity >> 4);
    out->reason = entry.reason;
    return 1U;
}

void MOD_GetPipelineHealthSnapshot(mod_pipeline_health_t *snapshot)
{
    if (snapshot == NULL) return;
    uint32_t pm = enter_critical();
    memcpy(snapshot, (const void *)&pipeline_health, sizeof(*snapshot));
    exit_critical(pm);
}

void MOD_RequestPipelineFaultAcknowledge(void)
{
    uint32_t pm = enter_critical();
    if (pipeline_hard_lockout != 0U) {
        pipeline_ack_requested = 1U;
        pipeline_rearm_pending = 1U;
        pipeline_health.rearm_pending = 1U;
    } else {
        pipeline_health.soft_fault_latched = 0U;
        pipeline_health.sticky_latched = 0U;
        pipeline_health.sticky_stage_mask = 0U;
        pipeline_health.last_severity = MOD_PIPELINE_SEVERITY_NONE;
        pipeline_health.first_severity = MOD_PIPELINE_SEVERITY_NONE;
        LED_GPIO_Port->BRR = LED_Pin;
    }
    exit_critical(pm);
}

uint8_t MOD_PipelineRfLockedOut(void)
{
    return pipeline_hard_lockout ? 1U : 0U;
}

uint8_t MOD_PipelineNotificationPending(void)
{
    return pipeline_health.sticky_latched ? 1U : 0U;
}

mod_build_flavor_t MOD_GetBuildFlavor(void)
{
#if MOD_DEEP_DIAG_ENABLE
    return MOD_BUILD_FLAVOR_DIAG;
#else
    return MOD_BUILD_FLAVOR_PROD;
#endif
}

uint8_t mod_usb_transport_recent_at(uint32_t now, uint32_t window_ms)
{
    return (uint8_t)((usb_stream_active != 0U) &&
                     (g_last_usb_tick != 0U) &&
                     ((uint32_t)(now - g_last_usb_tick) <= window_ms));
}

uint8_t mod_usb_audio_recent_at(uint32_t now, uint32_t window_ms)
{
    return (uint8_t)((usb_audio_seen != 0U) &&
                     (g_last_usb_audio_tick != 0U) &&
                     ((uint32_t)(now - g_last_usb_audio_tick) <= window_ms));
}

uint8_t mod_external_render_reserve_low(void)
{
    if (active_internal_source != 0U) return 0U;
    if (transport_started == 0U) {
        return (pcm_count >= AUDIO_FRAME_SIZE) ? 1U : 0U;
    }
    return (render_count <= TRANSPORT_RENDER_LOW_WATER_EXTERNAL) ? 1U : 0U;
}

uint8_t MOD_AudioRealtimeCritical(uint32_t now)
{
    uint8_t reason = 0U;

    if (pipeline_hard_lockout != 0U) {
        audio_critical_reason = 0U;
        return 0U;
    }

    if (cfg_muted) {
        audio_critical_reason = 0U;
        return 0U;
    }

    if ((MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) && (local_audio_armed == 0U)) {
        audio_critical_reason = 0U;
        return 0U;
    }

    /* Most internal generators can tolerate the relaxed UI cadence, but CW is
     * operator-timed and shares the same tiny DMA-fed render backend. Treat
     * active CW as a realtime source so display/menu work cannot starve the
     * keyer into underruns. */
    if (active_internal_source) {
        if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
            if (mod_transport_stream_needed(now) == 0U) {
                audio_critical_reason = 0U;
                return 0U;
            }
            audio_critical_reason = AUDIO_CRIT_REASON_RENDER_LOW;
            return mod_transport_stream_needed(now);
        }
        if (active_mod_type != MOD_TYPE_CW) {
            audio_critical_reason = 0U;
            return 0U;
        }
        if (mod_transport_stream_needed(now) == 0U) {
            audio_critical_reason = 0U;
            return 0U;
        }
        if (render_count <= TRANSPORT_RENDER_LOW_WATER_CW) reason |= AUDIO_CRIT_REASON_RENDER_LOW;
        if ((cw_tx_live != 0U) || (cw_paddle_dit != 0U) || (cw_paddle_dah != 0U)) reason |= AUDIO_CRIT_REASON_CW;
        if ((now - g_last_cw_tick) <= AUDIO_CRITICAL_HOLDOFF_MS) reason |= AUDIO_CRIT_REASON_CW;
        audio_critical_reason = reason;
        if (reason != 0U) return 1U;
        audio_critical_reason = 0U;
        return 0U;
    }

    /* USB audio the transmit gate holds is dropped on arrival, as muted
     * audio is: once an over has drained (transport unstarted, no RF DMA) a
     * sound server's idle stream holds nothing back -- the panel, tuning and
     * CAT run as in receive. */
    if (mod_usb_stream_idle_held() != 0U) {
        audio_critical_reason = 0U;
        return 0U;
    }

    const uint8_t external_reserve_low = mod_external_render_reserve_low();
    if ((mod_usb_transport_recent_at(now, USB_AUDIO_CRITICAL_HOLDOFF_MS) != 0U) &&
        (external_reserve_low != 0U)) {
        reason |= AUDIO_CRIT_REASON_USB_STREAM | AUDIO_CRIT_REASON_RENDER_LOW;
    }
    if (pcm_count >= PCM_HIGH_WATER_SAMPLES) reason |= AUDIO_CRIT_REASON_PCM_HIGH;
    if (mod_usb_audio_recent_at(now, USB_AUDIO_CRITICAL_HOLDOFF_MS) != 0U) reason |= AUDIO_CRIT_REASON_USB_AUDIO;
    if ((audio_seen != 0U) &&
        ((uint32_t)(now - g_last_audio_tick) <= USB_AUDIO_CRITICAL_HOLDOFF_MS)) reason |= AUDIO_CRIT_REASON_RENDER_AUDIO;
    if ((reason != 0U) && (external_reserve_low != 0U)) reason |= AUDIO_CRIT_REASON_RENDER_LOW;
    audio_critical_reason = reason;
    if (reason != 0U) return 1U;
    return 0U;
}

/* `config_apply` is set for the mode change itself. It may pass a carrier
 * apply that is only pending: that retune waits for requested == active
 * (carrier_timing_apply_can_run_now), so holding the mode change for it
 * deadlocked both -- leaving TX TEST after its knob picked a band on another
 * PLL setting stuck the radio on AUDIO BUSY until reset. apply_config
 * re-plans the pending carrier (MOD_ValidateCarrierClockForCurrentModeEx),
 * and TX stays inhibited until that apply lands. */
static uint8_t mod_blocking_io_allowed_snapshot(uint32_t now, uint32_t *value_out, uint8_t config_apply)
{
    uint8_t allow = 1U;
    uint32_t value = 0U;

    if (MOD_AudioRealtimeCritical(now) != 0U) {
        allow = 0U;
        value = audio_critical_reason;
    } else {
        uint32_t pm = enter_critical();
        uint32_t render_snap = render_count;
        uint32_t pcm_snap = pcm_count;
        uint8_t render_busy = (uint8_t)(render_push_in_progress | render_pop_in_progress);
        uint8_t pcm_busy = (uint8_t)(pcm_push_in_progress | pcm_pop_in_progress);
        uint8_t internal = active_internal_source;
        uint8_t started = (uint8_t)transport_started;
        uint8_t usb_active = usb_stream_active;
        uint8_t dma_running = dma_streams_running;
        uint8_t muted = cfg_muted;
        uint32_t last_usb = g_last_usb_tick;
        uint32_t last_audio = g_last_audio_tick;
        uint32_t last_usb_audio = g_last_usb_audio_tick;
        /* A held stream counts as none (mod_usb_stream_idle_held): going by
         * its packets, which refresh the USB ticks every millisecond, a
         * sound server keeping the card open held every mode change,
         * autosave and card access until it closed. */
        if (mod_usb_stream_idle_held() != 0U) { usb_active = 0U; last_usb = 0U; last_usb_audio = 0U; }
        uint8_t retune_busy = (uint8_t)(carrier_clock_retune_in_progress |
                                        (config_apply ? 0U : carrier_frequency_apply_pending));
        mod_type_t type = active_mod_type;
        mod_config_t cfg = (mod_config_t)active_config;
        exit_critical(pm);

        const uint8_t cw_idle = ((muted == 0U) && (internal != 0U) && (type == MOD_TYPE_CW))
                              ? mod_transport_cw_idle(now) : 0U;
        uint32_t reserve = 0U;
        if (muted == 0U) {
            if ((internal != 0U) && (cw_idle == 0U)) {
                reserve = (type == MOD_TYPE_CW) ? TRANSPORT_RENDER_IO_RESERVE_CW
                                                : TRANSPORT_RENDER_IO_RESERVE_INTERNAL;
            } else if ((started != 0U) || (usb_active != 0U)) {
                reserve = (type == MOD_TYPE_FM) ? TRANSPORT_RENDER_IO_RESERVE_FM
                                                : TRANSPORT_RENDER_IO_RESERVE_EXTERNAL;
            }
            /* TX TEST streams only while the paddle is down, and
             * MOD_AudioRealtimeCritical has refused above by then. Paddle
             * up, its FIFO is flushed, so the reserve of 2 it used to ask for
             * could never be met: unmuted TX TEST refused every blocking I/O,
             * the mode change out of it included, which left TX TEST keyable
             * behind the home screen after the page closed. */
            if (cfg == MOD_CFG_TX_CARRIER_TEST) reserve = 0U;
        }
        if ((reserve != 0U) && (render_snap < reserve)) {
            allow = 0U;
            value = render_snap;
        }
        if (pcm_snap >= PCM_HIGH_WATER_SAMPLES) {
            allow = 0U;
            value = pcm_snap;
        }
        if ((pcm_snap != 0U) || (render_snap != 0U) ||
            (pcm_busy != 0U) || (render_busy != 0U)) {
            allow = 0U;
            value = (pcm_snap << 16) | (render_snap & 0xFFFFUL);
        }
        if ((started != 0U) || (usb_active != 0U) || (dma_running != 0U)) {
            allow = 0U;
            value = ((uint32_t)started << 16) |
                    ((uint32_t)usb_active << 8) |
                    (uint32_t)dma_running;
        }
        if ((last_usb != 0U) &&
            ((uint32_t)(now - last_usb) <= AUDIO_RECENT_ACTIVITY_MS)) {
            allow = 0U;
            value = (uint32_t)(now - last_usb);
        }
        if ((last_audio != 0U) &&
            ((uint32_t)(now - last_audio) <= AUDIO_RECENT_ACTIVITY_MS)) {
            allow = 0U;
            value = (uint32_t)(now - last_audio);
        }
        if ((last_usb_audio != 0U) &&
            ((uint32_t)(now - last_usb_audio) <= AUDIO_RECENT_ACTIVITY_MS)) {
            allow = 0U;
            value = (uint32_t)(now - last_usb_audio);
        }
        if ((muted == 0U) && (internal != 0U) && (cfg != MOD_CFG_TX_CARRIER_TEST) && (cw_idle == 0U)) {
            allow = 0U;
            value = ((uint32_t)type << 16) | (uint32_t)cfg;
        }
        if (retune_busy != 0U) {
            allow = 0U;
            value = 0xFFFFFFFFUL;
        }
    }

    if (value_out != NULL) *value_out = value;
    return allow;
}

uint8_t MOD_BlockingIoAllowed(uint32_t now)
{
    return mod_blocking_io_allowed_snapshot(now, NULL, 0U);
}

static uint8_t mod_config_apply_allowed(uint32_t now)
{
    return mod_blocking_io_allowed_snapshot(now, NULL, 1U);
}

uint8_t MOD_BackgroundIoAllowed(uint32_t now)
{
    static uint32_t last_defer_tick = 0U;
    uint32_t value = 0U;
    uint8_t allow = mod_blocking_io_allowed_snapshot(now, &value, 0U);

    if ((allow == 0U) && (last_defer_tick != now)) {
        last_defer_tick = now;
        mod_pipeline_note(MOD_PIPELINE_STAGE_SCHEDULER,
                          MOD_PIPELINE_REASON_SCHED_IO_DEFERRED,
                          value);
    }
    return allow;
}

uint8_t MOD_NoncriticalUiAllowed(uint32_t now)
{
    if (MOD_AudioRealtimeCritical(now) != 0U) return 0U;

    uint32_t pm = enter_critical();
    uint32_t render_snap = render_count;
    uint32_t pcm_snap = pcm_count;
    uint8_t render_busy = (uint8_t)(render_push_in_progress | render_pop_in_progress);
    uint8_t pcm_busy = (uint8_t)(pcm_push_in_progress | pcm_pop_in_progress);
    uint8_t started = (uint8_t)transport_started;
    uint8_t usb_active = usb_stream_active;
    uint8_t internal = active_internal_source;
    uint8_t muted = cfg_muted;
    mod_type_t type = active_mod_type;
    exit_critical(pm);

    if (muted != 0U) return 1U;
    /* An internal source with no stream to feed -- TX TEST with the paddle
     * up, an idle CW keyer -- has no render reserve to protect: its FIFO is
     * flushed, so the reserve below could never be met and the LCD froze
     * (the TX TEST page never drew while unmuted). Whenever that stream does
     * run, MOD_AudioRealtimeCritical has already refused above. */
    if ((internal != 0U) && (mod_transport_stream_needed(now) == 0U)) return 1U;
    /* Gated USB audio with nothing started: an idle sound-server stream, held
     * like muted audio (see MOD_AudioRealtimeCritical). */
    if ((usb_tx_gate_closed != 0U) && (started == 0U) && (internal == 0U) &&
        !MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) return 1U;
    if ((render_busy != 0U) || (pcm_busy != 0U)) return 0U;
    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) {
        uint32_t queued = pcm_snap + render_snap * AUDIO_FRAME_SIZE;
        return (queued >= AUDIO_FRAME_SIZE * 80U) ? 1U : 0U;
    }
    if (pcm_snap >= PCM_HIGH_WATER_SAMPLES) return 0U;
    if ((started == 0U) && (usb_active == 0U) && (internal == 0U)) return 1U;

    uint32_t reserve = TRANSPORT_RENDER_TARGET_FRAMES_EXTERNAL / 2U;
    if (type == MOD_TYPE_FM) {
        reserve = TRANSPORT_RENDER_TARGET_FRAMES_FM / 2U;
    } else if (internal != 0U) {
        reserve = (type == MOD_TYPE_CW) ? TRANSPORT_RENDER_TARGET_FRAMES_CW
                                        : (TRANSPORT_RENDER_TARGET_FRAMES_INTERNAL / 2U);
    }
    if (reserve < 2U) reserve = 2U;
    return (render_snap >= reserve) ? 1U : 0U;
}

static uint8_t mod_deadtime_update_enabled_requested(void)
{
    return (uint8_t)((HRTIM_GetDeadTimeRiseTicks() != 0U) || (HRTIM_GetDeadTimeFallTicks() != 0U));
}

static uint8_t mod_deadtime_update_enabled_applied(void)
{
    return (uint8_t)((carrier_apply_deadtime_rise_ticks != 0U) || (carrier_apply_deadtime_fall_ticks != 0U));
}

typedef struct {
    uint16_t period_ticks;
    uint16_t null_ticks;
    uint8_t  frac_cycles;
    int8_t   frac_extra;
    uint16_t deadtime_ticks;
    uint16_t deadtime_rise_ticks;
    uint16_t deadtime_fall_ticks;
    int16_t  duty_offset_ticks;
    uint8_t  need_full_reconfigure;
    uint8_t  need_timing_update;
    uint8_t  streams_were_running;
} mod_carrier_apply_plan_t;

__attribute__((cold)) static mod_carrier_apply_plan_t mod_build_active_carrier_apply_plan(void)
{
    mod_carrier_apply_plan_t plan;

    plan.period_ticks = active_carrier->period_ticks;
    plan.null_ticks = active_carrier->null_ticks;
    plan.frac_cycles = active_carrier->frac_cycles;
    plan.frac_extra = active_carrier->frac_extra;
    plan.deadtime_ticks = HRTIM_GetDeadTimeTicks();
    plan.deadtime_rise_ticks = HRTIM_GetDeadTimeRiseTicks();
    plan.deadtime_fall_ticks = HRTIM_GetDeadTimeFallTicks();
    plan.duty_offset_ticks = HRTIM_GetDutyOffsetTicks();
    plan.streams_were_running = dma_streams_running;
    plan.need_full_reconfigure = (!carrier_apply_valid)
                              || (carrier_apply_period_ticks != plan.period_ticks)
                              || (carrier_apply_null_ticks != plan.null_ticks)
                              || (carrier_apply_frac_cycles != plan.frac_cycles)
                              || (carrier_apply_frac_extra != plan.frac_extra)
                              || (mod_deadtime_update_enabled_applied() != mod_deadtime_update_enabled_requested());
    plan.need_timing_update = plan.need_full_reconfigure
                           || (carrier_apply_deadtime_ticks != plan.deadtime_ticks)
                           || (carrier_apply_deadtime_rise_ticks != plan.deadtime_rise_ticks)
                           || (carrier_apply_deadtime_fall_ticks != plan.deadtime_fall_ticks)
                           || (carrier_apply_duty_offset_ticks != plan.duty_offset_ticks);
    return plan;
}

static void mod_commit_applied_carrier_state(const mod_carrier_apply_plan_t *plan,
                                             mod_carrier_apply_path_t apply_path)
{
    if (plan == NULL) return;

    carrier_apply_valid = 1U;
    carrier_apply_period_ticks = plan->period_ticks;
    carrier_apply_null_ticks = plan->null_ticks;
    carrier_apply_frac_cycles = plan->frac_cycles;
    carrier_apply_frac_extra = plan->frac_extra;
    carrier_apply_deadtime_ticks = plan->deadtime_ticks;
    carrier_apply_deadtime_rise_ticks = plan->deadtime_rise_ticks;
    carrier_apply_deadtime_fall_ticks = plan->deadtime_fall_ticks;
    carrier_apply_duty_offset_ticks = plan->duty_offset_ticks;
    last_carrier_apply_path = (uint8_t)apply_path;
    mod_transport_refresh_step_guard();
    mod_pa_runtime_sync_applied_state_from_shadow();
}

__attribute__((cold)) static HAL_StatusTypeDef mod_apply_carrier_plan(const mod_carrier_apply_plan_t *plan,
                                                uint8_t allow_live_update,
                                                uint8_t refresh_null_buffers)
{
    uint8_t restart_streams = 0U;
    HAL_StatusTypeDef st;

    if (plan == NULL) return HAL_ERROR;
    if (!plan->need_timing_update) return HAL_OK;

    rf_en_force_idle();
    /* A new fraction is a new master period, which only the full reconfigure
     * writes (need_full_reconfigure); the timing update checks its duty
     * offset against the same fraction. */
    HRTIM_SetCarrierFraction(plan->frac_cycles, plan->frac_extra);

    if (plan->need_full_reconfigure)
    {
        restart_streams = plan->streams_were_running;
        if (restart_streams) mod_dma_stop_streams();

        st = HRTIM_ReconfigureCarrier(&hhrtim1,
                                      plan->period_ticks,
                                      plan->null_ticks);
        if (st != HAL_OK)
        {
            rf_en_force_idle();
            return st;
        }

        if (refresh_null_buffers) MOD_FillAllNullBuffers();
        if (restart_streams) mod_dma_start_streams();
        mod_commit_applied_carrier_state(plan, MOD_CARRIER_APPLY_PATH_FULL_RECONFIG);
        return HAL_OK;
    }

    if (allow_live_update && plan->streams_were_running)
    {
        st = HRTIM_ApplyTimingUpdate(&hhrtim1, plan->period_ticks);
        if (st == HAL_OK)
        {
            mod_commit_applied_carrier_state(plan, MOD_CARRIER_APPLY_PATH_TIMING_UPDATE_LIVE);
            return HAL_OK;
        }
    }

    restart_streams = plan->streams_were_running;
    if (restart_streams) mod_dma_stop_streams();

    st = HRTIM_ApplyTimingUpdate(&hhrtim1, plan->period_ticks);
    if (st != HAL_OK)
    {
        st = HRTIM_ReconfigureCarrier(&hhrtim1,
                                      plan->period_ticks,
                                      plan->null_ticks);
        if (st != HAL_OK)
        {
            rf_en_force_idle();
            return st;
        }
        if (refresh_null_buffers) MOD_FillAllNullBuffers();
        if (restart_streams) mod_dma_start_streams();
        mod_commit_applied_carrier_state(plan, MOD_CARRIER_APPLY_PATH_FULL_RECONFIG);
        return HAL_OK;
    }

    if (refresh_null_buffers) MOD_FillAllNullBuffers();
    if (restart_streams) mod_dma_start_streams();
    mod_commit_applied_carrier_state(plan, MOD_CARRIER_APPLY_PATH_TIMING_UPDATE_STOPPED);
    return HAL_OK;
}

uint8_t mod_runtime_timing_only_update_possible(void)
{
    return carrier_apply_valid
        && (carrier_apply_period_ticks == active_carrier->period_ticks)
        && (carrier_apply_null_ticks == active_carrier->null_ticks)
        && (carrier_apply_frac_cycles == active_carrier->frac_cycles)
        && (carrier_apply_frac_extra == active_carrier->frac_extra)
        && (mod_deadtime_update_enabled_applied() == mod_deadtime_update_enabled_requested());
}

void mod_runtime_apply_active_timing_update(void)
{
    mod_carrier_apply_plan_t plan = mod_build_active_carrier_apply_plan();

    if (mod_apply_carrier_plan(&plan, 1U, 0U) != HAL_OK)
        Error_Handler();
}

void mod_runtime_reconfigure_active_carrier(void)
{
    mod_carrier_apply_plan_t plan = mod_build_active_carrier_apply_plan();
    plan.need_full_reconfigure = 1U;
    plan.need_timing_update = 1U;

    if (mod_apply_carrier_plan(&plan, 0U, 1U) != HAL_OK)
        Error_Handler();
}

static void apply_config(mod_config_t cfg)
{
    mod_carrier_apply_plan_t plan;

    apply_config_count++;
    mod_tx_pipeline_select_profile(cfg);
    MOD_UpdateCarrierTxReadyCache();
    plan = mod_build_active_carrier_apply_plan();

    if (mod_apply_carrier_plan(&plan, 0U, plan.need_timing_update ? 1U : 0U) != HAL_OK)
        Error_Handler();

    active_config = (uint32_t)cfg;
    mod_tx_pipeline_reset();
    MOD_ValidateCarrierClockForCurrentModeEx(1U);
    MOD_UpdateCarrierTxReadyCache();
    /* Prime only a stream that is about to run. An idle keyer or a muted
     * source would leave the frames queued with no DMA to drain them. */
    if ((active_internal_source != 0U) && (mod_transport_stream_needed(HAL_GetTick()) != 0U))
    {
        uint32_t prime = ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST)
                       ? 2U
                       : ((active_mod_type == MOD_TYPE_CW)
                          ? TRANSPORT_RENDER_TARGET_FRAMES_CW
                          : 4U);
        while (prime-- > 0U) mod_tx_pipeline_render_int();
    }
}

void MOD_ApplyRequestedConfigNow(void)
{
    uint32_t pm = enter_critical();
    mod_config_t req = (mod_config_t)requested_config;
    uint8_t need_reset = requested_reset;
    exit_critical(pm);

    if ((uint32_t)req >= MOD_CFG_COUNT) req = MOD_CFG_PROD_SSB_14MHZ;
    if ((req != (mod_config_t)active_config) || need_reset || (req == MOD_CFG_TX_CARRIER_TEST)) {
        uint32_t now = HAL_GetTick();
        if (mod_config_apply_allowed(now) == 0U) {
            mod_pipeline_note(MOD_PIPELINE_STAGE_SCHEDULER,
                              MOD_PIPELINE_REASON_SCHED_UI_DEFERRED,
                              (uint32_t)req);
            return;
        }
        pm = enter_critical();
        requested_reset = 0U;
        exit_critical(pm);
        apply_config(req);
    }
}

__attribute__((cold)) void MOD_Init(void)
{
    mod_tx_pipeline_init();

    sanitize_outphase_policy_settings();
    build_outphase_lut();
    cw_build_ramp_lut();
    sanitize_cw_settings();
    MTRAIN_Init();
    cw_paddle_guard_reset();
    /* Initialize calibration structures with zero offsets */
    memset(&phase_cal_active, 0, sizeof(phase_cal_active));
    memset(theta_linearity_pd, 0, sizeof(theta_linearity_pd));
    active_cal_flags = 0U;

    requested_config = (uint32_t)MOD_CFG_PROD_SSB_14MHZ;
    active_config    = (uint32_t)MOD_CFG_PROD_SSB_14MHZ;
    last_mode        = (uint32_t)MOD_CFG_PROD_SSB_14MHZ;
    apply_audio_profile(AUDIO_PROFILE_CLEAN);
    mod_tx_pipeline_select_profile(MOD_CFG_PROD_SSB_14MHZ);
    pa_profiles_fill_from_legacy();
    requested_reset = 1U;

    HRTIM_SetDeadTimeTicksRF(cfg_deadtime_rise_ticks, cfg_deadtime_fall_ticks);

    /* Try to load user settings from flash. If none found or invalid,
     * defaults above remain in effect and still need the same PA runtime
     * requested/resolved/applied state that settings_apply() would create. */
    uint8_t settings_loaded = MOD_LoadSettings();
    if (settings_loaded == 0U) {
        pa_resolve_active_band_profile(1U, 0U, 0U);
    }
    /* A record's mode this image lacks became PROD SSB in MOD_LoadSettings. */
    MOD_SetPaGeometry(cfg_pa_geometry);

    /* Resolve the final boot-time carrier/mode geometry first, then mirror
     * the validated hardware bring-up sequence: cold-start with DT off, then
     * unconditionally run the runtime Stop -> Reconfigure -> Start path before
     * the RF transport is initialized and parked. This keeps factory-reset
     * boots deterministic. */
    mod_tx_pipeline_select_profile((mod_config_t)requested_config);
    HRTIM_SetDeadTimeTicksRF(cfg_deadtime_rise_ticks, cfg_deadtime_fall_ticks);
    HRTIM_SetDutyOffsetTicks(cfg_duty_offset_ticks);
    HRTIM_SetCarrierFraction(active_carrier->frac_cycles, active_carrier->frac_extra);
    HRTIM_Phase_Init(&hhrtim1, active_carrier->period_ticks, active_carrier->null_ticks);
    if (HRTIM_ReconfigureCarrier(&hhrtim1,
                                 active_carrier->period_ticks,
                                 active_carrier->null_ticks) != HAL_OK)
        Error_Handler();
    {
        mod_carrier_apply_plan_t boot_plan = mod_build_active_carrier_apply_plan();
        boot_plan.need_full_reconfigure = 1U;
        boot_plan.need_timing_update = 1U;
        mod_commit_applied_carrier_state(&boot_plan, MOD_CARRIER_APPLY_PATH_FULL_RECONFIG);
    }

    /* Complete the initial profile/DSP application synchronously during boot
     * after the known-good carrier bring-up path has already run once. */
    if ((requested_config != active_config) || requested_reset) {
        requested_reset = 0U;
        apply_config((mod_config_t)requested_config);
    } else {
        mod_tx_pipeline_reset();
    }

    /* Seed both DMA halves with a valid idle branch pair so the DMA engine
     * never streams stale or zero-initialized compare values into HRTIM on
     * boot or immediately after a fresh factory reset. */
    MOD_FillAllNullBuffers();
}

static const int16_t usb_conceal_frame[USB_CONCEAL_SAMPLES] = {0};

/* Frames concealed (unmuted) in the current gap, and why the first one was.
 * USB interrupt only. A gap is only known to be lost audio once a packet
 * resumes the stream (mod_usb_gap_resumed): every stream also ends with one,
 * the frames between the host's last packet and the stall or alt 0. */
static uint8_t usb_gap_concealed;
static uint8_t usb_gap_reason;

static uint16_t usb_frame_next(uint16_t frame)
{
    return mod_usb_frame_next_value(frame);
}

static void mod_usb_hold_muted(void);

/* The host stopped sending without closing the stream: a player killed or
 * stopped with a drop, a host-side underrun, a sound server suspending the
 * sink -- the endpoint goes quiet a few frames before alt 0 arrives, if it
 * arrives. That used to be a hard lockout, which tripped the radio into
 * PROTECTION (receive included) even with TX muted. End the stream the way
 * a closed one ends instead: no underrun accounting (usb_stream_active), an
 * empty ring and an unstarted transport, so an over drains the frames it has
 * and drops out of TX by itself, and the next packet re-arms frame tracking
 * and starts from TRANSPORT_START_THRESHOLD. Still counted under `pipeline`. */
static void mod_usb_stream_stalled(mod_pipeline_reason_t reason, uint32_t value)
{
    usb_stream_active = 0U;
    usb_frame_tracking_armed = 0U;
    usb_missing_frame_streak = 0U;
    usb_rearm_good_frames = 0U;
    usb_gap_concealed = 0U;
    mod_usb_hold_muted();
    mod_pipeline_note_severity(MOD_PIPELINE_STAGE_USB, reason,
                               MOD_PIPELINE_SEVERITY_NONE, value);
}

static void mod_usb_conceal_missing_frame(uint16_t frame, mod_pipeline_reason_t reason,
                                          uint32_t value)
{
    usb_missing_frames = sat_inc_u32(usb_missing_frames);
    if (usb_missing_frame_streak < 0xFFFFU) usb_missing_frame_streak++;
    usb_rearm_good_frames = 0U;
    usb_expected_frame_number = usb_frame_next(frame);

    if (usb_missing_frame_streak >= USB_PACKET_MISS_LOCKOUT_FRAMES) {
        mod_usb_stream_stalled(reason, value);
        return;
    }
    /* Muted or gated, the ring is held empty: there is nothing to conceal. */
    if (mod_usb_tx_held() != 0U) return;

    /* Counted and logged now; latched as a fault only if the stream comes
     * back (mod_usb_gap_resumed). A normal stream end left the RF PIPELINE
     * notice up after every USB-audio transmission. */
    mod_pipeline_note_severity(MOD_PIPELINE_STAGE_USB, reason,
                               MOD_PIPELINE_SEVERITY_NONE, value);
    if (usb_gap_concealed == 0U) usb_gap_reason = (uint8_t)reason;
    if (usb_gap_concealed < 0xFFU) usb_gap_concealed++;
    pcm_push(usb_conceal_frame, USB_CONCEAL_SAMPLES);
    MOD_RequestRealtimeRender();
}

/* A packet arrived: a gap still open behind it was audio lost mid-stream and
 * concealed, so latch it now, valued by the frames concealed (the reason's
 * `pipeline` count takes this note on top of the per-frame ones). Every path
 * that ends or resets the stream clears the streak, so a gap it closed never
 * gets here. */
static void mod_usb_gap_resumed(void)
{
    if ((usb_gap_concealed != 0U) && (usb_missing_frame_streak != 0U) &&
        (usb_frame_tracking_armed != 0U)) {
        mod_pipeline_note_severity(MOD_PIPELINE_STAGE_USB,
                                   (mod_pipeline_reason_t)usb_gap_reason,
                                   MOD_PIPELINE_SEVERITY_CONCEALED, usb_gap_concealed);
    }
    usb_gap_concealed = 0U;
}

static uint8_t mod_usb_advance_to_frame(uint16_t frame)
{
    frame &= USB_FRAME_NUMBER_MASK;
    mod_usb_frame_plan_t plan = mod_usb_packet_frame_plan(
        usb_frame_tracking_armed, usb_expected_frame_number, frame);
    if (plan.arm != 0U) {
        usb_frame_tracking_armed = 1U;
        usb_expected_frame_number = frame;
        return 1U;
    }
    if (plan.sequence_error != 0U) {
        mod_pipeline_note_severity(MOD_PIPELINE_STAGE_USB,
                                   MOD_PIPELINE_REASON_USB_FRAME_SEQUENCE,
                                   MOD_PIPELINE_SEVERITY_HARD,
                                   ((uint32_t)usb_expected_frame_number << 16) | frame);
        return 0U;
    }
    for (uint16_t index = 0U;
         (index < plan.missing_count) && (pipeline_hard_lockout == 0U) &&
         (usb_frame_tracking_armed != 0U);
         index++) {
        uint16_t missing = usb_expected_frame_number;
        mod_usb_conceal_missing_frame(missing,
                                      MOD_PIPELINE_REASON_USB_MISSING_PACKET,
                                      missing);
    }
    return 1U;
}

void MOD_RecordUsbPacketAnomaly(uint16_t usb_frame_number, uint32_t byte_count)
{
    if (RADIO_Config()->mic_source != RADIO_SOURCE_USB) return;
    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) return;
    usb_packet_size_anomalies = sat_inc_u32(usb_packet_size_anomalies);
    usb_frame_number &= USB_FRAME_NUMBER_MASK;
    if (mod_usb_advance_to_frame(usb_frame_number) == 0U) return;
    mod_usb_conceal_missing_frame(usb_frame_number,
                                  (byte_count == 0U) ? MOD_PIPELINE_REASON_USB_ZERO_PACKET
                                                     : MOD_PIPELINE_REASON_USB_BAD_PACKET_SIZE,
                                  byte_count);
}

__attribute__((cold)) void MOD_RecordUsbPipelineEvent(mod_usb_pipeline_event_t event, uint32_t value)
{
    mod_pipeline_reason_t reason = MOD_PIPELINE_REASON_NONE;

    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) return;

    switch (event) {
        case MOD_USB_PIPE_EVENT_STREAM_ACTIVE:
            usb_alt_active = 1U;
            usb_stream_active = 0U;
            /* Do NOT arm frame tracking here. Selecting alt setting 1 means
             * the host has opened the streaming interface, not that it will
             * deliver a packet in the very next frame: it may still be
             * scheduling the first period, or merely probing the device's
             * formats and about to select alt 0 again. Linux does exactly
             * that when a sound server enumerates a newly plugged device.
             *
             * Arming here made the 1 kHz SOF handler count every frame from
             * that moment as a missing packet, so three idle frames -- 3 ms --
             * hard-locked the RF pipeline on plug-in. mod_usb_packet_frame_plan
             * already adopts the live frame number when it is called unarmed,
             * so the first packet that really arrives arms tracking at the
             * frame it arrived in, and tracking is exact from there. */
            usb_frame_tracking_armed = 0U;
            usb_expected_frame_number = 0U;
            usb_missing_frame_streak = 0U;
            usb_rearm_good_frames = 0U;
            usb_gap_concealed = 0U;
            return;
        case MOD_USB_PIPE_EVENT_STREAM_INACTIVE:
            usb_alt_active = 0U;
            usb_stream_active = 0U;
            usb_frame_tracking_armed = 0U;
            usb_missing_frame_streak = 0U;
            usb_rearm_good_frames = 0U;
            usb_gap_concealed = 0U;
            return;
        case MOD_USB_PIPE_EVENT_ZERO_PACKET:
            reason = MOD_PIPELINE_REASON_USB_ZERO_PACKET;
            break;
        case MOD_USB_PIPE_EVENT_OUT_INCOMPLETE:
            reason = MOD_PIPELINE_REASON_USB_OUT_INCOMPLETE;
            break;
        case MOD_USB_PIPE_EVENT_REARM_FAILED:
            reason = MOD_PIPELINE_REASON_USB_REARM_FAILED;
            break;
        case MOD_USB_PIPE_EVENT_FEEDBACK_TX_FAILED:
            reason = MOD_PIPELINE_REASON_USB_FEEDBACK_TX_FAILED;
            break;
        default:
            reason = MOD_PIPELINE_REASON_USB_BAD_PACKET_SIZE;
            break;
    }
    mod_pipeline_note(MOD_PIPELINE_STAGE_USB, reason, value);
}

void MOD_RecordUsbHardwareFault(uint16_t usb_frame_number, uint16_t usb_status)
{
    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) return;
    mod_pipeline_note_severity(MOD_PIPELINE_STAGE_USB,
                               MOD_PIPELINE_REASON_USB_HW_ERROR,
                               MOD_PIPELINE_SEVERITY_HARD,
                               ((uint32_t)(usb_frame_number & USB_FRAME_NUMBER_MASK) << 16) |
                               (uint32_t)usb_status);
}

void MOD_RecordHrtimFault(uint32_t hrtim_status)
{
    mod_pipeline_note_severity(MOD_PIPELINE_STAGE_HRTIM,
                               MOD_PIPELINE_REASON_HRTIM_FAULT,
                               MOD_PIPELINE_SEVERITY_HARD,
                               hrtim_status);
}

/* Muted, nothing renders and nothing drains the PCM ring. A host that kept
 * streaming filled it in 128 ms, and every packet after that was a hard PCM
 * overflow that tripped the radio into PROTECTION with TX off -- and the
 * stale ring locked the pipeline out again the moment TX was unmuted. Keep
 * the ring empty and the transport unstarted instead: unmuting starts it
 * from TRANSPORT_START_THRESHOLD like a new stream. USB interrupt only, the
 * ring's single producer; the generation bump discards a pop the render
 * worker may have been preempted in, as in MOD_UsbStreamReset. */
static void mod_usb_hold_muted(void)
{
    if ((pcm_count == 0U) && (transport_started == 0U)) return;
    uint32_t pm = enter_critical();
    pcm_wr_idx = 0U; pcm_rd_idx = 0U; pcm_count = 0U;
    pcm_pop_in_progress = 0U; pcm_reset_seq++;
    transport_started = 0U;
    exit_critical(pm);
}

void MOD_SubmitUsbPacket(const int16_t *mono, uint32_t count, uint16_t usb_frame_number)
{
    if (RADIO_Config()->mic_source != RADIO_SOURCE_USB) return;
    CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_USB_SUBMIT);
    uint32_t now = HAL_GetTick();
    usb_frame_number &= USB_FRAME_NUMBER_MASK;

    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) {
        CPU_DIAG_END(MOD_CPU_DIAG_USB_SUBMIT, cpu_diag_start);
        return;
    }

    if ((mono == NULL) || (count < MOD_USB_AUDIO_LEGAL_MIN_SAMPLES) ||
        (count > MOD_USB_AUDIO_LEGAL_MAX_SAMPLES)) {
        MOD_RecordUsbPacketAnomaly(usb_frame_number, count * 4U);
        CPU_DIAG_END(MOD_CPU_DIAG_USB_SUBMIT, cpu_diag_start);
        return;
    }
    if (mod_usb_advance_to_frame(usb_frame_number) == 0U) {
        CPU_DIAG_END(MOD_CPU_DIAG_USB_SUBMIT, cpu_diag_start);
        return;
    }
    mod_usb_gap_resumed();

    /* USB owns these aligned scalar fields. Avoid masking the priority-0 RF
     * DMA IRQ merely to publish status that can tolerate a mixed snapshot. */
    g_last_usb_tick = now;
    usb_packets++;
    usb_last_submit_count = count;
    usb_rx_samples += count;
    usb_stream_active = 1U;
    active_audio_source = MOD_AUDIO_SOURCE_USB;
    usb_alt_active = 1U;
    usb_missing_frame_streak = 0U;
    usb_expected_frame_number = usb_frame_next(usb_frame_number);
    if (usb_rearm_good_frames < 0xFFU) usb_rearm_good_frames++;
    g_last_usb_audio_tick = now;
    usb_audio_seen = 1U;
    if ((usb_packet_size_min == 0U) || (count < usb_packet_size_min)) usb_packet_size_min = count;
    if (count > usb_packet_size_max) usb_packet_size_max = count;

    if ((pipeline_hard_lockout != 0U) && (pipeline_ack_requested == 0U)) {
        CPU_DIAG_END(MOD_CPU_DIAG_USB_SUBMIT, cpu_diag_start);
        return;
    }
    /* Muted, or held by the transmit gate (USB VOX off, no CAT transmit):
     * a sound server streaming silence into the radio is not a
     * transmission, and the receiver must not give way to it. */
    if (mod_usb_tx_held() != 0U) {
        mod_usb_hold_muted();
        CPU_DIAG_END(MOD_CPU_DIAG_USB_SUBMIT, cpu_diag_start);
        return;
    }
    /* Unmuted audio arriving for a stream not yet rendering is a transmission
     * on its way: the receiver gives way from the first packet
     * (RADIO_TxHandover). FM buffers more before it starts, and a receive
     * half could be lost in between. */
    if (transport_started == 0U) mod_tx_handover_mark();
    pcm_push(mono, count);
    MOD_RequestRealtimeRender();
    CPU_DIAG_END(MOD_CPU_DIAG_USB_SUBMIT, cpu_diag_start);
}

__attribute__((cold)) uint8_t MOD_LocalAudioAcquire(void)
{
    return MOD_LocalAudioAcquireSource(MOD_AUDIO_SOURCE_SD);
}

uint8_t MOD_LocalAudioAcquireSource(mod_audio_source_t source)
{
    if (source != MOD_AUDIO_SOURCE_SD && source != MOD_AUDIO_SOURCE_MIC) return 0U;
    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) return active_audio_source == source;

    uint32_t now = HAL_GetTick();
    if (active_internal_source != 0U || MOD_BlockingIoAllowed(now) == 0U) return 0U;
    if (active_mod_type == MOD_TYPE_CW || active_mod_type == MOD_TYPE_TEST) return 0U;

    MOD_UsbStreamReset();
    uint32_t pm = enter_critical();
    active_audio_source = source;
    local_audio_armed = 0U;
    local_audio_eos = 0U;
    local_audio_io_scope = 0U;
    exit_critical(pm);
    return 1U;
}

void MOD_LocalAudioRelease(void)
{
    uint32_t pm = enter_critical();
    local_audio_armed = 0U;
    local_audio_eos = 0U;
    local_audio_io_scope = 0U;
    active_audio_source = MOD_AUDIO_SOURCE_NONE;
    exit_critical(pm);
    MOD_UsbStreamReset();
}

uint8_t MOD_LocalAudioSubmit(const int16_t *mono, uint32_t count)
{
    if (mono == NULL || count == 0U || count > PCM_RING_SAMPLES) return 0U;
    uint32_t pm = enter_critical();
    uint8_t accept = (uint8_t)((MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) &&
                               (local_audio_eos == 0U) &&
                               (pcm_count + count <= PCM_RING_SAMPLES) &&
                               (pcm_push_in_progress == 0U));
    exit_critical(pm);
    if (accept == 0U) return 0U;
    pcm_push(mono, count);
    g_last_audio_tick = HAL_GetTick();
    audio_seen = 1U;
    if (local_audio_armed != 0U) MOD_RequestRealtimeRender();
    return 1U;
}

uint8_t MOD_LocalAudioArm(void)
{
    uint32_t threshold;
    if (!MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source) || local_audio_eos != 0U) return 0U;
    threshold = (active_mod_type == MOD_TYPE_FM) ? TRANSPORT_START_THRESHOLD_FM
                                                 : TRANSPORT_START_THRESHOLD;
    if (pcm_count < threshold) return 0U;
    local_audio_armed = 1U;
    MOD_RequestRealtimeRender();
    return 1U;
}

void MOD_LocalAudioEndOfStream(void)
{
    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) local_audio_eos = 1U;
}

uint8_t MOD_LocalAudioDrainComplete(void)
{
    return (uint8_t)((pcm_count == 0U) && (render_count == 0U) &&
                     (dma_streams_running == 0U));
}

uint32_t MOD_LocalAudioQueuedSamples(void)
{
    uint32_t pm = enter_critical();
    uint32_t count = pcm_count + render_count * AUDIO_FRAME_SIZE;
    exit_critical(pm);
    return count;
}

uint32_t MOD_LocalAudioPcmFreeSamples(void)
{
    uint32_t pm = enter_critical();
    uint32_t free_samples = PCM_RING_SAMPLES - pcm_count;
    exit_critical(pm);
    return free_samples;
}

mod_audio_source_t MOD_GetAudioSource(void)
{
    return (mod_audio_source_t)active_audio_source;
}

uint8_t MOD_LocalAudioIoBegin(void)
{
    uint32_t pm = enter_critical();
    uint32_t queued = pcm_count + render_count * AUDIO_FRAME_SIZE;
    uint8_t allow = (uint8_t)((active_audio_source == MOD_AUDIO_SOURCE_SD) &&
                              (local_audio_io_scope == 0U) &&
                              (pipeline_hard_lockout == 0U) &&
                              (pcm_push_in_progress == 0U) &&
                              (pcm_pop_in_progress == 0U) &&
                              (render_push_in_progress == 0U) &&
                              (render_pop_in_progress == 0U) &&
                              (carrier_clock_retune_in_progress == 0U) &&
                              ((local_audio_armed == 0U) ||
                               (queued >= AUDIO_FRAME_SIZE * 48U)));
    if (allow != 0U) local_audio_io_scope = 1U;
    exit_critical(pm);
    return allow;
}

void MOD_LocalAudioIoEnd(void)
{
    local_audio_io_scope = 0U;
}

uint8_t MOD_LocalAudioIoScopeActive(void)
{
    return local_audio_io_scope;
}

void MOD_LocalAudioSetClean(uint8_t clean)
{
    local_audio_clean = clean ? 1U : 0U;
}


void MOD_UsbSofTick(uint16_t usb_frame_number)
{
    uint8_t feedback_active = 0U;
    uint32_t total_samples = 0U;
    uint32_t pcm_samples = 0U;
    int32_t target_total = (int32_t)TRANSPORT_TARGET_TOTAL_SAMPLES;

    usb_frame_number &= USB_FRAME_NUMBER_MASK;

    usb_sof_count++;
    /* Muted, the ring is held empty (mod_usb_hold_muted): regulating the
     * host against it would only wind the integrator up for the unmute. */
    feedback_active = (uint8_t)(usb_alt_active && usb_stream_active && (mod_usb_tx_held() == 0U));
    pcm_samples = pcm_count;
    total_samples = pcm_count + render_count * AUDIO_FRAME_SIZE;
    if (active_mod_type == MOD_TYPE_FM) {
        target_total = (int32_t)TRANSPORT_TARGET_TOTAL_SAMPLES_FM;
    }

    if ((usb_alt_active != 0U) && (usb_frame_tracking_armed != 0U) &&
        (pipeline_hard_lockout == 0U)) {
        mod_usb_frame_plan_t plan = mod_usb_sof_frame_plan(
            usb_frame_tracking_armed, usb_expected_frame_number,
            usb_frame_number);
        if (plan.sequence_error == 0U) {
            for (uint16_t index = 0U;
                 (index < plan.missing_count) && (pipeline_hard_lockout == 0U) &&
                 (usb_frame_tracking_armed != 0U);
                 index++) {
                uint16_t missing = usb_expected_frame_number;
                mod_usb_conceal_missing_frame(missing,
                                              MOD_PIPELINE_REASON_USB_MISSING_PACKET,
                                              missing);
            }
        } else {
            mod_pipeline_note_severity(MOD_PIPELINE_STAGE_USB,
                                       MOD_PIPELINE_REASON_USB_FRAME_SEQUENCE,
                                       MOD_PIPELINE_SEVERITY_HARD,
                                       ((uint32_t)usb_expected_frame_number << 16) |
                                       usb_frame_number);
        }
    }

    mod_usb_feedback_state_t feedback = {
        usb_feedback_integrator, usb_feedback_q10_14
    };
    mod_usb_feedback_step(&feedback, feedback_active, pcm_samples,
                          total_samples, target_total,
                          PCM_LOW_WATER_SAMPLES, PCM_HIGH_WATER_SAMPLES,
                          USB_FEEDBACK_NOMINAL_Q10_14,
                          USB_FEEDBACK_I_CLAMP,
                          USB_FEEDBACK_TRIM_MAX_Q10_14);
    usb_feedback_integrator = feedback.integrator;
    usb_feedback_q10_14 = feedback.feedback_q10_14;
}

void MOD_RequestRealtimeRender(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

static uint8_t mod_realtime_render_request_needed(uint32_t now)
{
    uint32_t target;

    if ((pipeline_hard_lockout != 0U) && (pipeline_ack_requested == 0U)) return 0U;
    if ((requested_config != active_config) || (requested_reset != 0U) ||
        (cfg_muted != 0U)) return 0U;
    if ((MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) && (local_audio_armed == 0U)) return 0U;

    if (active_internal_source != 0U) {
        if ((pipeline_hard_lockout == 0U) &&
            (mod_transport_stream_needed(now) == 0U)) return 0U;
        target = ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST)
               ? 2U
               : ((active_mod_type == MOD_TYPE_CW)
                  ? TRANSPORT_RENDER_TARGET_FRAMES_CW
                  : TRANSPORT_RENDER_TARGET_FRAMES_INTERNAL);
        return (render_count < target) ? 1U : 0U;
    }

    if (transport_started == 0U) {
        uint32_t threshold = (active_mod_type == MOD_TYPE_FM)
                           ? TRANSPORT_START_THRESHOLD_FM
                           : TRANSPORT_START_THRESHOLD;
        return (pcm_count >= threshold) ? 1U : 0U;
    }
    target = (active_mod_type == MOD_TYPE_FM)
           ? TRANSPORT_RENDER_TARGET_FRAMES_FM
           : TRANSPORT_RENDER_TARGET_FRAMES_EXTERNAL;
    return ((render_count < target) && (pcm_count >= AUDIO_FRAME_SIZE)) ? 1U : 0U;
}

void MOD_RealtimeRenderWorker(void)
{
    if (RADIO_CalActive() || RADIO_State() == RADIO_OFF) return;
    uint32_t now = HAL_GetTick();
    uint32_t rendered = 0U;
    uint32_t target = 0U;
    uint32_t budget = 0U;

    if ((pipeline_hard_lockout != 0U) && (pipeline_ack_requested == 0U)) return;
    if ((requested_config != active_config) || (requested_reset != 0U)) return;
    if (cfg_muted != 0U) return;
    if ((MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) && (local_audio_armed == 0U)) return;

    if (active_internal_source != 0U) {
        if ((pipeline_hard_lockout == 0U) &&
            (mod_transport_stream_needed(now) == 0U)) return;
        target = ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST)
               ? 2U
               : ((active_mod_type == MOD_TYPE_CW)
                  ? TRANSPORT_RENDER_TARGET_FRAMES_CW
                  : TRANSPORT_RENDER_TARGET_FRAMES_INTERNAL);
        budget = ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST)
               ? 1U
               : ((active_mod_type == MOD_TYPE_CW)
                  ? TRANSPORT_RENDER_BUDGET_CW
                  : TRANSPORT_RENDER_BUDGET_INTERNAL);
        /* A keyer or the TX TEST carrier starting its stream: this run fills
         * the render queue, a receive half or two long, before the main loop
         * has marked the stream started. The receiver gives way now
         * (RADIO_TxHandover), as it does for the first unmuted USB packet,
         * instead of counting a glitch at every key-down. */
        if (transport_started == 0U) mod_tx_handover_mark();
        while ((rendered < budget) && (render_count < target)) {
            uint32_t before = render_count;
            mod_tx_pipeline_render_int();
            if (render_count == before) break;
            rendered++;
        }
        if (render_count < target) MOD_RequestRealtimeRender();
        return;
    }

    if (transport_started == 0U) {
        uint32_t threshold = (active_mod_type == MOD_TYPE_FM)
                           ? TRANSPORT_START_THRESHOLD_FM
                           : TRANSPORT_START_THRESHOLD;
        if (pcm_count < threshold) return;
        transport_started = 1U;
        mod_tx_handover_mark();
    }

    if (active_mod_type == MOD_TYPE_FM) {
        target = TRANSPORT_RENDER_TARGET_FRAMES_FM;
        budget = TRANSPORT_RENDER_BUDGET_FM;
    } else if (MOD_AudioRealtimeCritical(now) != 0U) {
        target = TRANSPORT_RENDER_TARGET_FRAMES_CRITICAL;
        budget = TRANSPORT_RENDER_BUDGET_CRITICAL;
    } else {
        target = TRANSPORT_RENDER_TARGET_FRAMES_EXTERNAL;
        budget = TRANSPORT_RENDER_BUDGET_EXTERNAL;
    }

    while ((rendered < budget) && (render_count < target) &&
           (pcm_count >= AUDIO_FRAME_SIZE)) {
        uint32_t before = render_count;
        mod_tx_pipeline_render_ext();
        if (render_count == before) break;
        rendered++;
    }
    if ((render_count < target) && (pcm_count >= AUDIO_FRAME_SIZE)) {
        MOD_RequestRealtimeRender();
    }
}

void MOD_ServiceBackground(uint32_t now)
{
    if (RADIO_CalActive() || RADIO_State() == RADIO_OFF) return;
    CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_SERVICE_BG);
    CPU_SCOPE_BEGIN(MOD_CPU_SCOPE_SERVICE_BG);
    static uint32_t last_config_defer_tick = 0U;

    cw_paddle_guard_service(now);
    MTRAIN_Service(now);
    MOD_ServiceCarrierClockRetune();
    MOD_UpdateCarrierTxReadyCache();

    /* Apply deferred config change. apply_config() handles TIM6/DMA stop
     * internally; do NOT disable IRQs here — USB interrupts must keep
     * firing throughout HRTIM reconfiguration. */
    uint32_t pm_cfg = enter_critical();
    uint32_t req = requested_config;
    uint8_t  need_reset = requested_reset;
    exit_critical(pm_cfg);

    if (req != active_config || need_reset)
    {
        if (mod_config_apply_allowed(now) == 0U) {
            if (last_config_defer_tick != now) {
                last_config_defer_tick = now;
                mod_pipeline_note(MOD_PIPELINE_STAGE_SCHEDULER,
                                  MOD_PIPELINE_REASON_SCHED_UI_DEFERRED,
                                  (req << 8) | (uint32_t)need_reset);
            }
        } else {
            pm_cfg = enter_critical();
            if ((requested_config == req) && (requested_reset == need_reset)) {
                requested_reset = 0U;
            } else {
                req = requested_config;
                requested_reset = 0U;
            }
            exit_critical(pm_cfg);
            apply_config((mod_config_t)req);
        }
    }

    if ((active_internal_source != 0U) &&
        ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) &&
        (mod_transport_stream_needed(now) == 0U)) {
        mod_tx_test_gate_reset();
    }
    /* AUTO's key-down hop, once per key-down, before the first frame is
     * rendered on the old clock: a keyer or TX TEST loses nothing, USB audio
     * the frames rendered from its first packets (the PCM behind them is
     * kept). The microphone hops at PTT, before it starts (radio_audio.c). */
    static uint8_t tx_hop_asked = 0U;
    if ((dma_streams_running == 0U) && (mod_transport_stream_needed(now) != 0U)) {
        if (tx_hop_asked == 0U) { tx_hop_asked = 1U; (void)MOD_TxClockHop(); }
    } else {
        tx_hop_asked = 0U;
    }
    if (mod_realtime_render_request_needed(now) != 0U) {
        MOD_RequestRealtimeRender();
    }
    mod_pipeline_service_rearm(now);
    mod_transport_service_dma_state(now);

    CPU_SCOPE_END(MOD_CPU_SCOPE_SERVICE_BG);
    CPU_DIAG_END(MOD_CPU_DIAG_SERVICE_BG, cpu_diag_start);
}

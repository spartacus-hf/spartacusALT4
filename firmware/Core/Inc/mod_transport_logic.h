#ifndef MOD_TRANSPORT_LOGIC_H
#define MOD_TRANSPORT_LOGIC_H

/* Pure transport/RF arithmetic shared by target code and host tests.
 * Keep this header independent of HAL/CMSIS so the implementation can be
 * compiled by an ordinary host compiler.
 */

#include <math.h>
#include <stdint.h>

#define MOD_USB_FRAME_MASK       0x07FFU
#define MOD_USB_FRAME_HALF_RANGE 0x0400U
#define MOD_DEADTIME_FINE_MULTIPLIER 8ULL
/* HRTIM_DTxR.DTPRSC: tDTG = 2^DTPRSC * tHRTIM / 8 (RM0440 table 237), so 000
 * is the 8 ticks per tHRTIM converted with above -- 735 ps at 170 MHz. HAL's
 * names for this field read backwards on G4: its "MUL8" is 110, 47 ns a tick,
 * which made every DEADTIME setting 64 times too long (1 ns held both outputs
 * of a 28 MHz branch low). */
#define MOD_DEADTIME_DTPRSC 0U
_Static_assert((1ULL << MOD_DEADTIME_DTPRSC) * MOD_DEADTIME_FINE_MULTIPLIER == 8ULL,
               "dead-time prescaler and ps<->tick conversion disagree");
#define MOD_HRTIM_COMPARE_FLOOR_MUL32 0x60U
#define MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32 \
    (2U * MOD_HRTIM_COMPARE_FLOOR_MUL32)
#define MOD_HRTIM_MAX_PERIOD_REGISTER_MUL32 0xFFDFU
/* The master spans at least two carrier periods and MPER tops out at 0xFFDF
 * (RM0440 28.5.6), so 2P must fit: P <= 32751. */
#define MOD_HRTIM_MAX_CARRIER_PERIOD_MUL32 \
    (MOD_HRTIM_MAX_PERIOD_REGISTER_MUL32 / 2U)

#if defined(__GNUC__)
#define MOD_LOGIC_INLINE static inline __attribute__((always_inline))
#else
#define MOD_LOGIC_INLINE static inline
#endif

typedef struct {
    uint8_t arm;
    uint8_t sequence_error;
    uint16_t first_missing;
    uint16_t missing_count;
} mod_usb_frame_plan_t;

MOD_LOGIC_INLINE uint16_t mod_usb_frame_next_value(uint16_t frame)
{
    return (uint16_t)((frame + 1U) & MOD_USB_FRAME_MASK);
}

MOD_LOGIC_INLINE mod_usb_frame_plan_t mod_usb_packet_frame_plan(uint8_t armed,
                                                                 uint16_t expected,
                                                                 uint16_t frame)
{
    mod_usb_frame_plan_t plan = {0U, 0U, 0U, 0U};
    frame &= MOD_USB_FRAME_MASK;
    expected &= MOD_USB_FRAME_MASK;
    if (armed == 0U) {
        plan.arm = 1U;
        return plan;
    }
    uint16_t distance = (uint16_t)((frame - expected) & MOD_USB_FRAME_MASK);
    if (distance >= MOD_USB_FRAME_HALF_RANGE) {
        plan.sequence_error = 1U;
        return plan;
    }
    plan.first_missing = expected;
    plan.missing_count = distance;
    return plan;
}

MOD_LOGIC_INLINE mod_usb_frame_plan_t mod_usb_sof_frame_plan(uint8_t armed,
                                                              uint16_t expected,
                                                              uint16_t frame)
{
    mod_usb_frame_plan_t plan = {0U, 0U, 0U, 0U};
    frame &= MOD_USB_FRAME_MASK;
    expected &= MOD_USB_FRAME_MASK;
    if (armed == 0U) return plan;

    uint16_t distance = (uint16_t)((frame - expected) & MOD_USB_FRAME_MASK);
    if (distance < MOD_USB_FRAME_HALF_RANGE) {
        plan.first_missing = expected;
        plan.missing_count = distance;
    } else if ((distance != 0U) &&
               (expected != mod_usb_frame_next_value(frame))) {
        /* The USB IRQ services CTR before SOF. expected==frame+1 is the
         * normal result when the packet and SOF flags arrived together. */
        plan.sequence_error = 1U;
    }
    return plan;
}

typedef struct {
    int32_t integrator;
    uint32_t feedback_q10_14;
} mod_usb_feedback_state_t;

typedef struct {
    float branch_a_pi;
    float branch_c_pi;
} mod_outphase_pair_t;

typedef struct {
    uint16_t carrier_period_ticks;
    uint16_t master_period_ticks;
    uint16_t local_compare_ticks;
    uint16_t master_compare_ticks;
} mod_hrtim_phase_geometry_t;

/* The compare values the atomic transport streams are MCMP1/MCMP2 reset
 * times: the compare floor plus a raw phase in [0, period). Every phase of
 * the period is legal; nothing else is. */
MOD_LOGIC_INLINE uint8_t mod_hrtim_phase_compare_legal(uint32_t carrier_period_ticks, uint32_t value)
{
    return (uint8_t)((value >= MOD_HRTIM_COMPARE_FLOOR_MUL32) &&
                     ((value - MOD_HRTIM_COMPARE_FLOOR_MUL32) < carrier_period_ticks));
}

/* HRTIM output set/reset sources, as the bits of SETx1R/RSTx1R/SETx2R/RSTx2R
 * (RM0440; the same positions in all four registers). */
#define MOD_HRTIM_EDGE_PER     (1UL << 2)
#define MOD_HRTIM_EDGE_CMP1    (1UL << 3)
#define MOD_HRTIM_EDGE_MSTCMP1 (1UL << 8)
#define MOD_HRTIM_EDGE_MSTCMP2 (1UL << 9)

typedef struct {
    uint32_t set1;
    uint32_t reset1;
    uint32_t set2;
    uint32_t reset2;
} mod_hrtim_branch_edges_t;

/* Pure form of the production outphase branch law. Phase values use the
 * firmware's established unit of pi radians: 0.5 is 90 degrees. */
MOD_LOGIC_INLINE mod_outphase_pair_t mod_outphase_branch_pair_pi(float theta_pi,
                                                                 float differential_pi,
                                                                 uint8_t swap)
{
    mod_outphase_pair_t pair = {
        theta_pi + differential_pi,
        -theta_pi - differential_pi
    };
    if (swap != 0U) {
        float temporary = pair.branch_a_pi;
        pair.branch_a_pi = pair.branch_c_pi;
        pair.branch_c_pi = temporary;
    }
    return pair;
}

MOD_LOGIC_INLINE void mod_usb_feedback_step(mod_usb_feedback_state_t *state,
                                            uint8_t active,
                                            uint32_t pcm_samples,
                                            uint32_t total_samples,
                                            int32_t target_total,
                                            uint32_t low_water,
                                            uint32_t high_water,
                                            uint32_t nominal_q10_14,
                                            int32_t integrator_clamp,
                                            int32_t trim_max_q10_14)
{
    if (state == 0) return;
    if ((active == 0U) || (target_total <= 0) ||
        (integrator_clamp < 0) || (trim_max_q10_14 < 0)) {
        state->integrator = 0;
        state->feedback_q10_14 = nominal_q10_14;
        return;
    }

    int64_t err_wide = (int64_t)target_total - (int64_t)total_samples;
    if (err_wide > target_total) err_wide = target_total;
    if (err_wide < -target_total) err_wide = -target_total;
    int32_t err = (int32_t)err_wide;
    if (pcm_samples >= high_water) {
        err = -target_total;
    } else if ((pcm_samples <= low_water) &&
               (total_samples < (uint32_t)target_total)) {
        err = target_total;
    }
    if (err > target_total) err = target_total;
    if (err < -target_total) err = -target_total;

    int64_t integrated = (int64_t)state->integrator + err;
    if (integrated > integrator_clamp) integrated = integrator_clamp;
    if (integrated < -integrator_clamp) integrated = -integrator_clamp;
    state->integrator = (int32_t)integrated;

    int64_t delta_wide = (int64_t)err * 16LL + state->integrator / 8;
    if (delta_wide > trim_max_q10_14) delta_wide = trim_max_q10_14;
    if (delta_wide < -trim_max_q10_14) delta_wide = -trim_max_q10_14;
    int64_t target_feedback = (int64_t)nominal_q10_14 + delta_wide;
    if (target_feedback < 0LL) target_feedback = 0LL;
    uint64_t filtered = ((uint64_t)state->feedback_q10_14 * 7ULL +
                         (uint64_t)target_feedback) / 8ULL;
    state->feedback_q10_14 =
        (filtered > UINT32_MAX) ? UINT32_MAX : (uint32_t)filtered;
}

enum {
    MOD_DMA_EVENT_HT = 0x01U,
    MOD_DMA_EVENT_TC = 0x02U,
    MOD_DMA_EVENT_ERROR = 0x04U
};

typedef enum {
    MOD_DMA_CHECK_OK = 0,
    MOD_DMA_CHECK_NO_EVENT,
    MOD_DMA_CHECK_TRANSFER_ERROR,
    MOD_DMA_CHECK_FLAGS_COALESCED,
    MOD_DMA_CHECK_CNDTR_ANOMALY,
    MOD_DMA_CHECK_CHANNEL_SKEW,
    MOD_DMA_CHECK_SEQUENCE_GAP,
    MOD_DMA_CHECK_REFILL_LATE
} mod_dma_check_t;

typedef struct {
    mod_dma_check_t result;
    uint8_t half;
    uint8_t next_expected_half;
} mod_dma_boundary_result_t;

MOD_LOGIC_INLINE void mod_dma_store_ordered_pair(uint32_t *words,
                                                 uint32_t pair_index,
                                                 uint32_t mcmp1,
                                                 uint32_t mcmp2)
{
    words[pair_index * 2U] = mcmp1;
    words[pair_index * 2U + 1U] = mcmp2;
}

MOD_LOGIC_INLINE uint32_t mod_logic_abs_diff_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

MOD_LOGIC_INLINE mod_dma_boundary_result_t mod_dma_validate_boundary(
    uint8_t events,
    uint32_t cndtr1,
    uint32_t cndtr2,
    uint32_t frame_samples,
    uint32_t buffer_samples,
    uint8_t expected_valid,
    uint8_t expected_half)
{
    mod_dma_boundary_result_t out = {MOD_DMA_CHECK_NO_EVENT, 0U, expected_half};
    if ((events & MOD_DMA_EVENT_ERROR) != 0U) {
        out.result = MOD_DMA_CHECK_TRANSFER_ERROR;
        return out;
    }
    uint8_t boundary = events & (MOD_DMA_EVENT_HT | MOD_DMA_EVENT_TC);
    if (boundary == (MOD_DMA_EVENT_HT | MOD_DMA_EVENT_TC)) {
        out.result = MOD_DMA_CHECK_FLAGS_COALESCED;
        return out;
    }
    if (boundary == 0U) return out;
    if ((frame_samples == 0U) ||
        ((uint64_t)buffer_samples != (uint64_t)frame_samples * 2ULL) ||
        (cndtr1 > buffer_samples) || (cndtr2 > buffer_samples)) {
        out.result = MOD_DMA_CHECK_CNDTR_ANOMALY;
        return out;
    }
    if (mod_logic_abs_diff_u32(cndtr1, cndtr2) > 1U) {
        out.result = MOD_DMA_CHECK_CHANNEL_SKEW;
        return out;
    }

    if (boundary == MOD_DMA_EVENT_HT) {
        out.half = 0U;
        out.next_expected_half = 1U;
        if ((expected_valid != 0U) && (expected_half != 0U)) {
            out.result = MOD_DMA_CHECK_SEQUENCE_GAP;
        } else if ((cndtr1 == 0U) || (cndtr1 > frame_samples)) {
            out.result = MOD_DMA_CHECK_REFILL_LATE;
        } else {
            out.result = MOD_DMA_CHECK_OK;
        }
    } else {
        out.half = 1U;
        out.next_expected_half = 0U;
        if ((expected_valid != 0U) && (expected_half != 1U)) {
            out.result = MOD_DMA_CHECK_SEQUENCE_GAP;
        } else if ((cndtr1 <= frame_samples) || (cndtr1 > buffer_samples)) {
            out.result = MOD_DMA_CHECK_REFILL_LATE;
        } else {
            out.result = MOD_DMA_CHECK_OK;
        }
    }
    return out;
}

MOD_LOGIC_INLINE mod_dma_check_t mod_dma_validate_refill(
    uint8_t half,
    uint32_t cndtr1,
    uint32_t cndtr2,
    uint32_t frame_samples,
    uint32_t buffer_samples,
    uint32_t min_slack_samples)
{
    if ((frame_samples == 0U) ||
        ((uint64_t)buffer_samples != (uint64_t)frame_samples * 2ULL) ||
        (cndtr1 > buffer_samples) || (cndtr2 > buffer_samples)) {
        return MOD_DMA_CHECK_CNDTR_ANOMALY;
    }
    if (mod_logic_abs_diff_u32(cndtr1, cndtr2) > 1U) {
        return MOD_DMA_CHECK_CHANNEL_SKEW;
    }
    if (half == 0U) {
        if ((cndtr1 > frame_samples) || (cndtr1 < min_slack_samples)) {
            return MOD_DMA_CHECK_REFILL_LATE;
        }
    } else if (half == 1U) {
        if ((cndtr1 <= frame_samples) ||
            ((uint64_t)cndtr1 < (uint64_t)frame_samples + min_slack_samples)) {
            return MOD_DMA_CHECK_REFILL_LATE;
        }
    } else {
        return MOD_DMA_CHECK_CNDTR_ANOMALY;
    }
    return MOD_DMA_CHECK_OK;
}

/* One HRTIM burst consumes two adjacent DMA words for every rendered sample.
 * Normalize that physical word count back to complete sample pairs before
 * applying the same circular-boundary contract as the legacy two-channel
 * publisher. An odd CNDTR means the controller is between MCMP1 and MCMP2 and
 * is never a safe half-buffer boundary. */
MOD_LOGIC_INLINE mod_dma_boundary_result_t mod_dma_validate_paired_boundary(
    uint8_t events,
    uint32_t cndtr_words,
    uint32_t frame_samples,
    uint32_t buffer_samples,
    uint8_t expected_valid,
    uint8_t expected_half)
{
    mod_dma_boundary_result_t out = {MOD_DMA_CHECK_NO_EVENT, 0U, expected_half};
    uint64_t buffer_words = (uint64_t)buffer_samples * 2ULL;
    if ((buffer_words > UINT32_MAX) ||
        (cndtr_words > (uint32_t)buffer_words) ||
        ((cndtr_words & 1U) != 0U)) {
        out.result = MOD_DMA_CHECK_CNDTR_ANOMALY;
        return out;
    }
    return mod_dma_validate_boundary(events,
                                     cndtr_words / 2U,
                                     cndtr_words / 2U,
                                     frame_samples,
                                     buffer_samples,
                                     expected_valid,
                                     expected_half);
}

MOD_LOGIC_INLINE mod_dma_check_t mod_dma_validate_paired_refill(
    uint8_t half,
    uint32_t cndtr_words,
    uint32_t frame_samples,
    uint32_t buffer_samples,
    uint32_t min_slack_samples)
{
    uint64_t buffer_words = (uint64_t)buffer_samples * 2ULL;
    if ((buffer_words > UINT32_MAX) ||
        (cndtr_words > (uint32_t)buffer_words) ||
        ((cndtr_words & 1U) != 0U)) {
        return MOD_DMA_CHECK_CNDTR_ANOMALY;
    }
    return mod_dma_validate_refill(half,
                                   cndtr_words / 2U,
                                   cndtr_words / 2U,
                                   frame_samples,
                                   buffer_samples,
                                   min_slack_samples);
}

/* CW keeps its render stream -- and with it the refusal of retunes, mode
 * changes and blocking I/O -- while the keyer is doing anything: a paddle
 * down, a symbol being sent or shaped, RF on, or less than the break-in
 * hold-off since the last keying while the radio has not yet gone back to
 * receive. Otherwise the keyer needs no stream and CW may be retuned like
 * any idle mode; the transport, DMA and queue checks at each call site
 * remain the guard that it really has stopped.
 *
 * Back in receive the hold-off no longer holds it: the RF DMA refill and
 * the idle renderer take the CPU that the receiver needs (together they are
 * over 100 %), so after every over the receiver missed its deadline every
 * few halves for the rest of the 2 s -- AUDIO GLITCHES, and on v8.06 once a
 * render underrun lockout. */
MOD_LOGIC_INLINE uint8_t mod_cw_keyer_needs_stream(uint8_t paddle_down,
                                                   uint8_t tx_live,
                                                   uint8_t rf_enabled,
                                                   uint8_t key_sample_active,
                                                   uint8_t radio_receiving,
                                                   uint32_t ms_since_keying,
                                                   uint32_t holdoff_ms)
{
    return (uint8_t)(((paddle_down | tx_live | rf_enabled | key_sample_active) != 0U) ||
                     ((radio_receiving == 0U) && (ms_since_keying <= holdoff_ms)));
}

/* TX CARRIER TEST: whether its carrier stream should run. Any sign of the key
 * -- the raw lever or the renderer's debounced gate -- holds it for a release
 * hang, so it ends only once the key has stayed up. Deciding on the raw lever
 * alone let contact bounce inside a key-down -- a
 * paddle reading open for ~4 ms, bench 2026-09-30 -- stop the RF DMA under a
 * live carrier and restart it a millisecond later; the restarted stream then
 * missed its 3 ms progress window and latched a hard RENDER_DEADLINE lockout
 * with FAULT_DMA. */
#define MOD_TX_TEST_RELEASE_HANG_MS 25U
MOD_LOGIC_INLINE uint8_t mod_tx_test_stream_wanted(uint8_t raw_lever, uint8_t gate_active,
                                                   uint32_t ms_since_key_seen)
{
    return (uint8_t)((raw_lever != 0U) || (gate_active != 0U) ||
                     (ms_since_key_seen <= MOD_TX_TEST_RELEASE_HANG_MS));
}

/* Whether the main loop may stop the RF DMA streams. Never while the
 * transmitter is up (TX PREPARE or TRANSMIT, or RF still permitted): the
 * renderer feeds null frames until the radio has left transmit, which it does
 * within its 80 ms request window. Stopping the stream first froze the HRTIM
 * compares under a live PA. Faults stop it through the emergency path, not
 * here. */
MOD_LOGIC_INLINE uint8_t mod_transport_may_stop_stream(uint8_t want_stream, uint8_t tx_up)
{
    return (uint8_t)((want_stream == 0U) && (tx_up == 0U));
}

/* The RF DMA progress watchdog (MOD_RealtimeHealthTick, 1 kHz): how many
 * ticks without a refill boundary it tolerates. A half lasts 1 ms; a stream
 * just started owes its first boundary one half plus the start itself, so it
 * gets a longer first window rather than a trip on a slow start. */
#define MOD_RF_STALL_TICKS       3U
#define MOD_RF_STALL_TICKS_START 6U
MOD_LOGIC_INLINE uint8_t mod_rf_stall_tripped(uint8_t stalled_ticks, uint8_t first_boundary_pending)
{
    return (uint8_t)(stalled_ticks >= (first_boundary_pending ? MOD_RF_STALL_TICKS_START
                                                               : MOD_RF_STALL_TICKS));
}

/* A hard pipeline lockout re-arms once acknowledged and either the transport
 * is idle (no stream wanted, none running: nothing can radiate, so nothing
 * needs proving) or a live source has refilled its queue. Requiring the refill
 * alone left an acknowledged lockout latched forever while TX was muted or the
 * key was up -- the renderer does not run then. */
MOD_LOGIC_INLINE uint8_t mod_pipeline_may_rearm(uint8_t transport_idle, uint8_t source_ready,
                                                uint32_t render_count, uint32_t target,
                                                uint8_t carrier_ready)
{
    if (transport_idle != 0U) return 1U;
    return (uint8_t)((source_ready != 0U) && (render_count >= target) && (carrier_ready != 0U));
}

uint16_t mod_deadtime_ticks_from_ps_at_clock(uint32_t ps, uint32_t hrtim_input_hz);
uint32_t mod_deadtime_ps_from_ticks_at_clock(uint16_t ticks, uint32_t hrtim_input_hz);
uint32_t mod_deadtime_ps_limit_at_clock(uint16_t period_ticks,
                                        uint64_t effective_compare_hz,
                                        uint32_t half_cycle_limit_pct);
int16_t mod_duty_permille_from_ticks(uint16_t half_period_ticks,
                                     int16_t ticks,
                                     int16_t permille_scale);
int16_t mod_duty_ticks_from_permille(uint16_t half_period_ticks,
                                     int16_t permille,
                                     int16_t permille_scale);
/* Inline: every renderer turns two branch phases a sample into ticks
 * through these, and as calls into this file they were a few percent of
 * the CPU that USB-audio SSB needs. */
MOD_LOGIC_INLINE float mod_phase_wrap_pi(float phase_pi)
{
    if (!isfinite(phase_pi)) return 0.0f;
    if ((phase_pi >= 1.0f) || (phase_pi < -1.0f)) {
        /* fmodf(t, 2) without the libm call where it is exact: t - 2 on
         * [2, 4) and t + 2 on (-4, -2) (Sterbenz), t itself on (-2, 0).
         * Every renderer wraps two branch phases a sample, most just past
         * +-1; the ends of each range keep fmodf and its signed zeros. */
        const float t = phase_pi + 1.0f;
        if ((t >= 2.0f) && (t < 4.0f)) phase_pi = t - 2.0f;
        else if ((t > -4.0f) && (t < -2.0f)) phase_pi = t + 2.0f;
        else if ((t > -2.0f) && (t < 0.0f)) phase_pi = t;
        else phase_pi = fmodf(t, 2.0f);
        if (phase_pi < 0.0f) phase_pi += 2.0f;
        phase_pi -= 1.0f;
    }
    return phase_pi;
}

MOD_LOGIC_INLINE uint16_t mod_hrtim_phase_raw_tick(uint16_t carrier_period_ticks, float phase_pi)
{
    if (carrier_period_ticks == 0U) return 0U;
    phase_pi = mod_phase_wrap_pi(phase_pi);
    /* A turn is the whole period. Twice (period / 2) is a tick short of an
     * odd period, and the tick lost at every wrap phase-modulated the
     * carrier once a turn: a tone at f0 from the dial came out with the dial
     * carrier, the mirror and 3 f0 near -51 dBc (period 373, 14 MHz). The
     * same on an even period. */
    float raw_float = (phase_pi + 1.0f) * (0.5f * (float)carrier_period_ticks);
    int32_t raw_tick = (int32_t)(raw_float + 0.5f);
    if (raw_tick < 0) raw_tick += (int32_t)carrier_period_ticks;
    if (raw_tick >= (int32_t)carrier_period_ticks) {
        raw_tick -= (int32_t)carrier_period_ticks;
    }
    return (uint16_t)raw_tick;
}

uint8_t mod_hrtim_phase_geometry(uint16_t carrier_period_ticks,
                                 int16_t duty_offset_ticks,
                                 uint16_t raw_phase_tick,
                                 mod_hrtim_phase_geometry_t *geometry);
/* The same with a fractional carrier (clock_plan.h clock_carrier_t): the
 * master runs `cycles` carrier periods plus `extra` ticks, so the carrier
 * averages P + extra/cycles. mod_hrtim_phase_geometry is cycles 2, extra 0. */
#define MOD_HRTIM_FRAC_EXTRA_MAX 8
/* A positive extra resets the branch timer at count `extra`, inside the high
 * half: it must stay clear of CMP1's window (one tHRTIM, 32 ticks, before
 * it) at the smallest CMP1, with room for the NCO's delays. */
_Static_assert(MOD_HRTIM_FRAC_EXTRA_MAX + 32 < MOD_HRTIM_COMPARE_FLOOR_MUL32,
               "a positive nudge reaches CMP1's window");
uint8_t mod_hrtim_phase_geometry_frac(uint16_t carrier_period_ticks,
                                      int16_t duty_offset_ticks,
                                      uint16_t raw_phase_tick,
                                      uint8_t cycles,
                                      int8_t extra,
                                      mod_hrtim_phase_geometry_t *geometry);
/* The duty offsets mod_hrtim_phase_geometry accepts at this period, *lo..*hi
 * (0 and 0, returning 0, for a period it refuses); _frac for a carrier whose
 * master adds `extra` ticks (a negative one takes room from the low half). */
uint8_t mod_hrtim_duty_offset_bounds(uint16_t carrier_period_ticks, int16_t *lo, int16_t *hi);
uint8_t mod_hrtim_duty_offset_bounds_frac(uint16_t carrier_period_ticks, int8_t extra,
                                          int16_t *lo, int16_t *hi);

/* RM0440 28.3.7 (Fig. 242) for one branch update. A new master compare moves
 * the branch timer's reset: the timer is at d = (target - prev + extra) mod P
 * when it lands (`extra`: the ticks the master adds, clock_carrier_t). The
 * reset sets TA1 and resets TA2, CMP1 does the opposite. A set within one
 * tHRTIM of a reset is discarded -- TA1 then stays low for a whole period,
 * the dropped half cycle -- and within two it is a pulse of one tHRTIM, a
 * runt for the gate driver; from two to three tHRTIM the pulse is still
 * there, its edge on the tHRTIM grid. So d must stay out of
 * [CMP1 - one tHRTIM, CMP1 + guard), `guard` being MOD_HRTIM_STEP_GUARD_TICKS:
 * the static geometry keeps its three tHRTIM (MOD_HRTIM_COMPARE_FLOOR_MUL32),
 * but holding every update to three would slew 10 m SSB to a tick per update.
 * Returns `target` if it is clear of the window, else the legal raw phase
 * nearest it: the reset just before the window, or just past it. The next
 * update aims at the true target again, so nothing is carried. */
#define MOD_HRTIM_TICKS_PER_CLOCK 32
#define MOD_HRTIM_STEP_GUARD_TICKS (2 * MOD_HRTIM_TICKS_PER_CLOCK)
MOD_LOGIC_INLINE uint32_t mod_hrtim_step_legal_raw(uint32_t prev, uint32_t target, uint32_t period,
                                                   uint32_t cmp1, uint32_t guard, int32_t extra)
{
    const int32_t p = (int32_t)period;
    int32_t d = (int32_t)target - (int32_t)prev + extra;
    while (d < 0) d += p;
    while (d >= p) d -= p;
    const int32_t lo = (int32_t)cmp1 - MOD_HRTIM_TICKS_PER_CLOCK;
    if ((d < lo) || (d >= (int32_t)cmp1 + (int32_t)guard)) return target;
    const int32_t before = lo - 1;
    int32_t after = (int32_t)cmp1 + (int32_t)guard;
    if (after > p) after = p; /* d = P: the reset on the roll-over itself */
    const int32_t nd = ((d - before) <= (after - d)) ? before : after;
    int32_t v = (int32_t)prev + nd - extra;
    while (v < 0) v += p;
    while (v >= p) v -= p;
    return (uint32_t)v;
}
/* The largest step, ticks either way, an update may make without
 * mod_hrtim_step_legal_raw having to act: a delay up to one tHRTIM short of
 * CMP1, an advance up to `guard` past it (0 if none). */
MOD_LOGIC_INLINE uint32_t mod_hrtim_step_room(uint32_t period, uint32_t cmp1, uint32_t guard, int32_t extra)
{
    const int32_t delay = (int32_t)cmp1 - MOD_HRTIM_TICKS_PER_CLOCK - 1 - extra;
    const int32_t advance = (int32_t)period - (int32_t)cmp1 - (int32_t)guard + extra;
    const int32_t room = (delay < advance) ? delay : advance;
    return (room > 0) ? (uint32_t)room : 0U;
}
/* Edge sources for one branch's complementary outputs. `phase_reset` is the
 * master compare that resets the branch timer (MOD_HRTIM_EDGE_MSTCMP1 for A,
 * MSTCMP2 for C), or 0 for edges from the timer alone; `swapped` exchanges
 * the two outputs (PA GEOMETRY A INVERTED). */
mod_hrtim_branch_edges_t mod_hrtim_branch_edges(uint8_t swapped, uint32_t phase_reset);

#undef MOD_LOGIC_INLINE

#endif /* MOD_TRANSPORT_LOGIC_H */

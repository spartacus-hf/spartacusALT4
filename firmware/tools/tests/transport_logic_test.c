#include "mod_transport_logic.h"

#include <stdint.h>
#include <stdio.h>

static unsigned failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void test_usb_frames(void)
{
    mod_usb_frame_plan_t plan = mod_usb_packet_frame_plan(0U, 0U, 100U);
    CHECK(plan.arm == 1U);
    CHECK(plan.missing_count == 0U);

    plan = mod_usb_packet_frame_plan(1U, 101U, 101U);
    CHECK(plan.sequence_error == 0U);
    CHECK(plan.missing_count == 0U);

    plan = mod_usb_packet_frame_plan(1U, 101U, 104U);
    CHECK(plan.first_missing == 101U);
    CHECK(plan.missing_count == 3U);

    plan = mod_usb_packet_frame_plan(1U, 2047U, 1U);
    CHECK(plan.first_missing == 2047U);
    CHECK(plan.missing_count == 2U);
    CHECK(mod_usb_frame_next_value(2047U) == 0U);

    plan = mod_usb_packet_frame_plan(1U, 300U, 299U);
    CHECK(plan.sequence_error == 1U);

    /* CTR is serviced before SOF when both USB flags arrive together. */
    plan = mod_usb_sof_frame_plan(1U, 501U, 500U);
    CHECK(plan.sequence_error == 0U);
    CHECK(plan.missing_count == 0U);
    plan = mod_usb_sof_frame_plan(1U, 500U, 503U);
    CHECK(plan.missing_count == 3U);

    plan = mod_usb_sof_frame_plan(0U, 500U, 700U);
    CHECK(plan.sequence_error == 0U);
    CHECK(plan.missing_count == 0U);

    /* Plug-in: the host selects alt setting 1 and only starts sending some
     * frames later, which is what a Linux sound server does when it probes a
     * newly attached device. MOD_RecordUsbPipelineEvent must leave tracking
     * unarmed at that point, and these two results are what makes that safe:
     * SOFs before the first packet report nothing missing, and the first
     * packet adopts the frame it actually arrived in.
     *
     * Arming on the alt-setting change instead hard-locked the RF pipeline
     * 3 ms after plug-in (STAGE USB / REASON MISS). */
    for (uint16_t idle = 0U; idle < 200U; idle++) {
        plan = mod_usb_sof_frame_plan(0U, 0U, (uint16_t)((440U + idle) & MOD_USB_FRAME_MASK));
        CHECK(plan.missing_count == 0U);
        CHECK(plan.sequence_error == 0U);
    }
    plan = mod_usb_packet_frame_plan(0U, 0U, 448U);
    CHECK(plan.arm == 1U);
    CHECK(plan.missing_count == 0U);
    /* Armed at 448, the next frame is clean and the one after skips one. */
    plan = mod_usb_packet_frame_plan(1U, mod_usb_frame_next_value(448U), 449U);
    CHECK(plan.missing_count == 0U);
    plan = mod_usb_packet_frame_plan(1U, 450U, 452U);
    CHECK(plan.first_missing == 450U);
    CHECK(plan.missing_count == 2U);

    for (uint16_t distance = 0U; distance <= MOD_USB_FRAME_MASK; distance++) {
        const uint16_t expected = 1900U;
        const uint16_t frame = (uint16_t)((expected + distance) & MOD_USB_FRAME_MASK);
        plan = mod_usb_packet_frame_plan(1U, expected, frame);
        if (distance < MOD_USB_FRAME_HALF_RANGE) {
            CHECK(plan.sequence_error == 0U);
            CHECK(plan.first_missing == expected);
            CHECK(plan.missing_count == distance);
        } else {
            CHECK(plan.sequence_error == 1U);
            CHECK(plan.missing_count == 0U);
        }

        plan = mod_usb_sof_frame_plan(1U, expected, frame);
        if (distance < MOD_USB_FRAME_HALF_RANGE) {
            CHECK(plan.sequence_error == 0U);
            CHECK(plan.missing_count == distance);
        } else if (distance == MOD_USB_FRAME_MASK) {
            CHECK(plan.sequence_error == 0U);
            CHECK(plan.missing_count == 0U);
        } else {
            CHECK(plan.sequence_error == 1U);
            CHECK(plan.missing_count == 0U);
        }
    }
}

static void test_feedback(void)
{
    const uint32_t nominal = 48U << 14;
    mod_usb_feedback_state_t state = {123, nominal + 100U};
    mod_usb_feedback_step(&state, 0U, 0U, 0U, 144, 48U, 432U,
                          nominal, 4096, 1 << 12);
    CHECK(state.integrator == 0);
    CHECK(state.feedback_q10_14 == nominal);

    mod_usb_feedback_step(&state, 1U, 50U, 100U, 144, 48U, 432U,
                          nominal, 4096, 1 << 12);
    CHECK(state.integrator > 0);
    CHECK(state.feedback_q10_14 > nominal);
    for (unsigned i = 0U; i < 200U; i++) {
        mod_usb_feedback_step(&state, 1U, 1000U, 1000U, 144, 48U, 432U,
                              nominal, 256, 128);
    }
    CHECK(state.integrator == -256);
    CHECK(state.feedback_q10_14 <= nominal + 128U);

    state.integrator = 0;
    state.feedback_q10_14 = UINT32_MAX;
    mod_usb_feedback_step(&state, 1U, 0U, 0U, 144, 48U, 432U,
                          UINT32_MAX, 4096, 4096);
    CHECK(state.feedback_q10_14 == UINT32_MAX);
}

static void test_dma_boundaries(void)
{
    mod_dma_boundary_result_t result =
        mod_dma_validate_boundary(MOD_DMA_EVENT_HT, 40U, 40U, 48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_OK);
    CHECK(result.half == 0U && result.next_expected_half == 1U);

    result = mod_dma_validate_boundary(MOD_DMA_EVENT_TC, 80U, 80U, 48U, 96U, 1U, 1U);
    CHECK(result.result == MOD_DMA_CHECK_OK);
    CHECK(result.half == 1U && result.next_expected_half == 0U);

    result = mod_dma_validate_boundary(MOD_DMA_EVENT_HT | MOD_DMA_EVENT_TC,
                                       40U, 40U, 48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_FLAGS_COALESCED);
    result = mod_dma_validate_boundary(MOD_DMA_EVENT_ERROR, 40U, 40U,
                                       48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_TRANSFER_ERROR);
    result = mod_dma_validate_boundary(MOD_DMA_EVENT_HT, 40U, 35U,
                                       48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_CHANNEL_SKEW);
    result = mod_dma_validate_boundary(MOD_DMA_EVENT_HT, 40U, 40U,
                                       48U, 96U, 1U, 1U);
    CHECK(result.result == MOD_DMA_CHECK_SEQUENCE_GAP);
    result = mod_dma_validate_boundary(MOD_DMA_EVENT_TC, 40U, 40U,
                                       48U, 96U, 1U, 1U);
    CHECK(result.result == MOD_DMA_CHECK_REFILL_LATE);
    result = mod_dma_validate_boundary(MOD_DMA_EVENT_HT, 0U, 0U,
                                       48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_REFILL_LATE);
    result = mod_dma_validate_boundary(MOD_DMA_EVENT_TC, 49U, 49U,
                                       48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_OK);
    result = mod_dma_validate_boundary(MOD_DMA_EVENT_HT, 1U, 1U,
                                       0x80000000UL, 0U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_CNDTR_ANOMALY);

    CHECK(mod_dma_validate_refill(0U, 8U, 8U, 48U, 96U, 8U) == MOD_DMA_CHECK_OK);
    CHECK(mod_dma_validate_refill(0U, 7U, 7U, 48U, 96U, 8U) == MOD_DMA_CHECK_REFILL_LATE);
    CHECK(mod_dma_validate_refill(1U, 56U, 56U, 48U, 96U, 8U) == MOD_DMA_CHECK_OK);
    CHECK(mod_dma_validate_refill(2U, 56U, 56U, 48U, 96U, 8U) == MOD_DMA_CHECK_CNDTR_ANOMALY);
}

static void test_paired_dma_boundaries(void)
{
    uint32_t words[6] = {0U};
    mod_dma_store_ordered_pair(words, 0U, 101U, 201U);
    mod_dma_store_ordered_pair(words, 1U, 102U, 202U);
    mod_dma_store_ordered_pair(words, 2U, 103U, 203U);
    CHECK(words[0] == 101U && words[1] == 201U);
    CHECK(words[2] == 102U && words[3] == 202U);
    CHECK(words[4] == 103U && words[5] == 203U);

    mod_dma_boundary_result_t result = mod_dma_validate_paired_boundary(
        MOD_DMA_EVENT_HT, 96U, 48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_OK);
    CHECK(result.half == 0U && result.next_expected_half == 1U);

    result = mod_dma_validate_paired_boundary(
        MOD_DMA_EVENT_TC, 192U, 48U, 96U, 1U, 1U);
    CHECK(result.result == MOD_DMA_CHECK_OK);
    CHECK(result.half == 1U && result.next_expected_half == 0U);
    result = mod_dma_validate_paired_boundary(
        MOD_DMA_EVENT_HT, 95U, 48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_CNDTR_ANOMALY);
    result = mod_dma_validate_paired_boundary(
        MOD_DMA_EVENT_HT, 194U, 48U, 96U, 0U, 0U);
    CHECK(result.result == MOD_DMA_CHECK_CNDTR_ANOMALY);
    CHECK(mod_dma_validate_paired_refill(0U, 16U, 48U, 96U, 8U) == MOD_DMA_CHECK_OK);
    CHECK(mod_dma_validate_paired_refill(0U, 15U, 48U, 96U, 8U) == MOD_DMA_CHECK_CNDTR_ANOMALY);

    uint8_t expected_half = 0U;
    for (uint32_t sample = 1U; sample <= 10000000U; sample++) {
        if ((sample % 48U) != 0U) continue;
        uint8_t event;
        uint32_t cndtr_words;
        if ((sample % 96U) == 0U) {
            event = MOD_DMA_EVENT_TC;
            cndtr_words = 192U;
        } else {
            event = MOD_DMA_EVENT_HT;
            cndtr_words = 96U;
        }
        result = mod_dma_validate_paired_boundary(event,
                                                  cndtr_words,
                                                  48U,
                                                  96U,
                                                  1U,
                                                  expected_half);
        CHECK(result.result == MOD_DMA_CHECK_OK);
        expected_half = result.next_expected_half;
    }
}

static void test_rf_conversions(void)
{
    CHECK(mod_deadtime_ticks_from_ps_at_clock(0U, 170000000U) == 0U);
    CHECK(mod_deadtime_ticks_from_ps_at_clock(1000000U, 170000000U) == 511U);
    CHECK(mod_deadtime_ticks_from_ps_at_clock(UINT32_MAX, UINT32_MAX) == 511U);
    /* RM0440 table 237, DTPRSC 000 at 170 MHz: tDTG 0.735 ns, eight of them
     * one fHRTIM clock. A 28 MHz half period is about 24 ticks, so the
     * console's 1 ns must stay one tick, not the 47 ns of DTPRSC 110. */
    CHECK(MOD_DEADTIME_DTPRSC == 0U);
    CHECK(mod_deadtime_ps_from_ticks_at_clock(1U, 170000000U) == 735U);
    CHECK(mod_deadtime_ps_from_ticks_at_clock(8U, 170000000U) == 5882U);
    CHECK(mod_deadtime_ticks_from_ps_at_clock(1000U, 170000000U) == 1U);
    CHECK(mod_deadtime_ticks_from_ps_at_clock(5882U, 170000000U) == 8U);
    for (uint16_t ticks = 1U; ticks <= 511U; ticks++) {
        uint32_t ps = mod_deadtime_ps_from_ticks_at_clock(ticks, 170000000U);
        uint16_t roundtrip = mod_deadtime_ticks_from_ps_at_clock(ps, 170000000U);
        CHECK(roundtrip == ticks);
    }
    /* Effective HRTIM compare time is input clock * 32 and exceeds uint32_t
     * throughout the normal 165-170 MHz operating range. These values guard
     * against silently narrowing the production clock at the API boundary. */
    CHECK(mod_deadtime_ps_limit_at_clock(388U, 5440000000ULL, 40U) == 14265U);
    CHECK(mod_deadtime_ps_limit_at_clock(377U, 5280000000ULL, 40U) == 14280U);
    CHECK(mod_deadtime_ps_limit_at_clock(34U, 170000000U, 40U) == 40000U);
    CHECK(mod_deadtime_ps_limit_at_clock(34U, UINT64_MAX, 40U) == 1U);

    CHECK(mod_duty_permille_from_ticks(50U, 25, 1000) == 500);
    CHECK(mod_duty_permille_from_ticks(50U, -25, 1000) == -500);
    CHECK(mod_duty_ticks_from_permille(50U, 500, 1000) == 25);
    CHECK(mod_duty_ticks_from_permille(50U, 2000, 1000) == 50);
    for (int16_t value = -1000; value <= 1000; value += 25) {
        int16_t ticks = mod_duty_ticks_from_permille(170U, value, 1000);
        int16_t roundtrip = mod_duty_permille_from_ticks(170U, ticks, 1000);
        int difference = roundtrip - value;
        if (difference < 0) difference = -difference;
        CHECK(difference <= 3);
    }
}

static void test_cw_idle(void)
{
    /* Any keyer activity keeps the stream, receiving or not. */
    for (uint8_t rx = 0U; rx < 2U; ++rx) {
        CHECK(mod_cw_keyer_needs_stream(1U, 0U, 0U, 0U, rx, 5000U, 2000U));
        CHECK(mod_cw_keyer_needs_stream(0U, 1U, 0U, 0U, rx, 5000U, 2000U));
        CHECK(mod_cw_keyer_needs_stream(0U, 0U, 1U, 0U, rx, 5000U, 2000U));
        CHECK(mod_cw_keyer_needs_stream(0U, 0U, 0U, 1U, rx, 5000U, 2000U));
    }
    /* The break-in hold-off holds it up to and including the limit... */
    CHECK(mod_cw_keyer_needs_stream(0U, 0U, 0U, 0U, 0U, 0U, 2000U));
    CHECK(mod_cw_keyer_needs_stream(0U, 0U, 0U, 0U, 0U, 2000U, 2000U));
    /* ...only while the radio has not gone back to receive. */
    CHECK(!mod_cw_keyer_needs_stream(0U, 0U, 0U, 0U, 1U, 0U, 2000U));
    CHECK(!mod_cw_keyer_needs_stream(0U, 0U, 0U, 0U, 1U, 2000U, 2000U));
    /* Only a fully idle keyer past the hold-off lets it go. */
    CHECK(!mod_cw_keyer_needs_stream(0U, 0U, 0U, 0U, 0U, 2001U, 2000U));
}

/* TX TEST keyed through a bouncing paddle: the lever recorded on the bench on
 * 2026-09-30 read open for ~4 ms inside a held DAH. Model the renderer's
 * two-frame debounce and check that the carrier stream is wanted through
 * every bounce and let go only after the release hang. */
static void test_tx_test_bounce(void)
{
    /* 1 ms per step: 0 = open, 1 = closed. Press with edge bounce, a 4 ms
     * and a 1 ms open glitch mid-hold, release with edge bounce. */
    static const uint8_t lever[] = {
        0,0,1,0,1,1,0,1,1,1, 1,1,1,1,1,1,1,1,1,1, 1,1,0,0,0,0,1,1,1,1,
        1,0,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,0,1,0,0, 1,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,
    };
    uint8_t stable = 0U, last_raw = 0U, count = 0U, started = 0U, stopped_at = 0U;
    uint32_t last_active = 0x80000000UL; /* long ago */
    for (uint32_t t = 0U; t < sizeof(lever); ++t) {
        const uint8_t raw = lever[t];
        /* tx_test_debounce_update, TX_TEST_DEBOUNCE_FRAMES = 2 */
        if (raw == stable) { last_raw = raw; count = 0U; }
        else {
            if (raw != last_raw) { last_raw = raw; count = 1U; }
            else if (count < 2U) count++;
            if (count >= 2U) { stable = raw; count = 0U; }
        }
        if (raw || stable) last_active = t;
        const uint8_t want = mod_tx_test_stream_wanted(raw, stable, t - last_active);
        if (want) started = 1U;
        if (started && !want && !stopped_at) stopped_at = (uint8_t)t;
        /* Once started, the stream must hold across every mid-hold glitch. */
        if (started && t < 52U) CHECK(want);
    }
    /* ...and let go once the lever has stayed open past the hang. */
    CHECK(stopped_at != 0U);
    CHECK(stopped_at > 52U && stopped_at <= 52U + 2U + MOD_TX_TEST_RELEASE_HANG_MS + 1U);

    /* The main loop never stops a stream under a transmitter that is up. */
    CHECK(!mod_transport_may_stop_stream(0U, 1U));
    CHECK(!mod_transport_may_stop_stream(1U, 0U));
    CHECK(mod_transport_may_stop_stream(0U, 0U));

    /* The progress watchdog gives a starting stream a longer first window. */
    CHECK(!mod_rf_stall_tripped(MOD_RF_STALL_TICKS, 1U));
    CHECK(mod_rf_stall_tripped(MOD_RF_STALL_TICKS_START, 1U));
    CHECK(mod_rf_stall_tripped(MOD_RF_STALL_TICKS, 0U));
    CHECK(!mod_rf_stall_tripped(MOD_RF_STALL_TICKS - 1U, 0U));

    /* An acknowledged lockout re-arms on an idle transport whatever the
     * queue holds (muted, key up), else only on a refilled live source. */
    CHECK(mod_pipeline_may_rearm(1U, 0U, 0U, 2U, 0U));
    CHECK(!mod_pipeline_may_rearm(0U, 1U, 1U, 2U, 1U));
    CHECK(!mod_pipeline_may_rearm(0U, 0U, 4U, 2U, 1U));
    CHECK(!mod_pipeline_may_rearm(0U, 1U, 4U, 2U, 0U));
    CHECK(mod_pipeline_may_rearm(0U, 1U, 2U, 2U, 1U));
}

/* The fractional carrier's geometry: the master runs N periods plus k
 * ticks, a negative k takes its ticks from the low half, and the master
 * period stays within MPER's 0xFFDF. */
static void test_fractional_geometry(void)
{
    mod_hrtim_phase_geometry_t g;
    CHECK(mod_hrtim_phase_geometry_frac(387U, 0, 0U, 2U, -1, &g) == 1U);
    CHECK(g.master_period_ticks == 773U && g.local_compare_ticks == 193U && g.master_compare_ticks == 96U);
    CHECK(mod_hrtim_phase_geometry_frac(192U, 0, 191U, 9U, 8, &g) == 1U);
    CHECK(g.master_period_ticks == 9U * 192U + 8U);
    /* N >= 2, |k| <= 8, N > |k|. */
    CHECK(mod_hrtim_phase_geometry_frac(387U, 0, 0U, 1U, 0, &g) == 0U);
    CHECK(g.master_period_ticks == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(387U, 0, 0U, 20U, 9, &g) == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(387U, 0, 0U, 20U, -9, &g) == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(387U, 0, 0U, 8U, 8, &g) == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(387U, 0, 0U, 8U, -8, &g) == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(387U, 0, 0U, 9U, -8, &g) == 1U);
    /* At P = 192 both halves are at three tHRTIM: no tick to take. */
    CHECK(mod_hrtim_phase_geometry_frac(192U, 0, 0U, 2U, -1, &g) == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(194U, 0, 0U, 2U, -1, &g) == 1U);
    CHECK(mod_hrtim_phase_geometry_frac(194U, 1, 0U, 2U, -1, &g) == 0U);
    /* MPER: 2 x 32751 + 1 = 0xFFDF is the last legal master period. */
    CHECK(mod_hrtim_phase_geometry_frac(32751U, 0, 0U, 2U, 1, &g) == 1U);
    CHECK(g.master_period_ticks == 0xFFDFU);
    CHECK(mod_hrtim_phase_geometry_frac(32751U, 0, 0U, 3U, 2, &g) == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(32751U, 0, 0U, 2U, 2, &g) == 0U);
    CHECK(mod_hrtim_phase_geometry_frac(21835U, 0, 0U, 3U, -2, &g) == 1U);
    CHECK(g.master_period_ticks == 0xFFDFU);

    /* The duty bounds follow k: every offset inside is legal, the next out
     * on either side is not. */
    for (uint32_t period = MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32; period <= 1200U; ++period) {
        for (int32_t k = -MOD_HRTIM_FRAC_EXTRA_MAX; k <= MOD_HRTIM_FRAC_EXTRA_MAX; ++k) {
            int16_t lo = 0, hi = 0;
            const uint8_t n = (uint8_t)((k < 0 ? -k : k) + 1 < 2 ? 2 : (k < 0 ? -k : k) + 1);
            CHECK(mod_hrtim_duty_offset_bounds_frac((uint16_t)period, (int8_t)k, &lo, &hi) == 1U);
            const uint8_t any = mod_hrtim_phase_geometry_frac((uint16_t)period, lo, 0U, n, (int8_t)k, &g);
            if (!any) {
                /* No room at all for this k: the bounds collapse onto the low one. */
                CHECK(k < 0 && lo == hi);
                continue;
            }
            CHECK(mod_hrtim_phase_geometry_frac((uint16_t)period, hi, 0U, n, (int8_t)k, &g) == 1U);
            CHECK(mod_hrtim_phase_geometry_frac((uint16_t)period, (int16_t)(lo - 1), 0U, n, (int8_t)k, &g) == 0U);
            CHECK(mod_hrtim_phase_geometry_frac((uint16_t)period, (int16_t)(hi + 1), 0U, n, (int8_t)k, &g) == 0U);
        }
    }
}

/* The branch-reset guard (RM0440 28.3.7): after an update the branch timer
 * is reset at count d = target - prev + k (mod P); a reset from one tHRTIM
 * before CMP1 to `guard` after it would drop or narrow a pulse. Over every
 * step at the short periods: the guard returns a legal edge, leaves every
 * legal step alone, moves an illegal one only to the nearest legal edge, and
 * never acts on a step within mod_hrtim_step_room. */
static void test_step_guard(void)
{
    static const uint16_t periods[] = {192U, 193U, 194U, 200U, 218U, 259U, 300U, 387U};
    const uint32_t guard = MOD_HRTIM_STEP_GUARD_TICKS;
    unsigned moved = 0U;
    for (unsigned pi = 0U; pi < sizeof(periods) / sizeof(periods[0]); ++pi) {
        const int32_t p = periods[pi];
        for (int32_t k = -MOD_HRTIM_FRAC_EXTRA_MAX; k <= MOD_HRTIM_FRAC_EXTRA_MAX; ++k) {
            int16_t lo = 0, hi = 0;
            (void)mod_hrtim_duty_offset_bounds_frac((uint16_t)p, (int8_t)k, &lo, &hi);
            const int16_t offsets[] = {lo, 0, hi};
            for (unsigned oi = 0U; oi < 3U; ++oi) {
                mod_hrtim_phase_geometry_t g;
                const uint8_t n = (uint8_t)((k < 0 ? -k : k) + 1 < 2 ? 2 : (k < 0 ? -k : k) + 1);
                if (!mod_hrtim_phase_geometry_frac((uint16_t)p, offsets[oi], 0U, n, (int8_t)k, &g)) continue;
                const int32_t cmp1 = g.local_compare_ticks;
                const int32_t win_lo = cmp1 - MOD_HRTIM_TICKS_PER_CLOCK;
                const int32_t win_hi = cmp1 + (int32_t)guard;
                const int32_t room = (int32_t)mod_hrtim_step_room((uint32_t)p, (uint32_t)cmp1, guard, k);
                /* With no update the nudge alone never lands in the window. */
                const int32_t d0 = ((k % p) + p) % p;
                CHECK(d0 < win_lo || d0 >= win_hi);
                for (int32_t prev = 0; prev < p; ++prev) {
                    for (int32_t target = 0; target < p; ++target) {
                        const uint32_t v = mod_hrtim_step_legal_raw((uint32_t)prev, (uint32_t)target,
                                                                    (uint32_t)p, (uint32_t)cmp1, guard, k);
                        CHECK(v < (uint32_t)p);
                        const int32_t d = ((target - prev + k) % p + p) % p;
                        const int32_t dv = (((int32_t)v - prev + k) % p + p) % p;
                        CHECK(dv < win_lo || dv >= win_hi);
                        if (d < win_lo || d >= win_hi) {
                            CHECK(v == (uint32_t)target);
                        } else {
                            ++moved;
                            /* The nearest legal edge: just before the window or at its end. */
                            const int32_t end = (win_hi >= p) ? 0 : win_hi;
                            CHECK(dv == win_lo - 1 || dv == end);
                            const int32_t to_before = d - (win_lo - 1);
                            const int32_t to_after = ((win_hi > p) ? p : win_hi) - d;
                            CHECK((dv == win_lo - 1) == (to_before <= to_after));
                        }
                        int32_t step = target - prev;
                        if (step > p / 2) step -= p;
                        if (step < -(p / 2)) step += p;
                        if (step <= room && step >= -room) CHECK(v == (uint32_t)target);
                    }
                }
            }
        }
    }
    CHECK(moved > 0U);
    /* 20 m (P 387, CMP1 193) at k = -1: steps up to 129 ticks either way
     * pass untouched; 10 m (P 192) at k = +8 still has 40. */
    CHECK(mod_hrtim_step_room(387U, 193U, guard, -1) == 129U);
    CHECK(mod_hrtim_step_room(387U, 193U, guard, 0) == 130U);
    CHECK(mod_hrtim_step_room(192U, 96U, guard, 8) == 40U);
    CHECK(mod_hrtim_step_room(192U, 96U, guard, 0) == 32U);
}

int main(void)
{
    test_tx_test_bounce();
    test_cw_idle();
    test_usb_frames();
    test_feedback();
    test_dma_boundaries();
    test_paired_dma_boundaries();
    test_rf_conversions();
    test_fractional_geometry();
    test_step_guard();
    if (failures != 0U) return 1;
    puts("transport/RF production logic: PASS");
    return 0;
}

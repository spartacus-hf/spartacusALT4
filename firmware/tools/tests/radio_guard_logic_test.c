/* The scan-level protection trips (radio_guard_logic.h): what trips, how
 * fast, and what a normal over never trips. */
#include "radio_guard_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

/* Scans of TX off at `vin`, then scans until the first trip at `tx_vin`
 * (0 = none within `max`). */
static unsigned vin_trip_after(uint16_t idle_vin, uint16_t tx_vin, unsigned max)
{
    radio_guard_t g;
    memset(&g, 0, sizeof(g));
    for (unsigned i = 0U; i < 40U; ++i) CHECK(radio_guard_idle(&g, idle_vin, 1U, 0.0f) == 0U);
    for (unsigned i = 1U; i <= max; ++i)
        if (radio_guard_tx_vin(&g, tx_vin)) return i;
    return 0U;
}

static void test_vin(void)
{
    /* 9 V contract: the normal full-drain sag to 7 V never trips; 6.2 V
     * trips on the 8th scan (2 ms). */
    CHECK(vin_trip_after(8800U, 7000U, 40000U) == 0U);
    CHECK(vin_trip_after(8800U, 6350U, 40000U) == 0U);
    CHECK(vin_trip_after(8800U, 6200U, 100U) == RADIO_GUARD_SCANS);
    CHECK(vin_trip_after(8800U, 0U, 100U) == RADIO_GUARD_SCANS);
    /* 12 V: the floor is 70 % of it. */
    CHECK(vin_trip_after(12000U, 8500U, 40000U) == 0U);
    CHECK(vin_trip_after(12000U, 8300U, 100U) == RADIO_GUARD_SCANS);
    /* Nothing learnt yet (TX straight after boot): 6.3 V. */
    radio_guard_t g;
    memset(&g, 0, sizeof(g));
    CHECK(radio_guard_vin_floor_mv(&g) == RADIO_GUARD_VIN_FLOOR_MV);
    /* Seven low scans and a good one: the count starts again. */
    memset(&g, 0, sizeof(g));
    (void)radio_guard_idle(&g, 8800U, 1U, 0.0f);
    for (unsigned k = 0U; k < 100U; ++k) {
        for (unsigned i = 0U; i < RADIO_GUARD_SCANS - 1U; ++i) CHECK(radio_guard_tx_vin(&g, 5000U) == 0U);
        CHECK(radio_guard_tx_vin(&g, 7500U) == 0U);
    }
    /* The idle figure saturates rather than wraps (VIN over 16.3 V). */
    memset(&g, 0, sizeof(g));
    (void)radio_guard_idle(&g, 20000U, 1U, 0.0f);
    CHECK(g.vin_idle == 255U);
}

static void test_stray(void)
{
    radio_guard_t g;
    memset(&g, 0, sizeof(g));
    /* Not judged (RF active, the detectors' hold): never. */
    for (unsigned i = 0U; i < 1000U; ++i) CHECK(radio_guard_idle(&g, 8800U, 0U, 5.0f) == 0U);
    /* Under 0.1 W, or NaN: never. */
    for (unsigned i = 0U; i < 1000U; ++i) CHECK(radio_guard_idle(&g, 8800U, 1U, 0.30f) == 0U);
    for (unsigned i = 0U; i < 1000U; ++i) CHECK(radio_guard_idle(&g, 8800U, 1U, NAN) == 0U);
    /* 0.1 W for 2 ms: trips on the 8th scan. */
    unsigned first = 0U;
    for (unsigned i = 1U; i <= 20U && !first; ++i) if (radio_guard_idle(&g, 8800U, 1U, 0.32f)) first = i;
    CHECK(first == RADIO_GUARD_SCANS);
    /* Interrupted: starts again. */
    memset(&g, 0, sizeof(g));
    for (unsigned k = 0U; k < 100U; ++k) {
        for (unsigned i = 0U; i < RADIO_GUARD_SCANS - 1U; ++i) CHECK(radio_guard_idle(&g, 8800U, 1U, 1.0f) == 0U);
        CHECK(radio_guard_idle(&g, 8800U, 1U, 0.0f) == 0U);
    }
    /* A stray count does not carry into TX as a |gamma| jump. */
    memset(&g, 0, sizeof(g));
    for (unsigned i = 0U; i < RADIO_GUARD_SCANS - 1U; ++i) (void)radio_guard_idle(&g, 8800U, 1U, 1.0f);
    CHECK(radio_guard_tx_vin(&g, 8000U) == 0U);
    CHECK(g.run == 0U);
}

/* An over: `learn` strong scans at |gamma| g0, then scans at g1 until the
 * first trip (0 = none within `max`). */
static unsigned swr_trip_after(float f, float g0, float g1, uint8_t tune, unsigned max)
{
    radio_guard_t g;
    memset(&g, 0, sizeof(g));
    (void)radio_guard_idle(&g, 8800U, 1U, 0.0f);
    for (unsigned i = 0U; i < RADIO_GUARD_SWR_LEARN; ++i) {
        CHECK(radio_guard_tx_vin(&g, 8000U) == 0U);
        CHECK(radio_guard_swr(&g, f, g0 * f, tune) == 0U);
    }
    for (unsigned i = 1U; i <= max; ++i) {
        (void)radio_guard_tx_vin(&g, 8000U);
        if (radio_guard_swr(&g, f, g1 * f, tune)) return i;
    }
    return 0U;
}

static void test_swr_jump(void)
{
    /* SWR 1.2 (|gamma| 0.09): steady, and up to 0.3 (SWR 1.86), never. */
    CHECK(swr_trip_after(1.5f, 0.09f, 0.09f, 0U, 40000U) == 0U);
    CHECK(swr_trip_after(1.5f, 0.09f, 0.30f, 0U, 40000U) == 0U);
    /* To 0.4 (SWR 2.3): the 8th scan. */
    CHECK(swr_trip_after(1.5f, 0.09f, 0.40f, 0U, 100U) == RADIO_GUARD_SCANS);
    /* A worse antenna to begin with: relative to it. */
    CHECK(swr_trip_after(1.5f, 0.33f, 0.55f, 0U, 40000U) == 0U);
    CHECK(swr_trip_after(1.5f, 0.33f, 0.62f, 0U, 100U) == RADIO_GUARD_SCANS);
    /* Better is never a trip. */
    CHECK(swr_trip_after(1.5f, 0.40f, 0.05f, 0U, 40000U) == 0U);
    /* TX TEST: a tuner turned with the carrier up. */
    CHECK(swr_trip_after(1.5f, 0.09f, 0.45f, 1U, 40000U) == 0U);
    /* Under 0.5 W forward: neither learnt nor judged. */
    CHECK(swr_trip_after(0.6f, 0.09f, 0.45f, 0U, 40000U) == 0U);

    /* The bridge's leakage as measured on 20 m (0.087 of forward plus a
     * 63 mV floor), learnt at full drain and then all over 0.5-3 W as speech
     * moves it: never. */
    radio_guard_t g;
    memset(&g, 0, sizeof(g));
    (void)radio_guard_idle(&g, 8800U, 1U, 0.0f);
    for (unsigned i = 0U; i < RADIO_GUARD_SWR_LEARN; ++i) (void)radio_guard_swr(&g, 1.7f, 0.087f * 1.7f + 0.063f, 0U);
    uint32_t seed = 12345U;
    for (unsigned i = 0U; i < 200000U; ++i) {
        seed = seed * 1664525U + 1013904223U;
        const float f = 0.707f + 1.0f * (float)(seed >> 8) / 16777216.0f;
        CHECK(radio_guard_swr(&g, f, 0.087f * f + 0.063f, 0U) == 0U);
    }
    /* Interrupted by a weak scan (a gap in speech): starts again. */
    for (unsigned k = 0U; k < 100U; ++k) {
        for (unsigned i = 0U; i < RADIO_GUARD_SCANS - 1U; ++i) CHECK(radio_guard_swr(&g, 1.5f, 0.9f, 0U) == 0U);
        CHECK(radio_guard_swr(&g, 0.2f, 0.0f, 0U) == 0U);
    }
    /* NaN never trips. */
    for (unsigned i = 0U; i < 100U; ++i) CHECK(radio_guard_swr(&g, 1.5f, NAN, 0U) == 0U);
    for (unsigned i = 0U; i < 100U; ++i) CHECK(radio_guard_swr(&g, NAN, 1.0f, 0U) == 0U);
    /* The next over learns afresh. */
    (void)radio_guard_idle(&g, 8800U, 1U, 0.0f);
    CHECK(g.swr_n == 0U);
}

int main(void)
{
    test_vin();
    test_stray();
    test_swr_jump();
    if (failures != 0U) {
        fprintf(stderr, "radio_guard_logic: %u failures\n", failures);
        return 1;
    }
    printf("radio_guard_logic: VIN floor max(6.3 V, 70 %%) in 2 ms, 9->7 V sag kept; |gamma| +0.25 in 2 ms, "
           "leakage over 0.5-1.7 W and TX TEST kept; stray RF 0.1 W in 2 ms; counts restart on a good scan\n");
    return 0;
}

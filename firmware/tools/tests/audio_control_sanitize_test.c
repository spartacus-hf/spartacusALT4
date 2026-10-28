#include "mod_control_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static unsigned failures;

#define CHECK_CLOSE(actual, expected) do { \
    float check_value = (actual); \
    if (!isfinite(check_value) || fabsf(check_value - (expected)) > 1.0e-7f) { \
        fprintf(stderr, "%s:%d: got %.9g, expected %.9g\n", \
                __FILE__, __LINE__, (double)check_value, (double)(expected)); \
        failures++; \
    } \
} while (0)

static void test_nan_uses_safe_defaults(void)
{
    CHECK_CLOSE(mod_control_sanitize(NAN, 0.95f, 0.1f, 1.0f), 0.95f);
    CHECK_CLOSE(mod_control_sanitize(NAN, 0.004f, 0.0f, 0.05f), 0.004f);
    CHECK_CLOSE(mod_control_sanitize(NAN, 2500.0f, 500.0f, 7500.0f), 2500.0f);
    CHECK_CLOSE(mod_control_sanitize(NAN, 0.75f, 0.1f, 1.0f), 0.75f);
    CHECK_CLOSE(mod_control_sanitize(NAN, 0.0f, -6.0f, 6.0f), 0.0f);
}

static void test_infinities_clamp_to_declared_limits(void)
{
    CHECK_CLOSE(mod_control_sanitize(INFINITY, 2500.0f, 500.0f, 7500.0f), 7500.0f);
    CHECK_CLOSE(mod_control_sanitize(-INFINITY, 0.55f, 0.0f, 1.0f), 0.0f);
    CHECK_CLOSE(mod_control_sanitize(INFINITY, 4.0f, 1.0f, 20.0f), 20.0f);
    CHECK_CLOSE(mod_control_sanitize(-INFINITY, 0.0f, -6.0f, 6.0f), -6.0f);
}

static void test_invalid_enums_use_declared_fallback(void)
{
    if (mod_control_enum_sanitize(2U, 4U, 1U) != 2U) failures++;
    if (mod_control_enum_sanitize((uint32_t)-1, 4U, 1U) != 1U) failures++;
    if (mod_control_enum_sanitize(4U, 4U, 1U) != 1U) failures++;
}

static void test_bounded_u8_rounding_avoids_nonfinite_conversion(void)
{
    if (mod_control_round_u8(NAN, 3U, 7U) != 3U) failures++;
    if (mod_control_round_u8(INFINITY, 3U, 7U) != 7U) failures++;
    if (mod_control_round_u8(-INFINITY, 3U, 7U) != 0U) failures++;
    if (mod_control_round_u8(4.49f, 3U, 7U) != 4U) failures++;
    if (mod_control_round_u8(4.50f, 3U, 7U) != 5U) failures++;
}

static void test_deadtime_conversion_avoids_nonfinite_conversion(void)
{
    if (mod_control_ns_to_ps(NAN) != 0U) failures++;
    if (mod_control_ns_to_ps(INFINITY) != 0U) failures++;
    if (mod_control_ns_to_ps(-INFINITY) != 0U) failures++;
    if (mod_control_ns_to_ps(-1.0f) != 0U) failures++;
    if (mod_control_ns_to_ps(12.345f) != 12345U) failures++;
    if (mod_control_ns_to_ps(5000000.0f) != UINT32_MAX) failures++;
}

static void test_rf_activity_metric_fails_closed(void)
{
    uint8_t active = 1U;
    CHECK_CLOSE(mod_control_activity_metric(NAN, &active), 0.0f);
    if (active != 0U) failures++;

    active = 1U;
    CHECK_CLOSE(mod_control_activity_metric(INFINITY, &active), 0.0f);
    if (active != 0U) failures++;

    active = 1U;
    CHECK_CLOSE(mod_control_activity_metric(1.5f, &active), 1.0f);
    if (active != 1U) failures++;
}

static void test_hot_path_finite_masks(void)
{
    float first = 0.25f;
    float second = -0.5f;
    mod_control_finite_pair_or_zero(&first, &second);
    CHECK_CLOSE(first, 0.25f);
    CHECK_CLOSE(second, -0.5f);

    first = NAN;
    second = 0.5f;
    mod_control_finite_pair_or_zero(&first, &second);
    CHECK_CLOSE(first, 0.0f);
    CHECK_CLOSE(second, 0.0f);

    first = -0.25f;
    second = -INFINITY;
    mod_control_finite_pair_or_zero(&first, &second);
    CHECK_CLOSE(first, 0.0f);
    CHECK_CLOSE(second, 0.0f);
}

int main(void)
{
    test_nan_uses_safe_defaults();
    test_infinities_clamp_to_declared_limits();
    test_invalid_enums_use_declared_fallback();
    test_bounded_u8_rounding_avoids_nonfinite_conversion();
    test_deadtime_conversion_avoids_nonfinite_conversion();
    test_rf_activity_metric_fails_closed();
    test_hot_path_finite_masks();
    if (failures != 0U) return 1;
    puts("control sanitization: finite scalars, enums, and bounded rounding: PASS");
    return 0;
}

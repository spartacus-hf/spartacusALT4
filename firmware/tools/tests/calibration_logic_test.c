#include "mod_calibration_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

static void test_legacy_decomposition(void)
{
    phase_cal_legacy_t legacy;
    phase_cal_t phase;
    float old_theta[PHASE_CAL_BINS];
    float theta_pd[PHASE_CAL_BINS];
    memset(&legacy, 0, sizeof(legacy));
    legacy.static_trim = 0.031f;
    legacy.amp_trim_a = -0.017f;
    legacy.amp_trim_c = 0.009f;
    for (uint32_t i = 0U; i < PHASE_CAL_BINS; i++) {
        legacy.envelope_table[i] = -0.04f + 0.08f * (float)i / (float)(PHASE_CAL_BINS - 1U);
        old_theta[i] = 0.01f - 0.02f * (float)i / (float)(PHASE_CAL_BINS - 1U);
    }

    mod_calibration_convert_legacy(&phase, theta_pd, &legacy, old_theta);
    for (uint32_t sample = 0U; sample <= 1000U; sample++) {
        float envelope = (float)sample / 1000.0f;
        float phi = -0.93f + 1.86f * envelope;
        float theta = 0.49f - 0.47f * envelope;
        float old_env = mod_calibration_interp(legacy.envelope_table, envelope);
        float old_theta_corr = mod_calibration_interp(old_theta, envelope);
        float old_a = phi + theta + old_theta_corr + legacy.static_trim
                    + old_env + legacy.amp_trim_a;
        float old_c = phi - theta - old_theta_corr - legacy.static_trim
                    - old_env + legacy.amp_trim_c;
        float common = mod_calibration_interp(phase.common_phase_pd, envelope);
        float differential = phase.differential_skew
                           + mod_calibration_interp(theta_pd, envelope);
        float new_a = phi + common + theta + differential;
        float new_c = phi + common - theta - differential;
        CHECK(fabsf(old_a - new_a) < 2.0e-6f);
        CHECK(fabsf(old_c - new_c) < 2.0e-6f);
    }
}

static void test_flags_and_interpolation(void)
{
    float table[PHASE_CAL_BINS];
    for (uint32_t i = 0U; i < PHASE_CAL_BINS; i++) table[i] = (float)i;
    CHECK(mod_calibration_interp(table, -1.0f) == 0.0f);
    CHECK(mod_calibration_interp(table, 1.0f) == (float)(PHASE_CAL_BINS - 1U));
    CHECK(fabsf(mod_calibration_interp(table, 0.5f) - 15.5f) < 1.0e-6f);
    CHECK(mod_calibration_interp(table, NAN) == 0.0f);
    CHECK(mod_calibration_interp(table, -INFINITY) == 0.0f);
    CHECK(mod_calibration_interp(table, INFINITY) == (float)(PHASE_CAL_BINS - 1U));

    uint16_t flags = SSB_CAL_FLAG_VALIDATED | SSB_CAL_FLAG_APPLY;
    CHECK(mod_calibration_is_active(flags, 0U) == 1U);
    CHECK(mod_calibration_is_active(flags, 1U) == 0U);
    CHECK(mod_calibration_is_active(SSB_CAL_FLAG_APPLY, 0U) == 1U);
    CHECK(mod_calibration_is_active(SSB_CAL_FLAG_VALIDATED, 0U) == 0U);
    flags |= SSB_CAL_FLAG_LEGACY_REVALIDATE;
    CHECK(mod_calibration_mark_edited(flags) == SSB_CAL_FLAG_APPLY);
}

int main(void)
{
    test_legacy_decomposition();
    test_flags_and_interpolation();
    if (failures != 0U) return 1;
    puts("typed calibration domains and legacy conversion: PASS");
    return 0;
}

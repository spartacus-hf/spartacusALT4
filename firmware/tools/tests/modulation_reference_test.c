#include "mod_transport_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define PI_F 3.14159265358979323846f

static void check_close(float actual, float expected, float tolerance)
{
    CHECK(isfinite(actual));
    CHECK(fabsf(actual - expected) <= tolerance);
}

static void test_phase_wrap(void)
{
    check_close(mod_phase_wrap_pi(0.0f), 0.0f, 0.0f);
    check_close(mod_phase_wrap_pi(0.999f), 0.999f, 0.0f);
    check_close(mod_phase_wrap_pi(-1.0f), -1.0f, 0.0f);
    check_close(mod_phase_wrap_pi(1.0f), -1.0f, 1.0e-7f);
    check_close(mod_phase_wrap_pi(2.25f), 0.25f, 1.0e-7f);
    check_close(mod_phase_wrap_pi(-2.25f), -0.25f, 1.0e-7f);
    check_close(mod_phase_wrap_pi(INFINITY), 0.0f, 0.0f);
    check_close(mod_phase_wrap_pi(NAN), 0.0f, 0.0f);
}

/* The wrap as it was, all through fmodf. */
static float phase_wrap_fmodf(float phase_pi)
{
    if (!isfinite(phase_pi)) return 0.0f;
    if ((phase_pi >= 1.0f) || (phase_pi < -1.0f)) {
        phase_pi = fmodf(phase_pi + 1.0f, 2.0f);
        if (phase_pi < 0.0f) phase_pi += 2.0f;
        phase_pi -= 1.0f;
    }
    return phase_pi;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* The fmodf-free ranges must give fmodf's result to the bit: every float
 * in [-6, 6] one ulp at a time near each range end, a fine grid across,
 * and scattered bit patterns. */
static void test_phase_wrap_matches_fmodf(void)
{
    static const float ends[] = {-5.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    unsigned mismatches = 0U;
    for (unsigned e = 0U; e < sizeof(ends) / sizeof(ends[0]); ++e) {
        float x = ends[e];
        for (int k = 0; k < 4096; ++k) x = nextafterf(x, -INFINITY);
        for (int k = 0; k < 8192; ++k, x = nextafterf(x, INFINITY)) {
            if (float_bits(mod_phase_wrap_pi(x)) != float_bits(phase_wrap_fmodf(x))) mismatches++;
        }
    }
    for (int k = -6000000; k <= 6000000; ++k) {
        const float x = (float)k * 1.0e-6f;
        if (float_bits(mod_phase_wrap_pi(x)) != float_bits(phase_wrap_fmodf(x))) mismatches++;
    }
    uint32_t state = 0x12345678U;
    for (int k = 0; k < 2000000; ++k) {
        state = state * 1664525U + 1013904223U;
        float x;
        memcpy(&x, &state, sizeof(x));
        const float got = mod_phase_wrap_pi(x), want = phase_wrap_fmodf(x);
        if (float_bits(got) != float_bits(want)) mismatches++;
    }
    CHECK(mismatches == 0U);
}

static void reconstruct(float common_pi,
                        mod_outphase_pair_t pair,
                        float *real_out,
                        float *imag_out)
{
    float phase_a = (common_pi + pair.branch_a_pi) * PI_F;
    float phase_c = (common_pi + pair.branch_c_pi) * PI_F;
    *real_out = 0.5f * (cosf(phase_a) + cosf(phase_c));
    *imag_out = 0.5f * (sinf(phase_a) + sinf(phase_c));
}

static void test_ideal_outphase_reconstruction(void)
{
    static const float magnitudes[] = {0.0f, 0.001f, 0.1f, 0.5f, 0.9f, 0.999f, 1.0f};
    static const float phases_pi[] = {-0.99f, -0.5f, -0.1f, 0.0f, 0.37f, 0.99f};

    for (uint32_t r_idx = 0U; r_idx < sizeof(magnitudes) / sizeof(magnitudes[0]); r_idx++) {
        float magnitude = magnitudes[r_idx];
        float theta_pi = acosf(magnitude) / PI_F;
        for (uint32_t p_idx = 0U; p_idx < sizeof(phases_pi) / sizeof(phases_pi[0]); p_idx++) {
            float common_pi = phases_pi[p_idx];
            for (uint8_t swap = 0U; swap <= 1U; swap++) {
                mod_outphase_pair_t pair = mod_outphase_branch_pair_pi(theta_pi, 0.0f, swap);
                float real_value;
                float imag_value;
                reconstruct(common_pi, pair, &real_value, &imag_value);
                check_close(real_value, magnitude * cosf(common_pi * PI_F), 3.0e-6f);
                check_close(imag_value, magnitude * sinf(common_pi * PI_F), 3.0e-6f);
            }
        }
    }
}

static void test_differential_calibration(void)
{
    const float theta_pi = 0.2f;
    const float differential_pi = 0.03f;
    mod_outphase_pair_t pair = mod_outphase_branch_pair_pi(theta_pi, differential_pi, 0U);
    check_close(pair.branch_a_pi, 0.23f, 1.0e-7f);
    check_close(pair.branch_c_pi, -0.23f, 1.0e-7f);

    float real_value;
    float imag_value;
    reconstruct(0.0f, pair, &real_value, &imag_value);
    check_close(real_value, cosf(0.23f * PI_F), 2.0e-6f);
    check_close(imag_value, 0.0f, 2.0e-6f);
}

static void test_hrtim_phase_contract(void)
{
    mod_hrtim_phase_geometry_t geometry;
    CHECK(mod_hrtim_phase_geometry(191U, 0, 0U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(192U, 0, 0U, &geometry) == 1U);
    CHECK(geometry.carrier_period_ticks == 192U);
    CHECK(geometry.master_period_ticks == 384U);
    CHECK(geometry.local_compare_ticks == 96U);
    CHECK(geometry.master_compare_ticks == 96U);
    CHECK(mod_hrtim_phase_geometry(192U, 0, 191U, &geometry) == 1U);
    CHECK(geometry.master_compare_ticks == 287U);
    CHECK(mod_hrtim_phase_geometry(192U, -1, 0U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(192U, 1, 0U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(384U, -96, 0U, &geometry) == 1U);
    CHECK(geometry.local_compare_ticks == 96U);
    CHECK(mod_hrtim_phase_geometry(384U, 96, 0U, &geometry) == 1U);
    CHECK(geometry.local_compare_ticks == 288U);
    CHECK(mod_hrtim_phase_geometry(384U, -97, 0U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(384U, 97, 0U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(192U, 0, 192U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(32751U, 0, 32750U, &geometry) == 1U);
    CHECK(geometry.master_period_ticks == 65502U);
    CHECK(mod_hrtim_phase_geometry(32752U, 0, 0U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(40000U, 0, 0U, &geometry) == 0U);
    CHECK(mod_hrtim_phase_geometry(192U, 0, 0U, NULL) == 0U);

    CHECK(mod_hrtim_phase_raw_tick(388U, -1.0f) == 0U);
    CHECK(mod_hrtim_phase_raw_tick(388U, 0.0f) == 194U);
    CHECK(mod_hrtim_phase_raw_tick(388U, 1.0f) == 0U);
    CHECK(mod_hrtim_phase_raw_tick(0U, 0.0f) == 0U);

    for (uint32_t period = MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32;
         period <= 5440U;
         period++) {
        uint16_t previous_compare = 0U;
        for (uint32_t raw = 0U; raw < period; raw++) {
            CHECK(mod_hrtim_phase_geometry((uint16_t)period,
                                           0,
                                           (uint16_t)raw,
                                           &geometry) == 1U);
            CHECK(geometry.local_compare_ticks >= MOD_HRTIM_COMPARE_FLOOR_MUL32);
            CHECK(geometry.local_compare_ticks <=
                  period - MOD_HRTIM_COMPARE_FLOOR_MUL32);
            CHECK(geometry.master_compare_ticks >= MOD_HRTIM_COMPARE_FLOOR_MUL32);
            CHECK(geometry.master_compare_ticks < geometry.master_period_ticks);
            /* The render-side check accepts every compare the encoder makes. */
            CHECK(mod_hrtim_phase_compare_legal(period, geometry.master_compare_ticks));
            if (raw != 0U) {
                CHECK(geometry.master_compare_ticks == previous_compare + 1U);
            }
            previous_compare = geometry.master_compare_ticks;
        }
        CHECK(!mod_hrtim_phase_compare_legal(period, MOD_HRTIM_COMPARE_FLOOR_MUL32 - 1U));
        CHECK(!mod_hrtim_phase_compare_legal(period, MOD_HRTIM_COMPARE_FLOOR_MUL32 + period));
    }
}

/* The PA profile clamps DUTY to these bounds: every offset inside them is a
 * legal geometry and the next one out on either side is not, at every period
 * the carrier can have. */
static void test_hrtim_duty_offset_bounds(void)
{
    mod_hrtim_phase_geometry_t geometry;
    int16_t lo = 7, hi = 7;
    CHECK(mod_hrtim_duty_offset_bounds(191U, &lo, &hi) == 0U);
    CHECK((lo == 0) && (hi == 0));
    CHECK(mod_hrtim_duty_offset_bounds(32752U, &lo, &hi) == 0U);
    CHECK(mod_hrtim_duty_offset_bounds(192U, &lo, &hi) == 1U);
    CHECK((lo == 0) && (hi == 0));
    CHECK(mod_hrtim_duty_offset_bounds(193U, &lo, &hi) == 1U);   /* 28.2 MHz at 170 MHz */
    CHECK((lo == 0) && (hi == 1));
    CHECK(mod_hrtim_duty_offset_bounds(194U, &lo, &hi) == 1U);
    CHECK((lo == -1) && (hi == 1));
    CHECK(mod_hrtim_duty_offset_bounds(388U, &lo, &hi) == 1U);
    CHECK((lo == -98) && (hi == 98));
    CHECK(mod_hrtim_duty_offset_bounds(192U, NULL, NULL) == 1U);
    for (uint32_t period = MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32;
         period <= MOD_HRTIM_MAX_CARRIER_PERIOD_MUL32;
         period += (period < 5440U) ? 1U : 97U) {
        CHECK(mod_hrtim_duty_offset_bounds((uint16_t)period, &lo, &hi) == 1U);
        CHECK((lo <= 0) && (hi >= 0));
        CHECK(mod_hrtim_phase_geometry((uint16_t)period, lo, 0U, &geometry) == 1U);
        CHECK(geometry.local_compare_ticks == MOD_HRTIM_COMPARE_FLOOR_MUL32);
        CHECK(mod_hrtim_phase_geometry((uint16_t)period, hi, 0U, &geometry) == 1U);
        CHECK(geometry.local_compare_ticks == period - MOD_HRTIM_COMPARE_FLOOR_MUL32);
        CHECK(mod_hrtim_phase_geometry((uint16_t)period, (int16_t)(lo - 1), 0U, &geometry) == 0U);
        CHECK(mod_hrtim_phase_geometry((uint16_t)period, (int16_t)(hi + 1), 0U, &geometry) == 0U);
    }
}

/* Event model of one branch timer in the atomic geometry: the master runs at
 * two carrier periods and resets the branch timer at its compare, 96 + raw
 * phase, taken from a phase NCO stepped by `step` ticks per 192 kHz update.
 * Between resets the timer rolls over on its own; a reset that lands before
 * the counter reaches PER cancels that roll-over (a coincident one still
 * rolls over). Output events apply in time order, reset beating set on one
 * tick (RM0440). Returns how many output-1 high or low times fall outside
 * half a period +- the largest reset move, i.e. the lost or doubled edges. */
static uint32_t branch_bad_half_periods(uint32_t period, double step, uint32_t phase_reset)
{
    enum { MASTERS = 4000, MAX_EVENTS = 64 };
    const mod_hrtim_branch_edges_t e = mod_hrtim_branch_edges(0U, phase_reset);
    const int64_t p = (int64_t)period, c = p / 2, m = 2 * p;
    const double ticks_per_update = 5.44e9 / 192000.0;
    const int64_t tolerance = (int64_t)ceil(fabs(step)) + 1;
    int64_t reset_prev = -1, edge_prev = -1;
    uint8_t out1 = 0U, started = 0U;
    uint32_t bad = 0U;

    for (int64_t k = 0; k <= MASTERS; ++k) {
        const double phase = 0.3 * (double)p + floor((double)(k * m) / ticks_per_update) * step;
        const int64_t raw = ((int64_t)floor(phase) % p + p) % p;
        const int64_t reset = k * m + MOD_HRTIM_COMPARE_FLOOR_MUL32 + raw;
        if (reset_prev >= 0) {
            int64_t when[MAX_EVENTS];
            uint32_t what[MAX_EVENTS];
            uint32_t n = 0U;
            for (int64_t t = reset_prev + c; t < reset; t += p) { when[n] = t; what[n++] = MOD_HRTIM_EDGE_CMP1; }
            for (int64_t t = reset_prev + p; t <= reset; t += p) { when[n] = t; what[n++] = MOD_HRTIM_EDGE_PER; }
            when[n] = reset; what[n++] = MOD_HRTIM_EDGE_MSTCMP1 | MOD_HRTIM_EDGE_MSTCMP2;
            for (uint32_t i = 1U; i < n; ++i) {            /* insertion sort by time */
                for (uint32_t j = i; (j > 0U) && (when[j - 1U] > when[j]); --j) {
                    int64_t tw = when[j]; when[j] = when[j - 1U]; when[j - 1U] = tw;
                    uint32_t tv = what[j]; what[j] = what[j - 1U]; what[j - 1U] = tv;
                }
            }
            for (uint32_t i = 0U; i < n;) {
                uint32_t sources = 0U;
                const int64_t t = when[i];
                while ((i < n) && (when[i] == t)) sources |= what[i++];
                uint8_t next = out1;
                if ((sources & e.set1) != 0U) next = 1U;
                if ((sources & e.reset1) != 0U) next = 0U;
                if (next != out1) {
                    if (started && (edge_prev >= 0) && (k > 4)) {
                        const int64_t width = t - edge_prev;
                        if ((width < c - tolerance) || (width > (p - c) + tolerance)) bad++;
                    }
                    edge_prev = t;
                    started = 1U;
                    out1 = next;
                }
            }
        }
        reset_prev = reset;
    }
    return bad;
}

static void test_hrtim_branch_edges(void)
{
    static const uint32_t periods[] = {193U, 254U, 382U, 557U, 1004U, 2957U};
    static const double steps[] = {-40.3, -13.78, -4.58, -0.42, 0.42, 4.58, 13.78, 40.3};

    mod_hrtim_branch_edges_t e = mod_hrtim_branch_edges(0U, MOD_HRTIM_EDGE_MSTCMP1);
    CHECK(e.set1 == (MOD_HRTIM_EDGE_PER | MOD_HRTIM_EDGE_MSTCMP1));
    CHECK(e.reset1 == MOD_HRTIM_EDGE_CMP1);
    CHECK(e.set2 == MOD_HRTIM_EDGE_CMP1);
    CHECK(e.reset2 == (MOD_HRTIM_EDGE_PER | MOD_HRTIM_EDGE_MSTCMP1));
    e = mod_hrtim_branch_edges(1U, MOD_HRTIM_EDGE_MSTCMP2);
    CHECK(e.set1 == MOD_HRTIM_EDGE_CMP1);
    CHECK(e.reset1 == (MOD_HRTIM_EDGE_PER | MOD_HRTIM_EDGE_MSTCMP2));
    CHECK(e.set2 == (MOD_HRTIM_EDGE_PER | MOD_HRTIM_EDGE_MSTCMP2));
    CHECK(e.reset2 == MOD_HRTIM_EDGE_CMP1);
    e = mod_hrtim_branch_edges(0U, 0U);
    CHECK((e.set1 == MOD_HRTIM_EDGE_PER) && (e.reset2 == MOD_HRTIM_EDGE_PER));

    for (uint32_t pi = 0U; pi < sizeof(periods) / sizeof(periods[0]); ++pi) {
        for (uint32_t si = 0U; si < sizeof(steps) / sizeof(steps[0]); ++si) {
            const uint32_t legacy = branch_bad_half_periods(periods[pi], steps[si], 0U);
            const uint32_t fixed = branch_bad_half_periods(periods[pi], steps[si], MOD_HRTIM_EDGE_MSTCMP1);
            /* PER alone loses the edge exactly when the reset moves earlier. */
            CHECK((steps[si] < 0.0) ? (legacy != 0U) : (legacy == 0U));
            CHECK(fixed == 0U);
            if ((fixed != 0U) || ((steps[si] < 0.0) != (legacy != 0U))) {
                fprintf(stderr, "  period %u step %.2f: legacy %u fixed %u\n",
                        (unsigned)periods[pi], steps[si], (unsigned)legacy, (unsigned)fixed);
            }
        }
    }
}

int main(void)
{
    test_phase_wrap();
    test_phase_wrap_matches_fmodf();
    test_ideal_outphase_reconstruction();
    test_differential_calibration();
    test_hrtim_phase_contract();
    test_hrtim_duty_offset_bounds();
    test_hrtim_branch_edges();
    if (failures != 0U) {
        fprintf(stderr, "modulation reference failures: %u\n", failures);
        return 1;
    }
    puts("modulation production reference: PASS");
    return 0;
}

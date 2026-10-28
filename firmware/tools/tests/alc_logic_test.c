#include "mod_alc_logic.h"

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

static float process(mod_alc_state_t *state, float input)
{
    return mod_alc_process_sample(state, input, 0.92f, 0.20f, 0.0025f);
}

static void test_zero_startup(void)
{
    mod_alc_state_t state;
    mod_alc_reset(&state);
    for (uint32_t i = 0U; i < MOD_ALC_LOOKAHEAD_SAMPLES; i++) {
        CHECK(process(&state, 0.5f) == 0.0f);
    }
    CHECK(fabsf(process(&state, 0.5f) - 0.5f) < 1.0e-6f);
}

static void test_impulse_ceiling(float impulse)
{
    mod_alc_state_t state;
    mod_alc_reset(&state);
    float maximum = 0.0f;
    for (uint32_t i = 0U; i < 3U * MOD_ALC_LOOKAHEAD_SAMPLES; i++) {
        float input = (i == 0U) ? impulse : 0.0f;
        float output = process(&state, input);
        float magnitude = fabsf(output);
        if (magnitude > maximum) maximum = magnitude;
        CHECK(isfinite(output));
        CHECK(magnitude <= 0.92f);
    }
    CHECK(maximum > 0.9199f);
}

static void test_nonfinite_input(void)
{
    mod_alc_state_t state;
    mod_alc_reset(&state);
    CHECK(process(&state, NAN) == 0.0f);
    CHECK(process(&state, INFINITY) == 0.0f);
}

static void test_nonfinite_coefficients_and_corrupt_state(void)
{
    mod_alc_state_t state;
    mod_alc_reset(&state);

    for (uint32_t i = 0U; i <= MOD_ALC_LOOKAHEAD_SAMPLES; i++) {
        float output = mod_alc_process_sample(&state, 2.0f, 0.92f, NAN, INFINITY);
        CHECK(isfinite(output));
        CHECK(fabsf(output) <= 0.92f);
    }

    state.index = MOD_ALC_LOOKAHEAD_SAMPLES;
    state.gain = NAN;
    state.max_peak = INFINITY;
    CHECK(mod_alc_process_sample(&state, 0.5f, 0.92f, 0.2f, 0.0025f) == 0.0f);
    CHECK(state.index < MOD_ALC_LOOKAHEAD_SAMPLES);
    CHECK(isfinite(state.gain));
    CHECK(isfinite(state.max_peak));
}

static void test_dynamic_sequence(void)
{
    mod_alc_state_t state;
    uint32_t random = 0x13579BDFU;
    mod_alc_reset(&state);
    for (uint32_t i = 0U; i < 12000U; i++) {
        random = random * 1664525U + 1013904223U;
        int32_t centered = (int32_t)(random >> 8) - (int32_t)0x007FFFFF;
        float input = (float)centered * (20.0f / 8388607.0f);
        if ((i % 257U) == 0U) input = (i & 1U) ? -50.0f : 50.0f;
        float output = process(&state, input);
        CHECK(isfinite(output));
        CHECK(fabsf(output) <= 0.92f);
    }
}

/* The frame form is the per-sample call run n times, bit for bit: outputs and
 * the whole state, over chunk sizes 1..48, speech-like decaying tones (which
 * make the outgoing sample the window maximum, the rescan path), sparse
 * non-finite inputs, a corrupt starting state and odd coefficients. */
static void test_block_matches_sample(void)
{
    static const float ceilings[] = {0.92f, 0.5f, 1.0f, -1.0f};
    static const float attacks[] = {0.20f, 0.20f, NAN, 1.5f};
    static const float releases[] = {0.0025f, 0.0025f, INFINITY, -0.5f};
    uint32_t random = 0x2468ACE1U;
    for (unsigned cfg = 0U; cfg < 4U; cfg++) {
        for (unsigned start = 0U; start < 2U; start++) {
            mod_alc_state_t a, b;
            mod_alc_reset(&a);
            if (start) {
                a.index = MOD_ALC_LOOKAHEAD_SAMPLES + 3U;
                a.gain = NAN;
            }
            b = a;
            float in[48], out_a[48];
            uint32_t chunk = 1U, t = 0U;
            for (uint32_t frame = 0U; frame < 900U; frame++) {
                chunk = (chunk % 48U) + 1U;
                for (uint32_t i = 0U; i < chunk; i++, t++) {
                    random = random * 1664525U + 1013904223U;
                    float amp = 3.0f * expf(-(float)(t % 4000U) / 900.0f);
                    float v = amp * sinf((float)t * 0.1309f) +
                              ((float)((int32_t)(random >> 9) - 0x3FFFFF) * (0.05f / 4194303.0f));
                    if ((random & 0x3FFU) == 7U) v = NAN;
                    if ((random & 0x7FFU) == 9U) v = -INFINITY;
                    if ((t % 1500U) == 0U) v = 40.0f;
                    in[i] = v;
                    out_a[i] = mod_alc_process_sample(&a, v, ceilings[cfg], attacks[cfg], releases[cfg]);
                }
                mod_alc_process_block(&b, in, chunk, ceilings[cfg], attacks[cfg], releases[cfg]);
                CHECK(memcmp(in, out_a, chunk * sizeof(float)) == 0);
                CHECK(memcmp(&a, &b, sizeof(a)) == 0);
            }
        }
    }
}

int main(void)
{
    test_zero_startup();
    test_impulse_ceiling(1.0f);
    test_impulse_ceiling(2.0f);
    test_impulse_ceiling(10.0f);
    test_impulse_ceiling(-10.0f);
    test_nonfinite_input();
    test_nonfinite_coefficients_and_corrupt_state();
    test_dynamic_sequence();
    test_block_matches_sample();
    if (failures != 0U) return 1;
    puts("ALC production core: zero startup, exact 0.92 ceiling, frame form bit-exact: PASS");
    return 0;
}

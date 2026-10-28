#include "mod_ssb_gate_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

static void test_hysteresis_and_hold(void)
{
    mod_ssb_gate_state_t state;
    memset(&state, 0, sizeof(state));
    CHECK(mod_ssb_gate_process(&state, 0.017f, 0.010f) == 0U);
    CHECK(mod_ssb_gate_process(&state, 0.018f, 0.010f) == 1U);
    CHECK(state.open == 1U);
    CHECK(state.hang == MOD_SSB_GATE_HOLD_SAMPLES);

    CHECK(mod_ssb_gate_process(&state, 0.0f, 0.010f) == 1U);
    CHECK(state.hang == MOD_SSB_GATE_HOLD_SAMPLES - 1U);
    CHECK(mod_ssb_gate_process(&state, 0.007f, 0.010f) == 1U);
    CHECK(state.hang == MOD_SSB_GATE_HOLD_SAMPLES);
    for (uint32_t i = 0U; i < MOD_SSB_GATE_HOLD_SAMPLES; i++) {
        CHECK(mod_ssb_gate_process(&state, 0.0f, 0.010f) == 1U);
    }
    CHECK(state.hang == 0U);
    CHECK(mod_ssb_gate_process(&state, 0.0f, 0.010f) == 1U);
    CHECK(state.open == 0U);
    CHECK(mod_ssb_gate_process(&state, 0.0f, 0.010f) == 0U);
}

static void test_fade_and_phase_memory(void)
{
    mod_ssb_gate_fade_state_t fade = {0.0f};
    mod_ssb_gate_state_t gate;
    memset(&gate, 0, sizeof(gate));

    CHECK(mod_ssb_gate_fade_update(&fade, 1U, 0.5f) == 0.5f);
    float previous = fade.gate_fade;
    uint32_t count = 0U;
    while ((fade.gate_fade >= MOD_SSB_GATE_NULL_ENV) && (count < 4000U)) {
        float current = mod_ssb_gate_fade_update(&fade, 0U, 0.0f);
        CHECK(isfinite(current));
        CHECK(current >= 0.0f);
        CHECK(current <= previous);
        previous = current;
        count++;
    }
    CHECK(count < 4000U);
    CHECK(fade.gate_fade < MOD_SSB_GATE_NULL_ENV);
    CHECK(mod_ssb_gate_fade_update(&fade, 1U, 2.0f) == 0.999f);
    CHECK(mod_ssb_gate_fade_update(0, 1U, 0.5f) == 0.0f);

    mod_ssb_gate_update_phi(&gate, 0.75f, 1U);
    CHECK(gate.last_phi == 0.75f);
    mod_ssb_gate_update_phi(&gate, -0.5f, 0U);
    CHECK(fabsf(gate.last_phi - 0.74625f) < 1.0e-6f);
    CHECK(mod_ssb_phase_stabilize_low_env(0.01f, 0.99f, -0.99f) == 0.99f);
    CHECK(mod_ssb_phase_stabilize_low_env(0.08f, 0.99f, -0.99f) == -0.99f);
    float midpoint = mod_ssb_phase_stabilize_low_env(0.05f, 0.99f, -0.99f);
    CHECK(fabsf(fabsf(midpoint) - 1.0f) < 1.0e-6f);
}

static void test_nonfinite_inputs_fail_closed(void)
{
    mod_ssb_gate_fade_state_t fade = {0.5f};
    mod_ssb_gate_state_t gate;
    memset(&gate, 0, sizeof(gate));

    CHECK(mod_ssb_gate_process(&gate, NAN, 0.010f) == 0U);
    CHECK(mod_ssb_gate_process(&gate, INFINITY, 0.010f) == 0U);
    CHECK(mod_ssb_gate_process(&gate, 0.5f, NAN) == 0U);
    CHECK(mod_ssb_gate_fade_update(&fade, 1U, NAN) == 0.0f);
    CHECK(mod_ssb_gate_fade_update(&fade, 1U, INFINITY) == 0.0f);
    mod_ssb_gate_update_phi(&gate, NAN, 1U);
    CHECK(gate.last_phi == 0.0f);
    CHECK(mod_ssb_phase_wrap_norm(NAN) == 0.0f);
    CHECK(mod_ssb_phase_wrap_norm(INFINITY) == 0.0f);
    CHECK(mod_ssb_phase_wrap_norm(-INFINITY) == 0.0f);
    CHECK(mod_ssb_phase_stabilize_low_env(NAN, 0.25f, -0.25f) == 0.25f);
    CHECK(mod_ssb_phase_stabilize_low_env(INFINITY, 0.25f, -0.25f) == 0.25f);
    CHECK(mod_ssb_phase_blend_norm(INFINITY, -0.25f, 0.5f) == -0.125f);
    CHECK(mod_ssb_phase_blend_norm(0.25f, -INFINITY, 0.5f) == 0.125f);
    CHECK(mod_ssb_phase_blend_norm(0.25f, -0.25f, NAN) == 0.25f);
}

int main(void)
{
    test_hysteresis_and_hold();
    test_fade_and_phase_memory();
    test_nonfinite_inputs_fail_closed();
    if (failures != 0U) return 1;
    puts("SSB production gate: hysteresis, hold, fade, and phase wrap: PASS");
    return 0;
}

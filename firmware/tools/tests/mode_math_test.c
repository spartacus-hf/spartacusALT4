#include "mod_mode_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define PI_F 3.14159265358979323846f

static void require_close(float actual, float expected, float tolerance,
                          const char *what)
{
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "%s: got %.9g, expected %.9g\n",
                what, (double)actual, (double)expected);
        exit(1);
    }
}

static void test_fm_phase_integrator_contract(void)
{
    require_close(mod_fm_phase_step_pi(1.0f, 2500.0f, 48000.0f),
                  5000.0f / 48000.0f, 1.0e-7f, "positive FM phase step");
    require_close(mod_fm_phase_step_pi(-0.5f, 5000.0f, 48000.0f),
                  -5000.0f / 48000.0f, 1.0e-7f, "negative FM phase step");
    require_close(mod_fm_phase_step_pi(NAN, 2500.0f, 48000.0f),
                  0.0f, 0.0f, "invalid FM audio");
}

static void test_fm_bounded_preemphasis_contract(void)
{
    const float sample_rate = 48000.0f;
    const float tau = 50.0e-6f;
    const float shelf = 1.5f;
    const float coefficient = 1.0f / (1.0f + sample_rate * tau);
    float state = 0.0f;

    float impulse = mod_fm_preemphasis_bounded(1.0f, &state, sample_rate,
                                               tau, shelf);
    require_close(state, coefficient, 1.0e-7f, "FM low-pass state");
    require_close(impulse, 1.0f + shelf * (1.0f - coefficient),
                  1.0e-7f, "FM high-shelf impulse");

    for (unsigned i = 0U; i < 200U; i++) {
        (void)mod_fm_preemphasis_bounded(1.0f, &state, sample_rate, tau, shelf);
    }
    require_close(state, 1.0f, 1.0e-6f, "FM DC state");
    require_close(mod_fm_preemphasis_bounded(1.0f, &state, sample_rate,
                                             tau, shelf),
                  1.0f, 1.0e-6f, "FM DC gain");
}

static void test_am_profile_contracts(void)
{
    require_close(mod_am_envelope_unclamped(1.0f, 0.75f, 0.55f,
                                             AM_PROFILE_EFFICIENT),
                  1.025f, 1.0e-7f, "efficient AM positive peak");
    require_close(mod_am_envelope_unclamped(-1.0f, 0.75f, 0.55f,
                                             AM_PROFILE_EFFICIENT),
                  0.475f, 1.0e-7f, "efficient AM negative peak");
    require_close(mod_am_envelope_unclamped(1.0f, 0.75f, 0.55f,
                                             AM_PROFILE_LINEAR_TEST),
                  1.1625f, 2.0e-7f, "linear AM positive peak");
    require_close(mod_am_envelope_unclamped(-1.0f, 0.75f, 0.55f,
                                             AM_PROFILE_LINEAR_TEST),
                  0.3375f, 1.0e-7f, "linear AM negative peak");
    require_close(mod_am_envelope_unclamped(NAN, 0.75f, 0.55f,
                                             AM_PROFILE_LINEAR_TEST),
                  0.75f, 1.0e-7f, "invalid AM audio");
}

static void test_cw_ramp_contract(void)
{
    require_close(mod_cw_blackman_harris_ramp(0.0f), 0.0f, 0.0f,
                  "CW ramp start");
    require_close(mod_cw_blackman_harris_ramp(1.0f), 1.0f, 0.0f,
                  "CW ramp end");
    float previous = 0.0f;
    for (unsigned i = 1U; i <= 512U; i++) {
        float position = (float)i / 512.0f;
        float current = mod_cw_blackman_harris_ramp(position);
        if (current + 1.0e-7f < previous) {
            fprintf(stderr, "CW ramp is not monotonic at %u: %.9g < %.9g\n",
                    i, (double)current, (double)previous);
            exit(1);
        }
        previous = current;
    }
    require_close(mod_cw_blackman_harris_ramp(0.5f),
                  0.35875f - 0.48829f * cosf(0.5f * PI_F)
                  + 0.14128f * cosf(PI_F)
                  - 0.01168f * cosf(1.5f * PI_F),
                  1.0e-7f, "CW ramp midpoint");
}

int main(void)
{
    test_fm_phase_integrator_contract();
    test_fm_bounded_preemphasis_contract();
    test_am_profile_contracts();
    test_cw_ramp_contract();
    puts("FM/AM/CW production math: phase, envelope, and ramp contracts");
    return 0;
}

#include "mod_dual_envelope_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#if !MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE
#error "The dual-envelope host test must explicitly enable the experiment"
#endif

static void require_close(float actual, float expected, float tolerance,
                          const char *what)
{
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "%s: got %.9g, expected %.9g\n",
                what, (double)actual, (double)expected);
        exit(1);
    }
}

static void verify_ideal_recombination(void)
{
    for (int phase_step = -20; phase_step <= 20; phase_step++) {
        float phase_pi = (float)phase_step * 0.125f;
        float wrapped = MOD_DualEnvelopeWrapPhasePi(phase_pi);
        float expected_i_unit = cosf(wrapped * 3.14159265358979323846f);
        float expected_q_unit = sinf(wrapped * 3.14159265358979323846f);

        for (int level_step = 0; level_step <= 20; level_step++) {
            float level = (float)level_step * 0.05f;
            mod_dual_envelope_command_t outphase =
                MOD_DualEnvelopeIdealOutphasing(phase_pi, level);
            mod_dual_envelope_command_t polar =
                MOD_DualEnvelopeIdealPolar(phase_pi, level);
            mod_complex_envelope_t outphase_sum =
                MOD_DualEnvelopeIdealRecombine(&outphase);
            mod_complex_envelope_t polar_sum =
                MOD_DualEnvelopeIdealRecombine(&polar);

            require_close(outphase_sum.i, level * expected_i_unit, 2.0e-6f,
                          "outphasing I");
            require_close(outphase_sum.q, level * expected_q_unit, 2.0e-6f,
                          "outphasing Q");
            require_close(polar_sum.i, level * expected_i_unit, 2.0e-6f,
                          "polar I");
            require_close(polar_sum.q, level * expected_q_unit, 2.0e-6f,
                          "polar Q");
        }
    }
}

static void verify_independent_and_invalid_inputs(void)
{
    mod_dual_envelope_command_t command =
        MOD_DualEnvelopeMake(4.25f, 0.25f, -3.25f, 0.75f);
    require_close(command.branch_a.phase_pi, 0.25f, 1.0e-7f, "A phase wrap");
    require_close(command.branch_c.phase_pi, 0.75f, 1.0e-7f, "C phase wrap");
    require_close(command.branch_a.envelope, 0.25f, 1.0e-7f, "A envelope");
    require_close(command.branch_c.envelope, 0.75f, 1.0e-7f, "C envelope");

    command = MOD_DualEnvelopeMake(NAN, NAN, INFINITY, 2.0f);
    require_close(command.branch_a.phase_pi, 0.0f, 0.0f, "NaN phase");
    require_close(command.branch_c.phase_pi, 0.0f, 0.0f, "infinite phase");
    require_close(command.branch_a.envelope, 0.0f, 0.0f, "NaN envelope");
    require_close(command.branch_c.envelope, 1.0f, 0.0f, "high envelope");

    command.command_valid = 0U;
    mod_complex_envelope_t output = MOD_DualEnvelopeIdealRecombine(&command);
    require_close(output.i, 0.0f, 0.0f, "invalid command I");
    require_close(output.q, 0.0f, 0.0f, "invalid command Q");
}

int main(void)
{
    verify_ideal_recombination();
    verify_independent_and_invalid_inputs();
    puts("future dual-envelope pure model: ideal recombination and input safety");
    return 0;
}

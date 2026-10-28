#ifndef MOD_DUAL_ENVELOPE_LOGIC_H
#define MOD_DUAL_ENVELOPE_LOGIC_H

/*
 * Pure mathematical contract for a possible future PA with independent
 * envelope control on both RF branches. REV1's production envelope path is
 * implemented by radio_analog.c; this optional model defaults to 0.
 *
 * Phase is expressed in units of pi and normalized to [-1, 1).  The ideal
 * combiner model is the arithmetic mean of the two complex branch commands,
 * so a unit-envelope, equal-phase pair produces a unit normalized output.
 */
#ifndef MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE
#define MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE 0
#endif

#if MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE

#include <math.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float phase_pi;
    float envelope;
} mod_branch_envelope_command_t;

typedef struct {
    mod_branch_envelope_command_t branch_a;
    mod_branch_envelope_command_t branch_c;
    uint8_t command_valid;
} mod_dual_envelope_command_t;

typedef struct {
    float i;
    float q;
} mod_complex_envelope_t;

static inline float MOD_DualEnvelopeClamp01(float value)
{
    if (!isfinite(value) || value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value;
}

static inline float MOD_DualEnvelopeWrapPhasePi(float phase_pi)
{
    if (!isfinite(phase_pi)) return 0.0f;
    phase_pi = fmodf(phase_pi + 1.0f, 2.0f);
    if (phase_pi < 0.0f) phase_pi += 2.0f;
    return phase_pi - 1.0f;
}

static inline mod_dual_envelope_command_t
MOD_DualEnvelopeMake(float phase_a_pi, float envelope_a,
                     float phase_c_pi, float envelope_c)
{
    mod_dual_envelope_command_t command;
    command.branch_a.phase_pi = MOD_DualEnvelopeWrapPhasePi(phase_a_pi);
    command.branch_a.envelope = MOD_DualEnvelopeClamp01(envelope_a);
    command.branch_c.phase_pi = MOD_DualEnvelopeWrapPhasePi(phase_c_pi);
    command.branch_c.envelope = MOD_DualEnvelopeClamp01(envelope_c);
    command.command_valid = 1U;
    return command;
}

/*
 * Present outphasing law expressed in the future command domain:
 *   0.5 * (exp(j(phi+theta)) + exp(j(phi-theta)))
 *       = envelope * exp(j*phi), theta = acos(envelope).
 */
static inline mod_dual_envelope_command_t
MOD_DualEnvelopeIdealOutphasing(float common_phase_pi, float envelope)
{
    float level = MOD_DualEnvelopeClamp01(envelope);
    float theta_pi = acosf(level) * 0.31830988618379067154f;
    return MOD_DualEnvelopeMake(common_phase_pi + theta_pi, 1.0f,
                                common_phase_pi - theta_pi, 1.0f);
}

/*
 * Ideal equal-phase polar drive for hardware that can command both branch
 * envelopes directly.  This deliberately makes no claim about PA efficiency,
 * combiner loading, supply modulation, or hardware-safe slew rates.
 */
static inline mod_dual_envelope_command_t
MOD_DualEnvelopeIdealPolar(float common_phase_pi, float envelope)
{
    float level = MOD_DualEnvelopeClamp01(envelope);
    return MOD_DualEnvelopeMake(common_phase_pi, level,
                                common_phase_pi, level);
}

static inline mod_complex_envelope_t
MOD_DualEnvelopeIdealRecombine(const mod_dual_envelope_command_t *command)
{
    mod_complex_envelope_t output = {0.0f, 0.0f};
    if ((command == NULL) || (command->command_valid == 0U)) return output;

    float phase_a = command->branch_a.phase_pi * 3.14159265358979323846f;
    float phase_c = command->branch_c.phase_pi * 3.14159265358979323846f;
    output.i = 0.5f * (command->branch_a.envelope * cosf(phase_a) +
                       command->branch_c.envelope * cosf(phase_c));
    output.q = 0.5f * (command->branch_a.envelope * sinf(phase_a) +
                       command->branch_c.envelope * sinf(phase_c));
    return output;
}

#endif /* MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE */

#endif /* MOD_DUAL_ENVELOPE_LOGIC_H */

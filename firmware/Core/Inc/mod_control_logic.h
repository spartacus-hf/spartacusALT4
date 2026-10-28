#ifndef MOD_CONTROL_LOGIC_H
#define MOD_CONTROL_LOGIC_H

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "numeric control guards require IEEE-754 binary32 floats");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "numeric control guards require IEEE-754 binary32 floats");

/*
 * Hot renderers call these guards for every sample. Masking the IEEE-754
 * exponent makes invalid values zero without adding a control-flow branch to
 * each inlined renderer. memcpy keeps the conversion alias-safe and is folded
 * to register moves by both supported compilers.
 */
static inline uint32_t mod_control_finite_mask(uint32_t bits)
{
    uint32_t exponent_difference =
        (bits & UINT32_C(0x7F800000)) ^ UINT32_C(0x7F800000);
    uint32_t finite = (exponent_difference |
                       (0U - exponent_difference)) >> 31;
    return 0U - finite;
}

static inline void mod_control_finite_pair_or_zero(float *first, float *second)
{
    uint32_t first_bits;
    uint32_t second_bits;
    memcpy(&first_bits, first, sizeof(first_bits));
    memcpy(&second_bits, second, sizeof(second_bits));
    uint32_t mask = mod_control_finite_mask(first_bits) &
                    mod_control_finite_mask(second_bits);
    first_bits &= mask;
    second_bits &= mask;
    memcpy(first, &first_bits, sizeof(*first));
    memcpy(second, &second_bits, sizeof(*second));
}

/*
 * Shared scalar setting contract. NaN means that no meaningful ordering or
 * direction exists, so it falls back to the declared safe default. Positive
 * and negative infinity retain their direction and clamp to the corresponding
 * documented limit.
 */
static inline float mod_control_sanitize(float value,
                                         float fallback,
                                         float lower,
                                         float upper)
{
    if (value != value) value = fallback;
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
}

static inline uint32_t mod_control_enum_sanitize(uint32_t value,
                                                 uint32_t count,
                                                 uint32_t fallback)
{
    return (value < count) ? value : fallback;
}

static inline uint8_t mod_control_round_u8(float value,
                                           uint8_t fallback,
                                           uint8_t upper)
{
    float bounded = mod_control_sanitize(value, (float)fallback, 0.0f,
                                         (float)upper);
    return (uint8_t)(bounded + 0.5f);
}

static inline uint32_t mod_control_ns_to_ps(float nanoseconds)
{
    if (!isfinite(nanoseconds) || (nanoseconds <= 0.0f)) return 0U;
    float picoseconds = nanoseconds * 1000.0f + 0.5f;
    if (picoseconds >= 4294967295.0f) return UINT32_MAX;
    return (uint32_t)picoseconds;
}

static inline float mod_control_activity_metric(float value,
                                                uint8_t *any_active)
{
    if (!isfinite(value)) {
        if (any_active != 0) *any_active = 0U;
        return 0.0f;
    }
    return mod_control_sanitize(value, 0.0f, 0.0f, 1.0f);
}

#endif /* MOD_CONTROL_LOGIC_H */

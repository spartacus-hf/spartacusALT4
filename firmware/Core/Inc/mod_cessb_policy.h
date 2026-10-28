#ifndef MOD_CESSB_POLICY_H
#define MOD_CESSB_POLICY_H

#include "modulator.h"

#include <stdint.h>

/* A stored legacy enable bit remains harmless in production. Profiles,
 * settings restore, public setters, and runtime sanitization share this gate. */
static inline uint8_t mod_cessb_enable_for_build(uint8_t requested)
{
#if MOD_CESSB_EXPERIMENT_ENABLE
    return requested ? 1U : 0U;
#else
    (void)requested;
    return 0U;
#endif
}

#endif /* MOD_CESSB_POLICY_H */

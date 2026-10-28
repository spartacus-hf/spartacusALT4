#include "mod_cessb_policy.h"

#include <stdint.h>
#include <stdio.h>

#if MOD_CESSB_EXPERIMENT_ENABLE
#error "The production host-test flavor must not enable the CESSB experiment"
#endif

int main(void)
{
    if (mod_cessb_enable_for_build(0U) != 0U) return 1;
    if (mod_cessb_enable_for_build(1U) != 0U) return 1;
    if (mod_cessb_enable_for_build(UINT8_MAX) != 0U) return 1;
    puts("CESSB policy: all production requests are quarantined: PASS");
    return 0;
}

#include "mod_analytic_logic.h"

#include <stdio.h>

#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
#error "disabled analytic compile-policy test was built with H12P enabled"
#endif

#if MOD_ANALYTIC12_COMPILED
#error "H12P implementation remains compiled when its feature flag is disabled"
#endif

#ifdef MOD_ANALYTIC12_LPF_TAPS
#error "H12P filter definitions remain visible when the feature is disabled"
#endif

int main(void)
{
    puts("analytic compile policy: disabled H12P implementation is absent");
    return 0;
}

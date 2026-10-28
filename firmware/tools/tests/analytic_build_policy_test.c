#include "modulator.h"

#include <stdio.h>
#include <stdlib.h>

static void require_equal(uint8_t actual, uint8_t expected, const char *what)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %u, expected %u\n",
                what, (unsigned)actual, (unsigned)expected);
        exit(1);
    }
}

int main(void)
{
    require_equal(MOD_SsbAnalyticBackendBuildAvailable(SSB_ANALYTIC_WEAVER),
                  1U, "Weaver availability");
    require_equal(MOD_SsbAnalyticBackendBuildAvailable(SSB_ANALYTIC_H12P),
                  MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE ? 1U : 0U,
                  "H12P availability");
    require_equal(MOD_SsbAnalyticBackendBuildAvailable(SSB_ANALYTIC_HILBERT),
                  MOD_DEEP_DIAG_ENABLE ? 1U : 0U, "H48 availability");
    require_equal(MOD_SsbAnalyticBackendBuildAvailable(SSB_ANALYTIC_HILBERT_PAIR_24),
                  MOD_DEEP_DIAG_ENABLE ? 1U : 0U, "H24 availability");
    require_equal(MOD_SsbAnalyticBackendBuildAvailable(SSB_ANALYTIC_HILBERT_PAIR_12),
                  MOD_DEEP_DIAG_ENABLE ? 1U : 0U, "H12 availability");
    require_equal(MOD_SsbAnalyticBackendBuildAvailable(SSB_ANALYTIC_COUNT),
                  0U, "sentinel availability");
    require_equal(MOD_SsbAnalyticBackendBuildAvailable((ssb_analytic_backend_t)255),
                  0U, "invalid availability");

    printf("analytic build policy: deep=%u H12P=%u\n",
           (unsigned)MOD_DEEP_DIAG_ENABLE,
           (unsigned)MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE);
    return 0;
}

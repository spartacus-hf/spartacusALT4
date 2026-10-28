/* A TCXO in place of REV1's 8 MHz DSC1003 (clock_plan.h): at a multiple of
 * 8 MHz the planner must offer exactly the settings it offers today -- the
 * same system clocks to the micro-hertz, with REF CAL applied -- so nothing
 * measured on the bench moves. Built at 16, 32 and 40 MHz by host_tests.py;
 * 48 MHz does not build (clock_plan.h). */
#include "clock_plan.h"

#include <stdio.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

/* CLOCK_PllCandidates(AUTO floor, max) and CLOCK_PllMicroHz(+12345 ppb) at 8 MHz. */
static const uint32_t want_hz[] = {150000000U, 150666667U, 152000000U, 153333333U, 154000000U, 154666667U, 156000000U, 157333333U, 158000000U, 158666667U, 160000000U, 161333333U, 162000000U, 162666667U, 164000000U, 165333333U, 166000000U, 166666667U, 168000000U, 169333333U, 170000000U};
static const uint64_t want_uhz[] = {
        150001851750000ULL, 150668526646667ULL, 152001876440000ULL, 153335226233333ULL,
        154001901130000ULL, 154668576026667ULL, 156001925820000ULL, 157335275613333ULL,
        158001950510000ULL, 158668625406667ULL, 160001975200000ULL, 161335324993333ULL,
        162001999890000ULL, 162668674786667ULL, 164002024580000ULL, 165335374373333ULL,
        166002049270000ULL, 166668724166667ULL, 168002073960000ULL, 169335423753333ULL,
        170002098650000ULL};

int main(void)
{
    clock_pll_t c[CLOCK_PLL_CANDIDATES_MAX];
    const unsigned n = CLOCK_PllCandidates(CLOCK_SYSCLK_AUTO_MIN_HZ, CLOCK_SYSCLK_MAX_HZ, c, CLOCK_PLL_CANDIDATES_MAX);
    CHECK(n == sizeof(want_hz) / sizeof(want_hz[0]));
    for (unsigned i = 0U; i < n && i < sizeof(want_hz) / sizeof(want_hz[0]); ++i) {
        CHECK(CLOCK_PllNominalHz(c[i]) == want_hz[i]);
        CHECK(CLOCK_PllMicroHz(c[i], 12345) == want_uhz[i]);
        CHECK(c[i].m % CLOCK_REF_SCALE == 0U);
    }
    const clock_pll_t boot = {CLOCK_BOOT_PLL_M, CLOCK_BOOT_PLL_N, CLOCK_BOOT_PLL_R};
    CHECK(CLOCK_PllLegal(boot));
    CHECK(CLOCK_PllNominalHz(boot) == 170000000U);
    CHECK(CLOCK_HseMicroHz(1000) - CLOCK_HseMicroHz(0) == (uint64_t)CLOCK_HSE_NOMINAL_HZ);   /* 1 ppm */
    if (failures) { fprintf(stderr, "%u clock_ref_scale checks failed\n", failures); return 1; }
    printf("clock_ref_scale: a %lu MHz reference gives the 8 MHz plans exactly (%u system clocks)\n",
           (unsigned long)(CLOCK_HSE_NOMINAL_HZ / 1000000UL), n);
    return 0;
}

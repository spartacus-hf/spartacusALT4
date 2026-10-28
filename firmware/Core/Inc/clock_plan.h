#ifndef CLOCK_PLAN_H
#define CLOCK_PLAN_H
/* Pure arithmetic of the REV1 clock tree: the 8 MHz reference oscillator on
 * HSE, the integer PLL that makes SYSCLK from it, the HRTIM carrier and LO
 * periods that divide SYSCLK x 32, and the planner that picks a PLL setting
 * for a frequency. No HAL: host-tested in the clock_plan suite.
 *
 * Every frequency the radio makes -- the transmit carrier, the receive LO,
 * the I2S sample rate, the TX phase-update rate -- is a ratio of the one
 * reference, so the reference's own error moves all of them together. A
 * correction in parts per billion (measured against NTP, see
 * `tools/rev1_diag.py refcal`) enters here and nowhere else. */
#include <stdint.h>
#include "radio_config.h"

/* The reference oscillator on HSE (bypass: a logic-level clock, not clipped
 * sine). REV1 carries an 8 MHz DSC1003 MEMS; a TCXO that replaces it should
 * be a multiple of 8 MHz -- 16, 24, 32 or 40 MHz. CLOCK_REF_SCALE then
 * multiplies every PLL M the planner tries, and each setting the radio picks
 * today comes out the same: the same SYSCLK, HRTIM grid and spurs. 48 MHz,
 * though inside the HSE input's range, would lose the 8 MHz / 3 settings
 * (M 18 is past the PLL's 16): 10 of the 21 clocks. HSE_VALUE
 * (stm32g4xx_hal_conf.h) must say the same (main.c checks); with a TCXO,
 * REF SOURCE LOCAL leaves the USB host's clock alone (radio_clocks.c). */
#ifndef CLOCK_HSE_NOMINAL_HZ               /* host tests try 16 MHz too */
#define CLOCK_HSE_NOMINAL_HZ      8000000UL
#endif
#define CLOCK_REF_SCALE           ((uint8_t)(CLOCK_HSE_NOMINAL_HZ / 8000000UL))
_Static_assert(CLOCK_HSE_NOMINAL_HZ % 8000000UL == 0UL && CLOCK_REF_SCALE * 3U <= 16U,
               "the reference is 8, 16, 24, 32 or 40 MHz: every 8 MHz PLL setting with M scaled stays legal");
/* The clock the radio boots on and falls back to: 170 MHz, 8 MHz / 2 x 85 / 2. */
#define CLOCK_BOOT_PLL_M          ((uint8_t)(2U * CLOCK_REF_SCALE))
#define CLOCK_BOOT_PLL_N          85U
#define CLOCK_BOOT_PLL_R          2U
#define CLOCK_REF_PPB_LIMIT       100000L    /* +-100 ppm; the DSC1003 is +-25 */
#define CLOCK_HRTIM_MUL           32U
#define CLOCK_MIN_PERIOD_TICKS    192U       /* two 96-tick compare floors */
#define CLOCK_SYSCLK_MAX_HZ       170000000UL
#define CLOCK_SYSCLK_MIN_HZ       165000000UL /* PRECISE/EXACT floor */
#define CLOCK_SYSCLK_AUTO_MIN_HZ  150000000UL /* AUTO scans from here */
/* What really bounds SYSCLK from below is the receive worker: the PCM1821
 * frame is 64 x the I2S divider SYSCLK cycles, and the worker must finish a
 * 64-frame half inside that. It costs ~49k cycles, peaking at 55.8-57.7k with
 * USB audio running (bench, v8.04). Divider 15 (960 cycles, 61.4k per half)
 * is what 165-170 MHz get, and 164 MHz too; AUTO takes any clock with it.
 * WIDE CLOCK RANGE accepts divider 14 (153.3-162.7 MHz, 57.3k: 97 % at the
 * peak) where the transmitter also fits (CLOCK_TxFits). Divider 13
 * (150-152 MHz, 53.2k) overran 47 times in 18 s. */
#define CLOCK_RX_FRAME_CYCLES_MIN       960U
#define CLOCK_RX_FRAME_CYCLES_WIDE_MIN  896U
/* The transmitter bounds SYSCLK too. Its interrupts and render worker need a
 * fixed number of cycles a millisecond whatever the clock, so a slower clock
 * leaves the main loop less: USB-audio SSB with the speech tools and TX PHASE
 * 384K took 99.7 % of the CPU at 153.3 MHz (bench, v8.06 before its render
 * work; 90 % at 164 MHz after it). The worst case per load class, measured on
 * v8.06 Release-Speed with the speech tools on (VOICE, compressor, EQ, mic AGC,
 * ALC; a full-scale 1 kHz tone) plus about 3 %: SSB 147.8k / 137.6k cycles a
 * millisecond at 384 / 192 kHz, AM 120.2k / 111.1k, CW 116.3k / 106.8k, FM
 * 96.9k / 87.4k; SSB under USB AUDIO POLICY DIGI CLEAN, which bypasses the
 * tools, 127.6k / 118.4k. With VOICE the tools are assumed on, because they
 * can be switched while transmitting. A clock fits a class when that load is
 * at most CLOCK_TX_BUDGET_PCT of it. */
#define CLOCK_TX_BUDGET_PCT             92U
typedef enum {
    CLOCK_TX_LOAD_SSB = 0,  /* SSB with the speech tools, and the internal test signals */
    CLOCK_TX_LOAD_SSB_CLEAN, /* SSB under DIGI CLEAN (FT8 and other data modes) */
    CLOCK_TX_LOAD_AM,
    CLOCK_TX_LOAD_CW,       /* CW and TX CARRIER TEST */
    CLOCK_TX_LOAD_FM,
    CLOCK_TX_LOAD_COUNT
} clock_tx_load_t;
#define CLOCK_RX_IF_TARGET_HZ     12000UL
/* An RX plan this close to the 12 kHz target keeps the calibrated image
 * rejection; farther out the fixed I/Q skew costs rejection (REV1.md). */
#define CLOCK_RX_IF_TOLERANCE_HZ  8000UL
#define CLOCK_PLL_CANDIDATES_MAX  24U

typedef struct { uint8_t m, n, r; } clock_pll_t;

/* The reference in micro-hertz, 8 MHz corrected by `ppb`. Exact. */
uint64_t CLOCK_HseMicroHz(int32_t ppb);
/* SYSCLK of a PLL setting in micro-hertz (rounded; error < 1 uHz). */
uint64_t CLOCK_PllMicroHz(clock_pll_t pll, int32_t ppb);
/* SYSCLK of a PLL setting at the nominal reference, rounded to 1 Hz. */
uint32_t CLOCK_PllNominalHz(clock_pll_t pll);
/* Inside RM0440/DS12288 limits: input 2.66-16 MHz, VCO 96-344 MHz, R even. */
uint8_t CLOCK_PllLegal(clock_pll_t pll);
/* Distinct SYSCLKs in [min_hz, max_hz], ascending, each with the smallest M
 * that reaches it (the highest PLL input frequency). Returns the count. */
unsigned CLOCK_PllCandidates(uint32_t min_hz, uint32_t max_hz, clock_pll_t *out, unsigned cap);

/* SYSCLK cycles per PCM1821 frame at this SYSCLK: 64 x the I2S divider the
 * receiver will choose (RADIO_I2sDivider, 176.4 kHz family). */
uint32_t CLOCK_RxFrameCycles(uint32_t sysclk_hz);

/* Worst-case TX cycles a millisecond of a load class (see CLOCK_TX_BUDGET_PCT),
 * at the 384 kHz phase rate when `phase384`; and whether SYSCLK carries it. */
uint32_t CLOCK_TxCyclesPerMs(clock_tx_load_t load, uint8_t phase384);
uint8_t CLOCK_TxFits(uint32_t sysclk_hz, clock_tx_load_t load, uint8_t phase384);

/* Nearest HRTIM carrier period for `target_hz` at a SYSCLK in uHz, and the
 * frequency that period really makes, minus the target, in milli-hertz. */
uint16_t CLOCK_CarrierPeriod(uint64_t sysclk_uhz, uint32_t target_hz);
int64_t CLOCK_PeriodErrorMilliHz(uint64_t sysclk_uhz, uint32_t period, uint32_t target_hz);
/* Frequency of an HRTIM period at a SYSCLK in uHz, in micro-hertz. */
uint64_t CLOCK_PeriodMicroHz(uint64_t sysclk_uhz, uint32_t period);

/* The TX images the 192 kHz phase-update NCO leaves at +-192 kHz, dBc, for a
 * grid residual `err_hz` and phase-update rate `update_hz` (ZOH images of the
 * staircase phase ramp). Rounded to a whole dB; 0 means no estimate. */
int32_t CLOCK_ImageDbc(uint32_t err_hz, uint32_t update_hz);

/* The TX carrier on the HRTIM grid. The branch timers run a whole period P
 * between the master compares that reset them; the master runs N carrier
 * periods plus k ticks, so the carrier averages P + k/N ticks, the fraction
 * entering as one k-tick nudge per master period (mod_hrtim_phase_geometry).
 * k = 0 is the whole period, the master at two or three periods. A whole
 * period alone left the carrier up to half a grid step from the dial -- 18 kHz
 * at 14 MHz -- for the TX NCO to rotate out in steps of up to 35 degrees at
 * every 192 kHz update, images at -20 dBc; with the fraction the residual is
 * mostly hundreds of hertz and the nudge a few 184 ps ticks. k is never
 * negative: that resets the timer just before its own roll-over, and in the
 * same fHRTIM cycle the roll-over wins (CLOCK_CarrierCostHz) -- the gate
 * smeared at every k = -1 on the bench (the first fractional build's HALF
 * presets). k reaches 8
 * for the fractions near a whole tick (up to 8/9), the 10 m worst case
 * -17 -> -27 dBc. */
typedef struct {
    uint16_t period;        /* P, ticks */
    uint8_t  cycles;        /* N, carrier periods per master period, >= 2 */
    int8_t   extra;         /* k, ticks the master adds, 0..8 */
} clock_carrier_t;
#define CLOCK_TX_UPDATE_HZ         192000UL  /* the TX phase-update rate (384K doubles it) */
#define CLOCK_CARRIER_EXTRA_MAX    8
/* How far the reference may drift from its calibration while the TX NCO follows
 * it (RADIO_RefTrimPpb): the trim's advances a plan must survive. */
#define CLOCK_TX_TRIM_MARGIN_PPB   5000U
#define CLOCK_CARRIER_CYCLES_MAX   255U
#define CLOCK_HRTIM_PERIOD_MAX     0xFFDFU   /* RM0440 table 232, MUL32 */
/* RM0440 28.3.7 (Fig. 242): set and reset on one output less than three
 * tHRTIM apart (96 ticks at MUL32) give a narrow pulse, or none within one. */
#define CLOCK_HRTIM_EDGE_GAP_TICKS 96U

/* What shapes the carrier plan besides the clock: the TX phase-update rate,
 * the share of an update one master period may span (1/master_div: the
 * master takes each new compare pair at its next roll-over, so a long master
 * period delays the modulation by up to that much; CW and the carrier test
 * have no modulation to delay), and the PA's duty offset (the plan is legal
 * at any: a positive nudge lengthens a high half). */
typedef struct {
    uint32_t update_hz;
    uint8_t  master_div;
    int16_t  duty_permille;  /* of a half period, PA_DUTY_PERMILLE_SCALE 1000 */
} clock_tx_shape_t;

/* The carrier a plan makes, micro-hertz, and its distance from `target_hz`
 * in milli-hertz. */
uint64_t CLOCK_CarrierMicroHz(uint64_t sysclk_uhz, const clock_carrier_t *c);
int64_t CLOCK_CarrierErrorMilliHz(uint64_t sysclk_uhz, const clock_carrier_t *c, uint32_t target_hz);
/* A plan's cost in TX images, as the grid residual that would leave the same
 * level at `update_hz`: the residual itself, the nudge's own spur (k/P of the
 * carrier, the floor of a 184 ps edge grid), or the jitter of master resets
 * the branch timer drops, whichever is largest. Hz. */
uint32_t CLOCK_CarrierCostHz(uint64_t sysclk_uhz, const clock_carrier_t *c, uint32_t target_hz,
                             uint32_t update_hz);
/* The plan for `target_hz` with the smallest cost. 0 (and *out zeroed) when
 * no period is legal (CLOCK_MIN_PERIOD_TICKS..65535). */
uint8_t CLOCK_CarrierPlan(uint64_t sysclk_uhz, uint32_t target_hz, const clock_tx_shape_t *shape,
                          clock_carrier_t *out);

/* One PLL setting judged for one tuned frequency. */
typedef struct {
    clock_pll_t pll;
    uint32_t sysclk_hz;     /* nominal, rounded */
    uint16_t period;        /* TX carrier period; 0 if none is legal */
    uint8_t  cycles;        /* its master period, N periods ... */
    int8_t   extra;         /* ... plus k ticks (CLOCK_CarrierPlan) */
    uint32_t tx_err_hz;     /* CLOCK_CarrierCostHz against the spectral centre */
    uint32_t floor_hz;      /* one tick of P as that cost: the 184 ps floor; UINT32_MAX if none */
    uint8_t  rx_quality;    /* RADIO_RxPlanLo: 2 clean, 1 at DC, 0 none */
    uint32_t rx_if_err_hz;  /* |IF| from the 12 kHz target; UINT32_MAX if no plan */
} clock_eval_t;

/* `tx_centre_hz` is where the transmitted spectrum sits relative to the dial
 * (+1500 USB, -1500 LSB, 0 otherwise): what the NCO images scale with is the
 * distance of that centre from the carrier grid, not of the dial. */
void CLOCK_Evaluate(clock_pll_t pll, int32_t ppb, uint32_t dial_hz, int32_t tx_centre_hz,
                    radio_rx_mode_t rx_mode, const clock_tx_shape_t *shape, clock_eval_t *e);
/* 3: clean and near the 12 kHz IF; 2: clean; 1: at DC; 0: no RX plan. */
uint8_t CLOCK_RxClass(const clock_eval_t *e);

/* A TX cost no PLL move could audibly improve: under CLOCK_TX_HOP_COST_HZ,
 * or within 3 dB of the 184 ps floor -- which from 17 m up is itself above
 * that (update_hz / P: 640 Hz at 18 MHz, 1 kHz at 28 MHz, 170 MHz). */
#define CLOCK_TX_HOP_COST_HZ 600U   /* about -50 dBc at 192 kHz */
uint8_t CLOCK_TxCostSettled(const clock_eval_t *e);

/* AUTO: among the candidates that can transmit, the best receive class
 * first and the smallest TX residual second. `current` (an index, or -1)
 * keeps its place unless its cost is not settled and another setting lowers
 * the NCO images by at least 3 dB, so tuning does not retune the PLL for
 * gains nobody could measure. */
int CLOCK_ChooseAuto(const clock_eval_t *e, unsigned n, int current);

/* TX-first: the setting the transmitter moves to at key-down when the
 * running one's cost is not settled (mostly 15 m, whose fractions the
 * running clocks rarely reach). Among the candidates with a clean receive
 * plan -- receive stays usable between overs -- the smallest cost, then the
 * receive class, then the higher SYSCLK; taken only if it at least halves
 * the cost (6 dB). Returns the index to run, or `current`. */
int CLOCK_ChooseTx(const clock_eval_t *e, unsigned n, int current);

#endif

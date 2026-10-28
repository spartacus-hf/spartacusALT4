/**
 * @file    hrtim_phase.h
 * @brief   HRTIM waveform configuration for the Chireix outphasing transmitter.
 *
 * Carrier period values are expressed in high-resolution HRTIM ticks.
 * The default HRTIM input clock is 170 MHz with MUL32 effective compare
 * timing, but experimental dynamic policies may retune the input clock while
 * RF is idle; use MOD_GetHrtim*ClockHz() for live timing math
 */
#ifndef HRTIM_PHASE_H
#define HRTIM_PHASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#define HRTIM_BASELINE_PERIOD       388U
#define HRTIM_BASELINE_HALF_PERIOD  194U
#define HRTIM_BASELINE_NULL          97U
/* Local output compare values need at least three fHRTIM clocks:
 * 3 * 32 = 96 high-resolution ticks. Master compare values are also used as
 * phase-reset commands for the outphasing branches; those must keep the legacy
 * near-full carrier range or SSB/FM common phase is clipped. */
#define HRTIM_OUTPUT_MIN_COMPARE     96U
#define HRTIM_PHASE_MIN_COMPARE       3U
#define HRTIM_MIN_COMPARE            HRTIM_OUTPUT_MIN_COMPARE
#define HRTIM_MIN_PERIOD_TICKS      192U

#define HRTIM_OUTPUT_SWAP_A        0x01U
#define HRTIM_OUTPUT_SWAP_C        0x02U
#define HRTIM_OUTPUT_SWAP_MASK     (HRTIM_OUTPUT_SWAP_A | HRTIM_OUTPUT_SWAP_C)

typedef struct {
    uint16_t period_ticks;
    uint16_t half_period_ticks;
    uint16_t null_ticks;
    uint16_t min_compare_ticks;
    uint16_t max_compare_ticks;
} hrtim_carrier_profile_t;

void HRTIM_Phase_Init(HRTIM_HandleTypeDef *hhrtim,
                      uint16_t period_ticks,
                      uint16_t null_ticks);
HAL_StatusTypeDef HRTIM_Phase_ColdStart(HRTIM_HandleTypeDef *hhrtim,
                                        uint16_t period_ticks,
                                        uint16_t null_ticks);
HAL_StatusTypeDef HRTIM_ReconfigureCarrier(HRTIM_HandleTypeDef *hhrtim,
                                           uint16_t period_ticks,
                                           uint16_t null_ticks);
HAL_StatusTypeDef HRTIM_ApplyTimingUpdate(HRTIM_HandleTypeDef *hhrtim,
                                          uint16_t period_ticks);
/* Calibrate the DLL for the running HRTIM clock (boot, after a PLL move). */
HAL_StatusTypeDef HRTIM_DllCalibrate(HRTIM_HandleTypeDef *hhrtim, uint32_t timeout_ms);

/* Dead-time injection API.
 *
 * The live DTG configuration uses the fine prescaler (DTPRSC 000, tHRTIM / 8).
 * The physical tick size is runtime-clock dependent: about 735 ps at the default 170 MHz
 * HRTIM input clock, and proportionally larger at lower experimental clocks.
 * Raw tick access remains available for compatibility and diagnostics; the
 * main UI/storage model should use physical time through MOD_SetDeadTime*Ns().
 */
void     HRTIM_SetDeadTimeTicks(uint16_t ticks);
uint16_t HRTIM_GetDeadTimeTicks(void);
void     HRTIM_SetDeadTimeTicksRF(uint16_t rising, uint16_t falling);
uint16_t HRTIM_GetDeadTimeRiseTicks(void);
uint16_t HRTIM_GetDeadTimeFallTicks(void);

/* Duty-cycle offset API (applied on next reconfig).
 *
 * Shifts the Timer A/E compare-1 point away from the ideal 50 % mark
 * by `offset` HRTIM ticks. Positive values lengthen the high time of
 * TA1/TE1 and shorten the low time; negative values do the opposite.
 *
 * Purpose: the UCC27524 (and similar non-inverting gate drivers) has
 * asymmetric rise/fall times. A 50 % square wave at the driver input
 * becomes a non-50 % waveform at the MOSFET gate, which directly causes
 * even-harmonic content and efficiency loss in a Class-D PA. Measure
 * the driver output high/low times on a scope and set this offset in
 * ticks to restore 50 % at the gate.
 *
 * Example: period = 388 ticks (14 MHz), UCC driver makes high 6 ticks
 * longer than low. Set offset = -3 to compensate.
 *
 * The modulator clamps this against the active carrier geometry before use
 * (mod_hrtim_duty_offset_bounds: CMP1 stays 96 ticks from both edges, so
 * on 10 m only 0 at P = 192, 0..+1 at 193 and -1..+1 at 194).
 * Default 0 = stock 50 % duty. Takes effect on the next HRTIM reconfig,
 * which happens automatically when called via MOD_SetDutyOffset().
 */
void    HRTIM_SetDutyOffsetTicks(int16_t offset);
int16_t HRTIM_GetDutyOffsetTicks(void);

/* Output-pair swap API.
 *
 * Swaps TA1<->TA2 and/or TE1<->TE2 at the HRTIM output source level. This is
 * the software equivalent of crossing the two MOSFET gate-drive pins of one
 * push-pull WTST primary. It corrects primary/secondary polarity without
 * changing the branch phase command used by the outphasing law.
 */
void    HRTIM_SetOutputSwapMask(uint8_t mask);
uint8_t HRTIM_GetOutputSwapMask(void);

/* Fractional carrier (clock_plan.h clock_carrier_t): the master runs `cycles`
 * carrier periods plus `extra` ticks, so the carrier averages
 * P + extra/cycles ticks. Applied on the next cold start or reconfigure. */
void    HRTIM_SetCarrierFraction(uint8_t cycles, int8_t extra);

#ifdef __cplusplus
}
#endif

#endif /* HRTIM_PHASE_H */

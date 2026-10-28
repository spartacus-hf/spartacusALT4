/**
 * @file    hrtim_phase.c
 * @brief   HRTIM waveform configuration for the Chireix outphasing transmitter.
 */

#include "hrtim_phase.h"
#include "main.h"
#include "modulator.h"
#include "mod_transport_logic.h"

/* Dead-time setting in raw DTG ticks. Stored here and applied in
 * hrtim_config_base_timers. 0 = disabled. Boot deliberately forces a
 * dead-time-free cold start first, then applies the requested value through
 * the validated Stop -> Reconfigure -> Start path using the fine DTPRSC 000
 * dead-time prescaler. */
static uint16_t g_deadtime_ticks = 0U;
static uint16_t g_deadtime_rise_ticks = 0U;
static uint16_t g_deadtime_fall_ticks = 0U;

/* Duty-cycle offset in HRTIM ticks. Applied to Timer A/E CMP1 on next update.
 * Positive = longer TA1/TE1 high time, negative = shorter. */
static int16_t g_duty_offset_ticks = 0;

/* Output-pair swap mask. Used for WTST primary polarity correction: swapping
 * the two complementary outputs of a branch is not the same operation as
 * changing the branch phase law. */
static uint8_t g_output_swap_mask = 0U;

/* The carrier fraction (clock_carrier_t): the master runs g_frac_cycles
 * carrier periods plus g_frac_extra ticks. Applied on the next reconfigure. */
static uint8_t g_frac_cycles = 2U;
static int8_t g_frac_extra = 0;

/* Timer A/E are phase-shifted by resetting their counters from Master CMP1/2.
 * The pre-atomic geometry keeps the complementary push-pull edges local to
 * each slave timer: there a master-compare edge and a local edge of the
 * opposite sense could land on one event, with ambiguous ordering on the short
 * high-band periods. The atomic geometry adds the resetting master compare as
 * a second source of the period-start edge only (mod_hrtim_branch_edges):
 * with PER alone that edge was lost whenever the reset came before the
 * roll-over.
 *
 * STM32G4 HRTIM compare values in MUL32 mode must be at least three fHRTIM
 * clock periods after counter reset, i.e. 0x60 high-resolution ticks. High
 * bands make the old period/4 idle phase illegal, and an illegal Master CMP
 * reset is exactly the observed every-15/16-cycles dropout pattern. The local
 * PER guard is therefore capped so Timer A/E CMP1 never drops below the same
 * hardware compare floor. */
#define HRTIM_SLAVE_PERIOD_ADVANCE_TICKS 64U
#define HRTIM_SLAVE_PERIOD_ADVANCE_DIV    4U

/* Fine dead-time hardware mode: tDTG = tHRTIM / 8 (DTPRSC 000, see
 * MOD_DEADTIME_DTPRSC), about 735 ps per raw tick at the default 170 MHz
 * HRTIM input; dynamic clock policies update the physical ps<->tick
 * conversion in the modulator layer. */
#define HRTIM_DT_PRESCALER_BITS   ((uint32_t)MOD_DEADTIME_DTPRSC << HRTIM_DTR_DTPRSC_Pos)
#define HRTIM_DT_MAX_RAW_TICKS    511U

_Static_assert(HRTIM_DT_PRESCALER_BITS == HRTIM_TIMDEADTIME_PRESCALERRATIO_DIV8,
               "DTPRSC 000 is HAL's DIV8 on STM32G4");
_Static_assert(HRTIM_DT_MAX_RAW_TICKS == (uint16_t)(HRTIM_DTR_DTR_Msk >> HRTIM_DTR_DTR_Pos),
               "Unexpected STM32G4 HRTIM deadtime value field width");

static inline uint16_t clamp_deadtime_ticks(uint16_t ticks)
{
    if (ticks > HRTIM_DT_MAX_RAW_TICKS) ticks = HRTIM_DT_MAX_RAW_TICKS;
    return ticks;
}

static inline uint8_t deadtime_enabled_raw(uint16_t rising, uint16_t falling)
{
    return (uint8_t)((rising != 0U) || (falling != 0U));
}

static inline uint8_t deadtime_enabled_now(void)
{
    return deadtime_enabled_raw(g_deadtime_rise_ticks, g_deadtime_fall_ticks);
}

static inline uint16_t deadtime_register_rising_ticks(uint32_t dtr)
{
    return (uint16_t)((dtr & HRTIM_DTR_DTR) >> HRTIM_DTR_DTR_Pos);
}

static inline uint16_t deadtime_register_falling_ticks(uint32_t dtr)
{
    return (uint16_t)((dtr & HRTIM_DTR_DTF) >> HRTIM_DTR_DTF_Pos);
}

static uint32_t build_deadtime_register(uint16_t rising, uint16_t falling)
{
    uint32_t dtr = 0U;
    rising = clamp_deadtime_ticks(rising);
    falling = clamp_deadtime_ticks(falling);
    dtr |= (uint32_t)HRTIM_DT_PRESCALER_BITS & HRTIM_DTR_DTPRSC;
    dtr |= ((uint32_t)rising << HRTIM_DTR_DTR_Pos) & HRTIM_DTR_DTR;
    dtr |= HRTIM_TIMDEADTIME_RISINGSIGN_POSITIVE & HRTIM_DTR_SDTR;
    dtr |= HRTIM_TIMDEADTIME_RISINGSIGNLOCK_WRITE & HRTIM_DTR_DTRSLK;
    dtr |= HRTIM_TIMDEADTIME_RISINGLOCK_WRITE & HRTIM_DTR_DTRLK;
    dtr |= ((uint32_t)falling << HRTIM_DTR_DTF_Pos) & HRTIM_DTR_DTF;
    dtr |= HRTIM_TIMDEADTIME_FALLINGSIGN_POSITIVE & HRTIM_DTR_SDTF;
    dtr |= HRTIM_TIMDEADTIME_FALLINGSIGNLOCK_WRITE & HRTIM_DTR_DTFSLK;
    dtr |= HRTIM_TIMDEADTIME_FALLINGLOCK_WRITE & HRTIM_DTR_DTFLK;
    return dtr;
}

static uint8_t deadtime_register_matches(uint32_t dtr, uint16_t rising, uint16_t falling)
{
    rising = clamp_deadtime_ticks(rising);
    falling = clamp_deadtime_ticks(falling);
    return (uint8_t)(((dtr & HRTIM_DTR_DTPRSC) == ((uint32_t)HRTIM_DT_PRESCALER_BITS & HRTIM_DTR_DTPRSC))
                  && (deadtime_register_rising_ticks(dtr) == rising)
                  && (deadtime_register_falling_ticks(dtr) == falling));
}

static inline void write_timer_deadtime_register(HRTIM_HandleTypeDef *hhrtim, uint32_t timer_idx)
{
    hhrtim->Instance->sTimerxRegs[timer_idx].DTxR = build_deadtime_register(g_deadtime_rise_ticks,
                                                                            g_deadtime_fall_ticks);
}

static inline void write_timer_deadtime_enable(HRTIM_HandleTypeDef *hhrtim, uint32_t timer_idx)
{
    MODIFY_REG(hhrtim->Instance->sTimerxRegs[timer_idx].OUTxR,
               HRTIM_OUTR_DTEN,
               deadtime_enabled_now() ? HRTIM_OUTR_DTEN : 0U);
}

static inline void write_timer_deadtime(HRTIM_HandleTypeDef *hhrtim, uint32_t timer_idx)
{
    write_timer_deadtime_register(hhrtim, timer_idx);
    write_timer_deadtime_enable(hhrtim, timer_idx);
}

void HRTIM_SetDeadTimeTicks(uint16_t ticks)
{
    g_deadtime_ticks = clamp_deadtime_ticks(ticks);
    g_deadtime_rise_ticks = g_deadtime_ticks;
    g_deadtime_fall_ticks = g_deadtime_ticks;
}
uint16_t HRTIM_GetDeadTimeTicks(void)    { return g_deadtime_ticks; }

void HRTIM_SetDeadTimeTicksRF(uint16_t rising, uint16_t falling)
{
    g_deadtime_rise_ticks = clamp_deadtime_ticks(rising);
    g_deadtime_fall_ticks = clamp_deadtime_ticks(falling);
    g_deadtime_ticks = (uint16_t)(((uint32_t)g_deadtime_rise_ticks + (uint32_t)g_deadtime_fall_ticks + 1U) / 2U);
}
uint16_t HRTIM_GetDeadTimeRiseTicks(void) { return g_deadtime_rise_ticks; }
uint16_t HRTIM_GetDeadTimeFallTicks(void) { return g_deadtime_fall_ticks; }

void HRTIM_SetDutyOffsetTicks(int16_t offset)
{
    /* The caller should clamp against the active carrier geometry. Keep the
     * raw request here so future period-aware callers do not fight a stale
     * hardcoded ceiling. */
    g_duty_offset_ticks = offset;
}
int16_t HRTIM_GetDutyOffsetTicks(void)   { return g_duty_offset_ticks; }

void HRTIM_SetOutputSwapMask(uint8_t mask)
{
    g_output_swap_mask = mask & HRTIM_OUTPUT_SWAP_MASK;
}

uint8_t HRTIM_GetOutputSwapMask(void) { return g_output_swap_mask; }

void HRTIM_SetCarrierFraction(uint8_t cycles, int8_t extra)
{
    g_frac_cycles = (cycles < 2U) ? 2U : cycles;
    g_frac_extra = (cycles < 2U) ? 0 : extra;
}

/* Compute the actual CMP1 value for Timer A/C given the current period.
 * Applies the duty offset and clamps so the compare value stays within
 * the valid range [hardware minimum, period margin]. */
static inline uint16_t compute_cmp1_with_duty(uint16_t period_ticks, uint16_t half_period)
{
    int32_t cmp = (int32_t)half_period + (int32_t)g_duty_offset_ticks;
    int32_t lo  = (int32_t)HRTIM_MIN_COMPARE;
    int32_t hi  = (int32_t)period_ticks - 4;
    if (hi < lo) hi = lo;
    if (cmp < lo) cmp = lo;
    if (cmp > hi) cmp = hi;
    return (uint16_t)cmp;
}

static inline uint16_t compute_slave_guard_ticks(uint16_t period_ticks, uint16_t cmp1_nominal)
{
    uint16_t guard = HRTIM_SLAVE_PERIOD_ADVANCE_TICKS;
    uint16_t period_guard = (uint16_t)(period_ticks / HRTIM_SLAVE_PERIOD_ADVANCE_DIV);

    if (period_guard < 4U) period_guard = 4U;
    if (guard > period_guard) guard = period_guard;
    if (cmp1_nominal > HRTIM_MIN_COMPARE) {
        uint16_t cmp_guard = (uint16_t)(cmp1_nominal - HRTIM_MIN_COMPARE);
        if (guard > cmp_guard) guard = cmp_guard;
    } else {
        guard = 0U;
    }
    return guard;
}

static inline void compute_slave_waveform_geometry(uint16_t period_ticks,
                                                   uint16_t half_period,
                                                   uint16_t *slave_period_ticks,
                                                   uint16_t *cmp1_ticks)
{
    uint16_t cmp_nominal = compute_cmp1_with_duty(period_ticks, half_period);
    uint16_t guard = compute_slave_guard_ticks(period_ticks, cmp_nominal);
    uint16_t slave_period = (guard < period_ticks) ? (uint16_t)(period_ticks - guard)
                                                   : period_ticks;
    uint16_t cmp = (cmp_nominal > guard) ? (uint16_t)(cmp_nominal - guard)
                                         : 4U;

    if (slave_period < HRTIM_MIN_PERIOD_TICKS) slave_period = period_ticks;

    uint16_t cmp_hi = (slave_period > 4U) ? (uint16_t)(slave_period - 4U)
                                          : slave_period;
    if (cmp_hi < HRTIM_MIN_COMPARE) cmp_hi = HRTIM_MIN_COMPARE;
    if (cmp < HRTIM_MIN_COMPARE) cmp = HRTIM_MIN_COMPARE;
    if (cmp > cmp_hi) cmp = cmp_hi;

    if (slave_period_ticks != NULL) *slave_period_ticks = slave_period;
    if (cmp1_ticks != NULL) *cmp1_ticks = cmp;
}

#if MOD_HRTIM_ATOMIC_EXPERIMENT
static inline void hrtim_arm_atomic_master_update(HRTIM_HandleTypeDef *hhrtim)
{
    /* BRSTDMA=10 blocks ordinary master updates until a DMA burst has
     * completed. Arm it only after the stopped-timer software update has
     * copied the legal initial compare values into the active registers. */
    MODIFY_REG(hhrtim->Instance->sMasterRegs.MCR,
               HRTIM_MCR_BRSTDMA | HRTIM_MCR_MREPU,
               HRTIM_MCR_BRSTDMA_1);
}

static HAL_StatusTypeDef hrtim_config_atomic_trigger(HRTIM_HandleTypeDef *hhrtim)
{
    HRTIM_TimeBaseCfgTypeDef trigger_timebase = {0};
    HRTIM_TimerCfgTypeDef trigger_cfg = {0};
    HRTIM_EventCfgTypeDef event_cfg = {0};

    /* TIM6 update is already exported as TRGO by MX_TIM6_Init. Route it
     * through EEV10 so one Timer-F reset request starts each HRTIM burst. */
    event_cfg.Source = HRTIM_EEV10SRC_TIM6_TRGO;
    event_cfg.Polarity = HRTIM_EVENTPOLARITY_HIGH;
    event_cfg.Sensitivity = HRTIM_EVENTSENSITIVITY_RISINGEDGE;
    event_cfg.Filter = HRTIM_EVENTFILTER_NONE;
    event_cfg.FastMode = HRTIM_EVENTFASTMODE_DISABLE;
    if (HAL_HRTIM_EventConfig(hhrtim, HRTIM_EVENT_10, &event_cfg) != HAL_OK) return HAL_ERROR;

    /* At DIV4, 0xFFDF is far slower than 48 kHz. Timer-F rollover therefore
     * cannot masquerade as a sample request; only its EEV10 reset can assert
     * RSTDE. 0xFFDF is the maximum period accepted by the STM32G4 HAL. */
    trigger_timebase.Period = 0xFFDFU;
    trigger_timebase.RepetitionCounter = 0U;
    trigger_timebase.PrescalerRatio = HRTIM_PRESCALERRATIO_DIV4;
    trigger_timebase.Mode = HRTIM_MODE_CONTINUOUS;
    if (HAL_HRTIM_TimeBaseConfig(hhrtim,
                                 HRTIM_TIMERINDEX_TIMER_F,
                                 &trigger_timebase) != HAL_OK) return HAL_ERROR;

    trigger_cfg.InterruptRequests = HRTIM_TIM_IT_NONE;
    /* The experiment owns DMA1 channel 1 directly instead of using the HAL
     * HRTIM DMA-start API. Keep the HAL request cache disabled and gate RSTDE
     * explicitly in the transport start/stop critical section. */
    trigger_cfg.DMARequests = HRTIM_TIM_DMA_NONE;
    trigger_cfg.DMASize = 1U;
    trigger_cfg.HalfModeEnable = HRTIM_HALFMODE_DISABLED;
    trigger_cfg.InterleavedMode = HRTIM_INTERLEAVED_MODE_DISABLED;
    trigger_cfg.StartOnSync = HRTIM_SYNCSTART_DISABLED;
    trigger_cfg.ResetOnSync = HRTIM_SYNCRESET_DISABLED;
    trigger_cfg.DACSynchro = HRTIM_DACSYNC_NONE;
    trigger_cfg.PreloadEnable = HRTIM_PRELOAD_DISABLED;
    trigger_cfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    trigger_cfg.BurstMode = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    trigger_cfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;
    trigger_cfg.PushPull = HRTIM_TIMPUSHPULLMODE_DISABLED;
    trigger_cfg.FaultEnable = HRTIM_TIMFAULTENABLE_NONE;
    trigger_cfg.FaultLock = HRTIM_TIMFAULTLOCK_READWRITE;
    trigger_cfg.DeadTimeInsertion = HRTIM_TIMDEADTIMEINSERTION_DISABLED;
    trigger_cfg.DelayedProtectionMode = HRTIM_TIMER_F_DELAYEDPROTECTION_DISABLED;
    trigger_cfg.UpdateTrigger = HRTIM_TIMUPDATETRIGGER_NONE;
    trigger_cfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_EEV_10;
    trigger_cfg.ResetUpdate = HRTIM_TIMUPDATEONRESET_DISABLED;
    trigger_cfg.ReSyncUpdate = HRTIM_TIMERESYNC_UPDATE_UNCONDITIONAL;
    if (HAL_HRTIM_WaveformTimerConfig(hhrtim,
                                      HRTIM_TIMERINDEX_TIMER_F,
                                      &trigger_cfg) != HAL_OK) return HAL_ERROR;

    /* RM0440 parses master CMP1 before CMP2. Clearing every slave selection
     * makes each BDMADR transaction exactly two ordered 32-bit transfers. */
    if (HAL_HRTIM_BurstDMAConfig(hhrtim,
                                 HRTIM_TIMERINDEX_MASTER,
                                 HRTIM_BURSTDMA_CMP1 | HRTIM_BURSTDMA_CMP2) != HAL_OK) return HAL_ERROR;
    if (HAL_HRTIM_BurstDMAConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_A, HRTIM_BURSTDMA_NONE) != HAL_OK) return HAL_ERROR;
    if (HAL_HRTIM_BurstDMAConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_B, HRTIM_BURSTDMA_NONE) != HAL_OK) return HAL_ERROR;
    if (HAL_HRTIM_BurstDMAConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_C, HRTIM_BURSTDMA_NONE) != HAL_OK) return HAL_ERROR;
    if (HAL_HRTIM_BurstDMAConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_D, HRTIM_BURSTDMA_NONE) != HAL_OK) return HAL_ERROR;
    if (HAL_HRTIM_BurstDMAConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_E, HRTIM_BURSTDMA_NONE) != HAL_OK) return HAL_ERROR;
    if (HAL_HRTIM_BurstDMAConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_F, HRTIM_BURSTDMA_NONE) != HAL_OK) return HAL_ERROR;
    return HAL_OK;
}
#endif

_Static_assert((MOD_HRTIM_EDGE_PER == HRTIM_SET1R_PER) && (MOD_HRTIM_EDGE_PER == HRTIM_RST1R_PER) &&
               (MOD_HRTIM_EDGE_PER == HRTIM_SET2R_PER) && (MOD_HRTIM_EDGE_PER == HRTIM_RST2R_PER),
               "HRTIM PER edge bit");
_Static_assert((MOD_HRTIM_EDGE_CMP1 == HRTIM_SET1R_CMP1) && (MOD_HRTIM_EDGE_CMP1 == HRTIM_RST1R_CMP1) &&
               (MOD_HRTIM_EDGE_CMP1 == HRTIM_SET2R_CMP1) && (MOD_HRTIM_EDGE_CMP1 == HRTIM_RST2R_CMP1),
               "HRTIM CMP1 edge bit");
_Static_assert((MOD_HRTIM_EDGE_MSTCMP1 == HRTIM_SET1R_MSTCMP1) && (MOD_HRTIM_EDGE_MSTCMP1 == HRTIM_RST1R_MSTCMP1) &&
               (MOD_HRTIM_EDGE_MSTCMP1 == HRTIM_SET2R_MSTCMP1) && (MOD_HRTIM_EDGE_MSTCMP1 == HRTIM_RST2R_MSTCMP1),
               "HRTIM MSTCMP1 edge bit");
_Static_assert((MOD_HRTIM_EDGE_MSTCMP2 == HRTIM_SET1R_MSTCMP2) && (MOD_HRTIM_EDGE_MSTCMP2 == HRTIM_RST1R_MSTCMP2) &&
               (MOD_HRTIM_EDGE_MSTCMP2 == HRTIM_SET2R_MSTCMP2) && (MOD_HRTIM_EDGE_MSTCMP2 == HRTIM_RST2R_MSTCMP2),
               "HRTIM MSTCMP2 edge bit");

#if MOD_HRTIM_ATOMIC_EXPERIMENT
#define HRTIM_BRANCH_A_PHASE_RESET MOD_HRTIM_EDGE_MSTCMP1   /* Timer A resets on MSTCMP1 */
#define HRTIM_BRANCH_C_PHASE_RESET MOD_HRTIM_EDGE_MSTCMP2   /* Timer E resets on MSTCMP2 */
#else
#define HRTIM_BRANCH_A_PHASE_RESET 0U
#define HRTIM_BRANCH_C_PHASE_RESET 0U
#endif

/* `phase_reset` is the master compare that resets this timer, 0 for edges
 * from the timer alone. HAL takes one set and one reset source per output
 * (IS_HRTIM_OUTPUTSET), so it configures the output and the source registers
 * are written whole afterwards. */
static inline HAL_StatusTypeDef hrtim_config_complementary_outputs(HRTIM_HandleTypeDef *hhrtim,
                                                                   uint32_t timer_idx,
                                                                   uint32_t output_1,
                                                                   uint32_t output_2,
                                                                   uint8_t swapped,
                                                                   uint32_t phase_reset,
                                                                   HRTIM_OutputCfgTypeDef *outcfg)
{
    const mod_hrtim_branch_edges_t e = mod_hrtim_branch_edges(swapped, phase_reset);

    outcfg->SetSource   = e.set1 & ~phase_reset;
    outcfg->ResetSource = e.reset1 & ~phase_reset;
    if (HAL_HRTIM_WaveformOutputConfig(hhrtim, timer_idx, output_1, outcfg) != HAL_OK) return HAL_ERROR;

    outcfg->SetSource   = e.set2 & ~phase_reset;
    outcfg->ResetSource = e.reset2 & ~phase_reset;
    if (HAL_HRTIM_WaveformOutputConfig(hhrtim, timer_idx, output_2, outcfg) != HAL_OK) return HAL_ERROR;

    HRTIM_Timerx_TypeDef *t = &hhrtim->Instance->sTimerxRegs[timer_idx];
    t->SETx1R = e.set1;
    t->RSTx1R = e.reset1;
    t->SETx2R = e.set2;
    t->RSTx2R = e.reset2;
    return HAL_OK;
}

__attribute__((cold)) static HAL_StatusTypeDef hrtim_config_base_timers(HRTIM_HandleTypeDef *hhrtim,
                                              uint16_t period_ticks,
                                              uint16_t null_ticks)
{
    HRTIM_TimeBaseCfgTypeDef  master_timebase = {0};
    HRTIM_TimeBaseCfgTypeDef  slave_timebase = {0};
    HRTIM_TimerCfgTypeDef     timcfg   = {0};
    HRTIM_CompareCfgTypeDef   cmp_cfg  = {0};
    HRTIM_OutputCfgTypeDef    outcfg   = {0};
    uint16_t half_period = (uint16_t)(period_ticks / 2U);
    uint16_t slave_period_ticks = period_ticks;
    uint16_t cmp1_ticks = half_period;

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    mod_hrtim_phase_geometry_t geometry;
    if (mod_hrtim_phase_geometry_frac(period_ticks,
                                      g_duty_offset_ticks,
                                      null_ticks,
                                      g_frac_cycles,
                                      g_frac_extra,
                                      &geometry) == 0U) return HAL_ERROR;
    slave_period_ticks = geometry.carrier_period_ticks;
    cmp1_ticks = geometry.local_compare_ticks;
    null_ticks = geometry.master_compare_ticks;
#else
    compute_slave_waveform_geometry(period_ticks, half_period, &slave_period_ticks, &cmp1_ticks);
#endif

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    /* HRTIM PER is the period itself (AN4539), unlike general-purpose ARR:
     * N carrier periods plus the fraction's k ticks. */
    master_timebase.Period            = (uint32_t)geometry.master_period_ticks;
#else
    master_timebase.Period            = (uint32_t)period_ticks;
#endif
    master_timebase.RepetitionCounter = 0x00;
    master_timebase.PrescalerRatio    = HRTIM_PRESCALERRATIO_MUL32;
    master_timebase.Mode              = HRTIM_MODE_CONTINUOUS;

    slave_timebase = master_timebase;
    slave_timebase.Period = (uint32_t)slave_period_ticks;

    if (HAL_HRTIM_TimeBaseConfig(hhrtim, HRTIM_TIMERINDEX_MASTER, &master_timebase) != HAL_OK) return HAL_ERROR;

    timcfg.InterruptRequests   = HRTIM_MASTER_IT_NONE;
    timcfg.DMARequests         = HRTIM_MASTER_DMA_NONE;
    timcfg.DMASrcAddress       = 0x0000;
    timcfg.DMADstAddress       = 0x0000;
    timcfg.DMASize             = 0x1;
    timcfg.HalfModeEnable      = HRTIM_HALFMODE_DISABLED;
    timcfg.InterleavedMode     = HRTIM_INTERLEAVED_MODE_DISABLED;
    timcfg.StartOnSync         = HRTIM_SYNCSTART_DISABLED;
    timcfg.ResetOnSync         = HRTIM_SYNCRESET_DISABLED;
    timcfg.DACSynchro          = HRTIM_DACSYNC_NONE;
    timcfg.PreloadEnable       = HRTIM_PRELOAD_ENABLED;
    /* Diagnostic atomic mode also starts independent so the legal idle pair
     * can be loaded before the first burst. Cold-start/reconfigure arms
     * BRSTDMA=10 after that stopped-timer software update. */
    timcfg.UpdateGating        = HRTIM_UPDATEGATING_INDEPENDENT;
    timcfg.BurstMode           = HRTIM_TIMERBURSTMODE_MAINTAINCLOCK;
    timcfg.RepetitionUpdate    =
#if MOD_HRTIM_ATOMIC_EXPERIMENT
                                   HRTIM_UPDATEONREPETITION_DISABLED;
#else
                                   HRTIM_UPDATEONREPETITION_ENABLED;
#endif
    timcfg.ReSyncUpdate        = HRTIM_TIMERESYNC_UPDATE_UNCONDITIONAL;

    if (HAL_HRTIM_WaveformTimerConfig(hhrtim, HRTIM_TIMERINDEX_MASTER, &timcfg) != HAL_OK) return HAL_ERROR;

    cmp_cfg.CompareValue       = null_ticks;
    if (HAL_HRTIM_WaveformCompareConfig(hhrtim, HRTIM_TIMERINDEX_MASTER,
                                    HRTIM_COMPAREUNIT_1, &cmp_cfg) != HAL_OK) return HAL_ERROR;
    if (HAL_HRTIM_WaveformCompareConfig(hhrtim, HRTIM_TIMERINDEX_MASTER,
                                    HRTIM_COMPAREUNIT_2, &cmp_cfg) != HAL_OK) return HAL_ERROR;

    if (HAL_HRTIM_TimeBaseConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_A, &slave_timebase) != HAL_OK) return HAL_ERROR;

    timcfg.InterruptRequests     = HRTIM_TIM_IT_NONE;
    timcfg.DMARequests           = HRTIM_TIM_DMA_NONE;
    timcfg.PreloadEnable         = HRTIM_PRELOAD_ENABLED;
    timcfg.UpdateGating          = HRTIM_UPDATEGATING_INDEPENDENT;
    timcfg.RepetitionUpdate      = HRTIM_UPDATEONREPETITION_ENABLED;
    timcfg.PushPull              = HRTIM_TIMPUSHPULLMODE_DISABLED;
    timcfg.FaultEnable           = HRTIM_TIMFAULTENABLE_NONE;
    timcfg.FaultLock             = HRTIM_TIMFAULTLOCK_READWRITE;
    timcfg.DeadTimeInsertion     = deadtime_enabled_now()
                                    ? HRTIM_TIMDEADTIMEINSERTION_ENABLED
                                    : HRTIM_TIMDEADTIMEINSERTION_DISABLED;
    timcfg.DelayedProtectionMode = HRTIM_TIMER_A_B_C_DELAYEDPROTECTION_DISABLED;
    timcfg.UpdateTrigger         = HRTIM_TIMUPDATETRIGGER_MASTER;
    timcfg.ResetTrigger          = HRTIM_TIMRESETTRIGGER_MASTER_CMP1;
    timcfg.ResetUpdate           = HRTIM_TIMUPDATEONRESET_ENABLED;

    if (HAL_HRTIM_WaveformTimerConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_A, &timcfg) != HAL_OK) return HAL_ERROR;
    write_timer_deadtime(hhrtim, HRTIM_TIMERINDEX_TIMER_A);

    cmp_cfg.CompareValue = cmp1_ticks;
    if (HAL_HRTIM_WaveformCompareConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_A,
                                    HRTIM_COMPAREUNIT_1, &cmp_cfg) != HAL_OK) return HAL_ERROR;

    outcfg.Polarity              = HRTIM_OUTPUTPOLARITY_HIGH;
    outcfg.IdleMode              = HRTIM_OUTPUTIDLEMODE_NONE;
    outcfg.IdleLevel             = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    /* Protected outputs: the drivers' inputs fall to inactive (low) in
     * hardware on a halted core (DBG_HRTIM1_STOP, a debugger or a hot-plug
     * flash during TX) and on the system fault -- CSS, LOCKUP, flash double
     * ECC (MX_HRTIM1_Init) -- and stay there until TX entry sets OENR again.
     * No fault input is enabled on these timers (FaultEnable NONE). */
    outcfg.FaultLevel            = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    outcfg.ChopperModeEnable     = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    outcfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

    if (hrtim_config_complementary_outputs(hhrtim,
                                           HRTIM_TIMERINDEX_TIMER_A,
                                           HRTIM_OUTPUT_TA1,
                                           HRTIM_OUTPUT_TA2,
                                           (g_output_swap_mask & HRTIM_OUTPUT_SWAP_A) != 0U,
                                           HRTIM_BRANCH_A_PHASE_RESET,
                                           &outcfg) != HAL_OK) return HAL_ERROR;

    if (HAL_HRTIM_TimeBaseConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_E, &slave_timebase) != HAL_OK) return HAL_ERROR;

    timcfg.ResetTrigger = HRTIM_TIMRESETTRIGGER_MASTER_CMP2;
    if (HAL_HRTIM_WaveformTimerConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_E, &timcfg) != HAL_OK) return HAL_ERROR;
    write_timer_deadtime(hhrtim, HRTIM_TIMERINDEX_TIMER_E);

    cmp_cfg.CompareValue = cmp1_ticks;
    if (HAL_HRTIM_WaveformCompareConfig(hhrtim, HRTIM_TIMERINDEX_TIMER_E,
                                    HRTIM_COMPAREUNIT_1, &cmp_cfg) != HAL_OK) return HAL_ERROR;

    if (hrtim_config_complementary_outputs(hhrtim,
                                           HRTIM_TIMERINDEX_TIMER_E,
                                           HRTIM_OUTPUT_TE1,
                                           HRTIM_OUTPUT_TE2,
                                           (g_output_swap_mask & HRTIM_OUTPUT_SWAP_C) != 0U,
                                           HRTIM_BRANCH_C_PHASE_RESET,
                                           &outcfg) != HAL_OK) return HAL_ERROR;
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    if (hrtim_config_atomic_trigger(hhrtim) != HAL_OK) return HAL_ERROR;
#endif
    return HAL_OK;
}

HAL_StatusTypeDef HRTIM_Phase_ColdStart(HRTIM_HandleTypeDef *hhrtim,
                                        uint16_t period_ticks,
                                        uint16_t null_ticks)
{
    if ((period_ticks <
#if MOD_HRTIM_ATOMIC_EXPERIMENT
         MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32
#else
         HRTIM_MIN_PERIOD_TICKS
#endif
        ) || (null_ticks >= period_ticks))
    {
        return HAL_ERROR;
    }

    if (hrtim_config_base_timers(hhrtim, period_ticks, null_ticks) != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (HAL_HRTIM_SoftwareUpdate(hhrtim,
        HRTIM_TIMERUPDATE_MASTER | HRTIM_TIMERUPDATE_A | HRTIM_TIMERUPDATE_E
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        | HRTIM_TIMERUPDATE_F
#endif
        ) != HAL_OK)
    {
        return HAL_ERROR;
    }

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    hrtim_arm_atomic_master_update(hhrtim);
#endif

    /* The TX outputs stay disabled (IDLE, low): radio.c enables them at TX
     * entry and disables them again at TX exit. */
    if (HAL_HRTIM_WaveformCountStart(hhrtim,
        HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_E
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        | HRTIM_TIMERID_TIMER_F
#endif
        ) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}

__attribute__((cold)) HAL_StatusTypeDef HRTIM_ApplyTimingUpdate(HRTIM_HandleTypeDef *hhrtim,
                                          uint16_t period_ticks)
{
    if (period_ticks <
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32
#else
        HRTIM_MIN_PERIOD_TICKS
#endif
        )
    {
        return HAL_ERROR;
    }

    uint16_t half_period = (uint16_t)(period_ticks / 2U);
    uint16_t slave_period_ticks = period_ticks;
    uint16_t cmp1 = half_period;

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    mod_hrtim_phase_geometry_t geometry;
    if (mod_hrtim_phase_geometry_frac(period_ticks,
                                      g_duty_offset_ticks,
                                      0U,
                                      g_frac_cycles,
                                      g_frac_extra,
                                      &geometry) == 0U) return HAL_ERROR;
    slave_period_ticks = geometry.carrier_period_ticks;
    cmp1 = geometry.local_compare_ticks;
#else
    compute_slave_waveform_geometry(period_ticks, half_period, &slave_period_ticks, &cmp1);
#endif

    hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].PERxR = (uint32_t)slave_period_ticks;
    hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_E].PERxR = (uint32_t)slave_period_ticks;
    hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP1xR = cmp1;
    hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_E].CMP1xR = cmp1;
    write_timer_deadtime(hhrtim, HRTIM_TIMERINDEX_TIMER_A);
    write_timer_deadtime(hhrtim, HRTIM_TIMERINDEX_TIMER_E);

    if (!deadtime_register_matches(hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].DTxR,
                                  g_deadtime_rise_ticks,
                                  g_deadtime_fall_ticks)
     || !deadtime_register_matches(hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_E].DTxR,
                                   g_deadtime_rise_ticks,
                                   g_deadtime_fall_ticks)
     || (((hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].OUTxR & HRTIM_OUTR_DTEN) != 0U) != (deadtime_enabled_now() != 0U))
     || (((hhrtim->Instance->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_E].OUTxR & HRTIM_OUTR_DTEN) != 0U) != (deadtime_enabled_now() != 0U)))
    {
        return HAL_ERROR;
    }

    if (HAL_HRTIM_SoftwareUpdate(hhrtim,
        HRTIM_TIMERUPDATE_A | HRTIM_TIMERUPDATE_E) != HAL_OK)
    {
        return HAL_ERROR;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HRTIM_ReconfigureCarrier(HRTIM_HandleTypeDef *hhrtim,
                                           uint16_t period_ticks,
                                           uint16_t null_ticks)
{
    if ((period_ticks <
#if MOD_HRTIM_ATOMIC_EXPERIMENT
         MOD_HRTIM_MIN_CARRIER_PERIOD_MUL32
#else
         HRTIM_MIN_PERIOD_TICKS
#endif
        ) || (null_ticks >= period_ticks))
    {
        return HAL_ERROR;
    }
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    /* Refuse a geometry hrtim_config_base_timers would refuse before anything
     * stops, so a refused request leaves the running carrier as it was. */
    {
        mod_hrtim_phase_geometry_t geometry;
        if (mod_hrtim_phase_geometry_frac(period_ticks, g_duty_offset_ticks, null_ticks,
                                          g_frac_cycles, g_frac_extra, &geometry) == 0U)
            return HAL_ERROR;
    }
#endif

    uint32_t pm = __get_PRIMASK();
    __disable_irq();

    /* The TX outputs come back as they were: enabled in TRANSMIT, disabled
     * otherwise (radio.c). Enabling them here unconditionally put them in RUN
     * after every band change while receiving, the counters then parked under
     * them -- only the low GPIO pins kept the drivers' inputs low. */
    const uint32_t tx_outputs = hhrtim->Instance->sCommonRegs.OENR & BOARD_TX_OUTPUTS;
    if (HAL_HRTIM_WaveformOutputStop(hhrtim, BOARD_TX_OUTPUTS) != HAL_OK)
    {
        __set_PRIMASK(pm);
        return HAL_ERROR;
    }

    if (HAL_HRTIM_WaveformCountStop(hhrtim,
        HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_E
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        | HRTIM_TIMERID_TIMER_F
#endif
        ) != HAL_OK)
    {
        __set_PRIMASK(pm);
        return HAL_ERROR;
    }

    __set_PRIMASK(pm);

    if (hrtim_config_base_timers(hhrtim, period_ticks, null_ticks) != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (HAL_HRTIM_SoftwareUpdate(hhrtim,
        HRTIM_TIMERUPDATE_MASTER | HRTIM_TIMERUPDATE_A | HRTIM_TIMERUPDATE_E
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        | HRTIM_TIMERUPDATE_F
#endif
        ) != HAL_OK)
    {
        return HAL_ERROR;
    }

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    hrtim_arm_atomic_master_update(hhrtim);
#endif

    pm = __get_PRIMASK();
    __disable_irq();

    if (HAL_HRTIM_WaveformCountStart(hhrtim,
        HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_E
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        | HRTIM_TIMERID_TIMER_F
#endif
        ) != HAL_OK)
    {
        __set_PRIMASK(pm);
        return HAL_ERROR;
    }

    /* OENR is write-1-to-enable: only the outputs that were on, and not over
     * an emergency stop that came in while the timers were reconfigured with
     * interrupts on -- its ODISR would be undone here. */
    if (!BOARD_EmergencyLatched()) hhrtim->Instance->sCommonRegs.OENR = tx_outputs;

    __set_PRIMASK(pm);
    return HAL_OK;
}

/* RM0440 28.3.24: calibrate the DLL once, start the counters only after
 * DLLRDY, then keep it calibrated periodically (CALRTE 11, every 2048 tHRTIM,
 * ~12 us) against drift. Periodic calibration sets DLLRDY by itself every
 * ~12 us (bench, 2026-10-07), so the flag means "calibrated at this clock"
 * only for a one-shot run with periodic calibration off: the second pass is
 * one that started after any run that straddled a clock change. Not HAL's
 * start/poll pair: the start never clears DLLRDY, and a poll that timed out
 * returned with the handle still locked, failing every retry and every HAL
 * call on the HRTIM after it. */
__attribute__((cold)) HAL_StatusTypeDef HRTIM_DllCalibrate(HRTIM_HandleTypeDef *hhrtim, uint32_t timeout_ms)
{
    HRTIM_Common_TypeDef *c = &hhrtim->Instance->sCommonRegs;
    c->DLLCR &= ~HRTIM_DLLCR_CALEN;
    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        c->ICR = HRTIM_ICR_DLLRDYC;
        c->DLLCR |= HRTIM_DLLCR_CAL;
        const uint32_t start = HAL_GetTick();
        while ((c->ISR & HRTIM_ISR_DLLRDY) == 0U) {
            if ((HAL_GetTick() - start) > timeout_ms) return HAL_TIMEOUT;
        }
    }
    MODIFY_REG(c->DLLCR, HRTIM_DLLCR_CALRTE, HRTIM_CALIBRATIONRATE_3);
    c->DLLCR |= HRTIM_DLLCR_CALEN;
    return HAL_OK;
}

__attribute__((cold)) void HRTIM_Phase_Init(HRTIM_HandleTypeDef *hhrtim,
                      uint16_t period_ticks,
                      uint16_t null_ticks)
{
    uint16_t saved_deadtime_ticks = g_deadtime_ticks;
    uint16_t saved_deadtime_rise_ticks = g_deadtime_rise_ticks;
    uint16_t saved_deadtime_fall_ticks = g_deadtime_fall_ticks;

    /* First cold start comes up with DT insertion disabled, then the requested
     * dead time is applied by an immediate runtime-style reconfigure once the
     * counters are already alive. This keeps startup deterministic on hardware. */
    g_deadtime_ticks = 0U;
    g_deadtime_rise_ticks = 0U;
    g_deadtime_fall_ticks = 0U;
    if (HRTIM_Phase_ColdStart(hhrtim, period_ticks, null_ticks) != HAL_OK)
    {
        g_deadtime_ticks = saved_deadtime_ticks;
        g_deadtime_rise_ticks = saved_deadtime_rise_ticks;
        g_deadtime_fall_ticks = saved_deadtime_fall_ticks;
        Error_Handler();
    }
    g_deadtime_ticks = saved_deadtime_ticks;
    g_deadtime_rise_ticks = saved_deadtime_rise_ticks;
    g_deadtime_fall_ticks = saved_deadtime_fall_ticks;
}

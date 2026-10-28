#include "mod_priv.h"
#include "radio.h"
#include "radio_analog.h"
#include "board_rev1.h"
#include "radio_clocks.h"

#define CW_LAST_VALID_REPEAT_LIMIT 2U

static uint32_t dma_envelope[RF_DMA_SIZE_MAX];
static uint16_t dma_sample_arr[RF_FRAME_SIZE_MAX];
/* Phase/NCO updates per audio sample: RF_INTERPOLATION (192 kHz), or twice
 * that with TX PHASE 384K, which halves every image the update staircase
 * leaves at the update rate -- the modulation's and the carrier NCO's alike.
 * Latched at each stream start, so a running stream never changes it. */
static uint32_t rf_interp = RF_INTERPOLATION;
#define RF_FRAME_N (AUDIO_FRAME_SIZE * rf_interp)
#define RF_DMA_N   (DMA_BUF_SIZE * rf_interp)
static uint32_t interpolate_a, interpolate_b, interpolate_env;
static uint32_t phase_delay_a = 96U, phase_delay_b = 96U;
static uint32_t envelope_audio[AUDIO_FRAME_SIZE];
static uint32_t carrier_nco;
static int32_t carrier_nco_step;
/* The step for the grid residual alone (carrier_nco_step_now), and how many
 * steps one ppb of reference error is at this carrier. carrier_nco_step adds
 * the reference's trim against the USB host (RADIO_RefTrimPpb) to the first,
 * per DMA half: the HRTIM grid runs from the MEMS reference, so the carrier
 * wanders with it -- +-12 Hz at 14 MHz over an FT8 over -- unless the NCO
 * takes it back out, as the receiver's NCO does. */
static int32_t carrier_nco_base;
static float carrier_nco_per_ppb;
/* RM0440's set/reset spacing for the stream's geometry (mod_hrtim_step_legal_raw):
 * the branch timers' CMP1, the master's extra ticks, and the step an update
 * may make before the guard must act. Fixed while a stream runs: any carrier
 * apply stops it. */
static uint16_t step_guard_cmp1;
static int8_t step_guard_extra;
static uint16_t step_guard_room;

static uint8_t cw_last_valid_repeat_count = 0U;
static volatile uint8_t transport_dma_fault_latched = 0U;

typedef struct {
    uint8_t frame_kind;
    uint8_t rf_active;
    uint8_t tx_live;
    uint8_t rf_gate_valid;
} DmaHalfRfState;

static DmaHalfRfState dma_half_rf_state[2];

static uint8_t mod_dma_start_streams_once(void);
static void fill_null_soft(uint32_t half);

#if MOD_HRTIM_ATOMIC_EXPERIMENT
static inline void atomic_timer_f_dma_request_disable(void)
{
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_F].TIMxDIER &=
        ~HRTIM_TIMDIER_RSTDE;
    __DMB();
}

static inline uint8_t atomic_timer_f_dma_request_enable(void)
{
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_F].TIMxDIER |=
        HRTIM_TIMDIER_RSTDE;
    __DMB();
    return (uint8_t)((HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_F].TIMxDIER &
                      HRTIM_TIMDIER_RSTDE) != 0U);
}

static inline void atomic_burst_parser_reinitialize(void)
{
    /* RM0440 requires a write to at least one burst update-selection register
     * after a transaction is interrupted. Rewriting the complete master
     * selection restarts parsing at MCMP1 while preserving the two-word
     * MCMP1/MCMP2 contract. */
    HRTIM1->sCommonRegs.BDMUPR = HRTIM_BURSTDMA_CMP1 | HRTIM_BURSTDMA_CMP2;
    __DMB();
}

static uint32_t phase_lerp(uint32_t from, uint32_t to, uint32_t fraction, uint32_t period)
{
    int32_t delta = (int32_t)to - (int32_t)from;
    if (delta > (int32_t)period / 2) delta -= (int32_t)period;
    if (delta < -(int32_t)period / 2) delta += (int32_t)period;
    int32_t value = (int32_t)from - 96 + delta * (int32_t)fraction / (int32_t)rf_interp;
    while (value < 0) value += (int32_t)period;
    return 96U + (uint32_t)value % period;
}

/* One DMA half, the common case: every compare of the frame, and the two
 * carried over from the last one, is legal (96 + [0, period)). Then each
 * lerp moves at most half a period, and each wrap -- phase_lerp's while
 * loop and modulo, and the NCO's modulo -- is one conditional add or
 * subtract. `interp` is a literal at each call, so its divisions compile to
 * shifts, and the envelope has a loop of its own so that neither loop runs
 * out of registers. Bit for bit what the general loop computes; with ten
 * divisions and a spill-bound body per 192 kHz update, that loop and the
 * envelope fill were a fifth of the CPU while USB-audio SSB transmitted,
 * and the renderer, starved, fell behind (a hard render underrun ~200 ms
 * into every over). */
static inline __attribute__((always_inline)) void dma_interleave_frame_legal(uint32_t base, uint32_t out,
                                                                            uint32_t period,
                                                                            const uint32_t interp,
                                                                            const uint32_t guarded)
{
    uint32_t prev = interpolate_env;
    uint32_t *env_out = &dma_envelope[out];
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; ++i) {
        const uint32_t e = envelope_audio[i];
        const int32_t ea0 = (int32_t)(prev >> 16U), eb0 = (int32_t)(prev & 4095U);
        const int32_t da = (int32_t)(e >> 16U) - ea0, db = (int32_t)(e & 4095U) - eb0;
        for (uint32_t j = 1U; j <= interp; ++j) {
            const uint32_t ea = (uint32_t)(ea0 + da * (int32_t)j / (int32_t)interp);
            const uint32_t eb = (uint32_t)(eb0 + db * (int32_t)j / (int32_t)interp);
            *env_out++ = (ea << 16U) | eb;
        }
        prev = e;
    }
    interpolate_env = prev;

    const int32_t p = (int32_t)period;
    uint32_t nco = carrier_nco;
    const uint32_t nco_step = (uint32_t)carrier_nco_step;
    uint32_t delay_a = phase_delay_a, delay_b = phase_delay_b;
    uint32_t from_a = interpolate_a, from_b = interpolate_b;
    uint16_t *pair = &dma_mcmp_pair[2U * out];
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; ++i) {
        const uint32_t a = dma_mcmp1[base + i], b = dma_mcmp2[base + i];
        int32_t delta_a = (int32_t)a - (int32_t)from_a;
        if (delta_a > p / 2) delta_a -= p;
        if (delta_a < -p / 2) delta_a += p;
        int32_t delta_b = (int32_t)b - (int32_t)from_b;
        if (delta_b > p / 2) delta_b -= p;
        if (delta_b < -p / 2) delta_b += p;
        const int32_t raw_a = (int32_t)from_a - 96, raw_b = (int32_t)from_b - 96;
        for (uint32_t j = 1U; j <= interp; ++j) {
            /* DAC transfers DHR to DOR before requesting its next DMA word.
             * Delay the phase command by that same one sample, preserving
             * phase/envelope sequence across circular half boundaries. */
            pair[0] = (uint16_t)delay_a;
            pair[1] = (uint16_t)delay_b;
            pair += 2;
            /* Increasing reset delay lowers RF frequency, hence the negative
             * sign in the NCO step. Common phase preserves branch separation. */
            nco += nco_step;
            const int32_t shift = (int32_t)(((uint64_t)nco * period) >> 32U);
            int32_t va = raw_a + delta_a * (int32_t)j / (int32_t)interp;
            int32_t vb = raw_b + delta_b * (int32_t)j / (int32_t)interp;
            if (va < 0) va += p; else if (va >= p) va -= p;
            if (vb < 0) vb += p; else if (vb >= p) vb -= p;
            va += shift;
            vb += shift;
            if (va >= p) va -= p;
            if (vb >= p) vb -= p;
            if (guarded) {
                va = (int32_t)mod_hrtim_step_legal_raw(delay_a - 96U, (uint32_t)va, period, step_guard_cmp1,
                                                       MOD_HRTIM_STEP_GUARD_TICKS, step_guard_extra);
                vb = (int32_t)mod_hrtim_step_legal_raw(delay_b - 96U, (uint32_t)vb, period, step_guard_cmp1,
                                                       MOD_HRTIM_STEP_GUARD_TICKS, step_guard_extra);
            }
            delay_a = 96U + (uint32_t)va;
            delay_b = 96U + (uint32_t)vb;
        }
        from_a = a; from_b = b;
    }
    interpolate_a = from_a; interpolate_b = from_b;
    carrier_nco = nco;
    phase_delay_a = delay_a;
    phase_delay_b = delay_b;
}

/* The fast loop's four instantiations, each out of line: inlined together
 * they made dma_interleave_half twice the build's complexity budget. One
 * call per half buffer. */
static __attribute__((noinline)) void dma_interleave_plain_4(uint32_t base, uint32_t out, uint32_t period)
{
    dma_interleave_frame_legal(base, out, period, RF_INTERPOLATION, 0U);
}

static __attribute__((noinline)) void dma_interleave_plain_8(uint32_t base, uint32_t out, uint32_t period)
{
    dma_interleave_frame_legal(base, out, period, RF_INTERPOLATION_MAX, 0U);
}

static __attribute__((noinline)) void dma_interleave_guarded_4(uint32_t base, uint32_t out, uint32_t period)
{
    dma_interleave_frame_legal(base, out, period, RF_INTERPOLATION, 1U);
}

static __attribute__((noinline)) void dma_interleave_guarded_8(uint32_t base, uint32_t out, uint32_t period)
{
    dma_interleave_frame_legal(base, out, period, RF_INTERPOLATION_MAX, 1U);
}

static uint8_t dma_frame_compares_legal(uint32_t base, uint32_t period)
{
    if ((interpolate_a - 96U >= period) || (interpolate_b - 96U >= period)) return 0U;
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; ++i) {
        if ((dma_mcmp1[base + i] - 96U >= period) || (dma_mcmp2[base + i] - 96U >= period)) return 0U;
    }
    return 1U;
}

static void dma_interleave_half(uint32_t half, uint8_t active)
{
    {
        /* A fast reference makes the grid high: the step that lowers the
         * carrier grows with it. */
        const float t = carrier_nco_per_ppb * (float)RADIO_RefTrimPpb();
        carrier_nco_step = carrier_nco_base + (int32_t)(t >= 0.0f ? t + 0.5f : t - 0.5f);
    }
    uint32_t base = half * AUDIO_FRAME_SIZE;
    uint32_t out = half * RF_FRAME_N;
    const uint32_t interp = rf_interp;
    uint32_t period = active_carrier->period_ticks;
    if (!period) period = 192U;
    RADIO_EnvelopeFill(&dma_mcmp1[base], &dma_mcmp2[base], envelope_audio,
                        AUDIO_FRAME_SIZE, period, active);
    if (interpolate_a < 96U || interpolate_a >= period + 96U) interpolate_a = dma_mcmp1[base];
    if (interpolate_b < 96U || interpolate_b >= period + 96U) interpolate_b = dma_mcmp2[base];
    /* The guard costs cycles in the hottest loop, so only where one update
     * can reach its window: a lerp moves at most half a period per sample,
     * spread over `interp` updates, and the NCO by its step, each a tick more
     * for rounding. With the carrier fraction the NCO step is a few ticks at
     * most, and the guard is needed mostly on 10 m (P = 192: 32 ticks of
     * room) and with large PA duty offsets. */
    const uint32_t nco_mag = (uint32_t)((carrier_nco_step < 0) ? -carrier_nco_step : carrier_nco_step);
    const uint32_t reach = period / (2U * interp) + (uint32_t)(((uint64_t)nco_mag * period) >> 32U) + 2U;
    const uint8_t guarded = (uint8_t)(reach > step_guard_room);
    const uint8_t legal = dma_frame_compares_legal(base, period);
    if (legal && ((interp == RF_INTERPOLATION) || (interp == RF_INTERPOLATION_MAX))) {
        if (interp == RF_INTERPOLATION) {
            if (guarded) dma_interleave_guarded_4(base, out, period);
            else dma_interleave_plain_4(base, out, period);
        } else {
            if (guarded) dma_interleave_guarded_8(base, out, period);
            else dma_interleave_plain_8(base, out, period);
        }
        __DMB();
        return;
    }
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; ++i) {
        uint32_t a = dma_mcmp1[base + i], b = dma_mcmp2[base + i], e = envelope_audio[i];
        int32_t da = (int32_t)(e >> 16U) - (int32_t)(interpolate_env >> 16U);
        int32_t db = (int32_t)(e & 4095U) - (int32_t)(interpolate_env & 4095U);
        for (uint32_t j = 1U; j <= interp; ++j, ++out) {
            /* DAC transfers DHR to DOR before requesting its next DMA word.
             * Delay the phase command by that same one sample, preserving
             * phase/envelope sequence across circular half boundaries. */
            dma_mcmp_pair[2U * out] = (uint16_t)phase_delay_a;
            dma_mcmp_pair[2U * out + 1U] = (uint16_t)phase_delay_b;
            const uint32_t prev_a = phase_delay_a - 96U, prev_b = phase_delay_b - 96U;
            phase_delay_a = phase_lerp(interpolate_a, a, j, period);
            phase_delay_b = phase_lerp(interpolate_b, b, j, period);
            /* Increasing reset delay lowers RF frequency, hence the negative
             * sign in the NCO step. Common phase preserves branch separation. */
            carrier_nco += (uint32_t)carrier_nco_step;
            uint32_t shift = (uint32_t)(((uint64_t)carrier_nco * period) >> 32U);
            phase_delay_a = 96U + (phase_delay_a - 96U + shift) % period;
            phase_delay_b = 96U + (phase_delay_b - 96U + shift) % period;
            if (guarded && (prev_a < period)) {
                phase_delay_a = 96U + mod_hrtim_step_legal_raw(prev_a, phase_delay_a - 96U, period, step_guard_cmp1,
                                                               MOD_HRTIM_STEP_GUARD_TICKS, step_guard_extra);
            }
            if (guarded && (prev_b < period)) {
                phase_delay_b = 96U + mod_hrtim_step_legal_raw(prev_b, phase_delay_b - 96U, period, step_guard_cmp1,
                                                               MOD_HRTIM_STEP_GUARD_TICKS, step_guard_extra);
            }
            uint32_t ea = (uint32_t)((int32_t)(interpolate_env >> 16U) + da * (int32_t)j / (int32_t)interp);
            uint32_t eb = (uint32_t)((int32_t)(interpolate_env & 4095U) + db * (int32_t)j / (int32_t)interp);
            dma_envelope[out] = (ea << 16U) | eb;
        }
        interpolate_a = a; interpolate_b = b; interpolate_env = e;
    }
    __DMB();
}

#else
#define dma_interleave_half(half_) ((void)(half_))
#endif

static void dma_clear_specific_flags(void)
{
    DMA1->IFCR = DMA_IFCR_CHTIF1 | DMA_IFCR_CTCIF1 | DMA_IFCR_CTEIF1 |
                 DMA_IFCR_CHTIF2 | DMA_IFCR_CTCIF2 | DMA_IFCR_CTEIF2;
}

static void dma_half_store_rf_state(uint32_t half,
                                    uint8_t frame_kind,
                                    uint8_t rf_active,
                                    uint8_t tx_live,
                                    uint8_t rf_gate_valid)
{
    if (half >= 2U) return;
    dma_half_rf_state[half].frame_kind = frame_kind;
    dma_half_rf_state[half].rf_active = rf_active;
    dma_half_rf_state[half].tx_live = tx_live;
    __DMB();
    dma_half_rf_state[half].rf_gate_valid = rf_gate_valid;
}

static void dma_half_store_idle(uint32_t half)
{
    dma_half_store_rf_state(half, RENDER_FRAME_KIND_MODULATED, 0U, 0U, 1U);
}

static void dma_half_store_frame_rf_state(uint32_t half, const RenderFrame *frame)
{
    if ((frame == NULL) || (frame->rf_gate_valid == 0U)) {
        dma_half_store_idle(half);
        return;
    }
    dma_half_store_rf_state(half,
                            frame->frame_kind,
                            frame->rf_active,
                            frame->tx_live,
                            frame->rf_gate_valid);
}

static void dma_half_store_all_idle(void)
{
    dma_half_store_idle(0U);
    dma_half_store_idle(1U);
}

/* The stream stops with raw register writes (no HAL timeouts, safe from an
 * interrupt), which leave HAL's bookkeeping where the last start put it:
 * htim6 BUSY and the DMA handle locked. HAL_TIM_Base_Start refuses a timer
 * that is not READY and HAL_DMA_Start a locked handle, so every stream start
 * after the first one of a boot failed with DMA_START_FAILED and the hard
 * pipeline lockout -- a TX TEST or CW key-down after any earlier one. Hand
 * both back the way HAL_TIM_Base_Stop and HAL_DMA_Abort did before they were
 * replaced. */
static void dma_hal_handles_idle(void)
{
    htim6.State = HAL_TIM_STATE_READY;
    __HAL_UNLOCK(&hdma_tim6_up);
    hdma_tim6_up.State = HAL_DMA_STATE_READY;
    hdma_tim6_up.ErrorCode = HAL_DMA_ERROR_NONE;
    __HAL_UNLOCK(&hdma_mcmp2);
    hdma_mcmp2.State = HAL_DMA_STATE_READY;
    hdma_mcmp2.ErrorCode = HAL_DMA_ERROR_NONE;
}

void mod_pipeline_emergency_stop(void)
{
    uint32_t ta, tc;

    BOARD_EmergencyStop();
    TIM6->DIER &= ~TIM_DIER_UDE;
    TIM6->CR1 &= ~TIM_CR1_CEN;
    /* RM0440: the DMA channel is disabled before the HRTIM's DMA request. */
    DMA1_Channel1->CCR &= ~DMA_CCR_EN;
    DMA1_Channel2->CCR &= ~DMA_CCR_EN;
    DMA1_Channel4->CCR &= ~DMA_CCR_EN;
    for (uint32_t i = 0U; i < 16U; i++) {
        if (((DMA1_Channel1->CCR | DMA1_Channel2->CCR) & DMA_CCR_EN) == 0U) break;
    }
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    atomic_timer_f_dma_request_disable();
#endif
    dma_clear_specific_flags();
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    atomic_burst_parser_reinitialize();
#endif

    dma_streams_running = 0U;
    dma_expected_half = 0U;
    dma_expected_half_valid = 0U;
    transport_dma_fault_latched = 1U;
    transport_dma_park_request = 1U;
    dma_half_store_all_idle();
    dma_hal_handles_idle();

    build_idle_branch_pair(&ta, &tc);
    HRTIM1->sMasterRegs.MCMP1R = ta;
    HRTIM1->sMasterRegs.MCMP2R = tc;
    HRTIM1->sCommonRegs.CR2 |= HRTIM_CR2_MSWU;
    dma_static_idle_parked = 1U;

    uint32_t pm = enter_critical();
    pcm_wr_idx = 0U;
    pcm_rd_idx = 0U;
    pcm_count = 0U;
    pcm_push_in_progress = 0U;
    pcm_pop_in_progress = 0U;
    pcm_reset_seq++;
    render_wr_idx = 0U;
    render_rd_idx = 0U;
    render_count = 0U;
    render_push_in_progress = 0U;
    render_pop_in_progress = 0U;
    render_reset_seq++;
    render_next_seq = 0U;
    render_dma_expected_seq = 0U;
    render_dma_seq_valid = 0U;
    transport_started = 0U;
    last_dma_valid = 0U;
    exit_critical(pm);
}

static void fill_fm_hold(uint32_t half)
{
    uint32_t *p1 = &dma_mcmp1[half * AUDIO_FRAME_SIZE];
    uint32_t *p2 = &dma_mcmp2[half * AUDIO_FRAME_SIZE];
    uint32_t ta, tc;
    uint32_t pm = enter_critical();
    uint8_t valid = fm_hold_valid;
    ta = fm_hold_mcmp1;
    tc = fm_hold_mcmp2;
    exit_critical(pm);
    if (valid == 0U) {
        fill_null_soft(half);
        return;
    }
    for (uint32_t i = 0; i < AUDIO_FRAME_SIZE; i++) { p1[i] = ta; p2[i] = tc; }
    dma_interleave_half(half, last_dma_valid && last_dma_frame.rf_active);
    if (last_dma_valid != 0U) {
        dma_half_store_frame_rf_state(half, &last_dma_frame);
    } else {
        dma_half_store_idle(half);
    }
}

static void apply_frame_rf_state(uint8_t frame_kind, uint8_t rf_active, uint8_t tx_live, uint8_t rf_gate_valid)
{
    if (rf_gate_valid == 0U) {
        return;
    }

    if (frame_kind == RENDER_FRAME_KIND_MODULATED) {
        cw_tx_live = 0U;
        cw_rf_enabled = 0U;
        rf_en_gpio_set(rf_active != 0U);
        return;
    }

    if ((frame_kind != RENDER_FRAME_KIND_CW) &&
        (frame_kind != RENDER_FRAME_KIND_TX_TEST)) {
        return;
    }

    cw_tx_live = tx_live;
    if ((frame_kind == RENDER_FRAME_KIND_CW) && (cw_sidetone_only() || (cfg_muted != 0U))) {
        cw_rf_enabled = 0U;
        rf_en_force_idle_preserve_cw();
        return;
    }

    cw_rf_enabled = rf_active;

    if (rf_active != 0U) {
        g_last_cw_tick = HAL_GetTick();
        rf_en_gpio_set(1U);
    } else {
        rf_en_force_idle_preserve_cw();
        /* Between elements, and through the SEMI tail, the keyer still holds
         * the session live: keep the radio in TRANSMIT with the drivers off.
         * Without this the radio went back to receive 80 ms after every
         * element, and the next one waited ~55 ms for the PIN bias to settle
         * again (TX_PREPARE) -- below 15 WPM, where a one-dit gap exceeds
         * 80 ms, that stretched the gaps inside every character. */
        if (tx_live != 0U) RADIO_RequestTx();
    }
}

static void dma_half_apply_playing(uint32_t half)
{
    if (half >= 2U) return;

    apply_frame_rf_state(dma_half_rf_state[half].frame_kind,
                         dma_half_rf_state[half].rf_active,
                         dma_half_rf_state[half].tx_live,
                         dma_half_rf_state[half].rf_gate_valid);
}

static void fill_last_valid(uint32_t half)
{
    uint32_t *p1 = &dma_mcmp1[half * AUDIO_FRAME_SIZE];
    uint32_t *p2 = &dma_mcmp2[half * AUDIO_FRAME_SIZE];
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; i++) {
        p1[i] = (uint32_t)last_dma_frame.mcmp1[i];
        p2[i] = (uint32_t)last_dma_frame.mcmp2[i];
    }
    dma_interleave_half(half, last_dma_frame.rf_active);
    if ((last_dma_frame.frame_kind == RENDER_FRAME_KIND_CW) &&
        (last_dma_frame.rf_active != 0U) &&
        (cw_last_valid_repeat_count < 255U)) {
        cw_last_valid_repeat_count++;
    }
    dma_half_store_frame_rf_state(half, &last_dma_frame);
}

static void fill_null_soft(uint32_t half)
{
    uint32_t *p1 = &dma_mcmp1[half * AUDIO_FRAME_SIZE];
    uint32_t *p2 = &dma_mcmp2[half * AUDIO_FRAME_SIZE];
    uint32_t ta, tc;
    build_idle_branch_pair(&ta, &tc);
    for (uint32_t i = 0; i < AUDIO_FRAME_SIZE; i++) { p1[i] = ta; p2[i] = tc; }
    dma_interleave_half(half, 0U);
    dma_half_store_idle(half);
}

static void external_transport_resync(void)
{
    uint32_t pm = enter_critical();
    render_wr_idx = 0U;
    render_rd_idx = 0U;
    render_count = 0U;
    render_push_in_progress = 0U;
    render_pop_in_progress = 0U;
    render_reset_seq++;
    render_next_seq = 0U;
    render_dma_expected_seq = 0U;
    render_dma_seq_valid = 0U;
    transport_started = 0U;
    last_dma_valid = 0U;
    dma_half_store_all_idle();
    exit_critical(pm);

    tim6_servo_frame_counter = 0U;
    tim6_servo_error_avg = 0;
    tim6_servo_bias = TRANSPORT_LOCKED_BIAS;
    transport_underrun_streak = 0U;
    transport_dma_park_request = 1U;
    transport_resyncs++;
}

void MOD_UsbStreamReset(void)
{
    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) return;
    mod_pipeline_note_controlled_reset();
    uint32_t pm = enter_critical();
    pcm_wr_idx = 0U; pcm_rd_idx = 0U; pcm_count = 0U;
    pcm_push_in_progress = 0U; pcm_pop_in_progress = 0U; pcm_reset_seq++;
    render_wr_idx = 0U; render_rd_idx = 0U; render_count = 0U;
    render_push_in_progress = 0U; render_pop_in_progress = 0U; render_reset_seq++;
    render_next_seq = 0U;
    render_dma_expected_seq = 0U;
    render_dma_seq_valid = 0U;
    transport_started = 0U;
    audio_seen = 0U;
    usb_audio_seen = 0U;
    audio_critical_reason = 0U;
    usb_stream_active = 0U;
    usb_alt_active = 0U;
    usb_missing_frame_streak = 0U;
    usb_frame_tracking_armed = 0U;
    usb_expected_frame_number = 0U;
    usb_rearm_good_frames = 0U;
    fm_hold_valid = 0U;
    g_last_usb_tick = 0U;
    g_last_usb_audio_tick = 0U;
    usb_feedback_q10_14 = USB_FEEDBACK_NOMINAL_Q10_14;
    dma_half_store_all_idle();
    exit_critical(pm);

    usb_feedback_integrator = 0;
    last_dma_valid = 0U;
    transport_underrun_streak = 0U;
    tim6_servo_bias = TRANSPORT_LOCKED_BIAS;
    tim6_servo_error_avg = 0;
    tim6_servo_frame_counter = 0U;
    transport_dma_park_request = 1U;
    rf_en_force_idle();
}

void MOD_RadioResetTransport(void)
{
    /* Foreground only. Inhibit physical RF before cancelling its DMA buffers. */
    BOARD_PermitRf(0U);
    mod_dma_stop_streams();
    MOD_LocalAudioRelease();
    last_dma_valid = 0U;
    dma_static_idle_parked = 0U;
}

static void fill_null(uint32_t half)
{
    uint32_t *p1 = &dma_mcmp1[half * AUDIO_FRAME_SIZE];
    uint32_t *p2 = &dma_mcmp2[half * AUDIO_FRAME_SIZE];
    uint32_t ta, tc;
    build_idle_branch_pair(&ta, &tc);
    for (uint32_t i = 0; i < AUDIO_FRAME_SIZE; i++) { p1[i] = ta; p2[i] = tc; }
    dma_interleave_half(half, 0U);
    dma_half_store_idle(half);
    rf_en_force_idle();
}

void mod_dma_park_static_idle(void)
{
    if (dma_streams_running != 0U) return;

    uint32_t ta, tc;
    build_idle_branch_pair(&ta, &tc);
    HRTIM1->sMasterRegs.MCMP1R = ta;
    HRTIM1->sMasterRegs.MCMP2R = tc;
    __DMB();
    HRTIM1->sCommonRegs.CR2 |= HRTIM_CR2_MSWU;
    __DMB();
    dma_static_idle_parked = 1U;
    transport_dma_park_request = 0U;
}

__attribute__((cold)) void MOD_FillAllNullBuffers(void)
{
    fill_null(0U); fill_null(1U);
    uint32_t pm = enter_critical();
    render_wr_idx = 0U; render_rd_idx = 0U; render_count = 0U;
    render_push_in_progress = 0U; render_pop_in_progress = 0U; render_reset_seq++;
    render_next_seq = 0U;
    render_dma_expected_seq = 0U;
    render_dma_seq_valid = 0U;
    fm_hold_valid = 0U;
    dma_half_store_all_idle();
    exit_critical(pm);
    if (dma_streams_running == 0U) mod_dma_park_static_idle();
}

HOT_INLINE void pcm_ring_write(uint32_t wr, const int16_t *src, uint32_t n)
{
    uint32_t first = PCM_RING_SAMPLES - wr;
    if (first >= n) {
        memcpy(&pcm_ring[wr], src, n * sizeof(int16_t));
    } else {
        memcpy(&pcm_ring[wr], src, first * sizeof(int16_t));
        memcpy(&pcm_ring[0],  src + first, (n - first) * sizeof(int16_t));
    }
}

HOT_INLINE void pcm_ring_read(int16_t *dst, uint32_t rd, uint32_t n)
{
    uint32_t first = PCM_RING_SAMPLES - rd;
    if (first >= n) {
        memcpy(dst, &pcm_ring[rd], n * sizeof(int16_t));
    } else {
        memcpy(dst,         &pcm_ring[rd], first * sizeof(int16_t));
        memcpy(dst + first, &pcm_ring[0],  (n - first) * sizeof(int16_t));
    }
}

/* PCM ring concurrency contract:
 * - pcm_push() serializes the single USB/audio-ingress producer. An unexpected
 *   concurrent producer is dropped instead of sharing a reserved write span.
 * - pcm_pop_frame() is the single render/background consumer.
 * - Large sample copies intentionally run with interrupts enabled; only index
 *   reservation/publication is protected by PRIMASK.
 * - pcm_reset_seq invalidates in-flight copies during stream/DSP reset. Reset
 *   may clear an active guard because the present single-core priority layout
 *   cannot start another same-role copy until the preempted copy resumes; its
 *   generation check then discards the stale reservation.
 * - Changing IRQ priorities, adding a producer/consumer, or resetting from a
 *   second execution context requires a generation-owned reservation design;
 *   simply clearing the guards would no longer be safe.
 */
void pcm_push(const int16_t *m, uint32_t n)
{
    if (!m || !n) return;
    uint32_t wr = 0U;
    uint32_t seq = 0U;
    uint8_t copy = 0U;
    uint8_t note_pre_overflow = 0U;
    uint32_t note_pre_value = 0U;
    uint8_t note_post_overflow = 0U;
    uint32_t note_post_value = 0U;

    uint32_t pm = enter_critical();
    if (pcm_push_in_progress != 0U) {
        pcm_drop_samples += n;
        note_pre_overflow = 1U;
        note_pre_value = n;
        exit_critical(pm);
        mod_pipeline_note(MOD_PIPELINE_STAGE_PCM,
                          MOD_PIPELINE_REASON_PCM_OVERFLOW,
                          note_pre_value);
        return;
    }
    pcm_push_in_progress = 1U;

    if (n > PCM_RING_SAMPLES) {
        m += (n - PCM_RING_SAMPLES);
        pcm_drop_samples += (n - PCM_RING_SAMPLES);
        note_pre_overflow = 1U;
        note_pre_value += n - PCM_RING_SAMPLES;
        n = PCM_RING_SAMPLES;
    }

    if ((pcm_pop_in_progress != 0U) && (pcm_count + n > PCM_RING_SAMPLES)) {
        uint32_t free = (pcm_count < PCM_RING_SAMPLES) ? (PCM_RING_SAMPLES - pcm_count) : 0U;
        uint32_t drop_new = n - free;
        m += drop_new;
        n = free;
        pcm_drop_samples += drop_new;
        pcm_drop_during_pop_samples += drop_new;
        note_pre_overflow = 1U;
        note_pre_value += drop_new;
    } else if (pcm_count + n > PCM_RING_SAMPLES) {
        uint32_t drop = pcm_count + n - PCM_RING_SAMPLES;
        pcm_rd_idx = (pcm_rd_idx + drop) % PCM_RING_SAMPLES;
        pcm_count -= drop;
        pcm_drop_samples += drop;
        note_pre_overflow = 1U;
        note_pre_value += drop;
    }

    if (n > 0U) {
        wr = pcm_wr_idx;
        seq = pcm_reset_seq;
        copy = 1U;
    }

    exit_critical(pm);

    if (copy == 0U) {
        pm = enter_critical();
        pcm_push_in_progress = 0U;
        exit_critical(pm);
    }

    if (note_pre_overflow != 0U) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_PCM,
                          MOD_PIPELINE_REASON_PCM_OVERFLOW,
                          note_pre_value);
    }

    if (copy == 0U) return;

    pcm_ring_write(wr, m, n);
    __DMB();

    pm = enter_critical();
    if (seq == pcm_reset_seq) {
        uint32_t new_count = pcm_count + n;
        if (new_count > PCM_RING_SAMPLES) {
            uint32_t drop = new_count - PCM_RING_SAMPLES;
            pcm_rd_idx = (pcm_rd_idx + drop) % PCM_RING_SAMPLES;
            pcm_drop_samples += drop;
            if (pcm_pop_in_progress != 0U) pcm_drop_during_pop_samples += drop;
            note_post_overflow = 1U;
            note_post_value = drop;
            new_count = PCM_RING_SAMPLES;
        }
        pcm_wr_idx = (wr + n) % PCM_RING_SAMPLES;
        pcm_count = new_count;
        if (pcm_count > pcm_peak) pcm_peak = pcm_count;
    }
    pcm_push_in_progress = 0U;
    exit_critical(pm);

    if (note_post_overflow != 0U) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_PCM,
                          MOD_PIPELINE_REASON_PCM_OVERFLOW,
                          note_post_value);
    }
}

uint8_t pcm_pop_frame(int16_t *m)
{
    if (!m) return 0U;
    uint32_t rd;
    uint32_t seq;

    uint32_t pm = enter_critical();
    if ((pcm_count < AUDIO_FRAME_SIZE) || (pcm_pop_in_progress != 0U)) {
        exit_critical(pm);
        return 0U;
    }

    rd = pcm_rd_idx;
    seq = pcm_reset_seq;
    pcm_pop_in_progress = 1U;
    exit_critical(pm);

    pcm_ring_read(m, rd, AUDIO_FRAME_SIZE);
    __DMB();

    pm = enter_critical();
    uint8_t ok = 0U;
    if ((pcm_pop_in_progress != 0U) &&
        (seq == pcm_reset_seq) &&
        (pcm_count >= AUDIO_FRAME_SIZE)) {
        pcm_rd_idx = (rd + AUDIO_FRAME_SIZE) % PCM_RING_SAMPLES;
        pcm_count -= AUDIO_FRAME_SIZE;
        ok = 1U;
    }
    pcm_pop_in_progress = 0U;
    exit_critical(pm);
    if (ok == 0U) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_PCM,
                          MOD_PIPELINE_REASON_PCM_RESET_DISCARD,
                          AUDIO_FRAME_SIZE);
    }
    return ok;
}

RenderFrame *rfifo_begin(void)
{
    RenderFrame *slot = NULL;
    uint8_t note_overflow = 0U;
    uint32_t note_value = 0U;
    uint32_t pm = enter_critical();
    if ((render_count >= RENDER_QUEUE_FRAMES) || (render_push_in_progress != 0U)) {
        render_overflows++;
        note_overflow = 1U;
        note_value = render_count;
    } else {
        slot = &render_fifo[render_wr_idx];
        render_push_seq = render_reset_seq;
        render_push_in_progress = 1U;
    }
    exit_critical(pm);
    if (note_overflow != 0U) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_RENDER,
                          MOD_PIPELINE_REASON_RENDER_OVERFLOW,
                          note_value);
    }
    return slot;
}

void rfifo_commit(void)
{
    uint8_t note_discard = 0U;
    uint32_t note_value = 0U;
    uint32_t pm = enter_critical();
    if ((render_push_in_progress != 0U) &&
        (render_push_seq == render_reset_seq) &&
        (render_count < RENDER_QUEUE_FRAMES)) {
        render_fifo[render_wr_idx].frame_seq = render_next_seq++;
        render_wr_idx = (render_wr_idx + 1U) % RENDER_QUEUE_FRAMES;
        render_count++;
        if (render_count > render_peak) render_peak = render_count;
        if (render_count < render_low_water) render_low_water = render_count;
    } else if (render_push_in_progress != 0U) {
        note_discard = 1U;
        note_value = render_count;
    }
    render_push_in_progress = 0U;
    exit_critical(pm);
    if (note_discard != 0U) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_RENDER,
                          MOD_PIPELINE_REASON_RENDER_RESET_DISCARD,
                          note_value);
    }
}

void rfifo_cancel(void)
{
    uint32_t pm = enter_critical();
    render_push_in_progress = 0U;
    exit_critical(pm);
}

#if MOD_HRTIM_ATOMIC_EXPERIMENT
/* CNDTR is read while the DMA runs, and each update is a two-word burst:
 * MCMP1, then MCMP2 one DMA arbitration later. A read landing between the
 * two sees an odd count only for that moment, and latching it as a torn pair
 * locked out healthy streams within a fraction of a second of a key-down.
 * Re-read until the pair completes; a count still odd after the bounded wait
 * is a burst that really stalled, and the validators latch it as before.
 * dma_cndtr_torn_reads counts the reads that needed the wait. */
#define DMA_PAIR_SETTLE_READS 32U
static volatile uint32_t dma_cndtr_torn_reads;

/* Out of line and out of CCM: it runs for about one read in four hundred. */
static __attribute__((noinline, cold)) uint32_t dma_cndtr_settle(uint32_t words)
{
    dma_cndtr_torn_reads++;
    for (uint32_t i = 0U; ((words & 1U) != 0U) && (i < DMA_PAIR_SETTLE_READS); ++i) {
        words = DMA1_Channel1->CNDTR;
    }
    return words;
}

HOT_INLINE uint32_t dma_cndtr_paired(void)
{
    const uint32_t words = DMA1_Channel1->CNDTR;
    return ((words & 1U) == 0U) ? words : dma_cndtr_settle(words);
}
#endif

static HOT_CODE void dma_refill_fault(mod_pipeline_reason_t reason, uint32_t value)
{
    transport_dma_faults = (transport_dma_faults == 0xFFFFFFFFUL)
                         ? transport_dma_faults : transport_dma_faults + 1U;
    transport_dma_fault_latched = 1U;
    mod_pipeline_note_severity(MOD_PIPELINE_STAGE_DMA, reason,
                               MOD_PIPELINE_SEVERITY_HARD, value);
}

HOT_CODE void mod_dma_irq_channel1(void)
{
    const uint32_t flags = DMA1->ISR &
        (DMA_ISR_HTIF1 | DMA_ISR_TCIF1 | DMA_ISR_TEIF1);
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    uint32_t cndtr1 = dma_cndtr_paired();
    uint32_t cndtr2 = cndtr1;
#else
    uint32_t cndtr1 = DMA1_Channel1->CNDTR;
    uint32_t cndtr2 = DMA1_Channel2->CNDTR;
#endif

    if ((flags & DMA_ISR_TEIF1) != 0U) {
        DMA1->IFCR = DMA_IFCR_CTEIF1;
        dma_ch1_error_count++;
        dma_refill_fault(MOD_PIPELINE_REASON_DMA_ERROR, flags);
        return;
    }
    if (dma_streams_running == 0U) {
        if ((flags & DMA_ISR_HTIF1) != 0U) DMA1->IFCR = DMA_IFCR_CHTIF1;
        if ((flags & DMA_ISR_TCIF1) != 0U) DMA1->IFCR = DMA_IFCR_CTCIF1;
        return;
    }
    uint8_t events = 0U;
    if ((flags & DMA_ISR_HTIF1) != 0U) events |= MOD_DMA_EVENT_HT;
    if ((flags & DMA_ISR_TCIF1) != 0U) events |= MOD_DMA_EVENT_TC;
    mod_dma_boundary_result_t boundary =
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        mod_dma_validate_paired_boundary(
        events, cndtr1, RF_FRAME_N, RF_DMA_N,
        dma_expected_half_valid, dma_expected_half);
#else
        mod_dma_validate_boundary(
        events, cndtr1, cndtr2, AUDIO_FRAME_SIZE, DMA_BUF_SIZE,
        dma_expected_half_valid, dma_expected_half);
#endif
    if (boundary.result == MOD_DMA_CHECK_NO_EVENT) return;
    if (boundary.result != MOD_DMA_CHECK_OK) {
        mod_pipeline_reason_t reason = MOD_PIPELINE_REASON_DMA_CNDTR_ANOMALY;
        uint32_t value = ((cndtr1 & 0xFFFFUL) << 16) | (cndtr2 & 0xFFFFUL);
        if (boundary.result == MOD_DMA_CHECK_FLAGS_COALESCED) {
            DMA1->IFCR = DMA_IFCR_CHTIF1 | DMA_IFCR_CTCIF1;
            reason = MOD_PIPELINE_REASON_DMA_FLAGS_COALESCED;
            value = flags;
        } else if (boundary.result == MOD_DMA_CHECK_CHANNEL_SKEW) {
            reason = MOD_PIPELINE_REASON_DMA_CHANNEL_SKEW;
        } else if (boundary.result == MOD_DMA_CHECK_SEQUENCE_GAP) {
            DMA1->IFCR = (boundary.half == 0U) ? DMA_IFCR_CHTIF1 : DMA_IFCR_CTCIF1;
            reason = MOD_PIPELINE_REASON_DMA_SEQUENCE_GAP;
            value = ((uint32_t)dma_expected_half << 16) | flags;
        } else if (boundary.result == MOD_DMA_CHECK_REFILL_LATE) {
            DMA1->IFCR = (boundary.half == 0U) ? DMA_IFCR_CHTIF1 : DMA_IFCR_CTCIF1;
            reason = MOD_PIPELINE_REASON_DMA_REFILL_LATE;
            value = cndtr1;
        }
        dma_refill_fault(reason, value);
        return;
    }

    uint8_t half = boundary.half;
    if ((DMA1->ISR & DMA_ISR_TEIF4) ||
        (!(DMA1_Channel2->CCR & DMA_CCR_EN) || !RADIO_EnvelopeBoundary(half, RF_DMA_N))) {
        /* A protection trip (SWR, VIN, BIAS ... RADIO_Trip) stops the envelope
         * DMA with the rest of the RF path under the emergency latch and logs
         * its own cause; the stream only has to stop with it. Reported here as
         * a DMA SKEW, every SWR trip at a key-down read RF PIPELINE FAULT, its
         * cause only on the status line (bench 2026-10-28, 12 m at SWR 3.3). */
        if (!(DMA1->ISR & DMA_ISR_TEIF4) && BOARD_EmergencyLatched()) {
            mod_pipeline_emergency_stop();
            return;
        }
        RADIO_Trip(RADIO_FAULT_DMA);
        dma_refill_fault(MOD_PIPELINE_REASON_DMA_CHANNEL_SKEW, DMA1_Channel2->CNDTR);
        return;
    }
    if (half == 0U) {
        DMA1->IFCR = DMA_IFCR_CHTIF1;
        dma_ht_count++;
        dma_write_half = 0U;
        dma_half_apply_playing(1U);
    } else {
        DMA1->IFCR = DMA_IFCR_CTCIF1;
        dma_tc_count++;
        dma_write_half = 1U;
        dma_half_apply_playing(0U);
    }
    dma_expected_half = boundary.next_expected_half;
    dma_expected_half_valid = 1U;

    rfifo_pop_to_dma(half);
    if (pipeline_hard_lockout != 0U) return;
    /* TIM6 ARR is now sample-DMA scheduled at 192 kHz. */

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    cndtr1 = dma_cndtr_paired();
    cndtr2 = cndtr1;
    mod_dma_check_t refill = mod_dma_validate_paired_refill(
        half, cndtr1, RF_FRAME_N, RF_DMA_N,
        DMA_REFILL_MIN_SLACK_SAMPLES * rf_interp);
#else
    cndtr1 = DMA1_Channel1->CNDTR;
    cndtr2 = DMA1_Channel2->CNDTR;
    mod_dma_check_t refill = mod_dma_validate_refill(
        half, cndtr1, cndtr2, AUDIO_FRAME_SIZE, DMA_BUF_SIZE,
        DMA_REFILL_MIN_SLACK_SAMPLES);
#endif
    if (refill == MOD_DMA_CHECK_CHANNEL_SKEW) {
        dma_refill_fault(MOD_PIPELINE_REASON_DMA_CHANNEL_SKEW,
                         ((cndtr1 & 0xFFFFUL) << 16) | (cndtr2 & 0xFFFFUL));
        return;
    }
    if (refill == MOD_DMA_CHECK_CNDTR_ANOMALY) {
        dma_refill_fault(MOD_PIPELINE_REASON_DMA_CNDTR_ANOMALY,
                         ((cndtr1 & 0xFFFFUL) << 16) | (cndtr2 & 0xFFFFUL));
        return;
    }
    if (refill == MOD_DMA_CHECK_REFILL_LATE) {
        dma_refill_fault(MOD_PIPELINE_REASON_DMA_REFILL_LATE, cndtr1);
        return;
    }

    pipeline_dma_progress_epoch++;
    MOD_RequestRealtimeRender();
}

HOT_CODE void mod_dma_irq_channel2(void)
{
    const uint32_t flags = DMA1->ISR & DMA_ISR_TEIF2;
    if (flags == 0U) return;
    DMA1->IFCR = DMA_IFCR_CTEIF2;
    dma_ch2_error_count++;
    dma_refill_fault(MOD_PIPELINE_REASON_DMA_ERROR, flags);
}

/* A render underrun's value in the pipeline log: what the renderer and the
 * core were doing when the queue ran dry (2026-10-02: one 9.5 s into an FT8
 * over, 48 ms of audio waiting in the PCM ring, no USB packet missing and
 * nothing else logged, so the renderer had stalled for the queue's ~28 ms).
 *   7:0   ms since the renderer last produced a radiating frame (255 max)
 *   8     PendSV pending        9  PendSV active: preempted, not idle
 *   10    a config change or DSP reset pending (the renderer waits for it)
 *   11    a frame mid-pop or mid-push
 *   23:16 PCM frames waiting (255 max)
 *   31:24 the highest pending exception (VECTPENDING)
 * Out of line: it runs once, as the lockout begins. */
static __attribute__((noinline, cold)) uint32_t underrun_snapshot(void)
{
    const uint32_t icsr = SCB->ICSR;
    uint32_t frames = pcm_count / AUDIO_FRAME_SIZE;
    uint32_t pending = (icsr & SCB_ICSR_VECTPENDING_Msk) >> SCB_ICSR_VECTPENDING_Pos;
    if (frames > 255U) frames = 255U;
    if (pending > 255U) pending = 255U;
    uint32_t v = MOD_TxRfRenderedAgeMs(HAL_GetTick()) | (frames << 16) | (pending << 24);
    if ((icsr & SCB_ICSR_PENDSVSET_Msk) != 0U) v |= 1UL << 8;
    if ((SCB->SHCSR & SCB_SHCSR_PENDSVACT_Msk) != 0U) v |= 1UL << 9;
    if ((requested_config != active_config) || (requested_reset != 0U)) v |= 1UL << 10;
    if ((pcm_pop_in_progress | pcm_push_in_progress | render_push_in_progress) != 0U) v |= 1UL << 11;
    return v;
}

HOT_CODE void rfifo_pop_to_dma(uint32_t half)
{
    CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_DMA_POP);
    uint32_t *d1 = &dma_mcmp1[half * AUDIO_FRAME_SIZE];
    uint32_t *d2 = &dma_mcmp2[half * AUDIO_FRAME_SIZE];
    RenderFrame *src = NULL;
    uint8_t have_frame = 0U;
    uint8_t pop_ok = 0U;
    uint8_t frame_kind = RENDER_FRAME_KIND_NONE;
    uint8_t rf_active = 0U;
    uint8_t tx_live = 0U;
    uint8_t rf_gate_valid = 0U;
    uint32_t frame_seq = 0U;
    uint32_t rd = 0U;
    uint32_t seq = 0U;

    if (!MOD_CarrierFrequencyReadyForTxFast()) {
        uint32_t pm_flush = enter_critical();
        render_wr_idx = 0U;
        render_rd_idx = 0U;
        render_count = 0U;
        render_push_in_progress = 0U;
        render_pop_in_progress = 0U;
        render_reset_seq++;
        render_next_seq = 0U;
        render_dma_expected_seq = 0U;
        render_dma_seq_valid = 0U;
        if (render_low_water > 0U) render_low_water = 0U;
        exit_critical(pm_flush);
        cw_tx_live = 0U;
        cw_rf_enabled = 0U;
        fill_null_soft(half);
        last_dma_valid = 0U;
        transport_dma_park_request = 1U;
        CPU_DIAG_END(MOD_CPU_DIAG_DMA_POP, cpu_diag_start);
        return;
    }

    if (((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) &&
        (MOD_TxCarrierTestPaddleActive() == 0U))
    {
        uint32_t pm_flush = enter_critical();
        render_wr_idx = 0U;
        render_rd_idx = 0U;
        render_count = 0U;
        render_push_in_progress = 0U;
        render_pop_in_progress = 0U;
        render_reset_seq++;
        render_next_seq = 0U;
        render_dma_expected_seq = 0U;
        render_dma_seq_valid = 0U;
        if (render_low_water > 0U) render_low_water = 0U;
        exit_critical(pm_flush);
        cw_tx_live = 0U;
        cw_rf_enabled = 0U;
        fill_null_soft(half);
        last_dma_valid = 0U;
        transport_dma_park_request = 1U;
        CPU_DIAG_END(MOD_CPU_DIAG_DMA_POP, cpu_diag_start);
        return;
    }

    uint32_t pm = enter_critical();
    if ((render_count > 0U) && (render_pop_in_progress == 0U))
    {
        rd = render_rd_idx;
        seq = render_reset_seq;
        src = &render_fifo[rd];
        if (src->rf_active && !RADIO_TxReady() && RADIO_TxPermitted()) {
            RADIO_RequestTx();
            exit_critical(pm);
            fill_null_soft(half);
            CPU_DIAG_END(MOD_CPU_DIAG_DMA_POP, cpu_diag_start);
            return;
        }
        render_pop_in_progress = 1U;
        have_frame = 1U;
    }
    exit_critical(pm);

    if (!have_frame)
    {
        uint8_t ur = 1U;
        /* Muted, the renderer stops on purpose (MOD_RealtimeRenderWorker) and
         * nothing may radiate: an empty queue is the stream winding down
         * until the main loop parks it, not an underrun. CW keeps only two or
         * three frames queued, so a mute during an element -- TRANSMIT MUTE,
         * `set put 5 1` -- emptied it within a few milliseconds and latched a
         * hard lockout and FAULT_DMA (bench, 2026-09-28). */
        if (cfg_muted != 0U) ur = 0U;
        if (ur && !active_internal_source) {
            uint32_t now = HAL_GetTick();
            if ((transport_started == 0U) ||
                (mod_usb_transport_recent_at(now, USB_GAP_RESET_MS) == 0U)) {
                ur = 0U;
            }
        }
        if (ur) {
            render_underruns++;
            mod_pipeline_note(MOD_PIPELINE_STAGE_RENDER,
                              MOD_PIPELINE_REASON_RENDER_UNDERRUN,
                              underrun_snapshot());
            if (!active_internal_source) {
                transport_underrun_streak++;
                if (transport_underrun_streak >= TRANSPORT_UNDERRUN_RESYNC_STREAK) {
                    external_transport_resync();
                }
            }
        }
        if (render_low_water > 0U) render_low_water = 0U;
        if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
            cw_tx_live = 0U;
            cw_rf_enabled = 0U;
            fill_null_soft(half);
            transport_dma_park_request = 1U;
        } else if (ur && !active_internal_source && active_mod_type == MOD_TYPE_FM) {
            fill_fm_hold(half);
        } else if (ur && active_internal_source && active_mod_type == MOD_TYPE_CW
                && last_dma_valid
                && (last_dma_frame.frame_kind == RENDER_FRAME_KIND_CW)
                && ((last_dma_frame.rf_active == 0U)
                 || (cw_last_valid_repeat_count < CW_LAST_VALID_REPEAT_LIMIT))
                && ((cw_tx_live != 0U) || (cw_rf_enabled != 0U)
                 || (cw_paddle_dit != 0U) || (cw_paddle_dah != 0U))) {
            fill_last_valid(half);
        } else {
            fill_null_soft(half);
            transport_dma_park_request = 1U;
        }
        CPU_DIAG_END(MOD_CPU_DIAG_DMA_POP, cpu_diag_start);
        return;
    }

    /* Keep the FIFO slot owned by the consumer until both compare arrays and
     * metadata have been copied. The critical section only reserves/commits the
     * slot index; the large 2 x 48-word copy runs with interrupts enabled. */
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; i++) {
        d1[i] = (uint32_t)src->mcmp1[i];
        d2[i] = (uint32_t)src->mcmp2[i];
    }
    dma_interleave_half(half, src->rf_active);
    frame_kind = src->frame_kind;
    rf_active = src->rf_active;
    tx_live = src->tx_live;
    rf_gate_valid = src->rf_gate_valid;
    frame_seq = src->frame_seq;
    if (frame_kind == RENDER_FRAME_KIND_CW) {
        last_dma_frame = *src;
    }

    pm = enter_critical();
    if ((render_pop_in_progress != 0U) &&
        (seq == render_reset_seq) &&
        (render_count > 0U) &&
        (render_rd_idx == rd)) {
        render_rd_idx = (render_rd_idx + 1U) % RENDER_QUEUE_FRAMES;
        render_count--;
        if (render_count < render_low_water) render_low_water = render_count;
        pop_ok = 1U;
    }
    render_pop_in_progress = 0U;
    exit_critical(pm);

    if (pop_ok == 0U) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_RENDER,
                          MOD_PIPELINE_REASON_RENDER_RESET_DISCARD,
                          render_count);
        fill_null_soft(half);
        last_dma_valid = 0U;
        transport_dma_park_request = 1U;
        CPU_DIAG_END(MOD_CPU_DIAG_DMA_POP, cpu_diag_start);
        return;
    }

    transport_underrun_streak = 0U;
    if ((render_dma_seq_valid != 0U) && (frame_seq != render_dma_expected_seq)) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_RENDER,
                          MOD_PIPELINE_REASON_RENDER_SEQUENCE_GAP,
                          frame_seq);
    }
    render_dma_expected_seq = frame_seq + 1U;
    render_dma_seq_valid = 1U;
    if (frame_kind == RENDER_FRAME_KIND_CW) {
        /* Snapshot already taken while the FIFO slot was still reserved. */
    } else {
        last_dma_frame.frame_kind = frame_kind;
    }
    last_dma_frame.frame_kind = frame_kind;
    last_dma_frame.rf_active = rf_active;
    last_dma_frame.tx_live = tx_live;
    last_dma_frame.rf_gate_valid = rf_gate_valid;
    last_dma_valid = 1U;
    cw_last_valid_repeat_count = 0U;
    dma_half_store_frame_rf_state(half, &last_dma_frame);
    CPU_DIAG_END(MOD_CPU_DIAG_DMA_POP, cpu_diag_start);
}


static uint32_t tim6_sched_rate_hz = AUDIO_RATE_HZ_U32;
static uint32_t tim6_sched_base_ticks = TIM6_DEFAULT_ARR_170M + 1U;
static uint32_t tim6_sched_rem_ticks = 0U;
static uint32_t tim6_sched_rem_accum = 0U;

__attribute__((cold)) void mod_tim6_scheduler_init(uint32_t timer_clk_hz, uint32_t sample_rate_hz)
{
    if (timer_clk_hz == 0U) timer_clk_hz = MOD_HRTIM_INPUT_CLK_DEFAULT_HZ;
    if (sample_rate_hz == 0U) sample_rate_hz = AUDIO_RATE_HZ_U32;
    tim6_sched_rate_hz = sample_rate_hz;
    tim6_sched_base_ticks = timer_clk_hz / sample_rate_hz;
    tim6_sched_rem_ticks = timer_clk_hz - (tim6_sched_base_ticks * sample_rate_hz);
    tim6_sched_rem_accum = 0U;
    if (tim6_sched_base_ticks < 3U) tim6_sched_base_ticks = 3U;
    mod_tim6_input_clock_hz = timer_clk_hz;
}

uint32_t mod_tim6_next_base_arr(void)
{
    uint32_t ticks = tim6_sched_base_ticks;
    tim6_sched_rem_accum += tim6_sched_rem_ticks;
    if (tim6_sched_rem_accum >= tim6_sched_rate_hz) {
        tim6_sched_rem_accum -= tim6_sched_rate_hz;
        ticks++;
    }
    if (ticks < 3U) ticks = 3U;
    return ticks - 1U;
}

HOT_CODE void tim6_schedule(void)
{
    uint32_t arr = mod_tim6_next_base_arr();

#if TRANSPORT_SERVO_ENABLE
    if (!active_internal_source && transport_started)
    {
        if (++tim6_servo_frame_counter >= TRANSPORT_SERVO_INTERVAL)
        {
            uint32_t pm = enter_critical();
            uint32_t total_samples = pcm_count + render_count * AUDIO_FRAME_SIZE;
            mod_type_t type = active_mod_type;
            exit_critical(pm);

            uint32_t target_total = (type == MOD_TYPE_FM)
                                  ? TRANSPORT_TARGET_TOTAL_SAMPLES_FM
                                  : TRANSPORT_TARGET_TOTAL_SAMPLES;
            uint32_t low_total = TRANSPORT_SERVO_EMERGENCY_LOW_FRAMES * AUDIO_FRAME_SIZE;
            uint32_t high_total = target_total +
                                  (TRANSPORT_SERVO_EMERGENCY_HIGH_EXTRA_FRAMES * AUDIO_FRAME_SIZE);
            int32_t err = (int32_t)total_samples - (int32_t)target_total;
            tim6_servo_frame_counter = 0U;
            tim6_servo_error_avg = (tim6_servo_error_avg * 7 + err) / 8;
            if (total_samples < low_total) {
                if (tim6_servo_bias < TRANSPORT_SERVO_BIAS_MAX) tim6_servo_bias++;
            } else if (total_samples > high_total) {
                if (tim6_servo_bias > -TRANSPORT_SERVO_BIAS_MAX) tim6_servo_bias--;
            } else {
                if      (tim6_servo_bias > 0) tim6_servo_bias--;
                else if (tim6_servo_bias < 0) tim6_servo_bias++;
            }
        }
        int8_t bias = tim6_servo_bias;
        if      (bias > 0) arr += (uint32_t) bias;
        else if (bias < 0 && arr > (uint32_t)(-bias))
                                      arr -= (uint32_t)(-bias);
    }
#endif
    if (arr < 2U) arr = 2U;
    htim6.Instance->ARR = arr;
    tim6_arr_last = arr;
}

static void mod_dma_prime_timing_base(void)
{
    tim6_servo_frame_counter  = 0U;
    tim6_servo_error_avg      = 0;
    tim6_servo_bias           = TRANSPORT_LOCKED_BIAS;
    mod_tim6_scheduler_init(MOD_GetTim6InputClockHz(), AUDIO_RATE_HZ_U32 * rf_interp);
    uint32_t accumulator = 0U;
    uint32_t clk = MOD_GetTim6InputClockHz();
    const uint32_t rate = AUDIO_RATE_HZ_U32 * rf_interp;
    for (uint32_t i = 0U; i < RF_FRAME_N; ++i) {
        accumulator += clk % rate;
        uint32_t ticks = clk / rate;
        if (accumulator >= rate) { accumulator -= rate; ++ticks; }
        dma_sample_arr[i] = (uint16_t)(ticks - 1U);
    }
    interpolate_a = interpolate_b = phase_delay_a = phase_delay_b = 96U;
    interpolate_env = 0U;
    tim6_arr_last             = mod_tim6_next_base_arr();
    dma_write_half            = 0U;

    __HAL_TIM_DISABLE_DMA(&htim6, TIM_DMA_UPDATE);
    __HAL_TIM_DISABLE(&htim6);
    htim6.Instance->CR1 |= TIM_CR1_ARPE;
    htim6.Instance->ARR  = tim6_arr_last;
    htim6.Instance->CNT  = 0U;
    htim6.Instance->EGR  = TIM_EGR_UG;
    htim6.Instance->CNT  = 0U;
}

__attribute__((cold)) void mod_dma_stop_streams(void)
{
    __HAL_TIM_DISABLE_DMA(&htim6, TIM_DMA_UPDATE);
    __HAL_TIM_DISABLE(&htim6);
    RADIO_EnvelopeStop();
    DMA1_Channel4->CCR &= ~DMA_CCR_EN;

    /* TIM6, the only intentional reset source, is stopped. RM0440: disable
     * the DMA channel before the HRTIM's DMA request (Timer F RSTDE); a
     * two-word transaction cut short is re-synchronised below. */
    if (dma_mcmp2_initialized)
    {
        DMA1_Channel1->CCR &= ~DMA_CCR_EN;
        DMA1_Channel2->CCR &= ~DMA_CCR_EN;
        for (uint32_t i = 0U; i < 16U; i++) {
            if (((DMA1_Channel1->CCR | DMA1_Channel2->CCR) & DMA_CCR_EN) == 0U) break;
        }
    }
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    atomic_timer_f_dma_request_disable();
#endif
    if (dma_mcmp2_initialized)
    {
        dma_clear_specific_flags();
        dma_hal_handles_idle();
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        atomic_burst_parser_reinitialize();
#endif
    }
    dma_streams_running = 0U;
    dma_expected_half = 0U;
    dma_expected_half_valid = 0U;
}

/* The carrier grid residual (grid - dial, the corrected reference applied) as
 * a 2^32-per-update phase step at the phase-update rate the DMA really runs:
 * the TIM6 ticks in dma_sample_arr on the true SYSCLK. The old step assumed
 * exactly 192 kHz and a whole-hertz residual; this one is exact to far below
 * a milli-hertz. Called after mod_dma_prime_timing_base fills the table. */
static int32_t carrier_nco_step_now(void)
{
    uint32_t ticks = 0U;
    for (uint32_t i = 0U; i < RF_FRAME_N; ++i) ticks += (uint32_t)dma_sample_arr[i] + 1U;
    if (ticks == 0U) return 0;
    const int64_t rate_mhz = (int64_t)((MOD_SysclkMicroHz() * RF_FRAME_N / ticks) / 1000U);
    if (rate_mhz <= 0) return 0;
    const int64_t num = MOD_GetCarrierErrorMilliHz() * 4294967296LL;
    /* One ppb of the carrier is carrier x 1e-6 mHz. */
    /* rate_mhz is under 2^31 (384 kHz in mHz): converted as int32, not by
     * the soft-float int64 routine. */
    carrier_nco_per_ppb = (float)MOD_GetCarrierRequestedHz() * 4294.967296f / (float)(int32_t)rate_mhz;
    return (int32_t)((num >= 0 ? num + rate_mhz / 2 : num - rate_mhz / 2) / rate_mhz);
}

void mod_transport_refresh_step_guard(void)
{
    mod_hrtim_phase_geometry_t g;
    uint16_t cmp1 = (uint16_t)(active_carrier->period_ticks / 2U), room = 0U;
    const int8_t extra = active_carrier->frac_extra;
    if (mod_hrtim_phase_geometry_frac(active_carrier->period_ticks, HRTIM_GetDutyOffsetTicks(), 0U,
                                      active_carrier->frac_cycles, extra, &g)) {
        cmp1 = g.local_compare_ticks;
        room = (uint16_t)mod_hrtim_step_room(g.carrier_period_ticks, cmp1, MOD_HRTIM_STEP_GUARD_TICKS, extra);
    }
    /* The fill interrupt reads the three together. */
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    step_guard_cmp1 = cmp1;
    step_guard_extra = extra;
    step_guard_room = room;
    __set_PRIMASK(pm);
}

uint32_t MOD_TxPhaseUpdateHzNext(void)
{
    return AUDIO_RATE_HZ_U32 * ((RADIO_Config()->clock_flags & RADIO_CLOCK_FLAG_PHASE384)
                                ? RF_INTERPOLATION_MAX : RF_INTERPOLATION);
}

uint32_t MOD_TxPhaseUpdateHz(void)
{
    /* The rate a running stream uses, or the one the next stream will. */
    if (dma_streams_running) return AUDIO_RATE_HZ_U32 * rf_interp;
    return MOD_TxPhaseUpdateHzNext();
}

/* The TX HRTIM timers (master, A, E, F) run at the carrier grid, within a few
 * kHz of the dial, and leak into the receiver inside the MCU even with the
 * branch pins parked low: on REV1 they were the strongest in-band birdie. With
 * nothing to transmit they have nothing to do, so the receiver parks them.
 * Stopping holds the counters; restarting sets all four enables in one write,
 * and A and E realign on the next master compare, well before any TX output
 * is enabled. Only an idle, statically parked transport may be stopped. */
#define TX_TIMER_ENABLES (HRTIM_TIMERID_MASTER | HRTIM_TIMERID_TIMER_A | \
                          HRTIM_TIMERID_TIMER_E | HRTIM_TIMERID_TIMER_F)
static volatile uint8_t tx_timers_parked = 0U;

void MOD_TxTimersPark(uint8_t park)
{
    uint32_t pm = enter_critical();
    const uint32_t running = HRTIM1->sMasterRegs.MCR & TX_TIMER_ENABLES;
    if (park) {
        if ((running == TX_TIMER_ENABLES) && (dma_streams_running == 0U) &&
            (dma_static_idle_parked != 0U)) {
            HRTIM1->sMasterRegs.MCR &= ~TX_TIMER_ENABLES;
            tx_timers_parked = 1U;
        }
    } else if (tx_timers_parked) {
        if (running == 0U) HRTIM1->sMasterRegs.MCR |= TX_TIMER_ENABLES;
        tx_timers_parked = 0U;
    }
    exit_critical(pm);
}

uint8_t MOD_TxTimersParked(void)
{
    return (uint8_t)(tx_timers_parked &&
                     ((HRTIM1->sMasterRegs.MCR & TX_TIMER_ENABLES) == 0U));
}

static uint8_t mod_dma_start_streams_once(void)
{
    if (!dma_mcmp2_initialized) return 0U;
    if (dma_streams_running != 0U) return 1U;

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    /* RSTDE covers both reset and natural roll-over. It must remain masked
     * while buffers and DMA state are prepared. */
    atomic_timer_f_dma_request_disable();
#endif
    /* The TX HRTIM timers may be parked while the radio receives. */
    MOD_TxTimersPark(0U);
    rf_interp = (RADIO_Config()->clock_flags & RADIO_CLOCK_FLAG_PHASE384)
              ? RF_INTERPOLATION_MAX : RF_INTERPOLATION;
    carrier_nco = 0U;
    interpolate_a = interpolate_b = interpolate_env = 0U;
    phase_delay_a = phase_delay_b = 96U;
    mod_dma_prime_timing_base();
    carrier_nco_per_ppb = 0.0f;
    carrier_nco_base = carrier_nco_step = carrier_nco_step_now();
    mod_transport_refresh_step_guard();
    dma_half_store_all_idle();
    rfifo_pop_to_dma(0U);
    rfifo_pop_to_dma(1U);
    dma_half_apply_playing(0U);

#if MOD_HRTIM_ATOMIC_EXPERIMENT
    const uint32_t burst_select = HRTIM_BURSTDMA_CMP1 | HRTIM_BURSTDMA_CMP2;
    const uint32_t master_control = HRTIM1->sMasterRegs.MCR;
    const uint32_t timer_f_dier = HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_F].TIMxDIER;
    const uint32_t timer_f_reset = HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_F].RSTxR;
    const uint32_t event10_control = HRTIM1->sCommonRegs.EECR2;
    if ((hdma_tim6_up.Init.Request != DMA_REQUEST_HRTIM1_F) ||
        (HRTIM1->sCommonRegs.BDMUPR != burst_select) ||
        (HRTIM1->sCommonRegs.BDTAUPR != 0U) ||
        (HRTIM1->sCommonRegs.BDTBUPR != 0U) ||
        (HRTIM1->sCommonRegs.BDTCUPR != 0U) ||
        (HRTIM1->sCommonRegs.BDTDUPR != 0U) ||
        (HRTIM1->sCommonRegs.BDTEUPR != 0U) ||
        (HRTIM1->sCommonRegs.BDTFUPR != 0U) ||
        ((master_control & HRTIM_TIMERID_TIMER_F) == 0U) ||
        ((master_control & HRTIM_MCR_BRSTDMA) != HRTIM_MCR_BRSTDMA_1) ||
        ((master_control & HRTIM_MCR_MREPU) != 0U) ||
        ((timer_f_dier & HRTIM_TIMDIER_RSTDE) != 0U) ||
        ((timer_f_reset & HRTIM_RSTR_EXTEVNT10) == 0U) ||
        ((event10_control & HRTIM_EECR2_EE10SRC) !=
         ((HRTIM_EEV10SRC_TIM6_TRGO << 24U) & HRTIM_EECR2_EE10SRC)) ||
        ((event10_control & HRTIM_EECR2_EE10SNS) !=
         ((HRTIM_EVENTSENSITIVITY_RISINGEDGE << 24U) & HRTIM_EECR2_EE10SNS)) ||
        ((TIM6->CR2 & TIM_CR2_MMS) != TIM_TRGO_UPDATE)) {
        return 0U;
    }

    /* Bound the masked-reset -> DMA-ready -> RSTDE-enabled -> TIM6-start
     * sequence so Timer F cannot approach its natural roll-over between the
     * deliberate TIM6 software update reset and the first 48 kHz edge. */
    uint32_t atomic_pm = enter_critical();
    htim6.Instance->CNT = 0U;
    htim6.Instance->EGR = TIM_EGR_UG;
    htim6.Instance->CNT = 0U;
    atomic_burst_parser_reinitialize();

    if (HAL_DMA_Start(&hdma_tim6_up,
                      (uint32_t)dma_mcmp_pair,
                      (uint32_t)&HRTIM1->sCommonRegs.BDMADR,
                      RF_DMA_N * 2U) != HAL_OK) {
        dma_hal_handles_idle();
        exit_critical(atomic_pm);
        return 0U;
    }
    __HAL_DMA_ENABLE_IT(&hdma_tim6_up, DMA_IT_HT | DMA_IT_TC | DMA_IT_TE);
    if (atomic_timer_f_dma_request_enable() == 0U) {
        DMA1_Channel1->CCR &= ~DMA_CCR_EN;
        dma_clear_specific_flags();
        dma_hal_handles_idle();
        atomic_burst_parser_reinitialize();
        exit_critical(atomic_pm);
        return 0U;
    }
#else
    /* Release builds retain the legacy publisher until the diagnostic path
     * has completed its target scope acceptance. */
    if (HAL_DMA_Start(&hdma_tim6_up,
                      (uint32_t)dma_mcmp1,
                      (uint32_t)&HRTIM1->sMasterRegs.MCMP1R,
                      DMA_BUF_SIZE) != HAL_OK) return 0U;
    __HAL_DMA_ENABLE_IT(&hdma_tim6_up, DMA_IT_HT | DMA_IT_TC | DMA_IT_TE);

    if (HAL_DMA_Start(&hdma_mcmp2,
                      (uint32_t)dma_mcmp2,
                      (uint32_t)&HRTIM1->sMasterRegs.MCMP2R,
                      DMA_BUF_SIZE) != HAL_OK) {
        DMA1_Channel1->CCR &= ~DMA_CCR_EN;
        dma_clear_specific_flags();
        hdma_tim6_up.State = HAL_DMA_STATE_READY;
        hdma_tim6_up.ErrorCode = HAL_DMA_ERROR_NONE;
        return 0U;
    }
    __HAL_DMA_ENABLE_IT(&hdma_mcmp2, DMA_IT_TE);
#endif

    RADIO_EnvelopeStart(dma_envelope, RF_DMA_N);
    DMA1_Channel4->CCR = 0U;
    DMA1_Channel4->CPAR = (uint32_t)&TIM6->ARR;
    DMA1_Channel4->CMAR = (uint32_t)dma_sample_arr;
    DMA1_Channel4->CNDTR = RF_FRAME_N;
    DMAMUX1_Channel3->CCR = DMA_REQUEST_TIM6_UP;
    DMA1->IFCR = DMA_IFCR_CGIF4;
    DMA1_Channel4->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC |
                         DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_PL_1 | DMA_CCR_EN;
    __HAL_TIM_ENABLE_DMA(&htim6, TIM_DMA_UPDATE);
    htim6.Instance->CNT = 0U;
#if !MOD_HRTIM_ATOMIC_EXPERIMENT
    __HAL_TIM_ENABLE_DMA(&htim6, TIM_DMA_UPDATE);
#endif
    if (HAL_TIM_Base_Start(&htim6) != HAL_OK) {
        DMA1_Channel1->CCR &= ~DMA_CCR_EN;
        DMA1_Channel2->CCR &= ~DMA_CCR_EN;
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        atomic_timer_f_dma_request_disable();
#endif
        dma_clear_specific_flags();
        dma_hal_handles_idle();
        __HAL_TIM_DISABLE_DMA(&htim6, TIM_DMA_UPDATE);
#if MOD_HRTIM_ATOMIC_EXPERIMENT
        atomic_burst_parser_reinitialize();
        exit_critical(atomic_pm);
#endif
        return 0U;
    }
    dma_streams_running = 1U;
    dma_expected_half = 0U;
    dma_expected_half_valid = 1U;
    dma_static_idle_parked = 0U;
    transport_dma_park_request = 0U;
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    exit_critical(atomic_pm);
#endif
    return 1U;
}

__attribute__((cold)) void mod_dma_start_streams(void)
{
    if (mod_dma_start_streams_once() == 0U) {
        dma_start_fail_count++;
        transport_dma_faults++;
        transport_dma_fault_latched = 1U;
        mod_pipeline_note_severity(MOD_PIPELINE_STAGE_DMA,
                                   MOD_PIPELINE_REASON_DMA_START_FAILED,
                                   MOD_PIPELINE_SEVERITY_HARD,
                                   dma_start_fail_count);
    }
}

__attribute__((cold)) void MOD_StartDMA(void)
{
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    /* The Cube-generated TIM6 MSP setup remains untouched. Retarget its DMA
     * handle once during diagnostic initialization; channel 2 deliberately
     * remains unused. */
    if (HAL_DMA_DeInit(&hdma_tim6_up) != HAL_OK) {
        dma_start_fail_count++;
        transport_dma_faults++;
        transport_dma_fault_latched = 1U;
        return;
    }
    hdma_tim6_up.Instance = DMA1_Channel1;
    hdma_tim6_up.Init.Request = DMA_REQUEST_HRTIM1_F;
    hdma_tim6_up.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_tim6_up.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_tim6_up.Init.MemInc = DMA_MINC_ENABLE;
    hdma_tim6_up.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_tim6_up.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD; /* zero-extended */
    hdma_tim6_up.Init.Mode = DMA_CIRCULAR;
    hdma_tim6_up.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    if (HAL_DMA_Init(&hdma_tim6_up) != HAL_OK) {
        dma_start_fail_count++;
        transport_dma_faults++;
        transport_dma_fault_latched = 1U;
        return;
    }
    dma_mcmp2_initialized = 1U;
#else
    hdma_mcmp2.Instance                 = DMA1_Channel2;
    hdma_mcmp2.Init.Request             = DMA_REQUEST_TIM6_UP;
    hdma_mcmp2.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_mcmp2.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_mcmp2.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_mcmp2.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_mcmp2.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_mcmp2.Init.Mode                = DMA_CIRCULAR;
    hdma_mcmp2.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
    if (HAL_DMA_Init(&hdma_mcmp2) != HAL_OK) {
        dma_start_fail_count++;
        dma_ch2_error_count++;
        transport_dma_faults++;
        transport_dma_fault_latched = 1U;
        mod_pipeline_note_severity(MOD_PIPELINE_STAGE_DMA,
                                   MOD_PIPELINE_REASON_DMA_START_FAILED,
                                   MOD_PIPELINE_SEVERITY_HARD,
                                   hdma_mcmp2.ErrorCode);
        return;
    }
    dma_mcmp2_initialized = 1U;
#endif

    MOD_FillAllNullBuffers();
    mod_dma_park_static_idle();
}

uint8_t MOD_RfDmaStreamsRunning(void) { return dma_streams_running; }

uint8_t MOD_RfDmaStaticIdleParked(void)
{
    return (uint8_t)((dma_static_idle_parked != 0U) &&
                     (dma_streams_running == 0U) &&
                     ((TIM6->CR1 & TIM_CR1_CEN) == 0U)
#if MOD_HRTIM_ATOMIC_EXPERIMENT
                     && ((HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_F].TIMxDIER &
                          HRTIM_TIMDIER_RSTDE) == 0U)
#endif
                     );
}

/* The CW keyer proper: CW on the internal source, not the TX TEST page. */
uint8_t mod_transport_cw_keyer_source(void)
{
    return (uint8_t)((active_internal_source != 0U) && (active_mod_type == MOD_TYPE_CW) &&
                     ((mod_config_t)active_config != MOD_CFG_TX_CARRIER_TEST));
}

/* When the TX TEST key (raw lever or debounced gate) was last seen down:
 * mod_tx_test_stream_wanted holds the stream MOD_TX_TEST_RELEASE_HANG_MS
 * past it. Written from the main loop and the render worker, one word. */
static volatile uint32_t tx_test_key_seen_ms = 0x80000000UL;
/* Stops the main loop held back for a transmitter still up (console). */
static uint32_t transport_stop_deferred;
uint32_t MOD_TransportStopDeferred(void) { return transport_stop_deferred; }

/* Whether the configured source wants its RF stream, lockout aside:
 * mod_transport_stream_needed adds the lockout, the re-arm asks this. */
static uint8_t transport_stream_wanted(uint32_t now)
{
    if (!MOD_CarrierFrequencyReadyForTx()) return 0U;
    if (cfg_muted != 0U) return 0U;

    if (active_internal_source != 0U) {
        if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
            /* A carrier the radio may not transmit here (RADIO_ConfigTxBlock:
             * below the LPF's TX MIN, untested LPF, ...) never starts its
             * stream. Keyed there, the radio stayed in receive under a
             * running carrier stream and every key-down ended in a hard
             * render underrun, RF PIPELINE FAULT: the 90M, 49M and 25M BC
             * presets, each just below its LPF's TX MIN (bench, 2026-10-06).
             * A stream already running is left to its release. */
            if (!dma_streams_running && !RADIO_TxPermitted()) return 0U;
            uint8_t dit = 0U, dah = 0U;
            const uint8_t raw = cw_get_paddles_for_tx_test(&dit, &dah);
            const uint8_t gate = MOD_TxCarrierTestPaddleActive();
            if (raw || gate) tx_test_key_seen_ms = now;
            return mod_tx_test_stream_wanted(raw, gate, now - tx_test_key_seen_ms);
        }

        /* A keyer that cannot radiate runs from the headphone renderer
         * (cw_monitor_step) and needs no TX stream; see cw_keyer_monitor_only. */
        if (active_mod_type == MOD_TYPE_CW) {
            if (cw_keyer_monitor_only() != 0U) return 0U;
            return mod_transport_cw_keyer_busy(now);
        }

        return 1U;
    }

    if (transport_started == 0U) return 0U;
    if ((MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) && (local_audio_eos != 0U) &&
        (pcm_count == 0U) && (render_count == 0U)) return 0U;
    return 1U;
}

uint8_t mod_transport_stream_needed(uint32_t now)
{
    if (pipeline_hard_lockout != 0U) return 0U;
    return transport_stream_wanted(now);
}

/* The transmitter is up: settling, transmitting, or RF still permitted. */
static uint8_t transport_tx_up(void)
{
    const radio_state_t st = RADIO_State();
    return (uint8_t)((st == RADIO_TX_PREPARE) || (st == RADIO_TRANSMIT) || (BOARD_RfActive() != 0U));
}

uint8_t mod_transport_cw_keyer_busy(uint32_t now)
{
    uint8_t dit = 0U, dah = 0U;
    const uint8_t paddle = cw_get_paddles_for_cw(&dit, &dah);
    return mod_cw_keyer_needs_stream(paddle, cw_tx_live, cw_rf_enabled, cw_key_sample_active,
                                     (uint8_t)(RADIO_State() == RADIO_RECEIVE),
                                     (uint32_t)(now - g_last_cw_tick), AUDIO_CRITICAL_HOLDOFF_MS);
}

/* Unmuted CW used to refuse every retune, mode change and blocking I/O
 * outright, so the operator could not tune a CW signal without first muting
 * TX. That refusal now applies only while the keyer is busy; an idle keyer
 * falls through to the ordinary transport/DMA/queue checks. */
__attribute__((cold)) uint8_t mod_transport_cw_idle(uint32_t now)
{
    if (mod_transport_cw_keyer_source() == 0U) return 0U;
    /* A keyer that cannot radiate has no TX stream to protect, keyed or not. */
    if (cw_keyer_monitor_only() != 0U) return 1U;
    return (uint8_t)(mod_transport_cw_keyer_busy(now) == 0U);
}

__attribute__((cold)) void mod_pipeline_service_rearm(uint32_t now)
{
    if ((pipeline_hard_lockout == 0U) || (pipeline_ack_requested == 0U)) return;

    uint32_t target;
    uint8_t source_ready;
    if (active_internal_source != 0U) {
        target = ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST)
               ? 2U
               : ((active_mod_type == MOD_TYPE_CW)
                  ? TRANSPORT_RENDER_TARGET_FRAMES_CW
                  : TRANSPORT_RENDER_TARGET_FRAMES_INTERNAL);
        source_ready = 1U;
    } else {
        target = (active_mod_type == MOD_TYPE_FM)
               ? TRANSPORT_RENDER_TARGET_FRAMES_FM
               : TRANSPORT_RENDER_TARGET_FRAMES_EXTERNAL;
        source_ready = (usb_rearm_good_frames >= 3U) ? 1U : 0U;
    }
    const uint8_t idle = (uint8_t)((dma_streams_running == 0U) && (transport_stream_wanted(now) == 0U) &&
                                   (transport_tx_up() == 0U));
    if (!mod_pipeline_may_rearm(idle, source_ready, render_count, target,
                                MOD_CarrierFrequencyReadyForTxFast())) {
        pipeline_rearm_pending = 1U;
        pipeline_health.rearm_pending = 1U;
        return;
    }

    uint32_t pm = enter_critical();
    pipeline_hard_lockout = 0U;
    pipeline_ack_requested = 0U;
    pipeline_rearm_pending = 0U;
    pipeline_health.hard_lockout = 0U;
    pipeline_health.rearm_pending = 0U;
    pipeline_health.soft_fault_latched = 0U;
    pipeline_health.sticky_latched = 0U;
    pipeline_health.sticky_stage_mask = 0U;
    pipeline_health.last_severity = MOD_PIPELINE_SEVERITY_NONE;
    pipeline_health.first_severity = MOD_PIPELINE_SEVERITY_NONE;
    transport_dma_fault_latched = 0U;
    exit_critical(pm);
    LED_GPIO_Port->BRR = LED_Pin;
}

/* What the RF DMA looked like when the progress watchdog tripped, for the
 * console (`pipeline`) and the SD event log: which of the pacing chain --
 * HRTIM timer F's DMA request, the DMA channel, TIM6 -- had stopped. */
static mod_rf_stall_t rf_stall;
void MOD_GetRfStall(mod_rf_stall_t *out) { *out = rf_stall; }

static void rf_stall_capture(uint32_t epoch, uint8_t ticks)
{
    rf_stall.count = rf_stall.count < 0xFFFFU ? (uint16_t)(rf_stall.count + 1U) : rf_stall.count;
    rf_stall.tick_ms = HAL_GetTick();
    rf_stall.epoch = epoch;
    rf_stall.ticks = ticks;
    rf_stall.radio_state = (uint8_t)RADIO_State();
    rf_stall.config = (uint8_t)active_config;
    rf_stall.dma_ccr = (uint16_t)DMA1_Channel1->CCR;
    rf_stall.dma_cndtr = (uint16_t)DMA1_Channel1->CNDTR;
    rf_stall.tim6_cr1 = (uint16_t)TIM6->CR1;
    rf_stall.hrtim_mcr = HRTIM1->sMasterRegs.MCR;
    rf_stall.timf_dier = HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_F].TIMxDIER;
    rf_stall.render_count = (uint8_t)render_count;
}

void MOD_RealtimeHealthTick(void)
{
    static uint32_t last_dma_epoch = 0U;
    static uint8_t stalled_ticks = 0U;
    /* A stream started since the last boundary owes its first one: it gets
     * MOD_RF_STALL_TICKS_START (mod_rf_stall_tripped). */
    static uint8_t first_pending = 0U;

    if ((dma_streams_running == 0U) || (pipeline_hard_lockout != 0U)) {
        last_dma_epoch = pipeline_dma_progress_epoch;
        stalled_ticks = 0U;
        first_pending = 1U;
        return;
    }
    uint32_t epoch = pipeline_dma_progress_epoch;
    if (epoch != last_dma_epoch) {
        last_dma_epoch = epoch;
        stalled_ticks = 0U;
        first_pending = 0U;
        return;
    }
    if (stalled_ticks < 0xFFU) stalled_ticks++;
    if (mod_rf_stall_tripped(stalled_ticks, first_pending)) {
        rf_stall_capture(epoch, stalled_ticks);
        stalled_ticks = 0U;
        mod_pipeline_note_severity(MOD_PIPELINE_STAGE_DMA,
                                   MOD_PIPELINE_REASON_RENDER_DEADLINE,
                                   MOD_PIPELINE_SEVERITY_HARD,
                                   epoch);
    }
}

__attribute__((cold)) void mod_transport_service_dma_state(uint32_t now)
{
    const uint8_t want_stream = mod_transport_stream_needed(now);

    if (pipeline_hard_lockout != 0U) return;

    if (want_stream != 0U) {
        if (active_internal_source != 0U) transport_started = 1U;
        /* Not under the emergency latch: the envelope DMA refuses to start
         * there (RADIO_EnvelopeStart), so the first boundary stops the stream
         * again, and USB audio would restart it every pass until the fault
         * is cleared. */
        if ((dma_streams_running == 0U) && (render_count >= 2U) && !BOARD_EmergencyLatched()) {
            mod_dma_start_streams();
        }
        return;
    }
    /* Not under a transmitter that is still up: the renderer's null frames
     * carry the stream until the radio has left transmit (at most its 80 ms
     * request window), then the next pass stops it. */
    if ((dma_streams_running != 0U) && !mod_transport_may_stop_stream(want_stream, transport_tx_up())) {
        ++transport_stop_deferred;
        return;
    }

    if ((dma_streams_running != 0U) ||
        (dma_static_idle_parked == 0U) ||
        (transport_dma_park_request != 0U)) {
        if (dma_streams_running != 0U) mod_dma_stop_streams();
        MOD_FillAllNullBuffers();
        transport_dma_park_request = 0U;
    }

    /* An internal source whose stream is not needed -- an idle CW keyer, an
     * unkeyed TX TEST, anything muted -- has an idle transport. Frames left
     * queued with no DMA to drain them, and a started flag nothing clears,
     * read as a busy transport to every retune, mode change and blocking-I/O
     * gate: CW could not be retuned or left once entered. */
    if ((active_internal_source != 0U) && (dma_streams_running == 0U) &&
        ((render_count != 0U) || (transport_started != 0U))) {
        MOD_FillAllNullBuffers();
        transport_started = 0U;
    }
}

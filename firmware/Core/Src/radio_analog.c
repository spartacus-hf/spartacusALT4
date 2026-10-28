#include "radio_analog.h"
#include "radio_clocks.h"
#include "radio_dsp.h"
#include "radio_rf_meter.h"
#include "radio_guard_logic.h"
#include "board_rev1.h"
#include "modulator.h"
#include "stm32g4xx_ll_adc.h"
#include <math.h>
#include <string.h>

static uint16_t adc_samples[14];
static volatile radio_telemetry_t telemetry;
static volatile uint8_t bias_enabled;
static radio_bias_loop_t bias_loop;
static radio_bias_avg_t bias_avg; /* telemetry.bias_mv: see RADIO_BiasAverageStep */
static float bias_duty;
static uint32_t bias_top = 1466U;
static uint8_t rail_bad, bias_low, bias_high, swr_bad;
/* The power/SWR meter (radio_rf_meter.h): a 1 ms (4-scan) mean of each
 * detector -- one ADC2 scan scatters tens of mV, which a peak would keep --
 * and the sums of the scans with RF up since the main loop last took them
 * (RADIO_MeterService, under PRIMASK). */
static uint16_t meter_hist[2][4];
static uint32_t meter_sum[2];
static uint8_t meter_k;
static rf_meter_acc_t meter_acc;
static float meter_plateau; /* port volts; RADIO_MeterService sets it */
/* Each detector's level at rest -- ADC offset plus the noise the 0 V floor
 * rectifies, about 2 mV on REV1 -- learned while no RF is up and taken off
 * both detectors when FWD/REV ZERO are left at 0. Through the diode's square
 * law 2 mV is ~9 mV of port voltage: on the reflected port that was SWR 1.09
 * on a 50 ohm load at 0.04 W (bench, 2026-10-02). The detectors' 10 ms hold
 * decays for 100 ms after RF before a scan counts. */
static uint32_t meter_rest_sum[2];
static uint16_t meter_rest_n; /* scans; taken every meter interval (<= 1 s, 4000) */
static uint16_t meter_rest_hold;
static volatile float meter_zero_mv[2];
/* What the interrupt computes with, built in the main loop (meter_params,
 * at boot and on every configuration change or meter interval): the diode
 * table, each detector's zero (mV) and scale, |gamma| at the SWR limit. */
static rf_meter_lin_t meter_lin;
static volatile float meter_zero[2], meter_scale[2], meter_limit;
/* The bridge leakage's constant floor on the reflected port (`metercal
 * leak`, port volts, at most 0.1 V here): a detector offset, not a
 * reflection, so the trips take it off. At 0.1 W it alone is |gamma| 0.2 --
 * QMX moved its trip to 0.5 W after false trips at low power; REV1 keeps
 * 0.1 W (OUTPHASING drains at full voltage) and removes the offset instead.
 * The proportional leakage stays in: the trips still err towards tripping. */
static volatile float meter_floor;
/* TX TEST's drain cap (TUNE %), a fraction of the full drain; the main loop
 * sets it, RADIO_EnvelopeFill reads it once per call. */
static volatile float envelope_cap = 1.0f;
radio_meter_t rf_meter;
static uint32_t meter_taken;
/* The trips that need nothing but the scan (radio_guard_logic.h). Written
 * by the ADC interrupt only; CCM, as RAM has no room. */
static radio_guard_t guard __attribute__((section(".ccmram_bss")));

static uint32_t analog_enter_critical(void)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    return 0U;
#else
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    return pm;
#endif
}

static void analog_exit_critical(uint32_t pm)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    (void)pm;
#else
    __set_PRIMASK(pm);
#endif
}

static uint8_t wait_bits(volatile uint32_t *reg, uint32_t mask, uint8_t set)
{
    for (uint32_t n = 0U; n < 1000000UL; ++n)
        if (((*reg & mask) != 0U) == (set != 0U)) return 1U;
    return 0U;
}

uint8_t RADIO_ReferenceInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    /* REV1 VREF+ has decoupling only. Scale 2 is 2.9 V on G474. */
    HAL_SYSCFG_VREFBUF_HighImpedanceConfig(SYSCFG_VREFBUF_HIGH_IMPEDANCE_DISABLE);
    HAL_SYSCFG_VREFBUF_VoltageScalingConfig(SYSCFG_VREFBUF_VOLTAGE_SCALE2);
    return HAL_SYSCFG_EnableVREFBUF() == HAL_OK;
}

void RADIO_AnalogClocks(void)
{
    TIM7->CR1 = 0U;
    TIM7->PSC = 0U;
    TIM7->ARR = (SystemCoreClock + 2000UL) / 4000UL - 1UL;
    TIM7->CR2 = TIM_CR2_MMS_1;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->CR1 = TIM_CR1_CEN;
    TIM4->PSC = 0U;
    TIM4->ARR = bias_top - 1U;
    TIM4->CCR1 = 0U;
    TIM4->CCMR1 = TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE;
    TIM4->CCER = TIM_CCER_CC1E;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

__attribute__((cold)) uint8_t RADIO_AnalogInit(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_DAC3_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_TIM4_CLK_ENABLE();
    __HAL_RCC_TIM7_CLK_ENABLE();
    /* ADC1 microphones share this common synchronous /4 clock. */
    ADC12_COMMON->CCR = (ADC12_COMMON->CCR & ~(ADC_CCR_CKMODE | ADC_CCR_PRESC)) |
                        ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1;
    ADC2->CR &= ~ADC_CR_DEEPPWD;
    ADC2->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(1U);
    ADC2->CR &= ~ADC_CR_ADCALDIF;
    ADC2->CR |= ADC_CR_ADCAL;
    if (!wait_bits(&ADC2->CR, ADC_CR_ADCAL, 0U)) return 0U;
    ADC2->CFGR = ADC_CFGR_DMAEN | ADC_CFGR_DMACFG | ADC_CFGR_EXTEN_0 |
                  (LL_ADC_REG_TRIG_EXT_TIM7_TRGO & ADC_CFGR_EXTSEL);
    ADC2->SMPR1 = 0x3FFFFFFFUL; /* 640.5 cycles: high impedance telemetry */
    ADC2->SMPR2 = 0x07FFFFFFUL;
    ADC2->SQR1 = 6U | (6UL << 6U) | (7UL << 12U) | (8UL << 18U) | (9UL << 24U);
    ADC2->SQR2 = 5UL | (11UL << 6U) | (12UL << 12U);
    DMA2_Channel1->CCR = 0U;
    DMA2_Channel1->CPAR = (uint32_t)&ADC2->DR;
    DMA2_Channel1->CMAR = (uint32_t)adc_samples;
    DMA2_Channel1->CNDTR = 14U;
    DMAMUX1_Channel8->CCR = DMA_REQUEST_ADC2;
    DMA2->IFCR = DMA_IFCR_CGIF1;
    DMA2_Channel1->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 |
                         DMA_CCR_PL_1 | DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE | DMA_CCR_EN;
    /* Above the RF, audio and USB interrupts (1), beside the HRTIM fault
     * (0). A scan lasts 250 us; while transmitting, the RF DMA refill
     * (interpolation and the envelope DAC fill) holds level 1 for about
     * 265 us. Behind it a scan was lost, the protection loop latched
     * FAULT_ADC (adc_late) and stopped the envelope DMA, and the next RF
     * boundary reported that as a DMA SKEW: every key-down on the first TX
     * bench run died within a second (docs/REV1.md). Everything this handler
     * shares with lower levels is a single word or sits under PRIMASK. */
    HAL_NVIC_SetPriority(DMA2_Channel1_IRQn, 0U, 0U);
    HAL_NVIC_EnableIRQ(DMA2_Channel1_IRQn);
    ADC2->ISR = ADC_ISR_ADRDY;
    ADC2->CR |= ADC_CR_ADEN;
    if (!wait_bits(&ADC2->ISR, ADC_ISR_ADRDY, 1U)) return 0U;
    bias_avg.primed = 0U; /* nothing from before a pause survives into the mean */
    RADIO_MeterReconfigure(); /* the SWR trip's table and limit before the first scan */
    ADC2->CR |= ADC_CR_ADSTART;
    /* DAC3 outputs are internal only: CH2 -> OPAMP3/PB1 (A),
     * CH1 -> OPAMP6/PB11 (B). VMSEL=11 is internal follower feedback. */
    /* HFSEL = 10: RM0440 26.4.x requires the high-frequency interface mode
     * whenever the DAC clock (AHB) exceeds 80 MHz, and the 10 setting at 160 MHz
     * and above; without it "the peripheral behavior is unpredictable" when a
     * trigger or update arrives inside the extended window. SYSCLK is 164-170
     * MHz (down to 153.3 MHz with WIDE CLOCK RANGE, where 10 only adds margin). */
    DAC3->MCR = DAC_MCR_HFSEL_1 |
                DAC_MCR_MODE1_0 | DAC_MCR_MODE1_1 | DAC_MCR_MODE2_0 | DAC_MCR_MODE2_1;
    DAC3->DHR12RD = 0U;
    DAC3->CR = DAC_CR_EN1 | DAC_CR_EN2;
    OPAMP3->CSR = OPAMP_CSR_VPSEL | OPAMP_CSR_VMSEL | OPAMP_CSR_HIGHSPEEDEN | OPAMP_CSR_OPAMPxEN;
    OPAMP6->CSR = OPAMP_CSR_VPSEL | OPAMP_CSR_VMSEL | OPAMP_CSR_HIGHSPEEDEN | OPAMP_CSR_OPAMPxEN;
    RADIO_AnalogClocks();
    return 1U;
}

__attribute__((cold)) uint8_t RADIO_AnalogPause(void)
{
    RADIO_AnalogStop();
    TIM7->CR1 = 0U;
    DMA2_Channel1->CCR = 0U;
    if (ADC2->CR & ADC_CR_ADSTART) {
        ADC2->CR |= ADC_CR_ADSTP;
        if (!wait_bits(&ADC2->CR, ADC_CR_ADSTART, 0U)) return 0U;
    }
    if (ADC2->CR & ADC_CR_ADEN) {
        ADC2->CR |= ADC_CR_ADDIS;
        if (!wait_bits(&ADC2->CR, ADC_CR_ADEN, 0U)) return 0U;
    }
    ADC2->ISR = ADC_ISR_OVR;
    return 1U;
}

uint8_t RADIO_AnalogRecover(void)
{
    if (!RADIO_AnalogPause()) return 0U;
    rail_bad = 0U;
    return RADIO_AnalogInit();
}

__attribute__((cold)) void RADIO_BiasEnable(uint8_t on)
{
    /* QMX runs its bias buck near 116 kHz and moves the PWM frequency so no
     * harmonic lands near the operating frequency. 100-130 kHz keeps every
     * harmonic >= 50 kHz from the carrier at any SYSCLK the planner uses.
     * Searched before masking interrupts (393 candidates). */
    const radio_config_t *c = RADIO_Config();
    uint32_t top = on ? RADIO_PwmReloadAway(SystemCoreClock, MOD_GetCarrierRequestedHz(), 1307U, 1699U, 1465U) + 1U
                      : bias_top;
    uint16_t target = RADIO_BiasTargetMv(c->bias_ma, telemetry.rail5_mv);
    uint32_t pm = analog_enter_critical();
    /* At RADIO_BIAS_MA_OFF the buck stays off, as in QDX/QMX: the diodes
     * keep the ~4 mA receive bias from R74/R77. */
    if (BOARD_EmergencyLatched() || !target) on = 0U;
    bias_enabled = 0U;
    bias_duty = 0.0f;
    bias_low = bias_high = 0U;
    TIM4->CCR1 = 0U;
    /* Never start above the target: a precharged C73 then only decays. */
    RADIO_BiasLoopStart(&bias_loop, telemetry.bias_mv < target ? telemetry.bias_mv : target);
    if (on) {
        bias_top = top;
        TIM4->ARR = bias_top - 1U; /* ARPE: applied at the next update */
        GPIOD->AFR[1] = (GPIOD->AFR[1] & ~(15UL << 16U)) | (2UL << 16U);
        GPIOD->MODER = (GPIOD->MODER & ~(3UL << 24U)) | (2UL << 24U);
        bias_enabled = 1U;
    } else {
        GPIOD->BRR = GPIO_PIN_12;
        GPIOD->MODER = (GPIOD->MODER & ~(3UL << 24U)) | (1UL << 24U);
    }
    analog_exit_critical(pm);
}

void RADIO_EnvelopeStop(void)
{
    uint32_t pm = analog_enter_critical();
    DAC3->CR &= ~(DAC_CR_DMAEN1 | DAC_CR_TEN1 | DAC_CR_TEN2);
    DMA1_Channel2->CCR &= ~DMA_CCR_EN;
    __DSB();
    DAC3->DHR12RD = 0U;
    analog_exit_critical(pm);
}

__attribute__((cold)) void RADIO_EnvelopeSetCodes(uint16_t code_a, uint16_t code_b)
{
    uint32_t pm = analog_enter_critical();
    RADIO_EnvelopeStop();
    if (!BOARD_EmergencyLatched()) {
        /* Same packing as RADIO_EnvelopeFill: CH2 (branch A) in the high half,
         * CH1 (branch B) in the low half of the dual holding register. */
        DAC3->DHR12RD = ((uint32_t)(code_a & 4095U) << 16U) | (uint32_t)(code_b & 4095U);
    }
    analog_exit_critical(pm);
}

__attribute__((cold)) void RADIO_AnalogStop(void)
{
    RADIO_BiasEnable(0U);
    RADIO_EnvelopeStop();
}

__attribute__((cold)) void RADIO_EnvelopeStart(uint32_t *samples, uint32_t count)
{
    uint32_t pm = analog_enter_critical();
    RADIO_EnvelopeStop();
    if (BOARD_EmergencyLatched() || samples == NULL || count == 0U) {
        analog_exit_critical(pm);
        return;
    }
    DMA1->IFCR = DMA_IFCR_CGIF2;
    DMA1_Channel2->CPAR = (uint32_t)&DAC3->DHR12RD;
    DMA1_Channel2->CMAR = (uint32_t)samples;
    DMA1_Channel2->CNDTR = count;
    DMAMUX1_Channel1->CCR = DMA_REQUEST_DAC3_CHANNEL1;
    DMA1_Channel2->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC |
                         DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1 | DMA_CCR_PL_1 | DMA_CCR_TEIE | DMA_CCR_EN;
    DAC3->SR = DAC_SR_DMAUDR1 | DAC_SR_DMAUDR2;
    uint32_t trig = DAC_CR_TSEL1_2 | DAC_CR_TSEL1_1 | DAC_CR_TSEL1_0 | DAC_CR_TEN1;
    DAC3->CR = DAC_CR_EN1 | DAC_CR_EN2 | DAC_CR_DMAEN1 | trig | (trig << 16U);
    analog_exit_critical(pm);
}

uint8_t RADIO_EnvelopeBoundary(uint32_t half, uint32_t count)
{
    uint32_t left = DMA1_Channel2->CNDTR;
    if ((DAC3->SR & (DAC_SR_DMAUDR1 | DAC_SR_DMAUDR2)) ||
        (DMA1->ISR & DMA_ISR_TEIF2) || !left || left > count) return 0U;
    return half ? (left > count / 2U) : (left <= count / 2U);
}

/* u % period, with the common one-period excess as a subtraction. */
static inline uint32_t envelope_wrap_period(uint32_t u, uint32_t period)
{
    if (u >= period) {
        u -= period;
        if (u >= period) u %= period;
    }
    return u;
}

void RADIO_EnvelopeFill(uint32_t *a, uint32_t *b, uint32_t *env, uint32_t count, uint32_t period, uint8_t active)
{
    const radio_config_t *c = RADIO_Config();
    /* Once per call, not per sample: this runs in the RF DMA interrupt, where
     * the per-sample fminf calls, config reloads and modulos cost a few
     * percent of the CPU that USB-audio SSB needs. The comparisons give
     * fminf's result for these finite (or, for a DAC code, NaN-to-4095)
     * operands, so every output word is what it was. */
    const uint8_t mode = c->envelope_mode;
    const float limit_v = (float)c->envelope_limit_mv;
    const float vin_v = (float)telemetry.vin_mv * 0.95f;
    const float fullscale = ((limit_v < vin_v) ? limit_v : vin_v) * 0.001f * envelope_cap;
    const float per_volt_a = c->envelope_dac_per_volt[0], per_volt_b = c->envelope_dac_per_volt[1];
    const float two_period = 2.0f * (float)period;
    const float spread_per_rad = (float)period / 6.28318530718f;
    const uint8_t decode = (uint8_t)(active && period && mode);
    for (uint32_t i = 0U; i < count; ++i) {
        float ea = active ? 1.0f : 0.0f, eb = ea;
        /* Decode the final compensated outphasing vector. Polar mode replaces
         * its differential phase with drain amplitude; hybrid shares both.
         * Signed shortest distance is essential across carrier wrap. The
         * sine is inlined: out of line, its call cost as much as its
         * polynomial, 48 times a millisecond in the RF interrupt. */
        if (decode) {
            int32_t delta = (int32_t)b[i] - (int32_t)a[i];
            if (delta > (int32_t)period / 2) delta -= (int32_t)period;
            if (delta < -(int32_t)period / 2) delta += (int32_t)period;
            float magnitude = RADIO_SineTurnInline(0.25f + (float)delta / two_period);
            if (magnitude < 0.0f) magnitude = 0.0f;
            float level = mode == 1U ? magnitude : sqrtf(magnitude);
            int32_t mid = (int32_t)a[i] - 96 + delta / 2;
            int32_t spread = mode == 1U ? 0 :
                (int32_t)((1.57079632679f - asinf(level)) * spread_per_rad); /* acos(level), asinf linked anyway */
            if (delta < 0) spread = -spread;
            int32_t ta = mid - spread, tb = mid + spread;
            while (ta < 0) ta += (int32_t)period;
            while (tb < 0) tb += (int32_t)period;
            a[i] = 96U + envelope_wrap_period((uint32_t)ta, period);
            b[i] = 96U + envelope_wrap_period((uint32_t)tb, period);
            ea = eb = level;
        }
        const float code_a = ea * fullscale * per_volt_a;
        const float code_b = eb * fullscale * per_volt_b;
        uint32_t da = (uint32_t)((code_a < 4095.0f) ? code_a : 4095.0f);
        uint32_t db = (uint32_t)((code_b < 4095.0f) ? code_b : 4095.0f);
        env[i] = db | (da << 16U);
    }
}

uint8_t RADIO_AnalogRailOk(void) { return rail_bad < 16U; }

/* The interrupt's view of the configuration: the table (a board's or the
 * built-in one) into RAM, swapped under PRIMASK only when it changed, and
 * the zeros (FWD/REV ZERO, or the learnt rest level while they are 0),
 * scales and limit, each a single word. */
void RADIO_MeterReconfigure(void)
{
    const radio_config_t *c = RADIO_Config();
    rf_meter_lin_t lin;
    RF_MeterLinBuild(RF_MeterTable(c->meter_port_mv), &lin);
    if (memcmp(&lin, &meter_lin, sizeof(lin)) != 0) {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        meter_lin = lin;
        __set_PRIMASK(pm);
    }
    for (unsigned d = 0U; d < 2U; ++d) {
        meter_zero[d] = (c->swr_offset[d] != 0.0f) ? c->swr_offset[d] * 1000.0f : meter_zero_mv[d];
        meter_scale[d] = c->swr_gain[d];
    }
    meter_limit = ((float)c->swr_limit_x100 - 100.0f) / ((float)c->swr_limit_x100 + 100.0f);
    const float floor_v = RF_MeterFloorAt(c->meter_floor_100uv, MOD_GetCarrierRequestedHz());
    meter_floor = floor_v < 0.1f ? floor_v : 0.1f;
}

/* QMX's Tune %: TX TEST, the tune-up carrier, at a fraction of the drain. */
static void meter_set_cap(uint32_t cfg)
{
    envelope_cap = (cfg == (uint32_t)MOD_CFG_TX_CARRIER_TEST) ? (float)RADIO_ConfigTunePct(RADIO_Config()) * 0.01f
                                                               : 1.0f;
}

/* Every main-loop pass, so until there is work it is two compares (bench:
 * the full version cost 1.2 % of the CPU in TX). The cap follows a change
 * of mode at once -- TX TEST keys only after its arming -- and a TUNE %
 * change at the next interval. */
static uint32_t meter_cap_cfg = 0xFFFFFFFFUL;
static uint16_t meter_update_ms = RADIO_METER_UPDATE_DEF_MS;

__attribute__((cold)) void RADIO_MeterService(uint32_t now)
{
    const uint32_t cfg = (uint32_t)MOD_GetTestConfig();
    if (cfg != meter_cap_cfg) {
        meter_cap_cfg = cfg;
        meter_set_cap(cfg);
    }
    if ((uint32_t)(now - meter_taken) < meter_update_ms) return;
    meter_taken = now;
    const radio_config_t *c = RADIO_Config();
    meter_update_ms = RADIO_ConfigMeterUpdateMs(c);
    meter_set_cap(cfg);
    RADIO_MeterReconfigure();
    rf_meter_acc_t a;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    a = meter_acc;
    memset(&meter_acc, 0, sizeof(meter_acc));
    meter_plateau = RF_METER_PLATEAU * a.peak;
    const uint32_t rest_n = meter_rest_n, rest[2] = {meter_rest_sum[0], meter_rest_sum[1]};
    meter_rest_n = 0U;
    meter_rest_sum[0] = meter_rest_sum[1] = 0U;
    __set_PRIMASK(pm);
    /* The rest level, ~1 s average; not while a calibration injects
     * TEST_CLK, nor from a few scans. */
    if (rest_n >= 100U && !RADIO_CalActive()) {
        for (unsigned d = 0U; d < 2U; ++d) {
            float mv = (float)rest[d] * 0.25f / (float)rest_n;
            if (mv > 30.0f) mv = 30.0f;
            meter_zero_mv[d] = (rf_meter.zero_seen ? 0.8f * meter_zero_mv[d] : 0.0f) +
                               (rf_meter.zero_seen ? 0.2f : 1.0f) * mv;
            rf_meter.zero_mv[d] = meter_zero_mv[d];
        }
        rf_meter.zero_seen = 1U;
    }
    if (a.n < RF_METER_MIN_SCANS) return; /* no RF this interval: the last reading stands */
    const uint32_t hz = MOD_GetCarrierRequestedHz();
    const mod_type_t type = MOD_GetModeType((mod_config_t)cfg);
    RF_MeterReading(&a, RF_MeterGainAt(c->meter_gain_x1000, hz), RF_MeterLeakAt(c->meter_leak_x10000, hz),
                    RF_MeterFloorAt(c->meter_floor_100uv, hz), (uint8_t)(type == MOD_TYPE_SSB || type == MOD_TYPE_AM),
                    &rf_meter.last);
    rf_meter.hz = hz;
    rf_meter.tick = now;
    ++rf_meter.seq;
}

uint8_t RADIO_MeterRecent(uint32_t now, uint32_t max_age_ms, rf_meter_reading_t *out)
{
    if (rf_meter.seq == 0U || (uint32_t)(now - rf_meter.tick) > max_age_ms) return 0U;
    if (out) *out = rf_meter.last;
    return 1U;
}

void RADIO_AnalogSnapshot(radio_telemetry_t *t)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    *t = telemetry;
    __set_PRIMASK(pm);
}

/* Why the telemetry ADC gave up, split by cause: a DMA transfer error, a
 * handler so late that a whole half-buffer passed under it, or an ADC
 * overrun. They mean different things and the fault code cannot say which.
 * Counted, never latched; CPU-only, so CCM. */
#if MOD_DEEP_DIAG_ENABLE
/* Not readable in this flavour (its telem line is compiled out), so counting
 * would be write-only storage on an image with no room for it. */
#define adc_bump(p) ((void)0)
void RADIO_AnalogFailCounts(uint32_t *te, uint32_t *late, uint32_t *ovr)
{
    if (te) *te = 0U;
    if (late) *late = 0U;
    if (ovr) *ovr = 0U;
}
#else
static volatile uint16_t adc_fail_te __attribute__((section(".ccmram_bss")));
static volatile uint16_t adc_fail_late __attribute__((section(".ccmram_bss")));
static volatile uint16_t adc_fail_ovr __attribute__((section(".ccmram_bss")));
static void adc_bump(volatile uint16_t *v) { if (*v < 0xFFFFU) ++*v; }

void RADIO_AnalogFailCounts(uint32_t *te, uint32_t *late, uint32_t *ovr)
{
    if (te) *te = adc_fail_te;
    if (late) *late = adc_fail_late;
    if (ovr) *ovr = adc_fail_ovr;
}
#endif

void RADIO_MeterHoldDetectors(void)
{
    const uint32_t pm = analog_enter_critical();
    meter_rest_hold = 400U;
    analog_exit_critical(pm);
}

/* radio_guard_logic.h's trips, out of line: with TX off, VIN learnt and
 * the bridge watched for RF nobody keyed; with TX permitted, VIN's floor.
 * With no LPF path conducting -- SETUP, PROTECTION, a calibration's filter
 * steps -- R74/R77 lift the PIN bias network and both detectors sit near
 * 0.44 V DC (bench, 2026-10-02): not RF, and not their rest level either. So
 * the bridge is judged, and its rest learnt, only in RECEIVE with an LPF
 * selected and no calibration, 100 ms (the detectors' hold) after anything
 * else, after RF and after a filter switch. */
static __attribute__((noinline)) void adc_guard_scan(uint16_t vin_mv, uint8_t rf_active)
{
    if (!BOARD_RfPermitted()) {
        if (RADIO_State() != RADIO_RECEIVE || RADIO_CalActive() || !RADIO_SelectedLpfBits()) meter_rest_hold = 400U;
        const uint8_t judge = (uint8_t)(!rf_active && !meter_rest_hold);
        const float f = judge ? rf_meter_lin_v(&meter_lin, (float)meter_sum[0] * 0.25f - meter_zero[0]) * meter_scale[0]
                              : 0.0f;
        if (radio_guard_idle(&guard, vin_mv, judge, f)) RADIO_Trip(RADIO_FAULT_SWR);
    } else if (radio_guard_tx_vin(&guard, vin_mv)) {
        RADIO_Trip(RADIO_FAULT_VIN);
    }
}

static inline __attribute__((always_inline)) void adc_scan_irq(void)
{
    uint32_t flags = DMA2->ISR & (DMA_ISR_HTIF1 | DMA_ISR_TCIF1 | DMA_ISR_TEIF1);
    DMA2->IFCR = DMA_IFCR_CGIF1;
    if ((flags & DMA_ISR_TEIF1) || flags == (DMA_ISR_HTIF1 | DMA_ISR_TCIF1) ||
        (ADC2->ISR & ADC_ISR_OVR)) {
        if (flags & DMA_ISR_TEIF1) adc_bump(&adc_fail_te);
        else if (flags == (DMA_ISR_HTIF1 | DMA_ISR_TCIF1)) adc_bump(&adc_fail_late);
        else adc_bump(&adc_fail_ovr);
        RADIO_Trip(RADIO_FAULT_ADC);
        return;
    }
    if (!(flags & (DMA_ISR_HTIF1 | DMA_ISR_TCIF1))) return;
    uint16_t *raw = &adc_samples[(flags & DMA_ISR_TCIF1) ? 7U : 0U];
    uint32_t ref = MOD_GetVrefMillivolts();
    uint16_t mv[7];
    for (unsigned i = 0U; i < 7U; ++i) mv[i] = (uint16_t)((raw[i] * ref + 2047UL) / 4095UL);
    /* The bias level everyone else reads is the 4 ms mean: one scan scatters
     * ~27 mV rms, which kept the +-30 mV TX-entry window from ever holding for
     * 10 ms. The loop and the trips below stay on this scan's mv[0]. */
    telemetry.bias_mv = RADIO_BiasAverageStep(&bias_avg, mv[0]);
    telemetry.fwd_mv = mv[1]; telemetry.rev_mv = mv[2];
    for (unsigned d = 0U; d < 2U; ++d) {
        meter_sum[d] += (uint32_t)mv[1U + d] - meter_hist[d][meter_k];
        meter_hist[d][meter_k] = mv[1U + d];
    }
    meter_k = (uint8_t)((meter_k + 1U) & 3U);
    telemetry.rail5_mv = mv[3] * 2U;
    telemetry.env_a_mv = (uint16_t)(mv[4] * 57UL / 10UL);
    telemetry.env_b_mv = (uint16_t)(mv[5] * 57UL / 10UL);
    telemetry.vin_mv = (uint16_t)(mv[6] * 57UL / 10UL);
    telemetry.tick = HAL_GetTick(); ++telemetry.sequence;
    const radio_config_t *c = RADIO_Config();
    if (telemetry.vin_mv > c->vin_limit_mv || raw[6] >= 4090U) RADIO_Trip(RADIO_FAULT_VIN);
    if (telemetry.rail5_mv < 4400U || telemetry.rail5_mv > 5500U) {
        if (rail_bad < 16U) ++rail_bad;
        /* The LM1084-5 needs about 6.3 V in, so the USB-C 5 V start-up
         * contract leaves this rail near 3.7 V until PD raises VIN. A low
         * rail only inhibits TX (RADIO_AnalogRailOk); an overvoltage rail or
         * a sag while transmitting is a protection fault. */
        if (rail_bad == 16U && (telemetry.rail5_mv > 5500U || BOARD_RfActive()))
            RADIO_Trip(RADIO_FAULT_5V);
    } else rail_bad = 0U;
    /* R74/R77 (1 k to 5 V) lift TX_BIAS whenever no LPF path conducts, so a
     * high reading with the buck idle is not a converter fault. 2 ms of
     * persistence lets a precharged C73 decay; a runaway still trips. */
    if (bias_enabled && bias_duty > 0.5f && mv[0] > c->bias_limit_mv) { if (bias_high < 8U) ++bias_high; }
    else bias_high = 0U;
    if (bias_high >= 8U) RADIO_Trip(RADIO_FAULT_BIAS);
    const uint16_t bias_target = RADIO_BiasTargetMv(c->bias_ma, telemetry.rail5_mv);
    if (telemetry.env_a_mv > c->envelope_limit_mv + 500U ||
        telemetry.env_b_mv > c->envelope_limit_mv + 500U) RADIO_Trip(RADIO_FAULT_ENV);
    if (BOARD_RfActive()) {
        /* A collapsed bias must stop RF within 10 ms; brief dips from load
         * steps recover faster than that. */
        if (mv[0] + 100U < bias_target) { if (bias_low < 40U) ++bias_low; } else bias_low = 0U;
        if (bias_low >= 40U || (bias_target && !bias_enabled)) RADIO_Trip(RADIO_FAULT_BIAS);
        /* The detectors as port peak volts: each 1 ms mean less its rest
         * level, through the diode table, times its scale. ADC full scale is
         * the 2.9 V VREFBUF. A clipped reflected detector is a real mismatch;
         * a clipped forward detector only understates forward power, which
         * overstates |gamma| and errs towards tripping. Two milliseconds of
         * persistence ignore detector skew at key edges. */
        const float f = rf_meter_lin_v(&meter_lin, (float)meter_sum[0] * 0.25f - meter_zero[0]) * meter_scale[0];
        const float r = rf_meter_lin_v(&meter_lin, (float)meter_sum[1] * 0.25f - meter_zero[1]) * meter_scale[1];
        meter_rest_hold = 400U;
        const float r_trip = r - meter_floor;
        if (raw[2] >= 4090U || rf_meter_swr_over(f, r_trip, meter_limit)) { if (swr_bad < 8U) ++swr_bad; } else swr_bad = 0U;
        if (swr_bad >= 8U) RADIO_Trip(RADIO_FAULT_SWR);
        if (radio_guard_swr(&guard, f, r_trip, (uint8_t)(MOD_GetTestConfig() == MOD_CFG_TX_CARRIER_TEST)))
            RADIO_Trip(RADIO_FAULT_SWR);
        rf_meter_acc_add(&meter_acc, f, r, 0.5f * (float)((uint32_t)telemetry.env_a_mv + telemetry.env_b_mv),
                         (float)telemetry.vin_mv, meter_plateau);
    } else {
        swr_bad = 0U;
        if (meter_rest_hold) --meter_rest_hold;
        else if (meter_rest_n < 0xFFFFU) {
            meter_rest_sum[0] += meter_sum[0];
            meter_rest_sum[1] += meter_sum[1];
            ++meter_rest_n;
        }
    }
    adc_guard_scan(telemetry.vin_mv, BOARD_RfActive());
    if (bias_enabled) {
        /* Conservative bounded integral controller at 4 kHz. No unbounded
         * wind-up; duty reaches zero immediately on a trip. Bench tuning is
         * required before marking an LPF qualified. */
        bias_duty = RADIO_BiasLoopStep(&bias_loop, mv[0], bias_target, c->bias_limit_mv, telemetry.vin_mv);
        TIM4->CCR1 = (uint32_t)(bias_duty * (float)bias_top / 1024.0f + 0.5f);
        if (RADIO_BiasEnvelopeFault(&bias_loop, bias_duty, mv[0], telemetry.vin_mv)) RADIO_Trip(RADIO_FAULT_BIAS);
    }
    telemetry.bias_duty = bias_enabled ? (uint16_t)(bias_duty + 0.5f) : 0U;
}

/* Every exit marks a SOF this interrupt held up (radio_clocks.c). */
void DMA2_Channel1_IRQHandler(void)
{
    adc_scan_irq();
    RADIO_RefSofIsrReturn();
}

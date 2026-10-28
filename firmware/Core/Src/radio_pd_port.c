#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "main.h"
#include "radio.h"
#include "radio_analog.h"
#include "radio_pd.h"
#include "board_rev1.h"
#include "usbpd_hw_if.h"
#include "usbpd_timersserver.h"
#include "usbpd_devices_conf.h"

static uint16_t timer_start[TIM_MAX], timer_duration[TIM_MAX];
static uint8_t timer_ready;
static void (*vbus_callback)(uint32_t, USBPD_PWR_VBUSConnectionStatusTypeDef);
static uint8_t vbus_previous;

static uint16_t pd_counter(void)
{
    /* LPTIM is asynchronous to APB: read until two consecutive values agree. */
    uint16_t a = (uint16_t)LPTIM1->CNT, b = a;
    /* A free-running timer can cross a tick between reads forever in a
     * pathological bus/debug state. Four samples are enough for a coherent
     * read at 1 MHz while keeping every USB-PD callback bounded. */
    for (unsigned n = 0U; n < 4U; ++n) {
        b = a;
        a = (uint16_t)LPTIM1->CNT;
        if (a == b) return a;
    }
    return a;
}

void USBPD_TIM_Init(void)
{
    if (timer_ready) return;
    __HAL_RCC_HSI_ENABLE();
    for (unsigned n = 0U; n < 1000000U && !(RCC->CR & RCC_CR_HSIRDY); ++n) __NOP();
    if (!(RCC->CR & RCC_CR_HSIRDY)) return;
    MODIFY_REG(RCC->CCIPR, RCC_CCIPR_LPTIM1SEL, RCC_CCIPR_LPTIM1SEL_1);
    __HAL_RCC_LPTIM1_CLK_ENABLE();
    LPTIM1->CR = 0U;
    LPTIM1->CFGR = LPTIM_CFGR_PRESC_2; /* HSI16 / 16 = 1 MHz, independent of RF PLL */
    LPTIM1->CR = LPTIM_CR_ENABLE;
    LPTIM1->ARR = 65535U;
    for (unsigned n = 0U; n < 100000U && !(LPTIM1->ISR & LPTIM_ISR_ARROK); ++n) __NOP();
    if (!(LPTIM1->ISR & LPTIM_ISR_ARROK)) return;
    LPTIM1->ICR = LPTIM_ICR_ARROKCF;
    LPTIM1->CR |= LPTIM_CR_CNTSTRT;
    timer_ready = 1U;
}
void USBPD_TIM_DeInit(void) { /* Retain independent timebase across cable detach. */ }
uint16_t RADIO_PdMicros(void) { return timer_ready ? pd_counter() : 0U; }
uint8_t RADIO_PdTimerReady(void) { return timer_ready; }
void USBPD_TIM_Start(TIM_identifier id, uint32_t us)
{
    if ((unsigned)id >= TIM_MAX) return;
    timer_start[id] = pd_counter();
    timer_duration[id] = (uint16_t)(us > 60000U ? 60000U : us);
}
uint32_t USBPD_TIM_IsExpired(TIM_identifier id)
{
    return (unsigned)id >= TIM_MAX || !timer_ready ||
        (uint16_t)(pd_counter() - timer_start[id]) >= timer_duration[id];
}

uint16_t HW_IF_PWR_GetVoltage(uint8_t port)
{
    (void)port;
    radio_telemetry_t t;
    RADIO_AnalogSnapshot(&t);
    return t.vin_mv;
}
int16_t HW_IF_PWR_GetCurrent(uint8_t port) { (void)port; return 0; }
USBPD_StatusTypeDef HW_IF_PWR_SetVoltage(uint8_t port, uint16_t mv)
{ (void)port; (void)mv; return USBPD_ERROR; }
USBPD_FunctionalState HW_IF_PWR_VBUSIsEnabled(uint8_t port)
{ (void)port; return USBPD_DISABLE; }

uint8_t USBPD_PWR_IF_GetVBUSStatus(uint8_t port, USBPD_VBUSPOWER_STATUS status)
{
    uint32_t mv = HW_IF_PWR_GetVoltage(port);
    /* ADC_VIN is the combined rail. Never fabricate vSafe0V when external DC
     * masks VBUS. External DC can therefore prevent attachment/HR completion. */
    if (status == USBPD_PWR_BELOWVSAFE0V) return mv < 800U;
    if (status == USBPD_PWR_VSAFE5V) return mv >= 4400U && mv <= 5500U;
    return mv < 3670U;
}
int32_t BSP_USBPD_PWR_Init(uint32_t port) { return port ? -1 : 0; }
int32_t BSP_USBPD_PWR_VBUSInit(uint32_t port) { return port ? -1 : 0; }
int32_t BSP_USBPD_PWR_VBUSDeInit(uint32_t port) { return port ? -1 : 0; }
int32_t BSP_USBPD_PWR_VCONNInit(uint32_t port, uint32_t cc)
{ (void)port; (void)cc; return -1; }
int32_t BSP_USBPD_PWR_VCONNDeInit(uint32_t port, uint32_t cc)
{ (void)port; (void)cc; return 0; }
int32_t BSP_USBPD_PWR_RegisterVBUSDetectCallback(uint32_t port,
    void (*callback)(uint32_t, USBPD_PWR_VBUSConnectionStatusTypeDef))
{
    if (port) return -1;
    vbus_callback = callback;
    vbus_previous = 255U;
    return 0;
}
void RADIO_PdPortPoll(void)
{
    uint8_t present = !USBPD_PWR_IF_GetVBUSStatus(0U, USBPD_PWR_SNKDETACH);
    if (present == vbus_previous || !vbus_callback) return;
    vbus_previous = present;
    vbus_callback(0U, present ? VBUS_CONNECTED : VBUS_NOT_CONNECTED);
}

/* RAM-only override owned by the CC probe in radio_pd.c. Nothing persists it,
 * so a power cycle always returns to whatever the stored setting asks for --
 * which for a board with R3/R4 fitted is the termination that lets an
 * unpowered radio start from USB-C in the first place. */
static uint8_t cc_force;

uint32_t REV1_UcpdCcEnable(uint32_t st_value)
{
    /* R3/R4 already present Rd (they let a USB-C source start the unpowered
     * board). Paralleling the internal 5.1 k Rd would give 2.55 k, below a
     * 3 A source's vRd threshold, so the UCPD pulls stay off in that case --
     * but then the CC analog PHY is off too and the Type-C detector reads
     * vOPEN on both lines. The boot sense applies the pulls for one millisecond
     * only; direct BMC startup then uses R3/R4 alone. */
    return (RADIO_Config()->pd_internal_rd || cc_force) ? st_value : LL_UCPD_CCENABLE_NONE;
}

void RADIO_PdApplyCcTermination(void)
{
    if (!(UCPD1->CFG1 & UCPD_CFG1_UCPDEN)) return; /* CAD_Init applies it */
    uint32_t pm = __get_PRIMASK();
    __disable_irq(); /* the CAD owning Ports[0] runs from SysTick */
    CCxPin_TypeDef cc = Ports[0].CCx;
    uint32_t value = cc == CC1 ? LL_UCPD_CCENABLE_CC1 :
                     cc == CC2 ? LL_UCPD_CCENABLE_CC2 : LL_UCPD_CCENABLE_CC1CC2;
    LL_UCPD_SetccEnable(UCPD1, REV1_UcpdCcEnable(value));
    __set_PRIMASK(pm);
}

void RADIO_PdSetCcForce(uint8_t on)
{
    cc_force = on ? 1U : 0U;
    RADIO_PdApplyCcTermination();
}

/* The sense window applies the internal Rd for about a millisecond, which is
 * a real CC transition as far as the peripheral is concerned. Mask the IRQ
 * and discard the events it creates; radio_pd.c also keeps CAD disabled after
 * the window because TYPEC_VSTATE retains its last debounced value when
 * CCENABLE goes to zero. */
void RADIO_PdCcSenseBegin(void)
{
    NVIC_DisableIRQ(UCPD1_IRQn);
    RADIO_PdSetCcForce(1U);
}
void RADIO_PdCcSenseEnd(void)
{
    RADIO_PdSetCcForce(0U);
    UCPD1->ICR = UCPD_ICR_TYPECEVT1CF | UCPD_ICR_TYPECEVT2CF;
    NVIC_ClearPendingIRQ(UCPD1_IRQn);
    NVIC_EnableIRQ(UCPD1_IRQn);
}
uint8_t RADIO_PdCcForced(void) { return cc_force; }

/* Both Type-C detector fields, read as one SR sample so the two lines always
 * describe the same instant. Bits 1:0 are CC1, bits 3:2 are CC2. */
uint8_t RADIO_PdCcState(void)
{
    const uint32_t sr = UCPD1->SR;
    return (uint8_t)(((sr & UCPD_SR_TYPEC_VSTATE_CC1) >> UCPD_SR_TYPEC_VSTATE_CC1_Pos) |
                     (((sr & UCPD_SR_TYPEC_VSTATE_CC2) >> UCPD_SR_TYPEC_VSTATE_CC2_Pos) << 2U));
}

void RADIO_PdPowerHold(uint8_t on)
{
    if (on) GPIOB->BSRR = BOARD_POWER_HOLD_PIN;
    else GPIOB->BRR = BOARD_POWER_HOLD_PIN;
}

/* The combined USB/DC rail, which is all this PCB can measure. */
uint16_t RADIO_PdVbusMv(void) { return HW_IF_PWR_GetVoltage(0U); }

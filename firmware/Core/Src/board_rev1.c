#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "board_rev1.h"
#include "radio_config.h"

static volatile uint8_t rf_permit;
static volatile uint8_t rf_active;
static volatile uint8_t emergency_latched;
/* PC10 (branch A) and PC6 (branch B) drive BSS123 inverters (Q30/Q7, 10 k
 * gate pull-downs) whose drains carry 470 ohm pull-ups to the 5 V rail
 * (R32/R145, reworked from VIN on 2026-09-16; see docs/REV1.md finding 1):
 *   MCU high -> enable node at GND: UCC27524 ENx low = disabled, outputs held
 *               low; MC74ACT541 OE low = enabled, outputs follow the RF inputs.
 *   MCU low  -> enable node at 5 V: UCC27524 enabled; MC74ACT541 outputs go
 *               high-Z, leaving the PA gates floating (no gate pull-downs).
 * MCU high is therefore the idle/fault state for both assemblies (the RF
 * inputs are forced low at the same time). Only a UCC27524 branch drives its
 * enable low, and only while RF is active. */
static uint32_t rf_active_low_pins;
static uint8_t button_active_high;
static uint8_t button_locked;
static uint8_t button_probe_high;
static uint8_t button_probe_hits[2];
static uint32_t button_probe_at;

/* The RF interlock is shared by foreground state transitions and the ADC/DMA
 * protection path.  Keep the decision and the GPIO mode/output writes in one
 * short critical section so a protection interrupt cannot observe a partly
 * enabled transmitter.  Host policy tests replace the CMSIS primitives with
 * no-op stubs; the production path always uses PRIMASK. */
static uint32_t board_enter_critical(void)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    return 0U;
#else
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    return pm;
#endif
}

static void board_exit_critical(uint32_t pm)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    (void)pm;
#else
    __set_PRIMASK(pm);
#endif
}

static void board_data_sync(void)
{
#if !defined(MOD_HOST_TEST_CRITICAL_STUBS)
    __DSB();
#endif
}

void BOARD_EarlyInit(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;
    (void)RCC->AHB2ENR;
    /* Set the latch before enabling the output, including watchdog resets. */
    GPIOB->BSRR = GPIO_PIN_10;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 20U)) | (1UL << 20U);
    GPIOB->BRR = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14;
    GPIOB->MODER = (GPIOB->MODER & ~(63UL << 24U)) | (21UL << 24U);
    GPIOA->BRR = GPIO_PIN_8 | GPIO_PIN_9;
    /* Before settings load: MCU-high enables are safe for either driver. */
    GPIOC->BRR = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9;
    GPIOC->BSRR = BOARD_RF_ENABLE_PINS;
    GPIOA->MODER = (GPIOA->MODER & ~(15UL << 16U)) | (5UL << 16U);
    GPIOC->MODER = (GPIOC->MODER & ~(0x3FFUL << 12U)) | (0x155UL << 12U);
}

static void rf_inputs_low(void)
{
    GPIOA->BRR = BOARD_RF_A_PINS;
    GPIOC->BRR = BOARD_RF_B_PINS;
    GPIOA->MODER = (GPIOA->MODER & ~(15UL << 16U)) | (5UL << 16U);
    GPIOC->MODER = (GPIOC->MODER & ~(15UL << 16U)) | (5UL << 16U);
}

static void rf_enables_write(uint32_t low_pins)
{
    low_pins &= BOARD_RF_ENABLE_PINS;
    GPIOC->BSRR = (BOARD_RF_ENABLE_PINS & ~low_pins) | (low_pins << 16U);
    GPIOC->MODER = (GPIOC->MODER & ~((3UL << 12U) | (3UL << 20U))) | (1UL << 12U) | (1UL << 20U);
}

static void gpio(GPIO_TypeDef *port, uint32_t pins, uint32_t mode, uint32_t pull)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = pins;
    g.Mode = mode;
    g.Pull = pull;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &g);
}

void BOARD_GPIOInit(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    BOARD_EarlyInit();
    GPIOB->BRR = GPIO_PIN_7 | GPIO_PIN_9;
    GPIOE->BRR = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
    GPIOD->BRR = GPIO_PIN_0 | GPIO_PIN_1 | BOARD_AUX_PINS | GPIO_PIN_12;
    GPIOA->BSRR = GPIO_PIN_4;
    GPIOE->BSRR = GPIO_PIN_10 | GPIO_PIN_11;
    gpio(GPIOB, GPIO_PIN_7 | GPIO_PIN_9, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);
    gpio(GPIOE, GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 |
         GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_13, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);
    gpio(GPIOD, GPIO_PIN_0 | GPIO_PIN_1 | BOARD_AUX_PINS | GPIO_PIN_12,
         GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);
    gpio(GPIOA, GPIO_PIN_4, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);
    /* LCD on SPI4: PE12 SCK and PE14 MOSI (AF5); PE13, the SPI4 MISO pin, is
     * the DC GPIO. The 10.6 MHz bus and its CS/DC/RST lines get fast edges,
     * as do the SD CS line and SPI1 (configured by the SD driver). */
    GPIOE->AFR[1] = (GPIOE->AFR[1] & ~((15UL << 16U) | (15UL << 24U))) | (5UL << 16U) | (5UL << 24U);
    GPIOE->OSPEEDR |= 0x3FFUL << 20U;
    GPIOE->MODER = (GPIOE->MODER & ~((3UL << 24U) | (3UL << 28U))) | (2UL << 24U) | (2UL << 28U);
    GPIOA->OSPEEDR |= 3UL << 8U;
    gpio(GPIOE, GPIO_PIN_0 | GPIO_PIN_1, GPIO_MODE_INPUT, GPIO_PULLUP);
    BOARD_SetButtonPolarity(0U);
    /* Analog/header pins stay non-driving. SWD, NRST and BOOT0 retain their
     * recovery roles; PF0 remains the external HSE input. */
    gpio(GPIOA, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_10,
         GPIO_MODE_ANALOG, GPIO_NOPULL);
    gpio(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_6 |
         GPIO_PIN_11 | GPIO_PIN_15, GPIO_MODE_ANALOG, GPIO_NOPULL);
    gpio(GPIOC, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 |
         GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15,
         GPIO_MODE_ANALOG, GPIO_NOPULL);
    gpio(GPIOD, GPIO_PIN_7 | GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15,
         GPIO_MODE_ANALOG, GPIO_NOPULL);
    gpio(GPIOE, GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_15, GPIO_MODE_ANALOG, GPIO_NOPULL);
    gpio(GPIOF, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_9 | GPIO_PIN_10, GPIO_MODE_ANALOG, GPIO_NOPULL);
    /* AF selection is prepared while TX pins remain low GPIOs. */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~0xFFUL) | 0xDDUL;
    GPIOC->AFR[1] = (GPIOC->AFR[1] & ~0xFFUL) | 0x33UL;
    GPIOA->OSPEEDR |= 15UL << 16U;
    GPIOC->OSPEEDR |= 15UL << 16U;
}

void BOARD_EmergencyStop(void)
{
    uint32_t pm = board_enter_critical();
    emergency_latched = 1U;
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN | RCC_AHB2ENR_GPIODEN;
    (void)RCC->AHB2ENR;
    /* Low RF inputs plus high enable pins keep either driver's outputs low. */
    rf_inputs_low();
    GPIOC->BRR = BOARD_RX_ENABLE_PIN;
    rf_enables_write(0U);
    GPIOD->BRR = BOARD_AUX_PINS | GPIO_PIN_12;
    GPIOD->MODER = (GPIOD->MODER & ~(3UL << 24U)) | (1UL << 24U);
    if ((RCC->APB2ENR & RCC_APB2ENR_HRTIM1EN) != 0U)
        HRTIM1->sCommonRegs.ODISR = BOARD_TX_OUTPUTS | BOARD_RX_OUTPUTS | BOARD_TEST_OUTPUT;
    GPIOB->BRR = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14;
    GPIOB->MODER = (GPIOB->MODER & ~(63UL << 24U)) | (21UL << 24U);
    /* DAC3 is fed by DMA1 channel 2. Disable it before clearing DAC trigger
     * and data registers so a pending circular transfer cannot reassert an
     * envelope after this latch. */
    if ((RCC->AHB1ENR & RCC_AHB1ENR_DMA1EN) != 0U) {
        DMA1_Channel2->CCR &= ~DMA_CCR_EN;
        board_data_sync();
        DMA1->IFCR = DMA_IFCR_CGIF2;
    }
    if ((RCC->AHB2ENR & RCC_AHB2ENR_DAC3EN) != 0U) {
        DAC3->CR &= ~(DAC_CR_DMAEN1 | DAC_CR_TEN1 | DAC_CR_TEN2);
        board_data_sync();
        DAC3->DHR12RD = 0U;
    }
    rf_permit = 0U;
    rf_active = 0U;
    board_exit_critical(pm);
}

uint8_t BOARD_EmergencyLatched(void) { return emergency_latched; }
void BOARD_ClearEmergency(void) { emergency_latched = 0U; }

void BOARD_SetRfDrivers(uint8_t ucc27524_mask, uint8_t act541_mask)
{
    uint32_t pm = board_enter_critical();
    uint32_t ucc = (uint32_t)ucc27524_mask & ~(uint32_t)act541_mask & 3U;
    rf_permit = 0U;
    rf_active = 0U;
    rf_inputs_low();
    rf_active_low_pins = ((ucc & 1U) ? GPIO_PIN_10 : 0U) | ((ucc & 2U) ? GPIO_PIN_6 : 0U);
    rf_enables_write(0U);
    board_exit_critical(pm);
}

void BOARD_PermitRf(uint8_t permit)
{
    uint32_t pm = board_enter_critical();
    rf_permit = (permit && !emergency_latched) ? 1U : 0U;
    if (!rf_permit) {
        rf_inputs_low();
        rf_enables_write(0U);
        rf_active = 0U;
    }
    board_exit_critical(pm);
}

void BOARD_WriteRf(uint8_t active)
{
    uint32_t pm = board_enter_critical();
    active = (active && rf_permit && !emergency_latched) ? 1U : 0U;
    if (!active) {
        /* Inputs low first, then disable: no enabled driver sees an AF edge. */
        rf_inputs_low();
        rf_enables_write(0U);
    } else {
        GPIOC->BRR = BOARD_RX_ENABLE_PIN;
        rf_enables_write(rf_active_low_pins);
        GPIOA->MODER = (GPIOA->MODER & ~(15UL << 16U)) | (10UL << 16U);
        GPIOC->MODER = (GPIOC->MODER & ~(15UL << 16U)) | (10UL << 16U);
    }
    rf_active = active;
    board_exit_critical(pm);
}

uint8_t BOARD_RfActive(void) { return rf_active; }
uint8_t BOARD_RfPermitted(void) { return rf_permit; }

void BOARD_SetRx(uint8_t active)
{
    uint32_t pm = board_enter_critical();
    if (rf_active || rf_permit || emergency_latched) active = 0U;
    GPIOC->BSRR = active ? BOARD_RX_ENABLE_PIN : ((uint32_t)BOARD_RX_ENABLE_PIN << 16U);
    board_exit_critical(pm);
}

#define BUTTON_PROBE_MS   4U
#define BUTTON_PROBE_HITS 3U

static void button_pulls(uint8_t pull_down)
{
    /* PD2..PD6 PUPDR: 01 pull-up, 10 pull-down. Inputs stay inputs. */
    GPIOD->PUPDR = (GPIOD->PUPDR & ~(0x3FFUL << 4U)) |
                   ((pull_down ? 0x2AAUL : 0x155UL) << 4U);
}

void BOARD_SetButtonPolarity(uint8_t active_high)
{
    active_high = active_high ? 1U : 0U;
    if (button_locked && active_high == button_active_high) return;
    /* A request that disagrees with the learned polarity may be a wrong
     * setting; relearn from the next press rather than locking the UI out. */
    gpio(GPIOD, BOARD_BUTTON_PINS, GPIO_MODE_INPUT,
         active_high ? GPIO_PULLDOWN : GPIO_PULLUP);
    button_active_high = active_high;
    button_probe_high = active_high;
    button_locked = 0U;
    button_probe_hits[0] = button_probe_hits[1] = 0U;
}

uint8_t BOARD_ButtonService(uint32_t now, uint8_t *active_high)
{
    if (!button_locked && now - button_probe_at >= BUTTON_PROBE_MS) {
        /* Sample after the pull has settled: under a pull-up only a line held
         * at GND reads low, under a pull-down only a line held at 3.3 V reads
         * high. Floating idle lines never look pressed in either phase. */
        uint32_t idr = GPIOD->IDR & BOARD_BUTTON_PINS;
        uint8_t pressed = (button_probe_high ? idr : (~idr & BOARD_BUTTON_PINS)) != 0U;
        uint8_t *hits = &button_probe_hits[button_probe_high];
        *hits = pressed ? (uint8_t)(*hits + 1U) : 0U;
        if (*hits >= BUTTON_PROBE_HITS) {
            button_active_high = button_probe_high;
            button_locked = 1U;
        } else {
            button_probe_high ^= 1U;
            button_pulls(button_probe_high);
        }
        button_probe_at = now;
    }
    if (active_high) *active_high = button_active_high;
    return button_locked;
}

uint8_t BOARD_ButtonPressed(uint8_t index)
{
    if (index >= 5U || !button_locked) return 0U;
    uint8_t high = (GPIOD->IDR & (1UL << (index + 2U))) ? 1U : 0U;
    return high == button_active_high;
}

/* ---- Spare jumper pins and the optional rotary encoder ----
 *
 * Every entry is a pin BOARD_GPIOInit parks as analog and that reaches a
 * header. PA10 (JP20-1) is deliberately absent: it is UCPD1_DBCC2 and has to
 * stay un-pulled so the dead-battery Rd cannot engage. PD11/PD13 are not
 * connected and PD14/PD15 are shorted together by R148, so none of them is a
 * usable independent input. The order is persisted in settings. */
typedef struct {
    GPIO_TypeDef *port;
    uint8_t bit;
    char label[5];
} board_free_pin_t;

static const board_free_pin_t board_free_pins[] = {
    {NULL,   0U, "OFF"},
    {GPIOB,  0U, "PB0"},  {GPIOB, 15U, "PB15"},                        /* JP20 2..3 */
    {GPIOC, 11U, "PC11"}, {GPIOC, 12U, "PC12"}, {GPIOC, 13U, "PC13"},  /* JP20 4..6 */
    {GPIOC, 14U, "PC14"}, {GPIOC, 15U, "PC15"},                        /* JP20 7..8 */
    {GPIOD,  7U, "PD7"},                                               /* JP19-8 */
    {GPIOE, 15U, "PE15"}, {GPIOE,  8U, "PE8"},  {GPIOE,  7U, "PE7"},   /* JP7 1..3 */
    {GPIOF, 10U, "PF10"}, {GPIOF,  9U, "PF9"},  {GPIOF,  2U, "PF2"},   /* JP7 4..6 */
    {GPIOF,  1U, "PF1"}                                                /* JP7-7 */
};
#define BOARD_FREE_PIN_COUNT (sizeof(board_free_pins) / sizeof(board_free_pins[0]))
_Static_assert(BOARD_FREE_PIN_COUNT == RADIO_ENC_PIN_COUNT,
               "settings encode the spare-pin table index; keep both in step");
_Static_assert(RADIO_ENC_PIN_PC11 == 3U && RADIO_ENC_PIN_PC12 == 4U &&
               RADIO_ENC_PIN_PC13 == 5U, "default encoder pins moved in the table");

/* Quadrature transitions for state (A << 1) | B, indexed (previous << 2) | now.
 * A two-bit jump is contact bounce or a missed sample and counts as zero. */
static const int8_t enc_transition[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

#define ENC_SW_DEBOUNCE_MS 20U
#define ENC_SW_LONG_MS     600U

/* A debounced push key sampled by the 1 kHz tick: the encoder's shaft switch
 * and the optional FUNC key. A press that turns the knob before release is a
 * gesture of its own and is neither a click nor a long press. */
typedef struct {
    uint8_t  raw, stable, long_sent, turned;
    uint32_t edge_ms, press_ms;
} enc_key_t;

static uint8_t enc_pin[3];      /* a, b, sw table indices */
static uint8_t enc_flags;
static uint8_t enc_detent = 4U;
static uint8_t enc_state;       /* last sampled (A << 1) | B */
static int16_t enc_subcount;    /* quadrature edges not yet a whole step */
static volatile int8_t enc_steps;
static volatile int8_t enc_held_steps; /* detents turned with the shaft pressed */
static volatile uint8_t enc_click;
static volatile uint8_t enc_long;
static enc_key_t enc_sw;
static uint32_t enc_ms;
/* FUNC key: the pin asked for, the pin in use (0 when off or taken by the
 * encoder), and its pending events. */
static uint8_t fkey_requested, fkey_pin;
static volatile int8_t fkey_steps;     /* detents turned with FUNC held */
static volatile uint8_t fkey_click, fkey_long;
static enc_key_t fkey;

_Static_assert(RADIO_ENC_PIN_PC14 == 6U && RADIO_FKEY_PIN_DEFAULT == RADIO_ENC_PIN_PC14,
               "the FUNC key defaults to PC14 in the spare-pin table");

uint8_t BOARD_FreePinCount(void) { return (uint8_t)BOARD_FREE_PIN_COUNT; }

const char *BOARD_FreePinLabel(uint8_t index)
{
    return board_free_pins[index < BOARD_FREE_PIN_COUNT ? index : 0U].label;
}

/* 1 when the line reads asserted, honouring the common-to-3.3V jumper. */
static uint8_t enc_line(uint8_t index)
{
    if (!index || index >= BOARD_FREE_PIN_COUNT) return 0U;
    const board_free_pin_t *p = &board_free_pins[index];
    uint8_t high = (p->port->IDR & (1UL << p->bit)) ? 1U : 0U;
    return (enc_flags & RADIO_ENC_FLAG_HIGH) ? high : (uint8_t)(high ^ 1U);
}

static void enc_pin_mode(uint8_t index, uint32_t mode, uint32_t pull)
{
    if (!index || index >= BOARD_FREE_PIN_COUNT) return;
    const board_free_pin_t *p = &board_free_pins[index];
    gpio(p->port, 1UL << p->bit, mode, pull);
}

static uint32_t enc_pull(void)
{
    return (enc_flags & RADIO_ENC_FLAG_NOPULL) ? GPIO_NOPULL
         : ((enc_flags & RADIO_ENC_FLAG_HIGH) ? GPIO_PULLDOWN : GPIO_PULLUP);
}

static uint8_t enc_uses(uint8_t index)
{
    return (uint8_t)(index && ((index == enc_pin[0]) || (index == enc_pin[1]) || (index == enc_pin[2])));
}

/* Adopt the present level of a key without reporting it: a key held while it
 * is (re)configured emits nothing until it is released and pressed again. */
static void key_adopt(enc_key_t *k, uint8_t index)
{
    k->raw = k->stable = enc_line(index);
    k->long_sent = k->stable;
    k->turned = 0U;
    k->edge_ms = k->press_ms = enc_ms;
}

/* Claim the requested FUNC pin unless the encoder uses it; release the old
 * one unless the encoder has just taken it over. Same pull and polarity as
 * the encoder, which sits on the same board. */
static void fkey_apply(void)
{
    const uint8_t next = (fkey_requested < BOARD_FREE_PIN_COUNT && !enc_uses(fkey_requested))
                       ? fkey_requested : 0U;
    if (fkey_pin && (fkey_pin != next) && !enc_uses(fkey_pin)) enc_pin_mode(fkey_pin, GPIO_MODE_ANALOG, GPIO_NOPULL);
    if (next) enc_pin_mode(next, GPIO_MODE_INPUT, enc_pull());
    uint32_t pm = board_enter_critical();
    fkey_pin = next;
    key_adopt(&fkey, next);
    fkey_steps = 0;
    fkey_click = 0U;
    fkey_long = 0U;
    board_exit_critical(pm);
}

void BOARD_FuncKeyConfigure(uint8_t pin)
{
    fkey_requested = pin;
    fkey_apply();
}

void BOARD_EncoderConfigure(uint8_t a, uint8_t b, uint8_t sw, uint8_t flags, uint8_t detent)
{
    if (a >= BOARD_FREE_PIN_COUNT) a = 0U;
    if (b >= BOARD_FREE_PIN_COUNT) b = 0U;
    if (sw >= BOARD_FREE_PIN_COUNT) sw = 0U;
    if (detent < RADIO_ENC_DETENT_MIN || detent > RADIO_ENC_DETENT_MAX) detent = 4U;
    const uint8_t next[3] = {a, b, sw};
    /* Release pins this call drops before claiming the new set, so a pin that
     * moves from one line to another is never configured twice. */
    for (unsigned i = 0U; i < 3U; ++i) {
        uint8_t old = enc_pin[i];
        if (!old || old == next[0] || old == next[1] || old == next[2]) continue;
        enc_pin_mode(old, GPIO_MODE_ANALOG, GPIO_NOPULL);
    }
    enc_flags = flags & RADIO_ENC_FLAG_MASK;
    const uint32_t pull = enc_pull();
    for (unsigned i = 0U; i < 3U; ++i) enc_pin_mode(next[i], GPIO_MODE_INPUT, pull);
    uint32_t pm = board_enter_critical();
    enc_pin[0] = a; enc_pin[1] = b; enc_pin[2] = sw;
    enc_detent = detent;
    /* Adopt the present position instead of counting the difference from the
     * previous wiring as motion. */
    enc_state = (uint8_t)((enc_line(a) << 1) | enc_line(b));
    enc_subcount = 0;
    enc_steps = 0;
    enc_held_steps = 0;
    enc_click = 0U;
    enc_long = 0U;
    key_adopt(&enc_sw, sw);
    board_exit_critical(pm);
    /* The encoder may have taken the FUNC pin, or changed its pull. */
    fkey_apply();
}

/* A detent turned with the shaft pressed is a separate gesture (hold and
 * turn, QMX's way of moving the tuning cursor), and so is one turned with the
 * FUNC key held; either way that press is no longer a click or a long press. */
static void enc_count(int8_t direction)
{
    volatile int8_t *count = &enc_steps;
    if (enc_sw.stable) { enc_sw.turned = 1U; count = &enc_held_steps; }
    else if (fkey.stable) { fkey.turned = 1U; count = &fkey_steps; }
    if (direction > 0) { if (*count < 100) ++*count; }
    else if (*count > -100) --*count;
}

static void key_tick(enc_key_t *k, uint8_t raw, volatile uint8_t *click, volatile uint8_t *long_ev)
{
    if (raw != k->raw) { k->raw = raw; k->edge_ms = enc_ms; }
    if (enc_ms - k->edge_ms < ENC_SW_DEBOUNCE_MS) return;
    if (raw != k->stable) {
        k->stable = raw;
        if (raw) { k->press_ms = enc_ms; k->long_sent = 0U; k->turned = 0U; }
        else if (!k->long_sent && !k->turned) *click = 1U;
    }
    if (k->stable && !k->long_sent && !k->turned && enc_ms - k->press_ms >= ENC_SW_LONG_MS) {
        k->long_sent = 1U;
        *long_ev = 1U;
    }
}

void BOARD_EncoderTick(void)
{
    ++enc_ms;
    if (enc_pin[0] && enc_pin[1]) {
        uint8_t now = (uint8_t)((enc_line(enc_pin[0]) << 1) | enc_line(enc_pin[1]));
        int8_t delta = enc_transition[((enc_state << 2) | now) & 15U];
        enc_state = now;
        if (enc_flags & RADIO_ENC_FLAG_REVERSE) delta = (int8_t)-delta;
        if (delta) {
            enc_subcount += delta;
            while (enc_subcount >= (int16_t)enc_detent) {
                enc_subcount -= (int16_t)enc_detent;
                enc_count(1);
            }
            while (enc_subcount <= -(int16_t)enc_detent) {
                enc_subcount += (int16_t)enc_detent;
                enc_count(-1);
            }
        }
    }
    if (enc_pin[2]) key_tick(&enc_sw, enc_line(enc_pin[2]), &enc_click, &enc_long);
    if (fkey_pin) key_tick(&fkey, enc_line(fkey_pin), &fkey_click, &fkey_long);
}

uint8_t BOARD_EncoderReadEvents(board_encoder_events_t *ev)
{
    uint32_t pm = board_enter_critical();
    if (ev) {
        ev->steps = enc_steps;
        ev->held_steps = enc_held_steps;
        ev->click = enc_click;
        ev->long_click = enc_long;
        ev->held = enc_sw.stable;
        ev->fkey_steps = fkey_steps;
        ev->fkey_click = fkey_click;
        ev->fkey_long = fkey_long;
        ev->fkey_held = fkey.stable;
    }
    enc_steps = 0;
    enc_held_steps = 0;
    enc_click = 0U;
    enc_long = 0U;
    fkey_steps = 0;
    fkey_click = 0U;
    fkey_long = 0U;
    uint8_t fitted = (enc_pin[0] && enc_pin[1]) || enc_pin[2] || fkey_pin;
    board_exit_critical(pm);
    return fitted;
}

uint8_t BOARD_EncoderRead(int8_t *steps, uint8_t *click, uint8_t *long_click)
{
    board_encoder_events_t ev;
    uint8_t fitted = BOARD_EncoderReadEvents(&ev);
    if (steps) *steps = (int8_t)(ev.steps + ev.held_steps + ev.fkey_steps);
    if (click) *click = ev.click;
    if (long_click) *long_click = ev.long_click;
    return fitted;
}

uint8_t BOARD_FuncKeyPin(void) { return fkey_pin; }

void BOARD_SetAux(uint8_t mask)
{
    uint32_t pm = board_enter_critical();
    if (emergency_latched) mask = 0U;
    uint32_t high = ((uint32_t)mask & 7U) << 8U;
    GPIOD->BSRR = high | ((BOARD_AUX_PINS & ~high) << 16U);
    board_exit_critical(pm);
}

void BOARD_SetBacklight(uint8_t percent)
{
    if (percent > 100U) percent = 100U;
    __HAL_RCC_TIM17_CLK_ENABLE();
    TIM17->PSC = (SystemCoreClock / 1000000UL) - 1UL;
    TIM17->ARR = 999U;
    TIM17->CCR1 = (uint32_t)percent * 10U;
    TIM17->CCMR1 = TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE;
    TIM17->CCER = TIM_CCER_CC1E;
    TIM17->BDTR = TIM_BDTR_MOE;
    TIM17->EGR = TIM_EGR_UG;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(15UL << 4U)) | (1UL << 4U);
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 18U)) | (2UL << 18U);
    TIM17->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

void BOARD_PowerOff(void)
{
    BOARD_EmergencyStop();
    BOARD_SetBacklight(0U);
    GPIOB->BRR = BOARD_POWER_HOLD_PIN;
}

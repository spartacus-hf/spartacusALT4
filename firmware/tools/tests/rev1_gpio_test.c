#define _GNU_SOURCE
#include "board_rev1.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "radio_config.h"
#include <sys/mman.h>

uint32_t SystemCoreClock = 170000000U;
/* HAL boundary only. All board decisions and register writes run production C. */
void HAL_GPIO_Init(GPIO_TypeDef *g, const GPIO_InitTypeDef *init)
{
    for (unsigned p = 0U; p < 16U; ++p) {
        if (!(init->Pin & (1U << p))) continue;
        g->MODER = (g->MODER & ~(3UL << (2U*p))) | ((init->Mode & 3U) << (2U*p));
        g->PUPDR = (g->PUPDR & ~(3UL << (2U*p))) | (init->Pull << (2U*p));
    }
}
static unsigned mode(GPIO_TypeDef *g, unsigned p) { return (g->MODER >> (2U*p)) & 3U; }
static unsigned pull(GPIO_TypeDef *g, unsigned p) { return (g->PUPDR >> (2U*p)) & 3U; }
/* BSRR is write-only on silicon; the mapped mock keeps the last written word.
 * Returns 1 for a pin last set, 0 for a pin last reset. */
static unsigned enable_level(uint32_t pin)
{
    uint32_t w = GPIOC->BSRR;
    assert(((w & pin) != 0U) != ((w & (pin << 16U)) != 0U));
    return (w & pin) != 0U;
}

/* Drive every button line to `level` while the board is probing (pull-up
 * phase only sees GND, pull-down phase only sees 3.3 V) and return the time. */
static uint32_t probe(uint32_t now, unsigned line, int level, uint32_t steps)
{
    for (uint32_t n = 0U; n < steps; ++n, now += 4U) {
        uint32_t idr;
        if (level < 0) idr = (pull(GPIOD, 2U) == 1U) ? BOARD_BUTTON_PINS : 0U; /* floating */
        else if (level) idr = ((pull(GPIOD, 2U) == 1U) ? BOARD_BUTTON_PINS : 0U) | (1UL << (line + 2U));
        else idr = ((pull(GPIOD, 2U) == 1U) ? BOARD_BUTTON_PINS : 0U) & ~(1UL << (line + 2U));
        *(volatile uint32_t *)&GPIOD->IDR = idr;
        (void)BOARD_ButtonService(now, NULL);
    }
    return now;
}


/* ---- rotary encoder helpers ---- */

/* Drive one encoder line in the mock IDR. Lines are active low by default, so
 * "asserted" means the pin reads 0. */
static void enc_set(GPIO_TypeDef *port, unsigned bit, int asserted, int active_high)
{
    uint32_t idr = port->IDR;
    uint32_t mask = 1UL << bit;
    int high = active_high ? asserted : !asserted;
    *(volatile uint32_t *)&port->IDR = high ? (idr | mask) : (idr & ~mask);
}

/* The Gray ring in clockwise order: 00 -> 01 -> 11 -> 10 -> 00. */
static const unsigned enc_ring[4] = {0U, 1U, 3U, 2U};
static unsigned enc_pos; /* index into enc_ring of the state currently presented */

/* Present quadrature state (A<<1)|B and sample it once. */
static void enc_step(unsigned state, int active_high)
{
    enc_set(GPIOC, 11U, (int)((state >> 1) & 1U), active_high);
    enc_set(GPIOC, 12U, (int)(state & 1U), active_high);
    for (unsigned i = 0U; i < 4U; ++i) if (enc_ring[i] == state) enc_pos = i;
    BOARD_EncoderTick();
}

/* Walk the ring one edge at a time, so a reverse turn retraces the real
 * sequence instead of repeating the state it started on. */
static void enc_turn(int detents, int forward, int active_high)
{
    for (int d = 0; d < detents; ++d)
        for (unsigned i = 0U; i < 4U; ++i)
            enc_step(enc_ring[(enc_pos + (forward ? 1U : 3U)) & 3U], active_high);
}

/* Settle on 00 and throw away whatever that took, so each block below starts
 * from a known position. */
static void enc_rest(int active_high)
{
    enc_step(0U, active_high);
    BOARD_EncoderRead(NULL, NULL, NULL);
}

static int8_t enc_drain_steps(void)
{
    int8_t steps = 0;
    BOARD_EncoderRead(&steps, NULL, NULL);
    return steps;
}

static void test_encoder(void)
{
    int8_t steps; uint8_t click, held;

    /* The spare-pin table must stay in step with the persisted indices, and
     * must never offer PA10 (UCPD1_DBCC2), PD11/PD13 (NC) or PD14/PD15 (tied
     * together by R148). */
    assert(BOARD_FreePinCount() == RADIO_ENC_PIN_COUNT);
    assert(!strcmp(BOARD_FreePinLabel(RADIO_ENC_PIN_OFF), "OFF"));
    assert(!strcmp(BOARD_FreePinLabel(RADIO_ENC_PIN_PC11), "PC11"));
    assert(!strcmp(BOARD_FreePinLabel(RADIO_ENC_PIN_PC12), "PC12"));
    assert(!strcmp(BOARD_FreePinLabel(RADIO_ENC_PIN_PC13), "PC13"));
    assert(!strcmp(BOARD_FreePinLabel(255U), "OFF")); /* out of range is inert */
    for (uint8_t i = 0U; i < BOARD_FreePinCount(); ++i) {
        const char *l = BOARD_FreePinLabel(i);
        assert(strcmp(l, "PA10") && strcmp(l, "PD11") && strcmp(l, "PD13") &&
               strcmp(l, "PD14") && strcmp(l, "PD15"));
    }

    /* Default wiring: JP20 4/5/6, switching to GND, internal pull-ups.
     * RADIO_ConfigEncoderDefaults produces exactly this; the rev1_policy suite
     * checks that, this suite drives the pins. */
    const uint8_t a = RADIO_ENC_PIN_PC11, b = RADIO_ENC_PIN_PC12, sw = RADIO_ENC_PIN_PC13;

    *(volatile uint32_t *)&GPIOC->IDR = 0xFFFFFFFFUL; /* all released (active low) */
    BOARD_EncoderConfigure(a, b, sw, 0U, 4U);
    assert(mode(GPIOC,11)==0U && mode(GPIOC,12)==0U && mode(GPIOC,13)==0U);
    assert(pull(GPIOC,11)==1U && pull(GPIOC,12)==1U && pull(GPIOC,13)==1U);
    assert(BOARD_EncoderRead(&steps, &click, &held) && steps == 0 && !click && !held);

    /* One detent per direction, at the default 4 edges per step. */
    enc_turn(1, 1, 0); assert(enc_drain_steps() == 1);
    enc_turn(1, 0, 0); assert(enc_drain_steps() == -1);
    enc_turn(5, 1, 0); assert(enc_drain_steps() == 5);
    enc_turn(3, 0, 0); assert(enc_drain_steps() == -3);
    assert(enc_drain_steps() == 0); /* the read drained it */

    /* A partial detent reports nothing but is not lost: finishing the cycle
     * still yields exactly one step. */
    enc_rest(0);
    enc_step(1U, 0); enc_step(3U, 0);
    assert(enc_drain_steps() == 0);
    enc_step(2U, 0); enc_step(0U, 0);
    assert(enc_drain_steps() == 1);

    /* Contact bounce on one line: a repeated or reversed sample must cancel
     * out rather than count, and an illegal two-bit jump counts as nothing. */
    enc_rest(0);
    enc_step(1U, 0); enc_step(1U, 0); enc_step(0U, 0); enc_step(1U, 0);
    enc_step(3U, 0); enc_step(2U, 0); enc_step(0U, 0);
    assert(enc_drain_steps() == 1); /* bounce on the first edge, one real detent */
    enc_step(3U, 0); /* 00 -> 11 is a skipped state */
    enc_step(0U, 0);
    assert(enc_drain_steps() == 0);

    /* REVERSE flips the reported direction. */
    BOARD_EncoderConfigure(a, b, sw, RADIO_ENC_FLAG_REVERSE, 4U);
    enc_pos = 0U;
    enc_turn(2, 1, 0); assert(enc_drain_steps() == -2);
    enc_turn(2, 0, 0); assert(enc_drain_steps() == 2);

    /* A coarser encoder: detent 1 reports every quadrature edge. */
    BOARD_EncoderConfigure(a, b, sw, 0U, 1U);
    enc_pos = 0U;
    enc_turn(2, 1, 0); assert(enc_drain_steps() == 8);

    /* Common to 3.3 V selects pull-downs and inverts the active level. */
    *(volatile uint32_t *)&GPIOC->IDR = 0U;
    BOARD_EncoderConfigure(a, b, sw, RADIO_ENC_FLAG_HIGH, 4U);
    enc_pos = 0U;
    assert(pull(GPIOC,11)==2U && pull(GPIOC,12)==2U && pull(GPIOC,13)==2U);
    enc_turn(3, 1, 1); assert(enc_drain_steps() == 3);

    /* External pulls fitted: no internal pull, same decoding. */
    BOARD_EncoderConfigure(a, b, sw,
                           (uint8_t)(RADIO_ENC_FLAG_HIGH | RADIO_ENC_FLAG_NOPULL), 4U);
    assert(pull(GPIOC,11)==0U && pull(GPIOC,12)==0U && pull(GPIOC,13)==0U);

    /* The push switch: debounced, short click on release, long press latched
     * once. 20 ms debounce, 600 ms hold. */
    *(volatile uint32_t *)&GPIOC->IDR = 0xFFFFFFFFUL;
    BOARD_EncoderConfigure(a, b, sw, 0U, 4U);
    (void)BOARD_EncoderRead(NULL, NULL, NULL);
    enc_set(GPIOC, 13U, 1, 0);
    for (unsigned n = 0U; n < 10U; ++n) BOARD_EncoderTick(); /* inside the debounce */
    assert(BOARD_EncoderRead(NULL, &click, &held) && !click && !held);
    for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
    enc_set(GPIOC, 13U, 0, 0);
    for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
    BOARD_EncoderRead(NULL, &click, &held);
    assert(click && !held);
    enc_set(GPIOC, 13U, 1, 0);
    for (unsigned n = 0U; n < 700U; ++n) BOARD_EncoderTick();
    BOARD_EncoderRead(NULL, &click, &held);
    assert(held && !click); /* long press fires while still held */
    enc_set(GPIOC, 13U, 0, 0);
    for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
    BOARD_EncoderRead(NULL, &click, &held);
    assert(!click && !held); /* release after a long press is not also a click */

    /* Hold and turn: detents turned with the shaft down are reported apart,
     * the press is neither a click nor a long press, even past 600 ms, and
     * free detents afterwards are ordinary steps again. */
    {
        board_encoder_events_t ev;
        enc_rest(0);
        enc_set(GPIOC, 13U, 1, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        enc_turn(2, 1, 0);
        enc_turn(1, 0, 0);
        for (unsigned n = 0U; n < 700U; ++n) BOARD_EncoderTick();
        assert(BOARD_EncoderReadEvents(&ev));
        assert(ev.held_steps == 1 && ev.steps == 0 && !ev.click && !ev.long_click && ev.held);
        enc_set(GPIOC, 13U, 0, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        enc_turn(3, 1, 0);
        assert(BOARD_EncoderReadEvents(&ev));
        assert(ev.steps == 3 && ev.held_steps == 0 && !ev.click && !ev.long_click && !ev.held);
        /* The legacy read sums both kinds of motion for the panel self-test. */
        enc_set(GPIOC, 13U, 1, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        enc_turn(2, 1, 0);
        enc_set(GPIOC, 13U, 0, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        enc_turn(1, 1, 0);
        assert(enc_drain_steps() == 3);
    }

    /* FUNC key on PC14: click, long press, FUNC + turn, and the encoder
     * winning a pin both want. */
    {
        board_encoder_events_t ev;
        const uint8_t pc14 = RADIO_ENC_PIN_PC14;
        BOARD_EncoderConfigure(a, b, sw, 0U, 4U);
        BOARD_FuncKeyConfigure(pc14);
        assert(BOARD_FuncKeyPin() == pc14 && mode(GPIOC, 14) == 0U);
        enc_rest(0);
        (void)BOARD_EncoderReadEvents(&ev);
        enc_set(GPIOC, 14U, 1, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        enc_set(GPIOC, 14U, 0, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        assert(BOARD_EncoderReadEvents(&ev) && ev.fkey_click && !ev.fkey_long && !ev.click);
        enc_set(GPIOC, 14U, 1, 0);
        for (unsigned n = 0U; n < 700U; ++n) BOARD_EncoderTick();
        assert(BOARD_EncoderReadEvents(&ev) && ev.fkey_long && !ev.fkey_click && ev.fkey_held);
        enc_set(GPIOC, 14U, 0, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        assert(BOARD_EncoderReadEvents(&ev) && !ev.fkey_click && !ev.fkey_long);
        enc_set(GPIOC, 14U, 1, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        enc_turn(2, 0, 0);
        enc_set(GPIOC, 14U, 0, 0);
        for (unsigned n = 0U; n < 30U; ++n) BOARD_EncoderTick();
        assert(BOARD_EncoderReadEvents(&ev) && ev.fkey_steps == -2 && ev.steps == 0 && !ev.fkey_click);
        /* The encoder's shaft switch moved onto PC14: FUNC gives way. */
        BOARD_EncoderConfigure(a, b, pc14, 0U, 4U);
        assert(BOARD_FuncKeyPin() == 0U && mode(GPIOC, 14) == 0U && mode(GPIOC, 13) == 3U);
        BOARD_EncoderConfigure(a, b, sw, 0U, 4U);
        assert(BOARD_FuncKeyPin() == pc14 && mode(GPIOC, 14) == 0U);
        BOARD_FuncKeyConfigure(RADIO_ENC_PIN_OFF);
        assert(BOARD_FuncKeyPin() == 0U && mode(GPIOC, 14) == 3U);
        *(volatile uint32_t *)&GPIOC->IDR = 0xFFFFFFFFUL;
    }

    /* Reassigning pins releases the ones it drops back to analog and adopts
     * the new position instead of counting the change as motion. */
    const uint8_t pc14 = (uint8_t)(RADIO_ENC_PIN_PC13 + 1U);
    const uint8_t pc15 = (uint8_t)(RADIO_ENC_PIN_PC13 + 2U);
    assert(!strcmp(BOARD_FreePinLabel(pc14), "PC14") && !strcmp(BOARD_FreePinLabel(pc15), "PC15"));
    BOARD_EncoderConfigure(pc14, pc15, RADIO_ENC_PIN_OFF, 0U, 4U);
    assert(mode(GPIOC,11)==3U && mode(GPIOC,12)==3U && mode(GPIOC,13)==3U);
    assert(mode(GPIOC,14)==0U && mode(GPIOC,15)==0U);
    assert(BOARD_EncoderRead(&steps, NULL, NULL) && steps == 0);

    /* Fully disabled: nothing is sampled and nothing is reported. */
    BOARD_EncoderConfigure(RADIO_ENC_PIN_OFF, RADIO_ENC_PIN_OFF, RADIO_ENC_PIN_OFF, 0U, 4U);
    assert(mode(GPIOC,14)==3U && mode(GPIOC,15)==3U);
    for (unsigned n = 0U; n < 50U; ++n) BOARD_EncoderTick();
    assert(!BOARD_EncoderRead(&steps, &click, &held) && steps == 0 && !click && !held);

}

int main(void)
{
    const uintptr_t regions[] = {0x40000000U,0x40010000U,0x40020000U,0x48000000U,0x50000000U};
    for (unsigned n=0;n<sizeof(regions)/sizeof(regions[0]);++n)
        assert(mmap((void *)regions[n],65536,PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
    GPIOA->MODER = (2UL << 26U) | (2UL << 28U); /* preserve SWD */
    GPIOB->MODER = 3UL << 16U; /* preserve BOOT0 */
    BOARD_GPIOInit();
    assert(GPIOB->BSRR == GPIO_PIN_10 && mode(GPIOB,10)==1U);
    assert(mode(GPIOA,13)==2U && mode(GPIOA,14)==2U && mode(GPIOB,8)==3U);
    assert(mode(GPIOA,8)==1U && mode(GPIOA,9)==1U && mode(GPIOC,8)==1U && mode(GPIOC,9)==1U);
    assert(mode(GPIOB,12)==1U && mode(GPIOB,13)==1U && mode(GPIOB,14)==1U);
    assert(mode(GPIOA,10)==3U && mode(GPIOC,13)==3U && mode(GPIOE,8)==3U);
    /* LCD SPI4 SCK/MOSI are alternate function 5; reset/CS/DC are outputs. */
    assert(mode(GPIOE,12)==2U && ((GPIOE->AFR[1] >> 16U) & 15U)==5U);
    assert(mode(GPIOE,14)==2U && ((GPIOE->AFR[1] >> 24U) & 15U)==5U);
    assert(mode(GPIOE,10)==1U && mode(GPIOE,11)==1U && mode(GPIOE,13)==1U && mode(GPIOA,4)==1U);
    assert(((GPIOE->OSPEEDR >> 24U) & 3U)==3U && ((GPIOE->OSPEEDR >> 28U) & 3U)==3U);
    /* Reset handling: both enables high (UCC27524 disabled / ACT541 enabled
     * with low inputs) and all RF inputs are low push-pull outputs. */
    assert(mode(GPIOC,6)==1U && mode(GPIOC,10)==1U);
    assert(enable_level(GPIO_PIN_10) && enable_level(GPIO_PIN_6));
    /* driver codes per branch: 0 unknown, 1 UCC27524, 2 MC74ACT541 */
    for (unsigned a=0;a<3;++a) for (unsigned b=0;b<3;++b) {
        uint8_t ucc = (uint8_t)((a==1U?1U:0U)|(b==1U?2U:0U));
        uint8_t act = (uint8_t)((a==2U?1U:0U)|(b==2U?2U:0U));
        BOARD_SetRfDrivers(ucc, act);
        assert(enable_level(GPIO_PIN_10) && enable_level(GPIO_PIN_6));
        BOARD_WriteRf(1U); assert(!BOARD_RfActive());
        assert(enable_level(GPIO_PIN_10) && enable_level(GPIO_PIN_6));
        BOARD_PermitRf(1U); BOARD_WriteRf(1U);
        assert(BOARD_RfActive() && mode(GPIOA,8)==2U && mode(GPIOC,8)==2U);
        /* Active: only UCC27524 branches pull their MCU pin low. */
        assert(enable_level(GPIO_PIN_10) == (a != 1U));
        assert(enable_level(GPIO_PIN_6) == (b != 1U));
        BOARD_WriteRf(0U);
        assert(!BOARD_RfActive() && mode(GPIOA,8)==1U && mode(GPIOC,9)==1U);
        assert(enable_level(GPIO_PIN_10) && enable_level(GPIO_PIN_6));
        BOARD_WriteRf(1U);
        BOARD_SetRx(1U); assert(GPIOC->BSRR == ((uint32_t)GPIO_PIN_7 << 16U));
        BOARD_PermitRf(0U); assert(!BOARD_RfActive() && mode(GPIOA,8)==1U);
        assert(enable_level(GPIO_PIN_10) && enable_level(GPIO_PIN_6));
        BOARD_SetRx(1U); assert(GPIOC->BSRR == GPIO_PIN_7);
    }
    BOARD_SetRfDrivers(3U, 0U);
    BOARD_PermitRf(1U); BOARD_WriteRf(1U);
    assert(!enable_level(GPIO_PIN_10) && !enable_level(GPIO_PIN_6));
    GPIOC->MODER &= ~((3UL << 12U) | (3UL << 20U)); /* even if an enable pin was reconfigured */
    BOARD_EmergencyStop();
    assert(BOARD_EmergencyLatched() && !BOARD_RfActive());
    assert(mode(GPIOA,8)==1U && mode(GPIOA,9)==1U && mode(GPIOC,8)==1U && mode(GPIOC,9)==1U);
    assert(mode(GPIOC,6)==1U && mode(GPIOC,10)==1U);
    assert(enable_level(GPIO_PIN_10) && enable_level(GPIO_PIN_6));
    BOARD_PermitRf(1U); BOARD_WriteRf(1U); assert(!BOARD_RfActive());
    BOARD_SetRx(1U); assert(GPIOC->BSRR == ((uint32_t)GPIO_PIN_7 << 16U));
    BOARD_SetAux(7U); assert((GPIOD->BSRR & 65535U)==0U);
    BOARD_ClearEmergency(); BOARD_PermitRf(1U); BOARD_WriteRf(1U); assert(BOARD_RfActive());
    BOARD_PermitRf(0U);
    /* Buttons: idle lines float, so polarity is learned from a press under
     * alternating pulls, starting from either stored value. */
    uint32_t now = 1000U;
    for (unsigned stored=0;stored<2;++stored) for (unsigned jumper=0;jumper<2;++jumper) {
        BOARD_SetButtonPolarity(!stored); /* a disagreeing request always unlocks */
        BOARD_SetButtonPolarity(stored);
        assert(pull(GPIOD,2U) == (stored ? 2U : 1U) && pull(GPIOD,6U) == pull(GPIOD,2U));
        now = probe(now, 1U, -1, 40U); /* nobody presses: never locks or reads pressed */
        uint8_t learned = 7U;
        assert(!BOARD_ButtonService(now, &learned) && learned == stored);
        for (unsigned n=0;n<5;++n) assert(!BOARD_ButtonPressed(n));
        now = probe(now, 1U, (int)jumper, 8U);
        assert(BOARD_ButtonService(now, &learned) && learned == jumper);
        assert(pull(GPIOD,2U) == (jumper ? 2U : 1U) && pull(GPIOD,6U) == pull(GPIOD,2U));
        for (unsigned n=0;n<5;++n) {
            *(volatile uint32_t *)&GPIOD->IDR = jumper ? (1UL<<(n+2U)) : (BOARD_BUTTON_PINS & ~(1UL<<(n+2U)));
            assert(BOARD_ButtonPressed(n));
            *(volatile uint32_t *)&GPIOD->IDR = jumper ? 0U : BOARD_BUTTON_PINS;
            assert(!BOARD_ButtonPressed(n));
        }
        BOARD_SetButtonPolarity(jumper); /* agreeing request keeps the lock */
        assert(BOARD_ButtonService(now, NULL));
    }
    BOARD_SetButtonPolarity(0U);
    now = probe(now, 3U, 0, 1U); /* a single glitch sample does not lock */
    assert(!BOARD_ButtonService(now, NULL));
    test_encoder();
    BOARD_SetBacklight(255U); assert(TIM17->CCR1==1000U && mode(GPIOB,9)==2U);
    BOARD_PowerOff(); assert(GPIOB->BRR==GPIO_PIN_10 && TIM17->CCR1==0U && !BOARD_RfActive());
    puts("REV1 GPIO startup, UCC27524/ACT541 enables, RF/RX exclusion, button learning, "
         "rotary encoder decode and latched shutdown: PASS");
    return 0;
}

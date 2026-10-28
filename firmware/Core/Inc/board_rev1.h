#ifndef BOARD_REV1_H
#define BOARD_REV1_H

#include "stm32g4xx_hal.h"

#define BOARD_NAME "SPARTACUS REV1"
#define BOARD_RF_A_PINS (GPIO_PIN_8 | GPIO_PIN_9)
#define BOARD_RF_B_PINS (GPIO_PIN_8 | GPIO_PIN_9)
#define BOARD_RF_ENABLE_PINS (GPIO_PIN_6 | GPIO_PIN_10)
#define BOARD_RX_ENABLE_PIN GPIO_PIN_7
#define BOARD_POWER_HOLD_PIN GPIO_PIN_10
#define BOARD_BUTTON_PINS (GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6)
#define BOARD_AUX_PINS (GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10)
#define BOARD_TX_OUTPUTS (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TE1 | HRTIM_OUTPUT_TE2)
#define BOARD_RX_OUTPUTS (HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2)
#define BOARD_TEST_OUTPUT HRTIM_OUTPUT_TD1

/* EarlyInit has no initialized-data, HAL, FPU or CCM dependencies. */
void BOARD_EarlyInit(void);
void BOARD_GPIOInit(void);
void BOARD_EmergencyStop(void);
uint8_t BOARD_EmergencyLatched(void);
void BOARD_ClearEmergency(void); /* foreground, after peripherals have stopped */
/* Bit 0 is branch A (PC10), bit 1 is branch B (PC6). Enable pins idle high
 * for every assembly; a UCC27524 branch drives its pin low only while RF is
 * active, an ACT541 (or unknown) branch keeps it high. */
void BOARD_SetRfDrivers(uint8_t ucc27524_mask, uint8_t act541_mask);
void BOARD_PermitRf(uint8_t permit);
void BOARD_WriteRf(uint8_t active);
uint8_t BOARD_RfActive(void);
uint8_t BOARD_RfPermitted(void); /* TRANSMIT: BOARD_PermitRf(1) and no emergency latch */
void BOARD_SetRx(uint8_t active);
/* The common button jumper has no external pull resistors: idle lines float.
 * Polarity is learned from the first press by alternating the internal pulls;
 * the stored value only seeds the probe and buttons read released until then. */
void BOARD_SetButtonPolarity(uint8_t active_high);
uint8_t BOARD_ButtonService(uint32_t now, uint8_t *active_high);
uint8_t BOARD_ButtonPressed(uint8_t index);

/* Spare jumper pins (JP20 2..8, JP7 1..7, JP19-8) that BOARD_GPIOInit parks as
 * analog and nothing else claims. Index 0 is OFF and the order is frozen: it is
 * persisted in settings as RADIO_ENC_PIN_* (radio_config.h). */
uint8_t BOARD_FreePinCount(void); /* including OFF */
const char *BOARD_FreePinLabel(uint8_t index);
/* Front-panel rotary encoder built from three of those pins: a/b are the
 * quadrature pair, sw the shaft switch, and 0 leaves a line unused. `flags` is
 * RADIO_ENC_FLAG_*; `detent` is the quadrature edges per reported step. Pins
 * dropped by a later call go back to analog. Safe against a concurrent tick. */
void BOARD_EncoderConfigure(uint8_t a, uint8_t b, uint8_t sw, uint8_t flags, uint8_t detent);
/* 1 kHz sampler. SysTick only: constant time, one read per port, no loops.
 * Unlike the buttons the encoder pins have a firmware-supplied pull, so there
 * is nothing to learn and no probing. */
void BOARD_EncoderTick(void);
/* Pending encoder input since the previous read. A press that is turned
 * before release reports its detents in held_steps and is neither a click nor
 * a long press; long_click fires once the shaft has been held still for 600 ms. */
typedef struct {
    int8_t  steps;       /* detents with the shaft released, + = Gray 00-01-11-10 */
    int8_t  held_steps;  /* detents turned while the shaft was pressed */
    uint8_t click;       /* pressed and released, not turned, not long */
    uint8_t long_click;  /* held still for the long-press time */
    uint8_t held;        /* shaft is pressed now */
    int8_t  fkey_steps;  /* detents turned while the FUNC key was held */
    uint8_t fkey_click;  /* FUNC key pressed and released, knob not turned */
    uint8_t fkey_long;   /* FUNC key held still for the long-press time */
    uint8_t fkey_held;   /* FUNC key is pressed now */
} board_encoder_events_t;
/* Drains pending motion and switch edges. Returns 0 when no encoder is fitted. */
uint8_t BOARD_EncoderReadEvents(board_encoder_events_t *ev);
/* The same drain with held and free detents summed, for callers that only
 * count motion (the panel self-test). */
uint8_t BOARD_EncoderRead(int8_t *steps, uint8_t *click, uint8_t *long_click);
/* Optional FUNC key: a push button on another spare pin (default PC14, the
 * extra key on the LCD/encoder board). It takes the encoder's pull and
 * polarity flags; a pin one of the encoder lines uses is ignored, as is 0.
 * Events come through BOARD_EncoderReadEvents. */
void BOARD_FuncKeyConfigure(uint8_t pin);
uint8_t BOARD_FuncKeyPin(void); /* the pin in use, 0 when off or taken */
void BOARD_SetAux(uint8_t mask);
void BOARD_SetBacklight(uint8_t percent);
void BOARD_PowerOff(void);

#endif

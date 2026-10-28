#ifndef USB_DIAG_PINS_H
#define USB_DIAG_PINS_H

#include <stdint.h>

/* One row per REV1 GPIO that the pin map names. `port` is 0..6 for GPIOA..G. */
typedef struct {
    uint8_t     port;
    uint8_t     pin;
    const char *label;   /* net name from docs/rev1_pins.csv */
    const char *signal;  /* configured function from the same document */
} diag_pin_t;

extern const diag_pin_t diag_pins[];
extern const uint8_t diag_pin_count;

#endif

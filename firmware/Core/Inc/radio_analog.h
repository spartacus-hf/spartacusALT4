#ifndef RADIO_ANALOG_H
#define RADIO_ANALOG_H
#include <stdint.h>
#include "radio.h"
uint8_t RADIO_ReferenceInit(void);
uint8_t RADIO_AnalogInit(void);
uint8_t RADIO_AnalogRecover(void);
uint8_t RADIO_AnalogPause(void);
void RADIO_AnalogClocks(void);
void RADIO_BiasEnable(uint8_t on);
void RADIO_AnalogStop(void);
/* Park both envelope DACs at a static code, DMA off. Codes are 12 bit; the
 * caller owns the ceiling. Used by the self test's envelope ramp (the QMX
 * "PA mod. test") and by the ENV A/B bench calibration. */
void RADIO_EnvelopeSetCodes(uint16_t code_a, uint16_t code_b);
void RADIO_EnvelopeStart(uint32_t *samples, uint32_t count);
void RADIO_EnvelopeStop(void);
uint8_t RADIO_EnvelopeBoundary(uint32_t half, uint32_t count);
void RADIO_EnvelopeFill(uint32_t *a, uint32_t *b, uint32_t *env, uint32_t count, uint32_t period, uint8_t active);
void RADIO_AnalogSnapshot(radio_telemetry_t *t);
uint8_t RADIO_AnalogRailOk(void); /* 5 V rail inside 4.4-5.5 V (debounced) */
/* Telemetry-ADC failures by cause: DMA error, handler late by a whole
 * half-buffer, ADC overrun. Counted, never latched. */
void RADIO_AnalogFailCounts(uint32_t *te, uint32_t *late, uint32_t *ovr);

/* The power and SWR meter (radio_rf_meter.h). The ADC interrupt sums the
 * scans with RF up; RADIO_MeterService (main loop, every pass) turns them
 * into a reading every METER UPDATE MS and sets TX TEST's TUNE % drain cap.
 * `rf_meter` is a fixed symbol for the bench, which reads it over SWD while
 * the console is silent in TX. */
#include "radio_rf_meter.h"
typedef struct {
    rf_meter_reading_t last;  /* the last interval with RF */
    uint32_t tick;            /* when it was taken */
    uint32_t hz;              /* the carrier it was on */
    uint32_t seq;             /* readings so far */
    float zero_mv[2];         /* the detectors' learnt rest levels, mV */
    uint32_t zero_seen;       /* zero_mv holds a measurement */
} radio_meter_t;
extern radio_meter_t rf_meter;
void RADIO_MeterService(uint32_t now);
/* The ADC interrupt's copy of the meter and SWR-trip settings, rebuilt from
 * RADIO_Config(): at analog init, after any configuration change, and every
 * meter interval. */
void RADIO_MeterReconfigure(void);
/* The last reading if it is at most `max_age_ms` old (out may be NULL). */
uint8_t RADIO_MeterRecent(uint32_t now, uint32_t max_age_ms, rf_meter_reading_t *out);
/* The filters just switched: the detectors' readings do not count, as rest
 * level or as stray RF, for the next 100 ms. */
void RADIO_MeterHoldDetectors(void);

#endif

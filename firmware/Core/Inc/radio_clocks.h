#ifndef RADIO_CLOCKS_H
#define RADIO_CLOCKS_H
#include <stdint.h>
#include "radio_rx_front.h"
void RADIO_ClocksStop(void);
uint8_t RADIO_RxClockStart(uint32_t hz, radio_rx_mode_t mode);
uint32_t RADIO_RxClockHz(void);
/* dial - LO exactly, the IF the receive NCO removes; 0 with no LO running. */
float RADIO_RxOffsetHz(uint32_t dial_hz);
/* SYSCLK as it really runs (MOD_SysclkMicroHz), rounded to 1 Hz. */
uint32_t RADIO_SysclkHz(void);
/* Free-running TIM5 at SYSCLK for `clock stamp`. */
void RADIO_RefTimerStart(void);
/* The reference disciplined to the USB host's 1 kHz SOF (radio_clocks.c).
 * RADIO_RefSofEntry from USB_LP_IRQHandler's entry when the SOF raised it,
 * RADIO_RefSofTick from the SOF callback. RADIO_RefTrimPpb: what the receive
 * and transmit NCOs add to the REF CAL plan now (0 with no host, while
 * following is off, or during a calibration). RADIO_RefUsbPpb: the
 * reference's error against the host in ppb (positive = fast, the REF CAL
 * sign), when there is one. */
#ifndef REF_SOF_BENCH
#define REF_SOF_BENCH 0 /* 1: every SOF's stamp in ref_sof_raw, for SWD */
#endif
typedef struct {
    uint8_t state, have;          /* state: 0 idle, 1 acquiring, 2 locked */
    int32_t ppb, ppb_nco;
    uint32_t dropped, acquisitions;
    uint32_t stepped;             /* groups coasted by the step gate */
    uint32_t absorbed;            /* host SOF steps taken as a new origin */
    uint8_t alpha_percent, step_cycles;
    int32_t temp_mc;              /* RADIO_RefTempMilliC */
} radio_ref_sof_status_t;
/* The MCU's temperature (ADC5's internal sensor) in milli-degrees C,
 * INT32_MIN until the first reading; RADIO_RefTempService from the main loop. */
void RADIO_RefTempService(uint32_t now);
int32_t RADIO_RefTempMilliC(void);
void RADIO_RefSofEntry(uint32_t stamp, uint16_t fn);
void RADIO_RefSofTick(uint16_t fn);
void RADIO_RefSofAfterIsr(void); /* as each RF DMA interrupt returns */
void RADIO_RefSofIsrReturn(void); /* as every other interrupt that can delay USB's returns */
uint32_t RADIO_RefSofFrame(void); /* host frames counted since boot */
uint8_t RADIO_RefUsbPpb(int32_t *ppb);
int32_t RADIO_RefTrimPpb(void);
void RADIO_RefFollowEnable(uint8_t on);
/* The tracker's phase share (percent, 30-100) and step gate (cycles, 2-25)
 * until the next restart; an out-of-range value leaves that one as it is. */
void RADIO_RefSofTune(uint16_t alpha_percent, uint16_t step_cycles);
uint8_t RADIO_RefFollowEnabled(void);
void RADIO_RefSofStatus(radio_ref_sof_status_t *s);
uint8_t RADIO_RxClockQuality(void); /* RADIO_RxPlanLo result of the running clock */
uint8_t RADIO_RxAdc96(void);        /* the running plan has the ADC at ~96 kHz */
/* HRTIM ticks per second (SYSCLK x 32, reference-corrected): LO = this / period. */
uint64_t RADIO_HrClockHz(void);
/* Scanner: move the running receive LO to `period` in place -- no RX_EN
 * change, no audio restart; about 12 us with interrupts masked at 1.8 MHz.
 * RECEIVE state with the LO running and RF idle only; 0 otherwise. The
 * running period is RADIO_RxClockGeometry's, valid while RADIO_RxClockHz. */
uint8_t RADIO_RxLoHop(uint32_t period);
/* Seamless tuning: 1 keep the running LO (the NCO alone follows), 2 move it
 * in place to *period (RADIO_RxRetuneTo), 0 needs a full receiver restart. */
uint8_t RADIO_RxRetunePlan(uint32_t hz, radio_rx_mode_t mode, uint32_t *period);
uint8_t RADIO_RxRetuneTo(uint32_t period);
void RADIO_RxClockGeometry(radio_rx_geometry_t *g);
uint8_t RADIO_TestClockStart(uint32_t hz);
/* TEST_CLK at an exact HRTIM period, for the I/Q calibration: the offsets it
 * needs are one or two ticks from the LO period and cannot be named in hertz.
 * RECEIVE state only, like RADIO_TestClockStart. */
uint8_t RADIO_TestClockStartPeriod(uint32_t period);
/* BPF sweep: LO and test clock (HRTIM D, output disabled) at one period near
 * hz, so the injected carrier lands at DC. RECEIVE state only. */
uint8_t RADIO_MeasureClocksStart(uint32_t hz);
/* IRQ-safe TEST_CLK output gate; never enables with RF active or latched. */
void RADIO_TestOutput(uint8_t on);
void RADIO_TestClockStop(void);
#endif

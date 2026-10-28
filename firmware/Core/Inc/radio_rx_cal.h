#ifndef RADIO_RX_CAL_H
#define RADIO_RX_CAL_H

#include <stdint.h>
#include "radio_iq_fit.h"

/* Receive I/Q calibration, driven by RADIO_CAL_RX_IQ.
 *
 * This is the REV1 counterpart of the QMX I/Q and image diagnostics (QRP Labs
 * operating manual 1.03.002, 8.6.3 and 8.6.7), with the part QMX leaves to the
 * hardware done in software. QMX takes its quadrature from an Si5351A, where
 * an even integer divider makes 90 degrees exact, and applies no correction at
 * all: QRP Labs quote 60-75 dB of image rejection, varying unit to unit.
 * REV1 generates the same quadrature from HRTIM ticks and already carries
 * RX I GAIN, RX Q GAIN and RX SKEW SAMPLES through the hot path, so what is left
 * after the balanced integer-tick edges can be measured and cancelled per
 * board.
 *
 * The run needs no signal generator and no antenna. TEST_CLK reaches the LPF
 * input through R106 100 k, which is tens of microamps; a 50 ohm dummy load on
 * the connector keeps off-air signals out of the measurement, but the
 * correlator tolerates an antenna (see radio_iq_fit.h).
 *
 * Three passes of about 370 ms each:
 *   1. injection off, to measure the detector floor;
 *   2. injection on, uncorrected, which gives the imbalance and the
 *      coefficients that cancel it;
 *   3. injection on with those coefficients applied inside the correlator, so
 *      the residual image we report is measured rather than predicted.
 * Optionally two more, placing the same carrier in the wanted and then the
 * opposite sideband of the audio channel, which measures opposite-sideband
 * rejection end to end (I/Q correction plus the elliptic channel filter).
 *
 * Nothing is applied on its own: RADIO_IqCalApply is the explicit step, as
 * APPLY SWEEP BPF EDGES is for the sweep, and a save still has to follow. */

typedef enum {
    RADIO_IQCAL_IDLE, RADIO_IQCAL_RUNNING, RADIO_IQCAL_DONE,
    RADIO_IQCAL_FAILED, RADIO_IQCAL_STOPPED
} radio_iqcal_status_t;

typedef struct {
    uint8_t  status;          /* radio_iqcal_status_t */
    uint8_t  have_result;     /* `measured` holds a completed measurement */
    uint8_t  sideband_valid;  /* the opposite-sideband passes ran */
    const char *message;
    uint32_t rf_hz, lo_hz;    /* tuned frequency and the QSD LO behind it */
    int32_t  delta_hz;        /* injected carrier, relative to the LO */
    uint32_t lo_period, test_period;
    float    floor_dbfs;      /* bin level with the injection off */
    radio_iq_result_t measured;   /* imbalance and the coefficients for it */
    float    corrected_image_db;  /* measured residual with them applied */
    float    in_band_dbfs, opposite_dbfs, opposite_db;
} radio_iqcal_info_t;

/* RECEIVE state only; RADIO_CalStart enforces that. with_sideband adds the
 * two opposite-sideband passes, which only mean anything in USB or LSB. */
void    RADIO_IqCalBegin(uint32_t now, uint8_t with_sideband);
uint8_t RADIO_IqCalService(uint32_t now);  /* 1 while running */
void    RADIO_IqCalStop(void);
void    RADIO_IqCalInfo(radio_iqcal_info_t *info);
/* Write the measured coefficients to the live configuration. 0 when there is
 * no completed result, or when it is too large to be an imbalance rather than
 * a fault -- the settings themselves are bounded at 0.1..10 gain and +-0.5
 * phase, and a measurement outside that is reported, not applied. */
uint8_t RADIO_IqCalApply(void);

#endif

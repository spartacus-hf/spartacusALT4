#ifndef RADIO_AF_SWEEP_H
#define RADIO_AF_SWEEP_H
#include <stdint.h>

/* Audio-passband and image sweeps: the REV1 counterparts of the QMX "Audio
 * filter sweep" and "Image sweep" (QRP Labs operating manual 1.03.002, 8.6.1
 * and 8.6.3). Both need a 50 ohm dummy load, as QMX says of its own.
 *
 * QMX sweeps its internal signal generator and leaves the VFO alone. REV1
 * cannot: the test clock is an integer HRTIM period, and one tick is several
 * kilohertz at HF, so it cannot be moved 50 Hz. The carrier therefore stays on
 * one period and the demodulator moves instead. RADIO_AudioRetuneDsp() shifts
 * the 48 kHz NCO, which measures the same filter with the two ends swapped,
 * and it is already how the I/Q calibration reads one tone in the wanted and
 * then in the opposite sideband.
 *
 * What is measured is the power at the channel-filter output, before the AGC
 * (RADIO_DspLastPower). Measuring the demodulated audio instead would plot the
 * AGC's response, not the filter's.
 *
 * AF sweep: QMX's own grid, 100 Hz to 3850 Hz in 50 Hz steps, measured twice --
 * once with the carrier in the wanted sideband and once in the opposite one.
 * Expect the wanted curve flat between the 150 Hz and 3.2 kHz corners and the
 * opposite curve about 60 dB below it.
 *
 * Image sweep: one curve over demodulator offsets wide enough to show the four
 * responses QMX annotates -- the wanted signal and its opposite sideband, and
 * the image's two. The wanted sits at delta - IF in the 48 kHz stream and the
 * image at -(delta + IF), so they are 2*delta apart and the range is derived
 * from delta rather than fixed at QMX's -30.5/+7 kHz.
 *
 * The decimated stream is only 48 kHz wide, so a band whose smallest available
 * delta puts the image beyond +-23 kHz cannot be pictured this way and the run
 * says so. Nothing is lost but the picture: `rxcal` correlates at +-delta in
 * the raw 177 kHz stream, before decimation, and reports the same rejection as
 * a number on every band. */

#define RADIO_AF_POINTS      76U    /* QMX: 100..3850 Hz in 50 Hz steps */
#define RADIO_AF_FIRST_HZ    100
#define RADIO_AF_STEP_HZ     50
#define RADIO_AF_CURVES      2U     /* wanted sideband, opposite sideband */

/* Audio frequency of AF-sweep point k. */
int32_t RADIO_AfPointHz(unsigned k);
/* Demodulator offset in the 48 kHz stream for an AF-sweep point. `tone` is
 * where the injected carrier sits there; `lsb` selects the sideband sense.
 * RADIO_DspInit's offset_hz is the dial frequency, so a carrier at `tone`
 * comes out at audio (tone - offset): the wanted curve puts it at +f of the
 * dial in USB and -f in LSB, and the opposite curve on the other side. */
float RADIO_AfOffsetHz(float tone, unsigned k, unsigned curve, uint8_t lsb);

/* RADIO_DspInit's offset is the dial frequency in stream terms.  Its NCO
 * also adds the +/-1650 Hz SSB channel centre, so a sweep whose public axis
 * is an absolute stream frequency must remove that centre before retuning. */
float RADIO_AfDemodOffsetHz(float stream_hz, uint8_t lsb);

/* Does an AF sweep of a carrier at `tone` fit inside the 48 kHz stream?
 * The run visits tone -/+ RADIO_AF_POINTS_MAX_HZ for the two sideband curves
 * and steps AF_FLOOR_OFFSET_HZ toward DC for the floor, so the carrier has to
 * sit near the middle. It does not when the LO lands on the far side of the
 * carrier and the injection offset adds to the IF instead of cancelling it. */
#define RADIO_AF_STREAM_LIMIT_HZ 22000.0f
uint8_t RADIO_AfSpanFits(float tone);

/* Demodulator offsets an image sweep should visit, from the carrier position
 * `tone` and the image position `image` (both in the 48 kHz stream). Returns 0
 * when the pair does not fit inside the stream with a margin, which is what a
 * band with a large delta looks like. */
#define RADIO_IMAGE_LIMIT_HZ 23000.0f
#define RADIO_IMAGE_MARGIN_HZ 1500.0f
uint8_t RADIO_ImageSpan(float tone, float image, float *first, float *step);
/* Offset of image-sweep point k for a span that RADIO_ImageSpan produced. */
float RADIO_ImagePointHz(float first, float step, unsigned k);

/* Runtime, driven by RADIO_CAL_AF_SWEEP. */
typedef enum {
    RADIO_AF_IDLE, RADIO_AF_RUNNING, RADIO_AF_DONE, RADIO_AF_FAILED, RADIO_AF_STOPPED
} radio_af_status_t;

typedef struct {
    uint8_t status, image_mode, point, curves, clipped;
    uint32_t rf_hz, lo_hz;
    int32_t delta_hz;
    float tone_hz, image_hz;     /* carrier and image in the 48 kHz stream */
    float first_hz, step_hz;     /* image-sweep axis; 0 for an AF sweep */
    float floor_dbfs;            /* channel level with the injection off */
    float best_dbfs;             /* strongest measured point */
    float worst_ratio_db;        /* AF: least wanted-over-opposite in the
                                  * passband. Image: wanted over image. */
    const char *message;
    const char *export_name;     /* SD CSV of the last run, or NULL */
} radio_af_info_t;

/* RECEIVE state only, USB or LSB only; RADIO_CalStart enforces the first and
 * the run reports the second. image selects the image sweep. */
void    RADIO_AfSweepBegin(uint32_t now, uint8_t image);
uint8_t RADIO_AfSweepService(uint32_t now); /* 1 while running */
void    RADIO_AfSweepStop(void);
void    RADIO_AfSweepInfo(radio_af_info_t *info);
/* Level code of a measured point, in the radio_bpf_sweep.h encoding
 * (0 = not measured, 1..255 = -127..0 dBFS in 0.5 dB steps). */
uint8_t RADIO_AfSweepLevel(unsigned point, unsigned curve);

#endif

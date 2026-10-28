#ifndef RADIO_BPF_SWEEP_H
#define RADIO_BPF_SWEEP_H
#include <stdint.h>
#include "radio_config.h"

/* Receive band-pass sweep, the REV1 counterpart of the QMX "RF sweep".
 *
 * TEST_CLK (PB14, HRTIM D) reaches the LPF input through R106 100 k, so the
 * injected carrier takes the real receive path: LPF, Q8, U10 BPF, T401, QSD.
 * An integer HRTIM period cannot place a test carrier a few kHz from the LO
 * above ~16 MHz (neighbouring periods are > 50 kHz apart there), so the test
 * clock runs at exactly the LO period instead. The QSD then converts it to
 * DC, and the injection is chopped on alternate 64-frame I2S halves
 * (~1.4 kHz, far above the PCM1821 0.00025 fS DC-removal filter). A
 * synchronous on-minus-off average recovers its complex amplitude; op-amp
 * offsets, LO leakage and off-frequency signals cancel. */

#define RADIO_SWEEP_POINTS   64U
#define RADIO_SWEEP_FIRST_HZ 4000000UL
#define RADIO_SWEEP_SLOTS    5U   /* four U10 BPF channels, five PE2..PE6 LPFs */

/* What a run sweeps. BPF is the whole-range log sweep across the four U10
 * channels, with the crossover fit; it is what BPF SWEEP ~20S has always run.
 * LPF forces each of the five PE2..PE6 filters in turn over the same axis,
 * which QMX can only do by transmitting into its SWR bridge (operating manual
 * 8.6.5) -- REV1 does not have to, because TEST_CLK injects on the antenna
 * side of the low-pass filters, so the receive path already runs through them.
 * BAND is QMX's per-band linear RF sweep (8.6.2), centred on the tuned
 * frequency so its shape lines up with QRP Labs' published curves. */
typedef enum { RADIO_SWEEP_BPF, RADIO_SWEEP_LPF, RADIO_SWEEP_BAND } radio_sweep_axis_t;

/* Sweep frequency k for an axis. BPF and LPF are log-spaced over
 * RADIO_SWEEP_FIRST_HZ (LPF: 1 MHz) .. RADIO_RX_MAX_HZ; BAND is linear over
 * +-40 % of `centre`, which shows the peak and both skirts of any QMX band-
 * pass section. Clamped to the 1.0 .. RADIO_RX_MAX_HZ the test clock can
 * reach. */
uint32_t RADIO_SweepGridHz(uint8_t axis, uint32_t centre, unsigned k);
/* The grid of the run that is loaded now. */
uint32_t RADIO_SweepPointHz(unsigned k);

/* Levels are stored as bytes: 0 = not measured, 1..255 = -127 .. 0 dBFS in
 * 0.5 dB steps. */
uint8_t RADIO_SweepCode(float dbfs);
float RADIO_SweepCodeDb(uint8_t code);

/* Crossover fit. Configured slots keep the frequency order of their current
 * upper edges (unconfigured slots stay unconfigured). The sweep axis is cut
 * into one contiguous, non-empty run of points per slot, maximizing the
 * summed level of the selected filter; points where any slot is unmeasured
 * do not vote, and ties keep a crossover closest to the current edge. Each
 * edge is the geometric mean of the two points around its cut; the top slot
 * reaches RADIO_RX_MAX_HZ. Returns 0 (edges untouched) with fewer than two
 * configured slots or no voting point. */
uint8_t RADIO_SweepFitEdges(const uint8_t level[][RADIO_SWEEP_SLOTS], const uint32_t current[4], uint32_t edges[4]);

/* Synchronous detector: add +w*x for injected halves and -w*x for idle
 * halves over an even number of alternating halves. The Hann weight of half
 * index h is symmetric, so injected and idle weights balance exactly (DC
 * cancels), while stations off the chop harmonics are rejected ~100 dB
 * instead of ~40 dB with equal weights. */
typedef struct { float i, q, weight, peak; } radio_chop_t;
float RADIO_ChopWeight(unsigned index, unsigned halves);
void RADIO_ChopAdd(radio_chop_t *a, uint8_t on, float weight, float i, float q);
float RADIO_ChopDbfs(const radio_chop_t *a); /* |mean(on) - mean(off)| */

/* Runtime, driven by RADIO_CAL_BPF_SWEEP. */
typedef enum {
    RADIO_SWEEP_IDLE, RADIO_SWEEP_RUNNING, RADIO_SWEEP_DONE, RADIO_SWEEP_FAILED, RADIO_SWEEP_STOPPED
} radio_sweep_status_t;
typedef struct {
    uint8_t status, point, clipped, fit_ok;
    uint8_t axis, slots;     /* radio_sweep_axis_t, and how many it measured */
    uint32_t centre_hz;      /* BAND axis only */
    float floor_dbfs;
    const char *message;
    const char *export_name; /* SD CSV of the last run, or NULL */
} radio_sweep_info_t;
void RADIO_SweepBegin(uint32_t now, uint8_t axis);
uint8_t RADIO_SweepService(uint32_t now); /* 1 while running */
void RADIO_SweepStop(void);
void RADIO_SweepInfo(radio_sweep_info_t *info);
uint8_t RADIO_SweepLevel(unsigned point, unsigned slot);
/* Fitted edges of the last completed sweep, or NULL. */
const uint32_t *RADIO_SweepEdges(void);

#endif

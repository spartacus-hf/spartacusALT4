#ifndef RADIO_RF_METER_H
#define RADIO_RF_METER_H

#include <stdint.h>

/* Power and SWR from the REV1 bridge, T5 -- QRP Labs' QMX/QMX+ T507: a
 * Stockton tandem match on one BN43-1502 (10 turns : 1 turn through each
 * hole, so a coupled port carries a tenth of its wave's line voltage),
 * 49.9 ohm on each port, each port read by an SS14 into 0.1 uF || 100 k and
 * straight to ADC2: IN7 forward (PC1), IN8 reflected (PC2).
 *
 * What QMX does with it (operating manual 1.03.002, firmware history): SWR
 * protection every 1 ms against a threshold, off below 0.5 W; power from the
 * forward detector on a fixed scale; PEP in SSB. It reads SWR high on a flat
 * dummy load, more so with frequency (1.03 on 80 m to 1.48 on 10 m, forum
 * 101694802: the bridge's own leakage), and better at lower power (the
 * diode's threshold). Here the detector output is first turned back into the
 * port's peak voltage (the diode table), and the bridge's leakage on a 50 ohm
 * load, once measured per band, is taken out of the reflection.
 *
 * The per-scan helpers below run in the ADC interrupt (radio_analog.c); the
 * rest is main-loop arithmetic, host-tested by tools/tests/rf_meter_test.c. */

#define RF_METER_POINTS   12U
#define RF_METER_ANCHORS  10U
/* Detector output breakpoints, mV at the ADC pin. */
#define RF_METER_VDC_MV {0, 6, 15, 30, 60, 120, 220, 380, 650, 1100, 1800, 2900}
/* Port peak mV at each breakpoint: the ideal peak detector with an SS14-like
 * diode (nVt 28 mV, Is*R 0.300 V), tools/gen_rf_meter_table.py. Within 1 mV
 * of the curve above 6 mV. `metercal table` may replace it per board. */
#define RF_METER_DEFAULT_PORT_MV {0, 28, 46, 69, 109, 181, 294, 468, 754, 1222, 1939, 3058}

/* Line watts per port volt squared: (10 V)^2 / (2 * 50 ohm). */
#define RF_METER_W_PER_V2   1.0f
/* Forward port peak below which neither the SWR trip nor the SWR reading
 * applies: 0.316 V, 0.1 W. QMX uses 0.5 W; REV1 keeps it lower because in
 * OUTPHASING drain mode the drains sit at full voltage at any output. */
#define RF_METER_GATE_V     0.316f
#define RF_METER_SWR_MAX    99.0f
/* Scans of RF an interval needs before it counts (2 ms). */
#define RF_METER_MIN_SCANS  8U

extern const uint16_t rf_meter_vdc_mv[RF_METER_POINTS];
extern const uint16_t rf_meter_default_port_mv[RF_METER_POINTS];
extern const float rf_meter_inv_width[RF_METER_POINTS - 1U];
/* The ten HF bands' anchors for the per-band calibration, Hz. */
extern const uint32_t rf_meter_anchor_hz[RF_METER_ANCHORS];

/* Port peak volts for a detector output in mV (its zero already removed),
 * by linear interpolation in `port_mv`; above the last point the last
 * segment continues. */
static inline float rf_meter_port_v(const uint16_t *port_mv, float mv)
{
    if (!(mv > 0.0f)) return 0.0f;
    uint32_t i = 0U;
    while ((i < RF_METER_POINTS - 2U) && (mv >= (float)rf_meter_vdc_mv[i + 1U])) ++i;
    const float y0 = (float)port_mv[i], y1 = (float)port_mv[i + 1U];
    return 0.001f * (y0 + (mv - (float)rf_meter_vdc_mv[i]) * (y1 - y0) * rf_meter_inv_width[i]);
}

/* The table as the ADC interrupt uses it, in RAM: breakpoints (mV), port
 * volts and each segment's slope (V per mV) as floats, so a lookup is four
 * compares, no flash reads and no conversions (bench, DWT: the search over
 * the flash table cost ~600 cycles a scan at priority 0). RF_MeterLinBuild
 * fills it from a table in port mV; rf_meter_lin_v gives what
 * rf_meter_port_v gives. */
typedef struct {
    float x[RF_METER_POINTS];
    float y[RF_METER_POINTS];
    float k[RF_METER_POINTS - 1U];
} rf_meter_lin_t;

void RF_MeterLinBuild(const uint16_t *port_mv, rf_meter_lin_t *out);

static inline float rf_meter_lin_v(const rf_meter_lin_t *l, float mv)
{
    if (!(mv > 0.0f)) return 0.0f;
    uint32_t i = (mv >= l->x[6]) ? 6U : 0U;
    if (mv >= l->x[i + 3U]) i += 3U;
    if (i < RF_METER_POINTS - 2U && mv >= l->x[i + 1U]) ++i;
    if (i < RF_METER_POINTS - 2U && mv >= l->x[i + 1U]) ++i;
    return l->y[i] + (mv - l->x[i]) * l->k[i];
}

/* One scan's verdict for the SWR trip: forward above the gate and reflected
 * above the limit's share of it. `limit` is |gamma| at the SWR limit,
 * (L - 1) / (L + 1). The caller takes the leakage's constant floor off `r`
 * (radio_analog.c, meter_floor); its proportional part is NOT taken out, so
 * the trip errs towards tripping. */
static inline uint8_t rf_meter_swr_over(float f, float r, float limit)
{
    return (uint8_t)((f >= RF_METER_GATE_V) && (r > f * limit));
}

/* What the ADC interrupt adds up between two readings, for the scans with RF
 * up. f and r are port peak volts after a 1 ms (4-scan) mean of the
 * detector outputs.
 *
 * Two estimates of the forward level. The peak of the 1 ms means follows a
 * voice peak (QMX reads PEP in SSB), but with ADC2 scattering tens of mV a
 * scan, the largest of 400 noisy means sits a few sigma high -- 12 % of power
 * at 0.5 W with 27 mV. The plateau is the mean of f^2 over the scans above
 * `plateau` volts, which the main loop sets to 90 % of the previous
 * interval's peak: a carrier's level with the noise averaged out and the
 * keying ramps and gaps left out. */
typedef struct {
    float sff;     /* sum of f^2 */
    float srf;     /* sum of r*f */
    float peak;    /* largest f */
    float pff;     /* sum of f^2 over the plateau scans */
    float drain;   /* sum of the mean drain voltage, mV */
    float vin;     /* sum of the supply voltage, mV */
    uint32_t n;    /* scans */
    uint32_t np;   /* plateau scans */
} rf_meter_acc_t;

static inline void rf_meter_acc_add(rf_meter_acc_t *a, float f, float r, float drain_mv, float vin_mv,
                                    float plateau)
{
    const float ff = f * f;
    a->sff += ff;
    a->srf += r * f;
    if (f > a->peak) a->peak = f;
    if (f >= plateau) { a->pff += ff; ++a->np; }
    a->drain += drain_mv;
    a->vin += vin_mv;
    ++a->n;
}

typedef struct {
    float pep_w;     /* forward power shown: the peak in voice, the plateau otherwise */
    float peak_w;    /* forward, the interval's largest 1 ms mean */
    float plateau_w; /* forward, the plateau's mean (the peak when it had too few scans) */
    float avg_w;     /* forward, mean over the scans with RF */
    float ref_w;     /* reflected: PEP times rho^2 */
    float net_w;     /* into the load: PEP - reflected */
    float rho;       /* |gamma|, the bridge's leakage taken out */
    float rho_raw;   /* |gamma| as the bridge reads it (what QMX shows) */
    float swr, swr_raw;
    float drain_v;   /* mean PA drain voltage */
    float vin_v;     /* mean supply voltage while transmitting (its sag) */
    uint16_t scans;
    uint8_t swr_valid; /* forward PEP >= 0.1 W */
    uint8_t reserved;
} rf_meter_reading_t;

/* The reading for one interval's sums. `gain` is the band's power
 * correction (1 = the bridge's theory). The bridge's own reflection on a
 * 50 ohm load has two parts: `leak`, a |gamma| proportional to the forward
 * wave, and `floor_v`, a reflected-port signal in port volts that does not
 * follow the drive (it dominates at low power on 20 m and up); both 0 when
 * not measured, both taken out in the power domain. `voice` 1 in SSB and AM,
 * whose envelope moves (pep_w is then the peak). */
void RF_MeterReading(const rf_meter_acc_t *a, float gain, float leak, float floor_v, uint8_t voice,
                     rf_meter_reading_t *out);
/* The two parts from two captures on a 50 ohm load at different drive:
 * forward port volts f and the |gamma| read, with rho^2 = leak^2 +
 * (floor / f)^2. One capture, or two too close together (under 1.4x in f),
 * gives leak alone. */
void RF_MeterLeakFit(float f1, float rho1, float f2, float rho2, float *leak, float *floor_v);
float RF_MeterFloorAt(const uint16_t *floor_100uv, uint32_t hz);
/* The next interval's plateau threshold from this one's peak, port volts. */
#define RF_METER_PLATEAU 0.9f
float RF_MeterSwr(float rho);
/* The anchor nearest `hz` (in ratio), 0..RF_METER_ANCHORS-1. */
uint8_t RF_MeterAnchor(uint32_t hz);
/* Stored per-anchor values: power gain x1000 (0 = 1.000) and leakage x10000
 * (0 = none), for the anchor nearest `hz`. */
float RF_MeterGainAt(const uint16_t *gain_x1000, uint32_t hz);
float RF_MeterLeakAt(const uint16_t *leak_x10000, uint32_t hz);
/* A board's detector table, or the built-in one while it holds none (all
 * zero). RF_MeterTableValid: starts at 0, rises strictly, never below the
 * detector output itself, and stays under 4 V. */
const uint16_t *RF_MeterTable(const uint16_t *board);
uint8_t RF_MeterTableValid(const uint16_t *t);

#endif /* RADIO_RF_METER_H */

/* Size over speed: the ADC interrupt only reads the tables here; the
 * arithmetic below runs in the main loop a few times a second. */
#pragma GCC optimize ("Os")

#include "radio_rf_meter.h"
#include <math.h>

const uint16_t rf_meter_vdc_mv[RF_METER_POINTS] = RF_METER_VDC_MV;
const uint16_t rf_meter_default_port_mv[RF_METER_POINTS] = RF_METER_DEFAULT_PORT_MV;
/* 1 / (rf_meter_vdc_mv[i + 1] - rf_meter_vdc_mv[i]): no division per scan. */
const float rf_meter_inv_width[RF_METER_POINTS - 1U] = {
    1.0f / 6.0f, 1.0f / 9.0f, 1.0f / 15.0f, 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 100.0f,
    1.0f / 160.0f, 1.0f / 270.0f, 1.0f / 450.0f, 1.0f / 700.0f, 1.0f / 1100.0f
};
/* Mid-band on 160, 80, 60, 40, 30, 20, 17, 15, 12 and 10 m. */
const uint32_t rf_meter_anchor_hz[RF_METER_ANCHORS] = {
    1900000UL, 3650000UL, 5355000UL, 7100000UL, 10125000UL,
    14175000UL, 18118000UL, 21225000UL, 24940000UL, 28500000UL
};

void RF_MeterLinBuild(const uint16_t *port_mv, rf_meter_lin_t *out)
{
    for (uint32_t i = 0U; i < RF_METER_POINTS; ++i) {
        out->x[i] = (float)rf_meter_vdc_mv[i];
        out->y[i] = 0.001f * (float)port_mv[i];
    }
    for (uint32_t i = 0U; i + 1U < RF_METER_POINTS; ++i)
        out->k[i] = (out->y[i + 1U] - out->y[i]) / (out->x[i + 1U] - out->x[i]);
}

float RF_MeterSwr(float rho)
{
    if (!(rho > 0.0f)) return 1.0f;
    if (rho >= 0.98f) return RF_METER_SWR_MAX;
    const float swr = (1.0f + rho) / (1.0f - rho);
    return swr < RF_METER_SWR_MAX ? swr : RF_METER_SWR_MAX;
}

void RF_MeterReading(const rf_meter_acc_t *a, float gain, float leak, float floor_v, uint8_t voice,
                     rf_meter_reading_t *out)
{
    *out = (rf_meter_reading_t){0};
    out->swr = out->swr_raw = 1.0f;
    if (a->n == 0U) return;
    if (!(gain > 0.0f)) gain = 1.0f;
    if (!(leak > 0.0f)) leak = 0.0f;
    if (!(floor_v > 0.0f)) floor_v = 0.0f;
    out->scans = (uint16_t)(a->n > 0xFFFFUL ? 0xFFFFUL : a->n);
    out->peak_w = a->peak * a->peak * RF_METER_W_PER_V2 * gain;
    out->plateau_w = (a->np >= RF_METER_MIN_SCANS) ? a->pff / (float)a->np * RF_METER_W_PER_V2 * gain : out->peak_w;
    out->pep_w = voice ? out->peak_w : out->plateau_w;
    out->avg_w = a->sff / (float)a->n * RF_METER_W_PER_V2 * gain;
    out->drain_v = a->drain / (float)a->n * 0.001f;
    out->vin_v = a->vin / (float)a->n * 0.001f;
    /* Least squares of r on f: the strong scans of a key-down or a voice
     * peak decide it, the ramps and the gaps hardly count. */
    float rho = (a->sff > 0.0f) ? a->srf / a->sff : 0.0f;
    if (!(rho > 0.0f)) rho = 0.0f;
    if (rho > 0.999f) rho = 0.999f;
    out->rho_raw = rho;
    /* The bridge's leakage adds to the load's reflection at a phase nothing
     * here can see; on average the two add in power, so that is where it is
     * taken out: a 50 ohm load reads 1.00, a real mismatch keeps its size.
     * The floor is relative to the carrier's port voltage (the plateau's,
     * the peak's without one). */
    const float fv = (a->np >= RF_METER_MIN_SCANS) ? sqrtf(a->pff / (float)a->np) : a->peak;
    const float fr = (fv > 0.0f) ? floor_v / fv : 0.0f;
    const float r2 = rho * rho - leak * leak - fr * fr;
    out->rho = (r2 > 0.0f) ? sqrtf(r2) : 0.0f;
    out->swr_raw = RF_MeterSwr(out->rho_raw);
    out->swr = RF_MeterSwr(out->rho);
    out->ref_w = out->pep_w * out->rho * out->rho;
    out->net_w = out->pep_w - out->ref_w;
    out->swr_valid = (uint8_t)((a->peak >= RF_METER_GATE_V) && (a->n >= RF_METER_MIN_SCANS));
}

uint8_t RF_MeterAnchor(uint32_t hz)
{
    uint8_t best = 0U;
    float best_ratio = 1e9f;
    for (uint8_t i = 0U; i < RF_METER_ANCHORS; ++i) {
        const float a = (float)rf_meter_anchor_hz[i], f = (float)hz;
        const float ratio = (f > a) ? f / a : a / f;
        if (ratio < best_ratio) { best_ratio = ratio; best = i; }
    }
    return best;
}

float RF_MeterGainAt(const uint16_t *gain_x1000, uint32_t hz)
{
    const uint16_t g = gain_x1000[RF_MeterAnchor(hz)];
    return g ? (float)g * 0.001f : 1.0f;
}

float RF_MeterLeakAt(const uint16_t *leak_x10000, uint32_t hz)
{
    return (float)leak_x10000[RF_MeterAnchor(hz)] * 0.0001f;
}

void RF_MeterLeakFit(float f1, float rho1, float f2, float rho2, float *leak, float *floor_v)
{
    *leak = rho1 > 0.0f ? rho1 : 0.0f;
    *floor_v = 0.0f;
    if (!(f1 > 0.0f) || !(f2 > 0.0f) || !(rho2 > 0.0f)) return;
    if (f2 > f1) { float t = f1; f1 = f2; f2 = t; t = rho1; rho1 = rho2; rho2 = t; }
    if (f1 < 1.4f * f2) { *leak = rho1; return; } /* the stronger capture alone */
    const float a = f1 * f1, b = f2 * f2;
    float l2 = (rho1 * rho1 * a - rho2 * rho2 * b) / (a - b);
    if (l2 < 0.0f) l2 = 0.0f;
    float c2 = (rho2 * rho2 - l2) * b;
    if (c2 < 0.0f) c2 = 0.0f;
    *leak = sqrtf(l2);
    *floor_v = sqrtf(c2);
}

float RF_MeterFloorAt(const uint16_t *floor_100uv, uint32_t hz)
{
    return (float)floor_100uv[RF_MeterAnchor(hz)] * 0.0001f;
}

uint8_t RF_MeterTableValid(const uint16_t *t)
{
    if (t[0] != 0U || t[RF_METER_POINTS - 1U] > 4000U) return 0U;
    for (uint32_t i = 1U; i < RF_METER_POINTS; ++i)
        if (t[i] <= t[i - 1U] || t[i] < rf_meter_vdc_mv[i]) return 0U;
    return 1U;
}

const uint16_t *RF_MeterTable(const uint16_t *board)
{
    return (board[RF_METER_POINTS - 1U] != 0U) ? board : rf_meter_default_port_mv;
}

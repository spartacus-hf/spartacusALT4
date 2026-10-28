#include "radio_bpf_sweep.h"
#include <math.h>

/* (28333333 / 4000000)^(1/63): 64 log-spaced points, about 3.2 % apart. */
#define SWEEP_RATIO 1.0315632f
/* The LPF axis starts at the test clock's own floor, so the 160 m filter's
 * passband is on the plot: (28333333 / 1000000)^(1/63), about 5.5 % apart. */
#define SWEEP_LPF_FIRST_HZ 1000000UL
#define SWEEP_LPF_RATIO 1.0545140f

uint32_t RADIO_SweepGridHz(uint8_t axis, uint32_t centre, unsigned k)
{
    if (k >= RADIO_SWEEP_POINTS) k = RADIO_SWEEP_POINTS - 1U;
    if (axis == RADIO_SWEEP_BAND) {
        /* +-40 % of centre, linear, which puts the peak and both skirts of a
         * QMX band-pass section on one plot whatever the band. */
        if (!centre) centre = 7100000UL;
        const float low = 0.6f * (float)centre, high = 1.4f * (float)centre;
        float hz = low + (high - low) * ((float)k / (float)(RADIO_SWEEP_POINTS - 1U));
        if (hz < 1000000.0f) hz = 1000000.0f;
        if (hz > (float)RADIO_RX_MAX_HZ) hz = (float)RADIO_RX_MAX_HZ;
        return (uint32_t)(hz + 0.5f);
    }
    if (k == RADIO_SWEEP_POINTS - 1U) return RADIO_RX_MAX_HZ;
    const uint8_t lpf = axis == RADIO_SWEEP_LPF;
    float hz = (float)(lpf ? SWEEP_LPF_FIRST_HZ : RADIO_SWEEP_FIRST_HZ);
    const float ratio = lpf ? SWEEP_LPF_RATIO : SWEEP_RATIO;
    while (k--) hz *= ratio;
    return (uint32_t)(hz + 0.5f);
}

uint8_t RADIO_SweepCode(float dbfs)
{
    if (!(dbfs > -127.0f)) return 1U;
    if (dbfs >= 0.0f) return 255U;
    return (uint8_t)(2.0f * (dbfs + 127.5f) + 0.5f);
}

float RADIO_SweepCodeDb(uint8_t code) { return 0.5f * (float)code - 127.5f; }


/* Frequency ratio >= 1 between two edges (log-distance without logf). */
static float edge_ratio(uint32_t a, uint32_t b)
{
    return a > b ? (float)a / (float)b : (float)b / (float)a;
}

__attribute__((cold))
uint8_t RADIO_SweepFitEdges(const uint8_t level[][RADIO_SWEEP_SLOTS], const uint32_t current[4], uint32_t edges[4])
{
    enum { K = RADIO_SWEEP_POINTS };
    uint8_t order[4], vote[K];
    uint32_t cut_hz[K - 1]; /* geometric mean of points c and c + 1 */
    unsigned m = 0U, voters = 0U;
    float point = (float)RADIO_SWEEP_FIRST_HZ;
    for (unsigned c = 0U; c + 1U < K; ++c) {
        float next = c + 2U < K ? point * SWEEP_RATIO : (float)RADIO_RX_MAX_HZ;
        cut_hz[c] = (uint32_t)(sqrtf(point * next) + 0.5f);
        point = next;
    }
    for (unsigned s = 0U; s < 4U; ++s) {
        if (!current[s]) continue;
        unsigned j = m++;
        while (j && current[order[j - 1U]] > current[s]) { order[j] = order[j - 1U]; --j; }
        order[j] = (uint8_t)s;
    }
    for (unsigned k = 0U; k < K; ++k) {
        vote[k] = level[k][0] && level[k][1] && level[k][2] && level[k][3];
        voters += vote[k];
    }
    if (m < 2U || !voters) return 0U;
    /* best[k]: top score with runs 0..j covering points 0..k (int16: at most
     * 64 x 255). cut[j][k]: last point of run j-1 in that solution. */
    int16_t best[K], next[K];
    uint8_t cut[4][K];
    int32_t sum = 0;
    for (unsigned k = 0U; k < K; ++k) {
        sum += vote[k] ? level[k][order[0]] : 0;
        best[k] = (int16_t)sum;
    }
    for (unsigned j = 1U; j < m; ++j) {
        const uint32_t previous = current[order[j - 1U]];
        for (unsigned k = 0U; k < K; ++k) next[k] = INT16_MIN;
        for (unsigned k = j; k < K; ++k) {
            int32_t segment = 0, top = INT32_MIN;
            unsigned arg = k - 1U;
            float distance = 0.0f; /* ratio to the current edge, >= 1 */
            for (unsigned c = k; c-- > j - 1U;) {
                segment += vote[c + 1U] ? level[c + 1U][order[j]] : 0;
                int32_t total = (int32_t)best[c] + segment;
                if (total < top) continue;
                float d = edge_ratio(cut_hz[c], previous);
                if (total > top || d < distance) { top = total; arg = c; distance = d; }
            }
            next[k] = (int16_t)top;
            cut[j][k] = (uint8_t)arg;
        }
        for (unsigned k = 0U; k < K; ++k) best[k] = next[k];
    }
    uint32_t out[4] = {0U, 0U, 0U, 0U};
    unsigned k = K - 1U;
    out[order[m - 1U]] = RADIO_RX_MAX_HZ;
    for (unsigned j = m - 1U; j > 0U; --j) {
        k = cut[j][k];
        out[order[j - 1U]] = cut_hz[k];
    }
    for (unsigned s = 0U; s < 4U; ++s) edges[s] = out[s];
    return 1U;
}

float RADIO_ChopWeight(unsigned index, unsigned halves)
{
    if (!halves || index >= halves) return 0.0f;
    float s = sinf(3.14159265f * ((float)index + 0.5f) / (float)halves);
    return s * s;
}

void RADIO_ChopAdd(radio_chop_t *a, uint8_t on, float weight, float i, float q)
{
    if (!on) weight = -weight;
    a->i += weight * i;
    a->q += weight * q;
    a->weight += fabsf(weight);
    float m = fabsf(i) > fabsf(q) ? fabsf(i) : fabsf(q);
    if (m > a->peak) a->peak = m;
}

float RADIO_ChopDbfs(const radio_chop_t *a)
{
    if (!(a->weight > 0.0f)) return -200.0f;
    float m = 2.0f * sqrtf(a->i * a->i + a->q * a->q) / a->weight;
    return m > 1e-9f ? 20.0f * log10f(m) : -180.0f;
}

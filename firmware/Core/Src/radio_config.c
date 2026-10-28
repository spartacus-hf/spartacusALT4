/* Size over speed: nothing here runs in the RF or audio path. */
#pragma GCC optimize ("Os")

#include "radio_config.h"
#include "radio_rf_meter.h"
#include <math.h>
#include <string.h>

void RADIO_ConfigDefaults(radio_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->signature = RADIO_CONFIG_SIGNATURE;
    /* The ordered REV1 assembly is fitted with UCC27524 drivers on both
     * branches. TX still requires explicit confirmation of the whole setup. */
    c->driver_a = c->driver_b = RADIO_DRIVER_UCC27524;
    /* PD5/PD6 are wired like the other three buttons but drive radio actions
     * rather than the menu; leaving them unassigned wastes two panel keys. */
    c->sw4 = RADIO_SW_PTT;
    c->sw5 = RADIO_SW_POWER;
    RADIO_ConfigEncoderDefaults(c);
    c->backlight = 70U;
    c->volume = 20U;
    c->bias_ma = RADIO_BIAS_MA_DEFAULT;
    c->bias_limit_mv = 2800U;
    c->vin_limit_mv = 14700U;
    /* USB-C starts at the mandatory 5 V (R3/R4 Rd), where the LM1084-5 cannot
     * regulate; request a working rail as soon as the source advertises it. */
    c->pd_enable = 1U;
    c->pd_target_mv = 9000U;
    c->pd_max_mv = 12000U;
    c->pd_current_ma = 1000U;
    c->swr_limit_x100 = 300U;
    c->envelope_limit_mv = 12000U;
    c->rx_i_gain = c->rx_q_gain = c->mic_gain = 1.0f;
    c->swr_gain[0] = c->swr_gain[1] = 1.0f;
    c->clock_flags = RADIO_CLOCK_FLAGS_DEFAULT;
    RADIO_ConfigBpfDefaults(c);
    RADIO_ConfigLpfDefaults(c);
    RADIO_ConfigLpfUnfitted(c);
    /* The LPF tested mask is a bench result, intentionally zero: it gates
     * TX. The filters are fitted as QMX 60-15m / QMX+ (below); the PIN bias
     * is the QMX 30 mA default. */
}

/* REV1 receive BPF = QMX 60-15m (assembly 1.04n, G0UPL 2024-05-02), switched
 * by U10 BAND_1:BAND_0. Every coil starts at RX_IN (L401 C1A):
 *   slot 0  2B1  C403  39 pF  L401a tap (15 of 24 t)  15 m
 *   slot 1  2B2  C401  56 pF  L401a all 24 t          20 m, 17 m
 *   slot 2  2B3  C404 150 pF  L401b tap (11 of 34 t)  30 m
 *   slot 3  2B4  C402 220 pF  L401b all 34 t          60 m, 40 m
 * Crossovers follow the QMX RF sweep shapes: slot 3 rolls off above 8 MHz,
 * slot 2 stays flat to 13 MHz while slot 1 is 6 dB below its peak there, and
 * slot 1 has a deep notch just above 18.3 MHz. Each ham band keeps its QMX
 * filter; the BPF sweep replaces these with measured crossovers. */
void RADIO_ConfigBpfDefaults(radio_config_t *c)
{
    c->bpf_upper_hz[0] = RADIO_RX_MAX_HZ;
    c->bpf_upper_hz[1] = 18500000UL;
    c->bpf_upper_hz[2] = 13500000UL;
    c->bpf_upper_hz[3] = 8500000UL;
}

/* REV1 LPF = QMX+ 160-6m set without the 6 m filter (L519/L520), same
 * parts and designators, LPF_n = PE(2+n). Each is C-L-C-(L||C)-C between the
 * PA-side PIN diodes (RX tap C65) and the antenna-side diodes (TEST_CLK):
 *   LPF_0  C514 1200p  L506 4.42uH  C517 2200p  L511 3.54uH||C520 220p  C523 820p  160 m
 *   LPF_1  C515  680p  L508 1.83uH  C518 1200p  L512 1.55uH||C521 100p  C524 680p  80, 60 m
 *   LPF_2  C516  390p  L510 1.03uH  C519  680p  L513  866nH||C522  43p  C525 390p  40, 30 m
 *   LPF_3  C531  180p  L515  518nH  C532  330p  L516  435nH||C533  27p  C534 180p  20, 17, 15 m
 *   LPF_4  C535  120p  L517  313nH  C536  180p  L518  268nH||C537  15p  C538  82p  12, 11, 10 m
 * Upper edges keep every band on its QMX+ filter and move receive to the next
 * filter before the loss rises (50 ohm model: 3 dB at 2.9/6.2/11.2/22.4/38
 * MHz). The notches sit on the 3rd harmonic, because the push-pull PA has low
 * even harmonics. The TX floor is the lowest frequency with at least 10 dB
 * at 2f, 40 dB at 3f and 50 dB at 4f..9f of extra loss, which QMX+ itself
 * only just meets at the bottom of 80 m (11.9 dB at 2f, 43.1 dB at 3f).
 * Between one filter's edge and the next floor (2.5-3.45, 5.6-6.3,
 * 10.3-12.2 MHz, and below 1.7 MHz) this filter set cannot transmit
 * cleanly; receive is unaffected. Host suite rev1_lpf re-derives this. */
void RADIO_ConfigLpfDefaults(radio_config_t *c)
{
    static const uint32_t upper[5] = {2500000UL, 5600000UL, 10300000UL, 21450000UL, RADIO_RX_MAX_HZ};
    static const uint16_t tx_min[5] = {1700U, 3450U, 6300U, 12200U, 20500U};
    memcpy(c->lpf_upper_hz, upper, sizeof(upper));
    memcpy(c->lpf_tx_min_khz, tx_min, sizeof(tx_min));
}

/* Slots whose toroids are not wound. A zero upper edge takes the slot out of
 * covering_slot() entirely, so it is never selected for receive or for
 * transmit and its BSS123 is never turned on; a zero TX floor blocks TX on it
 * a second time. This is the same "slot not in use" encoding the edges already
 * had, so nothing else has to know about it.
 *
 * Current build: every toroid is wound, so the mask is empty and a fresh
 * install gets all five QMX+ filters and the full 1.0-28.3 MHz range. It stays
 * here because a part shortage is the normal reason to ship a partial filter
 * set, and because the same encoding is what LPF n UPPER MHZ writes when an
 * operator takes one filter out of service from the menu. A board whose
 * settings were saved while a slot was dark restores them with QMX+ LPF EDGES
 * in BOARD > FILTERS, or `set put <id> 0` on the console -- no reflash. */
#define LPF_UNFITTED_MASK 0x00U /* bit n disables LPF_n */

void RADIO_ConfigLpfUnfitted(radio_config_t *c)
{
    for (unsigned i = 0U; i < 5U; ++i) {
        if (!((LPF_UNFITTED_MASK >> i) & 1U)) continue;
        c->lpf_upper_hz[i] = 0UL;
        c->lpf_tx_min_khz[i] = 0U;
    }
}

/* The three spare JP20 pins carry a plain mechanical encoder: A and B are the
 * quadrature pair and the shaft switch is OK. Nothing on the PCB pulls them, so
 * the firmware supplies the pull and the common goes to GND (the KY-040 and
 * bare EC11 convention). enc_detent 4 is one UI step per mechanical click on
 * the usual 1-detent-per-quadrature-cycle encoder. */
void RADIO_ConfigEncoderDefaults(radio_config_t *c)
{
    c->enc_a = RADIO_ENC_PIN_PC11;
    c->enc_b = RADIO_ENC_PIN_PC12;
    c->enc_sw = RADIO_ENC_PIN_PC13;
    c->enc_flags = 0U; /* active low, internal pull-up */
    c->enc_detent = 4U;
    c->enc_reserved = 0U;
}

/* A pin may serve only one encoder line; a repeated selection would make one
 * physical pin drive two decoders. OFF is allowed on every line. */
static uint8_t encoder_pins_distinct(const radio_config_t *c)
{
    const uint8_t pin[3] = {c->enc_a, c->enc_b, c->enc_sw};
    for (unsigned i = 0U; i < 3U; ++i) {
        if (pin[i] == RADIO_ENC_PIN_OFF) continue;
        for (unsigned j = 0U; j < i; ++j)
            if (pin[j] == pin[i]) return 0U;
    }
    return 1U;
}

static const uint16_t cw_bandwidths[RADIO_CW_BW_COUNT] = RADIO_CW_BW_TABLE;

/* Each CW receive byte is 0 (its default) or a legal value. Out of line to
 * keep RADIO_ConfigValid within its complexity budget. */
static __attribute__((noinline)) uint8_t cw_receive_valid(const radio_config_t *c)
{
    if (c->cw_pitch_10hz && (c->cw_pitch_10hz < RADIO_CW_PITCH_MIN_HZ / 10U ||
                             c->cw_pitch_10hz > RADIO_CW_PITCH_MAX_HZ / 10U)) return 0U;
    if (c->cw_flags & ~RADIO_CW_FLAG_MASK) return 0U;
    if ((c->cw_flags & RADIO_CW_AGC_MASK) > RADIO_CW_AGC_FAST) return 0U;
    if (!c->cw_bw_10hz) return 1U;
    for (unsigned i = 0U; i < RADIO_CW_BW_COUNT; ++i)
        if (c->cw_bw_10hz * 10U == cw_bandwidths[i]) return 1U;
    return 0U;
}

static const uint16_t ssb_bandwidths[RADIO_SSB_BW_COUNT] = RADIO_SSB_BW_TABLE;
static const uint16_t am_bandwidths[RADIO_AM_BW_COUNT] = RADIO_AM_BW_TABLE;

/* A width byte is 0 (default) or one of the table's entries in its units. */
static uint8_t table_index(const uint16_t *table, unsigned n, uint16_t want, uint16_t fallback_hz)
{
    uint8_t fallback = 0U;
    for (uint8_t i = 0U; i < n; ++i) {
        if (table[i] == want) return i;
        if (table[i] == fallback_hz) fallback = i;
    }
    return fallback;
}

static uint8_t in_table(const uint16_t *table, unsigned n, uint32_t hz)
{
    for (unsigned i = 0U; i < n; ++i) if (table[i] == hz) return 1U;
    return 0U;
}

/* Each v27 voice-receive byte is 0 (its default) or a legal value. */
static __attribute__((noinline)) uint8_t voice_receive_valid(const radio_config_t *c)
{
    if (c->ssb_bw_100hz && !in_table(ssb_bandwidths, RADIO_SSB_BW_COUNT, c->ssb_bw_100hz * 100UL)) return 0U;
    if (c->am_bw_khz && !in_table(am_bandwidths, RADIO_AM_BW_COUNT, c->am_bw_khz * 1000UL)) return 0U;
    return (uint8_t)(c->rx_agc <= RADIO_RX_AGC_FAST && c->fm_squelch <= RADIO_FM_SQUELCH_MAX);
}

uint8_t RADIO_ConfigSsbBandwidthIndex(const radio_config_t *c)
{
    const uint16_t want = c->ssb_bw_100hz ? (uint16_t)(c->ssb_bw_100hz * 100U) : RADIO_SSB_BW_DEFAULT_HZ;
    return table_index(ssb_bandwidths, RADIO_SSB_BW_COUNT, want, RADIO_SSB_BW_DEFAULT_HZ);
}
uint16_t RADIO_ConfigSsbBandwidthHz(const radio_config_t *c) { return ssb_bandwidths[RADIO_ConfigSsbBandwidthIndex(c)]; }
uint16_t RADIO_SsbBandwidthAt(uint8_t index)
{
    return ssb_bandwidths[index < RADIO_SSB_BW_COUNT ? index : RADIO_SSB_BW_COUNT - 1U];
}
uint8_t RADIO_ConfigAmBandwidthIndex(const radio_config_t *c)
{
    const uint16_t want = c->am_bw_khz ? (uint16_t)(c->am_bw_khz * 1000U) : RADIO_AM_BW_DEFAULT_HZ;
    return table_index(am_bandwidths, RADIO_AM_BW_COUNT, want, RADIO_AM_BW_DEFAULT_HZ);
}
uint16_t RADIO_ConfigAmBandwidthHz(const radio_config_t *c) { return am_bandwidths[RADIO_ConfigAmBandwidthIndex(c)]; }
uint16_t RADIO_AmBandwidthAt(uint8_t index)
{
    return am_bandwidths[index < RADIO_AM_BW_COUNT ? index : RADIO_AM_BW_COUNT - 1U];
}
uint8_t RADIO_ConfigRxAgc(const radio_config_t *c)
{
    return (c->rx_agc >= RADIO_RX_AGC_OFF && c->rx_agc <= RADIO_RX_AGC_FAST) ? c->rx_agc : RADIO_RX_AGC_DEFAULT;
}

void RADIO_ConfigChannelSpans(const radio_config_t *c, int16_t spans[5][2])
{
    const int16_t ssb = (int16_t)RADIO_ConfigSsbBandwidthHz(c), low = (int16_t)RADIO_SSB_LOW_CUT_HZ;
    const int16_t cw = (int16_t)(RADIO_ConfigCwBandwidthHz(c) / 2U);
    const int16_t am = (int16_t)(RADIO_ConfigAmBandwidthHz(c) / 2U);
    spans[RADIO_RX_USB][0] = low;                       spans[RADIO_RX_USB][1] = (int16_t)(low + ssb);
    spans[RADIO_RX_LSB][0] = (int16_t)-(low + ssb);     spans[RADIO_RX_LSB][1] = (int16_t)-low;
    spans[RADIO_RX_CW][0] = (int16_t)-cw;               spans[RADIO_RX_CW][1] = cw;
    spans[RADIO_RX_AM][0] = (int16_t)-am;               spans[RADIO_RX_AM][1] = am;
    spans[RADIO_RX_FM][0] = -10000;                     spans[RADIO_RX_FM][1] = 10000;
}

uint16_t RADIO_ConfigCwPitchHz(const radio_config_t *c)
{
    const uint16_t hz = (uint16_t)(c->cw_pitch_10hz * 10U);
    return (hz >= RADIO_CW_PITCH_MIN_HZ && hz <= RADIO_CW_PITCH_MAX_HZ) ? hz : RADIO_CW_PITCH_DEFAULT_HZ;
}

uint8_t RADIO_ConfigCwBandwidthIndex(const radio_config_t *c)
{
    const uint16_t want = c->cw_bw_10hz ? (uint16_t)(c->cw_bw_10hz * 10U) : RADIO_CW_BW_DEFAULT_HZ;
    uint8_t fallback = 0U;
    for (uint8_t i = 0U; i < RADIO_CW_BW_COUNT; ++i) {
        if (cw_bandwidths[i] == want) return i;
        if (cw_bandwidths[i] == RADIO_CW_BW_DEFAULT_HZ) fallback = i;
    }
    return fallback;
}

uint16_t RADIO_ConfigCwBandwidthHz(const radio_config_t *c)
{
    return cw_bandwidths[RADIO_ConfigCwBandwidthIndex(c)];
}

uint16_t RADIO_CwBandwidthAt(uint8_t index)
{
    return cw_bandwidths[index < RADIO_CW_BW_COUNT ? index : RADIO_CW_BW_COUNT - 1U];
}

uint8_t RADIO_ConfigCwAgc(const radio_config_t *c)
{
    return (c->cw_flags & RADIO_CW_AGC_MASK) == RADIO_CW_AGC_FAST ? RADIO_CW_AGC_FAST : RADIO_CW_AGC_SLOW;
}

uint8_t RADIO_ConfigCwDecode(const radio_config_t *c)
{
    return (uint8_t)((c->cw_flags & RADIO_CW_FLAG_NO_DECODE) == 0U);
}

/* Each v28 meter field is 0 (its default) or a legal value. A detector table
 * is all zero or a curve the meter can use: from 0, strictly rising, never
 * below the detector output itself, under 4 V. */
static __attribute__((noinline)) uint8_t meter_valid(const radio_config_t *c)
{
    static const uint16_t vdc[RF_METER_POINTS] = RF_METER_VDC_MV;
    const uint16_t update_ms = (uint16_t)(c->meter_update_10ms * 10U);
    if ((c->meter_flags & ~RADIO_METER_FLAG_MASK) ||
        (c->meter_update_10ms && (update_ms < RADIO_METER_UPDATE_MIN_MS || update_ms > RADIO_METER_UPDATE_MAX_MS)) ||
        c->meter_hang_100ms > RADIO_METER_HANG_MAX_MS / 100U ||
        (c->tune_pct && (c->tune_pct < RADIO_TUNE_PCT_MIN || c->tune_pct > 100U))) return 0U;
    if (c->meter_port_mv[RF_METER_POINTS - 1U]) {
        if (c->meter_port_mv[0] || c->meter_port_mv[RF_METER_POINTS - 1U] > 4000U) return 0U;
        for (unsigned i = 1U; i < RF_METER_POINTS; ++i)
            if (c->meter_port_mv[i] <= c->meter_port_mv[i - 1U] || c->meter_port_mv[i] < vdc[i]) return 0U;
    } else {
        for (unsigned i = 0U; i < RF_METER_POINTS; ++i) if (c->meter_port_mv[i]) return 0U;
    }
    for (unsigned i = 0U; i < RF_METER_ANCHORS; ++i) {
        if (c->meter_gain_x1000[i] && (c->meter_gain_x1000[i] < 250U || c->meter_gain_x1000[i] > 4000U)) return 0U;
        if (c->meter_leak_x10000[i] > 5000U || c->meter_floor_100uv[i] > 5000U) return 0U;
    }
    return 1U;
}

uint8_t RADIO_ConfigTxPanel(const radio_config_t *c) { return (uint8_t)((c->meter_flags & RADIO_METER_FLAG_PANEL_OFF) == 0U); }

uint16_t RADIO_ConfigMeterUpdateMs(const radio_config_t *c)
{
    const uint16_t ms = (uint16_t)(c->meter_update_10ms * 10U);
    return (ms >= RADIO_METER_UPDATE_MIN_MS && ms <= RADIO_METER_UPDATE_MAX_MS) ? ms : RADIO_METER_UPDATE_DEF_MS;
}

uint16_t RADIO_ConfigMeterHangMs(const radio_config_t *c)
{
    return (c->meter_hang_100ms && c->meter_hang_100ms <= RADIO_METER_HANG_MAX_MS / 100U)
         ? (uint16_t)(c->meter_hang_100ms * 100U) : RADIO_METER_HANG_DEF_MS;
}

uint8_t RADIO_ConfigTunePct(const radio_config_t *c)
{
    return (c->tune_pct >= RADIO_TUNE_PCT_MIN && c->tune_pct <= 100U) ? c->tune_pct : RADIO_TUNE_PCT_DEF;
}

uint8_t RADIO_ConfigValid(const radio_config_t *c)
{
    if (!c || c->signature != RADIO_CONFIG_SIGNATURE) return 0U;
    if (c->confirmed > 1U || c->driver_a > 2U || c->driver_b > 2U ||
        c->drain_a > 2U || c->drain_b > 2U || c->buttons_high > 1U ||
        c->sw4 > RADIO_SW_LAST || c->sw5 > RADIO_SW_LAST ||
        c->backlight > 100U || c->volume > 100U || c->rx_mode > RADIO_RX_FM ||
        c->mic_source > RADIO_SOURCE_MIC3 || c->usb_iq > 1U ||
        c->envelope_mode > 2U || c->aux_tx_mask > 7U || c->pd_enable > 1U ||
        c->pd_internal_rd > 1U || c->rx_iq_invert > 1U) return 0U;
    if (c->enc_a >= RADIO_ENC_PIN_COUNT || c->enc_b >= RADIO_ENC_PIN_COUNT ||
        c->enc_sw >= RADIO_ENC_PIN_COUNT || c->enc_flags > RADIO_ENC_FLAG_MASK ||
        c->enc_detent < RADIO_ENC_DETENT_MIN || c->enc_detent > RADIO_ENC_DETENT_MAX ||
        c->enc_reserved || !encoder_pins_distinct(c)) return 0U;
    if (c->ref_ppb > RADIO_REF_PPB_LIMIT || c->ref_ppb < -RADIO_REF_PPB_LIMIT ||
        (c->clock_flags & ~RADIO_CLOCK_FLAG_MASK) || !cw_receive_valid(c) || !voice_receive_valid(c) ||
        !meter_valid(c)) return 0U;
    if (c->bias_limit_mv > 2850U || c->bias_ma < RADIO_BIAS_MA_OFF || c->bias_ma > RADIO_BIAS_MA_MAX ||
        c->vin_limit_mv < 5000U || c->vin_limit_mv > 14700U ||
        c->envelope_limit_mv > 14000U || c->pd_target_mv < 5000U ||
        c->pd_target_mv > 14000U || c->pd_max_mv < 5000U || c->pd_max_mv > 14000U ||
        c->pd_current_ma < 100U || c->pd_current_ma > 3000U ||
        (c->qualified_bands & ~31UL) ||
        c->swr_limit_x100 < 120U || c->swr_limit_x100 > 1000U) return 0U;
    for (unsigned i = 0; i < 2U; ++i) {
        if (!isfinite(c->envelope_dac_per_volt[i]) || c->envelope_dac_per_volt[i] < 0.0f ||
            c->envelope_dac_per_volt[i] > 4095.0f || !isfinite(c->swr_gain[i]) ||
            c->swr_gain[i] <= 0.0f || c->swr_gain[i] > 100.0f ||
            !isfinite(c->swr_offset[i]) || fabsf(c->swr_offset[i]) > 2.9f) return 0U;
    }
    if (!isfinite(c->rx_i_gain) || !isfinite(c->rx_q_gain) || !isfinite(c->rx_skew_samples) ||
        !isfinite(c->mic_gain) || c->rx_i_gain < 0.1f || c->rx_i_gain > 10.0f ||
        c->rx_q_gain < 0.1f || c->rx_q_gain > 10.0f ||
        fabsf(c->rx_skew_samples) > RADIO_IQ_SKEW_MAX ||
        c->mic_gain < 0.1f || c->mic_gain > 100.0f) return 0U;
    /* Slot numbers follow the PCB (LPF_0..4 and the BAND_0/1 mux channel),
     * which need not be in frequency order. Edges only have to be legal and
     * distinct so the selection below is unambiguous. */
    for (unsigned i = 0; i < 5U; ++i) {
        if (c->lpf_upper_hz[i] > RADIO_RX_MAX_HZ || c->lpf_tx_min_khz[i] > RADIO_RX_MAX_HZ / 1000U) return 0U;
        for (unsigned j = 0; j < i; ++j)
            if (c->lpf_upper_hz[i] && c->lpf_upper_hz[i] == c->lpf_upper_hz[j]) return 0U;
    }
    for (unsigned i = 0; i < 4U; ++i) {
        if (c->bpf_upper_hz[i] > RADIO_RX_MAX_HZ) return 0U;
        for (unsigned j = 0; j < i; ++j)
            if (c->bpf_upper_hz[i] && c->bpf_upper_hz[i] == c->bpf_upper_hz[j]) return 0U;
    }
    return 1U;
}

/* The lowest configured upper edge that still covers hz, whatever its slot. */
static unsigned covering_slot(const uint32_t *upper, unsigned count, uint32_t hz)
{
    unsigned best = count;
    for (unsigned i = 0U; i < count; ++i)
        if (upper[i] && hz <= upper[i] && (best == count || upper[i] < upper[best])) best = i;
    return best;
}

/* Receive needs far less than transmit, but it still drives RX_EN, the QSD
 * clocks and an LPF/BPF pair. None of that may happen on a board whose assembly
 * the operator has not described, so a fresh install is completely silent until
 * ASSEMBLY CONFIRM: the UI opens on the board pages and nothing else runs. */
const char *RADIO_ConfigRxBlock(const radio_config_t *c)
{
    if (!RADIO_ConfigValid(c)) return "CONFIG INVALID";
    if (!c->confirmed) return "NOT CONFIRMED";
    return NULL;
}

static const char *volatile storage_tx_lock;

void RADIO_ConfigSetStorageTxLock(const char *why) { storage_tx_lock = why; }
const char *RADIO_ConfigStorageTxLock(void) { return storage_tx_lock; }

const char *RADIO_ConfigTxBlock(const radio_config_t *c, uint32_t hz)
{
    const char *rx = RADIO_ConfigRxBlock(c);
    if (rx) return rx;
    if (storage_tx_lock) return storage_tx_lock;
    if (!c->driver_a || !c->driver_b || !c->drain_a || !c->drain_b) return "DRIVER/DRAIN UNSET";
    /* The two drain jumpers are independent, so a mis-entry can describe one
     * branch fed from the envelope follower and the other straight from VIN.
     * The follower sits well below VIN even at full DAC, so the branches would
     * run at different drain voltages: the outphasing null collapses and the
     * IMD with it. Both jumpers must be in the same position. */
    if (c->drain_a != c->drain_b) return "DRAIN A/B DIFFER";
    /* Worst case for the voltage: the lowest 5 V rail that permits TX. */
    if (c->bias_ma > RADIO_BIAS_MA_OFF && RADIO_BiasTargetMv(c->bias_ma, 4400U) + 50U > c->bias_limit_mv)
        return "BIAS OVER LIMIT";
    if ((c->drain_a == RADIO_DRAIN_VIN || c->drain_b == RADIO_DRAIN_VIN) && c->envelope_mode) return "ENVELOPE VS VIN DRAIN";
    if ((c->drain_a == RADIO_DRAIN_ENV && c->envelope_dac_per_volt[0] <= 0.0f) ||
        (c->drain_b == RADIO_DRAIN_ENV && c->envelope_dac_per_volt[1] <= 0.0f)) return "ENVELOPE UNCALIBRATED";
    uint8_t a, b;
    if (hz < 1000000UL || !RADIO_ConfigFilters(c, hz, &a, &b)) return "NO FILTER";
    if (!(c->qualified_bands & (1UL << a))) return "LPF UNTESTED";
    /* Too far below this LPF's band: harmonics fall in its passband. */
    if (!c->lpf_tx_min_khz[a] || hz < 1000UL * c->lpf_tx_min_khz[a]) return "BELOW LPF TX MIN";
    return NULL;
}

uint8_t RADIO_ConfigTxReady(const radio_config_t *c, uint32_t hz, uint8_t *lpf, uint8_t *bpf)
{
    if (RADIO_ConfigTxBlock(c, hz)) return 0U;
    uint8_t a, b;
    (void)RADIO_ConfigFilters(c, hz, &a, &b);
    if (lpf) *lpf = a;
    if (bpf) *bpf = b;
    return 1U;
}

uint8_t RADIO_ConfigFilters(const radio_config_t *c, uint32_t hz, uint8_t *lpf, uint8_t *bpf)
{
    unsigned a = covering_slot(c->lpf_upper_hz, 5U, hz);
    unsigned b = covering_slot(c->bpf_upper_hz, 4U, hz);
    if (lpf) *lpf = a < 5U ? (uint8_t)a : 255U;
    if (bpf) *bpf = b < 4U ? (uint8_t)b : 255U;
    return a < 5U && b < 4U && hz >= 1000000UL && hz <= 28333333UL;
}

/* PIN diode bias model from QRP Labs (QDX assembly manual 1.24, "A new bias
 * circuit for PCB Rev 4"; QMX/QMX+ reuse the circuit): per diode, the node
 * after each 33 ohm feed (R75/R76) sits at V = 0.6741 V + 12.1 ohm x I, which
 * lumps the diode drop, both 47 uH chokes, the LPF toroids and the LPF
 * switch. The 1 k from the 5 V rail (R74/R77) adds (V5 - V) / 1 k to the
 * buck's share. QRP Labs measured target and actual current to match within
 * the plotted accuracy; the node model is linear from about 10 to 50 mA. */
#define BIAS_NODE_MV    674.1f
#define BIAS_NODE_OHM   12.1f
#define BIAS_FEED_OHM   33.0f
#define BIAS_RAIL_OHM   1000.0f

/* A missing or implausible rail reading falls back to the nominal 5 V. */
static float bias_rail_mv(uint16_t rail5_mv)
{
    return rail5_mv >= 3000U && rail5_mv <= 6000U ? (float)rail5_mv : 5000.0f;
}

uint16_t RADIO_BiasTargetMv(uint16_t bias_ma, uint16_t rail5_mv)
{
    if (bias_ma <= RADIO_BIAS_MA_OFF) return 0U;
    const float node = BIAS_NODE_MV + BIAS_NODE_OHM * (float)bias_ma;
    float feed_ma = (float)bias_ma - (bias_rail_mv(rail5_mv) - node) / BIAS_RAIL_OHM; /* mV / ohm = mA */
    if (feed_ma < 0.0f) feed_ma = 0.0f;
    return (uint16_t)(node + BIAS_FEED_OHM * feed_ma + 0.5f);
}

float RADIO_BiasCurrentMa(uint16_t bias_mv, uint16_t rail5_mv)
{
    /* I = (V - node)/33 + (V5 - node)/1k with node = 0.6741 V + 12.1 I. */
    const float i = ((float)bias_mv - BIAS_NODE_MV) / BIAS_FEED_OHM +
                    (bias_rail_mv(rail5_mv) - BIAS_NODE_MV) / BIAS_RAIL_OHM;
    const float ma = i / (1.0f + BIAS_NODE_OHM / BIAS_FEED_OHM + BIAS_NODE_OHM / BIAS_RAIL_OHM);
    return ma > 0.0f ? ma : 0.0f;
}

void RADIO_BiasLoopStart(radio_bias_loop_t *s, uint16_t measured_mv)
{
    /* Start the reference where TX_BIAS already is (about one diode drop
     * while an LPF is selected) so enabling never steps the LC filter. */
    s->ref_mv = (float)measured_mv;
    s->integral = 0.0f;
    s->envelope_bad = 0U;
}

float RADIO_BiasLoopStep(radio_bias_loop_t *s, uint16_t measured_mv, uint16_t target_mv,
                         uint16_t limit_mv, uint16_t vin_mv)
{
    /* Plant: duty x VIN into 330 uH (several ohm DCR) / 470 uF and the two
     * 33 ohm PIN-diode feeds: a ~15 ms lag, or a ~400 Hz resonance with a low
     * DCR inductor. The reference slews 16 mV per scan (0.75->2.5 V in about
     * 27 ms) so the LC is never stepped; VIN feed-forward supplies most of
     * the duty and a 10 Hz integrator (gain scheduled by VIN) trims losses.
     * Simulated with switch/SS14 losses over 0.5-7 ohm DCR, 6.5-14 V, 10-45 mA
     * and the diode spread: settles in 40-80 ms without overshoot. The duty
     * never exceeds 50 %, the QMX limit, nor the trip limit plus 1.5 V of
     * losses over VIN. */
    const float vin = vin_mv > 5000U ? (float)vin_mv : 5000.0f;
    float ceiling = 1024.0f * ((float)limit_mv + 1500.0f) / vin;
    if (ceiling > 512.0f) ceiling = 512.0f;
    float target = (float)target_mv;
    if (s->ref_mv + 16.0f < target) s->ref_mv += 16.0f;
    else if (s->ref_mv - 16.0f > target) s->ref_mv -= 16.0f;
    else s->ref_mv = target;
    /* A buck can only source: well above the reference (C73 charged while no
     * LPF conducted) skip pulses until the diodes pull TX_BIAS down. */
    if ((float)measured_mv > s->ref_mv + 150.0f) return 0.0f;
    const float ki = 6.2831853f * 10.0f * 1024.0f / (4000.0f * vin); /* counts per mV per scan */
    const float error = s->ref_mv - (float)measured_mv;
    const float feed = 1024.0f * s->ref_mv / vin;
    /* Back-calculated clamp: the integral never holds more than the clamped
     * duty range needs, so there is no wind-up. */
    float integral = s->integral + ki * error;
    if (feed + integral > ceiling) integral = ceiling - feed;
    if (feed + integral < 0.0f) integral = -feed;
    s->integral = integral;
    float duty = feed + integral;
    if (!(duty > 0.0f)) duty = 0.0f;
    if (duty > ceiling) duty = ceiling;
    return duty;
}

uint8_t RADIO_BiasEnvelopeFault(radio_bias_loop_t *s, float duty, uint16_t measured_mv, uint16_t vin_mv)
{
    /* A healthy buck delivers duty x VIN less at most about 1.6 V of switch,
     * SS14 and inductor losses (at 45 mA, 7 ohm DCR and the slew current), so
     * TX_BIAS stays well above half of what the duty promises. Under half for
     * 5 ms means a short (QMX C504 failures), a lost sense path or a dead
     * switch: QMX shuts its SMPS down when the duty leaves its envelope too. */
    const float promised = duty / 1024.0f * (float)vin_mv;
    if ((float)measured_mv + 200.0f < 0.5f * promised) { if (s->envelope_bad < 20U) ++s->envelope_bad; }
    else s->envelope_bad = 0U;
    return s->envelope_bad >= 20U;
}

uint16_t RADIO_BiasAverageStep(radio_bias_avg_t *a, uint16_t mv)
{
    /* acc holds the mean x 16. C73 (470 uF) makes TX_BIAS itself quiet; the
     * scatter is in the measurement, white from one scan to the next, so the
     * mean is the node voltage. Priming on the first sample avoids a ramp from
     * zero after the ADC (re)starts. */
    if (!a->primed) {
        a->acc = (int32_t)mv << RADIO_BIAS_AVG_SHIFT;
        a->primed = 1U;
    } else {
        a->acc += (int32_t)mv - (a->acc >> RADIO_BIAS_AVG_SHIFT);
    }
    return (uint16_t)((a->acc + (1L << (RADIO_BIAS_AVG_SHIFT - 1U))) >> RADIO_BIAS_AVG_SHIFT);
}

uint8_t RADIO_BiasSettled(uint16_t mean_mv, uint16_t target_mv)
{
    if (!target_mv) return 1U;
    return (uint8_t)((uint32_t)mean_mv + RADIO_BIAS_WINDOW_MV >= target_mv &&
                     (uint32_t)mean_mv <= (uint32_t)target_mv + RADIO_BIAS_WINDOW_MV);
}

uint8_t RADIO_ConfigFuncKeyPin(const radio_config_t *c)
{
    const unsigned field = ((unsigned)c->boot_flags & RADIO_BOOT_FKEY_MASK) >> RADIO_BOOT_FKEY_SHIFT;
    return (uint8_t)((field ^ RADIO_FKEY_PIN_DEFAULT) & 0xFU);
}

void RADIO_ConfigSetFuncKeyPin(radio_config_t *c, uint8_t pin)
{
    const unsigned field = ((unsigned)pin ^ RADIO_FKEY_PIN_DEFAULT) & 0xFU;
    c->boot_flags = (uint16_t)(((unsigned)c->boot_flags & ~RADIO_BOOT_FKEY_MASK) |
                               (field << RADIO_BOOT_FKEY_SHIFT));
}

/* The drain jumpers decide what the drains can do, so they pick DRAIN
 * MODULATION as well. Both on the envelope follower: ENVELOPE, both branches
 * driven in phase and the amplitude set by the two shapers alone, which are
 * the easier amplitude control -- no outphasing (RADIO_EnvelopeFill). Either
 * on VIN: OUTPHASING, the only mode a fixed drain can run (the others are
 * refused as ENVELOPE VS VIN DRAIN). HYBRID is an explicit choice and stays
 * while both remain on the follower. Only a change of jumper does this, so
 * DRAIN MODULATION can still be set by hand afterwards and a loaded
 * configuration keeps what was saved. */
void RADIO_ConfigSetDrain(radio_config_t *c, uint8_t branch, uint8_t drain)
{
    uint8_t *jumper = branch ? &c->drain_b : &c->drain_a;
    if (*jumper == drain) return;
    *jumper = drain;
    if (c->drain_a == RADIO_DRAIN_VIN || c->drain_b == RADIO_DRAIN_VIN)
        c->envelope_mode = RADIO_DRAIN_MOD_OUTPHASING;
    else if (c->drain_a == RADIO_DRAIN_ENV && c->drain_b == RADIO_DRAIN_ENV &&
             c->envelope_mode == RADIO_DRAIN_MOD_OUTPHASING)
        c->envelope_mode = RADIO_DRAIN_MOD_ENVELOPE;
}

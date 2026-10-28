#ifndef RADIO_H
#define RADIO_H

#include "radio_config.h"

typedef enum { RADIO_SETUP, RADIO_RECEIVE, RADIO_TX_PREPARE, RADIO_TRANSMIT, RADIO_FAULT, RADIO_OFF } radio_state_t;
enum { RADIO_FAULT_ADC = 1U, RADIO_FAULT_VIN = 2U, RADIO_FAULT_5V = 4U,
       RADIO_FAULT_BIAS = 8U, RADIO_FAULT_SWR = 16U, RADIO_FAULT_ENV = 32U,
       RADIO_FAULT_DMA = 64U, RADIO_FAULT_CLOCK = 128U };
typedef struct {
    uint32_t sequence, tick, faults;
    /* bias_mv is the 4 ms mean of ADC_BIAS (RADIO_BiasAverageStep); the
     * others are single scans. */
    uint16_t bias_mv, fwd_mv, rev_mv, rail5_mv, env_a_mv, env_b_mv, vin_mv;
    uint16_t bias_duty; /* bias buck duty in counts of 1024, 0 while off */
} radio_telemetry_t;

void RADIO_Init(void);
void RADIO_Service(uint32_t now);
/* When the receiver last followed a tune in place (console `knob`). */
extern uint32_t rx_retune_tick;
void RADIO_RequestTx(void); /* bounded, IRQ-safe; actual gate follows accepted DMA frame */
uint8_t RADIO_TxHandover(void); /* TX requested while still receiving; IRQ-safe */
void RADIO_Trip(uint32_t fault);
void RADIO_ClearFault(void);
void RADIO_BeforeRetune(void);
void RADIO_AfterRetune(void);
void RADIO_GetTelemetry(radio_telemetry_t *t);
radio_state_t RADIO_State(void);
/* Reference oscillator correction (ppb) and RADIO_CLOCK_FLAG_* options. */
void RADIO_SetRefPpb(int32_t ppb);
void RADIO_SetClockFlags(uint8_t flags);
const char *RADIO_Status(void);
uint8_t RADIO_TxReady(void);
uint8_t RADIO_TxPermitted(void);
/* DIGI as CAT MD6/MD9 sets it (QMX's DiGi): TX AUDIO INPUT USB with USB AUDIO
 * POLICY DIGI CLEAN. USB/LSB then receive QMX's 150-3200 Hz. */
uint8_t RADIO_DigiReceive(void);
uint8_t RADIO_Ptt(void);
void RADIO_Shutdown(void);
/* SW4/SW5 actions that belong to the UI rather than the radio (band change,
 * menu, transmit mute) are latched here and consumed once by UI_AppHandle, so
 * the UI keeps ownership of its retune and audio-busy guards. */
enum { RADIO_PANEL_NONE, RADIO_PANEL_BAND_UP, RADIO_PANEL_BAND_DOWN,
       RADIO_PANEL_MENU, RADIO_PANEL_MUTE, RADIO_PANEL_MODE_NEXT };
uint8_t RADIO_TakePanelRequest(void);
/* Operator trims written in place. RADIO_SetConfig stops audio, resets the TX
 * transport and restarts the clocks, which is right for assembly changes and
 * wrong for a volume knob: these fields are read by the audio path on every
 * block, so they take effect on the next one. Values are clamped to the
 * RADIO_ConfigValid ranges. RADIO_SetRxMode refuses (returns 0) while a
 * calibration run owns the receiver. */
void RADIO_SetVolume(uint8_t percent);
void RADIO_SetMicGain(float gain);
/* At 0 % the panel is black and nothing on it can be read to turn it back
 * up, so the backlight never goes below this. */
#define RADIO_BACKLIGHT_MIN 5U
void RADIO_SetBacklight(uint8_t percent);
uint8_t RADIO_SetRxMode(uint8_t mode);
/* CW receive trims (radio_config_t cw_pitch_10hz, cw_bw_10hz, cw_flags),
 * written in place like the volume; the audio service restarts the receiver
 * DSP with them. Returns 0, changing nothing, for a value RADIO_ConfigValid
 * would refuse. */
uint8_t RADIO_SetCwReceive(uint8_t pitch_10hz, uint8_t bw_10hz, uint8_t flags);
/* Voice receive (radio_config_t ssb_bw_100hz, am_bw_khz, rx_agc, fm_squelch),
 * the same way. */
uint8_t RADIO_SetVoiceReceive(uint8_t ssb_bw_100hz, uint8_t am_bw_khz, uint8_t agc, uint8_t squelch);
/* The power and SWR meter's fields (settings v28: display, TUNE %,
 * calibration) from `c`, the rest of the board block untouched and nothing
 * restarted. 0 if they would not be valid. */
uint8_t RADIO_SetMeterConfig(const radio_config_t *c);
void RADIO_SetBootFlag(uint16_t mask, uint8_t on);
/* 0 when the pin is out of range or used by the encoder. */
uint8_t RADIO_SetFuncKeyPin(uint8_t pin);
enum { RADIO_CAL_NONE, RADIO_CAL_BIAS, RADIO_CAL_ENV_A, RADIO_CAL_ENV_B, RADIO_CAL_RX_TEST,
       RADIO_CAL_BPF_SWEEP, RADIO_CAL_SELFTEST, RADIO_CAL_RX_IQ, RADIO_CAL_AF_SWEEP,
       RADIO_CAL_LAST = RADIO_CAL_AF_SWEEP };
/* Calibration modes whose measurements consume the live PCM1821 stream.
 * Keep this policy next to the enum: omitting AF_SWEEP here used to make the
 * audio service suspend the receiver as soon as that sweep started. */
static inline uint8_t RADIO_CalUsesReceiver(uint8_t mode)
{
    return (mode == RADIO_CAL_RX_TEST || mode == RADIO_CAL_BPF_SWEEP ||
            mode == RADIO_CAL_SELFTEST || mode == RADIO_CAL_RX_IQ ||
            mode == RADIO_CAL_AF_SWEEP) ? 1U : 0U;
}
uint8_t RADIO_CalActive(void);
uint8_t RADIO_CalStart(uint8_t mode, uint16_t code);
void RADIO_CalStop(void);
/* Filter switching for the BPF sweep (RX calibration only). SelectFilters
 * drops RX_EN like a normal filter change; SelectBpf only moves U10. */
void RADIO_SelectFilters(uint8_t lpf, uint8_t bpf);
void RADIO_SelectBpf(uint8_t bpf);
/* PE2..PE6 read back from the port, right-aligned to bit 0: what the LPF
 * select transistors are actually being told, not what was last requested.
 * The self test compares it against the slot it asked for. */
uint32_t RADIO_SelectedLpfBits(void);

/* Why the telemetry watchdog in RADIO_Service tripped: the scan never ran,
 * or it stalled, and by how many milliseconds. Plus the retune pause/recover
 * failures. Counted, never latched. */
void RADIO_ServiceStallCounts(uint32_t *dead, uint32_t *late, uint32_t *gap_ms);
void RADIO_ServiceRetuneFails(uint32_t *pause, uint32_t *recover);

#endif

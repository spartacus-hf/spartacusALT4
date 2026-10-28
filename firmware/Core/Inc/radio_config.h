#ifndef RADIO_CONFIG_H
#define RADIO_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define RADIO_CONFIG_SIGNATURE 0x31564552UL /* REV1 */
typedef enum { RADIO_DRIVER_UNKNOWN, RADIO_DRIVER_UCC27524, RADIO_DRIVER_ACT541 } radio_driver_t;
typedef enum { RADIO_DRAIN_UNKNOWN, RADIO_DRAIN_ENV, RADIO_DRAIN_VIN } radio_drain_t;
/* envelope_mode, the DRAIN MODULATION setting. */
typedef enum { RADIO_DRAIN_MOD_OUTPHASING, RADIO_DRAIN_MOD_ENVELOPE, RADIO_DRAIN_MOD_HYBRID } radio_drain_mod_t;
typedef enum { RADIO_SW_NONE, RADIO_SW_PTT, RADIO_SW_RX_MODE, RADIO_SW_POWER,
               RADIO_SW_BAND_UP, RADIO_SW_BAND_DOWN, RADIO_SW_MENU, RADIO_SW_MUTE,
               RADIO_SW_LAST = RADIO_SW_MUTE } radio_switch_t;
typedef enum { RADIO_RX_USB, RADIO_RX_LSB, RADIO_RX_CW, RADIO_RX_AM, RADIO_RX_FM } radio_rx_mode_t;
/* The default receive channel around the dial per mode, Hz: what the LO
 * planner (RADIO_RxPlanLo) keeps clear of DC and the waterfall shades until
 * RADIO_RxSetChannelSpans hands it the configured widths
 * (RADIO_ConfigChannelSpans). */
#define RADIO_RX_CHANNEL_HZ {{300, 3000}, {-3000, -300}, {-350, 350}, {-5000, 5000}, {-10000, 10000}}
typedef enum { RADIO_SOURCE_USB, RADIO_SOURCE_MIC1, RADIO_SOURCE_MIC2, RADIO_SOURCE_MIC3 } radio_source_t;

/* Appended to settings v21. Fixed-width fields; never persist pointers or enums.
 * Zero/unknown assembly choices cannot authorize RF. Calibration belongs to
 * this PCB, not to imported prototype settings. */
typedef struct {
    uint32_t signature;
    uint8_t confirmed, driver_a, driver_b, drain_a;
    uint8_t drain_b, buttons_high, sw4, sw5;
    uint8_t backlight, volume, rx_mode, mic_source;
    uint8_t usb_iq, envelope_mode, aux_tx_mask, pd_enable;
    /* PIN diode forward bias per diode during TX, mA (QMX "PIN fwd bias").
     * RADIO_BIAS_MA_OFF keeps the bias buck off (~4 mA from R74/R77). */
    uint16_t bias_ma, bias_limit_mv, vin_limit_mv, pd_target_mv;
    uint16_t pd_current_ma, swr_limit_x100, envelope_limit_mv;
    /* 0: R3/R4 external 5.1 k Rd fitted, UCPD pulls off (USB-C cold start).
     * 1: R3/R4 removed, UCPD internal Rd (no USB-C start without DC power). */
    uint8_t pd_internal_rd;
    uint8_t rx_iq_invert; /* bench correction if USB/LSB appear swapped */
    uint32_t lpf_upper_hz[5];
    uint32_t bpf_upper_hz[4];
    float envelope_dac_per_volt[2];
    float swr_offset[2];
    float swr_gain[2];
    /* Differential PCM1821 channel skew in input samples. Positive means Q
     * leads I and is delayed; negative delays I. This occupied rx_phase in
     * settings <= v22, whose frequency-specific matrix value is not reused. */
    float rx_i_gain, rx_q_gain, rx_skew_samples;
    float mic_gain;
    uint32_t qualified_bands;
    /* Lowest TX frequency per LPF slot, kHz, below which that filter no
     * longer suppresses the 2nd/3rd harmonic enough; 0 = no TX on the slot.
     * Takes the former reserved_words[3] bytes, so the layout is unchanged. */
    uint16_t lpf_tx_min_khz[5];
    /* Bit 0: come up in the USB diagnostic console instead of USB audio.
     * A bench board is driven over that console, and re-entering it otherwise
     * means walking the LCD menu after every reset. Formerly reserved.
     * Bit 1: the LCD is mounted the other way round (landscape 270 degrees
     * instead of 90). How the panel sits in the case is a property of the
     * build, so it lives here and survives a factory reset of the settings.
     * RADIO_ConfigValid does not police these bits: firmware that does not
     * know one leaves it alone. */
    uint16_t boot_flags;
    /* ---- appended for settings v22; everything above is the v21 image ----
     * Front-panel rotary encoder on three spare jumper pins. Indices into the
     * board's spare-pin table; RADIO_ENC_PIN_OFF disables that line. The
     * encoder is optional: with enc_a or enc_b off the three buttons remain the
     * only input. Defaults are JP20 pins 4/5/6 = PC11/PC12/PC13. */
    uint8_t enc_a, enc_b, enc_sw, enc_flags;
    uint8_t enc_detent, enc_reserved;
    /* ---- appended for settings v24 ----
     * Highest fixed/PPS voltage the board may accept when no offer reaches
     * pd_target_mv. A 5/12/20 V charger against a 9 V target would otherwise
     * leave the radio on the mandatory 5 V, where the LM1084-5 cannot
     * regulate. Every volt above the target is burnt in that regulator, so
     * the selection takes the LOWEST offer above the target, not the highest.
     * Below pd_target_mv it simply means "no headroom"; RADIO_PdChoose
     * clamps. This occupies the two trailing padding bytes a v23 record
     * already carried as zero, so the record length is unchanged. */
    uint16_t pd_max_mv;
    /* ---- appended for settings v25 ----
     * Reference oscillator correction in parts per billion. HSE is a
     * DSC1003DL2 MEMS oscillator (+-25 ppm), and the carrier, the receive LO,
     * the I2S rate and the TX phase-update rate are all ratios of it, so its
     * error moves every frequency the radio makes together. Measured against
     * the host's NTP clock by `tools/rev1_diag.py refcal`; 0 = uncorrected. */
    int32_t ref_ppb;
    /* RADIO_CLOCK_FLAG_*: how the radio uses its clocks while receiving. */
    uint8_t clock_flags;
    /* ---- settings v26: CW receive, in the three bytes v25 kept at zero ----
     * 0 in each means its default, so a v25 record reads back unchanged.
     * cw_pitch_10hz: the CW beat note and the sidetone, 10 Hz units
     *   (RADIO_CW_PITCH_MIN_HZ..MAX_HZ); matching a signal to the sidetone
     *   zero-beats it, because the carrier is sent on the dial.
     * cw_bw_10hz: the CW channel filter, one of RADIO_CW_BW_TABLE / 10.
     * cw_flags: RADIO_CW_AGC_* in bits 0-1, RADIO_CW_FLAG_NO_DECODE. */
    uint8_t cw_pitch_10hz;
    uint8_t cw_bw_10hz;
    uint8_t cw_flags;
    /* ---- settings v27: voice receive, appended ----
     * 0 in each means its default, so a v26 record reads back unchanged.
     * ssb_bw_100hz: SSB receive filter width / 100 (RADIO_SSB_BW_TABLE),
     *   passband 300 Hz .. 300 Hz + width, like QMX's "Filter RX".
     * am_bw_khz: AM receive filter, total width in kHz (RADIO_AM_BW_TABLE).
     * rx_agc: RADIO_RX_AGC_* for USB/LSB/AM (CW keeps cw_flags' own).
     * fm_squelch: 0 off, else RADIO_FM_SQUELCH_DB(n) of channel power. */
    uint8_t ssb_bw_100hz;
    uint8_t am_bw_khz;
    uint8_t rx_agc;
    uint8_t fm_squelch;
    /* ---- settings v28: the power and SWR meter, appended ----
     * 0 in each means its default, so a v27 record reads back unchanged.
     * meter_flags: RADIO_METER_FLAG_*.
     * meter_update_10ms: reading interval / 10 ms (RADIO_METER_UPDATE_*).
     * meter_hang_100ms: how long the TX panel stays after TX / 100 ms.
     * tune_pct: TX TEST drain cap, % of the full drain (QMX "Tune %").
     * meter_port_mv: this board's detector table (radio_rf_meter.h), all 0
     *   for the built-in one.
     * meter_gain_x1000: per-anchor power correction against a reference
     *   instrument, 0 = the bridge's theory (1.000).
     * meter_leak_x10000: per-anchor |gamma| the bridge reads on a 50 ohm
     *   load (its own leakage), 0 = not measured.
     * meter_floor_100uv: per-anchor reflected-port signal that does not
     *   scale with the drive (gate-drive feedthrough, bench 2026-10-02:
     *   ~75 mV on 20 m), port volts x 10000, 0 = none. */
    uint8_t meter_flags;
    uint8_t meter_update_10ms;
    uint8_t meter_hang_100ms;
    uint8_t tune_pct;
    uint16_t meter_port_mv[12];
    uint16_t meter_gain_x1000[10];
    uint16_t meter_leak_x10000[10];
    uint16_t meter_floor_100uv[10];
} radio_config_t;

/* Stored records map this layout byte for byte (a v26 record ends at
 * ssb_bw_100hz, SETTINGS_V26_LENGTH), so nothing may be inserted above the
 * last field; new fields go at the end with a SETTINGS_VERSION bump. */
_Static_assert(offsetof(radio_config_t, enc_a) == 128U,
               "v21 settings prefix of radio_config_t must not change");

/* Encoder pin selection is an index into the spare-jumper table in
 * board_rev1.c, which static-asserts these constants. Keeping the numbers here
 * rather than including board_rev1.h leaves radio_config.c free of HAL headers,
 * which the rev1_policy/rev1_lpf/rev1_audio host suites rely on.
 *   0 OFF   1 PB0   2 PB15  3 PC11  4 PC12  5 PC13  6 PC14  7 PC15
 *   8 PD7   9 PE15 10 PE8  11 PE7  12 PF10 13 PF9  14 PF2  15 PF1
 * PA10 (JP20-1) is absent on purpose: it is UCPD1_DBCC2 and must stay
 * un-pulled. PD11/PD13 are not connected and PD14/PD15 are shorted by R148. */
#define RADIO_ENC_PIN_OFF      0U
#define RADIO_ENC_PIN_PC11     3U
#define RADIO_ENC_PIN_PC12     4U
#define RADIO_ENC_PIN_PC13     5U
#define RADIO_ENC_PIN_PC14     6U
#define RADIO_ENC_PIN_COUNT    16U /* including OFF */
#define RADIO_ENC_FLAG_HIGH    0x01U /* switches close to 3.3 V, not to GND */
#define RADIO_ENC_FLAG_REVERSE 0x02U /* swap the up/down sense */
#define RADIO_ENC_FLAG_NOPULL  0x04U /* external pulls fitted; leave PUPDR off */
#define RADIO_ENC_FLAG_MASK    0x07U
#define RADIO_ENC_DETENT_MIN   1U
#define RADIO_ENC_DETENT_MAX   4U

/* clock_flags. PARK_TX stops the TX HRTIM timers (master, A, E, F) while the
 * radio only receives: they run at the carrier grid, within a few kHz of the
 * dial, and leak into the receiver inside the MCU even with the branch pins
 * parked low. They restart before any TX stream or transmit. WIDE lets the
 * AUTO clock planner use 153.3-162.7 MHz as well as 164-170 MHz, at a 7 %
 * smaller receive cycle budget (clock_plan.h). PHASE384
 * doubles the TX phase/NCO update rate to 384 kHz. QCOMB subtracts the
 * PCM1821-locked periodic interference from the receive Q channel. */
#define RADIO_CLOCK_FLAG_PARK_TX     0x01U
#define RADIO_CLOCK_FLAG_WIDE        0x02U
#define RADIO_CLOCK_FLAG_PHASE384    0x04U
#define RADIO_CLOCK_FLAG_QCOMB       0x08U
#define RADIO_CLOCK_FLAG_MASK        0x0FU
#define RADIO_CLOCK_FLAGS_DEFAULT    RADIO_CLOCK_FLAG_PARK_TX
#define RADIO_REF_PPB_LIMIT          100000L

/* CW receive. The pitch is the audio tone a signal on the dial produces, and
 * the sidetone. 600 Hz is the common default of current transceivers and
 * sits in the 500-700 Hz range where copy in noise is best (Montnemery et al.,
 * via LA3ZA); QMX and v8.05 used 700. */
#define RADIO_CW_PITCH_MIN_HZ     400U
#define RADIO_CW_PITCH_MAX_HZ     900U
#define RADIO_CW_PITCH_DEFAULT_HZ 600U
/* CW channel filter bandwidths, Hz, in rx_cw_sos order (radio_rx_channel.inc,
 * tools/gen_rx_channel.py), each a soft-edged 8th-order Butterworth. 700 Hz by
 * default since v8.08 (500 through v8.07, which on the bench lost signals as
 * they drifted across a brick-wall edge); 1500 Hz is for finding stations.
 * A stored 0 means the default, so only an explicit choice stays put. */
#define RADIO_CW_BW_TABLE         {200U, 300U, 400U, 500U, 700U, 1000U, 1500U}
#define RADIO_CW_BW_COUNT         7U
#define RADIO_CW_BW_DEFAULT_HZ    700U
/* CW AGC (cw_flags bits 0-1). Both hang: the gain is held through the gaps
 * between elements and characters, then recovers at a steady dB rate, so
 * band noise does not swell up between dits (see radio_dsp.c). */
#define RADIO_CW_AGC_DEFAULT      0U /* stored 0: SLOW */
#define RADIO_CW_AGC_SLOW         1U
#define RADIO_CW_AGC_FAST         2U
#define RADIO_CW_AGC_MASK         0x03U
#define RADIO_CW_FLAG_NO_DECODE   0x04U /* receive CW decoder off */
#define RADIO_CW_FLAG_MASK        0x07U
/* SSB receive filter widths, Hz, in rx_ssb_sos order (tools/gen_rx_channel.py):
 * the passband runs 300 Hz up to 300 Hz + width. 2700 Hz is the filter every
 * version to v8.07 used, and stays the default. */
#define RADIO_SSB_BW_TABLE        {1800U, 2100U, 2400U, 2700U, 3000U, 3600U}
#define RADIO_SSB_BW_COUNT        6U
#define RADIO_SSB_BW_DEFAULT_HZ   2700U
#define RADIO_SSB_LOW_CUT_HZ      300U
/* DIGI receive (RADIO_DigiReceive): QMX's digital-mode filter, 150-3200 Hz
 * (rx_digi_sos), in place of 300 Hz + the SSB width; RADIO_DspSetVoice takes
 * it as the SSB width index RADIO_SSB_BW_DIGI. */
#define RADIO_DIGI_RX_LOW_HZ      150U
#define RADIO_DIGI_RX_HIGH_HZ     3200U
#define RADIO_SSB_BW_DIGI         RADIO_SSB_BW_COUNT
/* AM receive filter total widths, Hz, in rx_am_sos order; 10 kHz (+-5) is the
 * filter to v8.07. */
#define RADIO_AM_BW_TABLE         {6000U, 8000U, 10000U}
#define RADIO_AM_BW_COUNT         3U
#define RADIO_AM_BW_DEFAULT_HZ    10000U
/* Voice AGC (USB, LSB, AM): a hang AGC like CW's, the gain held after the
 * signal last set it, then recovering at a fixed dB rate. Stored 0 = MED. */
#define RADIO_RX_AGC_OFF          1U
#define RADIO_RX_AGC_SLOW         2U
#define RADIO_RX_AGC_MED          3U
#define RADIO_RX_AGC_FAST         4U
#define RADIO_RX_AGC_DEFAULT      RADIO_RX_AGC_MED
/* FM squelch: closed below this channel power, 6 dB steps from -120 dBFS
 * (level 1) to -66 dBFS (level 10); 0 is off. */
#define RADIO_FM_SQUELCH_MAX      10U
#define RADIO_FM_SQUELCH_DB(n)    (-126.0f + 6.0f * (float)(n))
uint16_t RADIO_ConfigSsbBandwidthHz(const radio_config_t *c);
uint8_t RADIO_ConfigSsbBandwidthIndex(const radio_config_t *c);
uint16_t RADIO_SsbBandwidthAt(uint8_t index);
uint16_t RADIO_ConfigAmBandwidthHz(const radio_config_t *c);
uint8_t RADIO_ConfigAmBandwidthIndex(const radio_config_t *c);
uint16_t RADIO_AmBandwidthAt(uint8_t index);
uint8_t RADIO_ConfigRxAgc(const radio_config_t *c);   /* RADIO_RX_AGC_OFF..FAST */
/* The receive channel around the dial for each RADIO_RX_* mode under this
 * configuration, Hz: the LO planner keeps it clear of DC, the waterfall
 * shades it. spans[mode][0..1] = low, high edge. */
void RADIO_ConfigChannelSpans(const radio_config_t *c, int16_t spans[5][2]);
uint16_t RADIO_ConfigCwPitchHz(const radio_config_t *c);
uint16_t RADIO_ConfigCwBandwidthHz(const radio_config_t *c);
/* Index of the configured bandwidth in RADIO_CW_BW_TABLE. */
uint8_t RADIO_ConfigCwBandwidthIndex(const radio_config_t *c);
/* RADIO_CW_BW_TABLE[index], the last entry past its end. */
uint16_t RADIO_CwBandwidthAt(uint8_t index);
/* RADIO_CW_AGC_SLOW or RADIO_CW_AGC_FAST. */
uint8_t RADIO_ConfigCwAgc(const radio_config_t *c);
uint8_t RADIO_ConfigCwDecode(const radio_config_t *c);

/* The power and SWR meter (v28). QMX updates its Pwr/SWR meter every 100 ms
 * and keeps it 50 ms after TX; the panel here stays a second, so the last
 * reading can be read. TUNE % is QMX's: 50 % of the drain, a quarter of the
 * power, for TX TEST used as an SWR tune-up carrier. */
#define RADIO_METER_FLAG_PANEL_OFF  0x01U /* no TX panel on the home screen */
#define RADIO_METER_FLAG_MASK       0x01U
#define RADIO_METER_UPDATE_MIN_MS   50U
#define RADIO_METER_UPDATE_MAX_MS   500U
#define RADIO_METER_UPDATE_DEF_MS   100U
#define RADIO_METER_HANG_MAX_MS     3000U
#define RADIO_METER_HANG_DEF_MS     1000U
#define RADIO_TUNE_PCT_MIN          10U
#define RADIO_TUNE_PCT_DEF          50U
uint8_t RADIO_ConfigTxPanel(const radio_config_t *c);
uint16_t RADIO_ConfigMeterUpdateMs(const radio_config_t *c);
uint16_t RADIO_ConfigMeterHangMs(const radio_config_t *c);
uint8_t RADIO_ConfigTunePct(const radio_config_t *c);

#define RADIO_BOOT_FLAG_DIAG_CONSOLE 0x0001U
#define RADIO_BOOT_FLAG_LCD_ROT270   0x0002U
/* boot_flags bits 2..5: the FUNC key's spare pin (an index into the same
 * table as the encoder pins), stored XOR RADIO_FKEY_PIN_DEFAULT so that an
 * untouched, zero field means PC14 -- the extra button on the LCD board, next
 * to the encoder on JP20. Firmware before v8.00 ignores these bits. */
#define RADIO_BOOT_FKEY_SHIFT        2U
#define RADIO_BOOT_FKEY_MASK         (0xFU << RADIO_BOOT_FKEY_SHIFT)
#define RADIO_FKEY_PIN_DEFAULT       6U  /* PC14, JP20-7 */
uint8_t RADIO_ConfigFuncKeyPin(const radio_config_t *c);
void RADIO_ConfigSetFuncKeyPin(radio_config_t *c, uint8_t pin);

#define RADIO_RX_MAX_HZ 28333333UL
#define RADIO_IQ_SKEW_MAX 1.5f
void RADIO_ConfigDefaults(radio_config_t *c);
/* Receive BPF edges for the fitted QMX 60-15m filter set (see .c). */
void RADIO_ConfigBpfDefaults(radio_config_t *c);
/* LPF upper edges and TX floors for the full QMX+ 160-10m set (see .c). */
void RADIO_ConfigLpfDefaults(radio_config_t *c);
/* Clear the slots whose toroids are not wound on this build, so they are never
 * selected. Applied on top of RADIO_ConfigLpfDefaults for a fresh install.
 * This build has every toroid wound, so it is a no-op; QMX+ LPF EDGES restores
 * all five on a board whose settings were saved while a slot was dark. */
void RADIO_ConfigLpfUnfitted(radio_config_t *c);
/* JP20 pins 4/5/6 (PC11/PC12/PC13), switching to GND with internal pull-ups. */
void RADIO_ConfigEncoderDefaults(radio_config_t *c);
uint8_t RADIO_ConfigValid(const radio_config_t *c);
/* Set one drain jumper (branch 0 = A, 1 = B); DRAIN MODULATION follows it. */
void RADIO_ConfigSetDrain(radio_config_t *c, uint8_t branch, uint8_t drain);
uint8_t RADIO_ConfigFilters(const radio_config_t *c, uint32_t hz, uint8_t *lpf, uint8_t *bpf);
uint8_t RADIO_ConfigTxReady(const radio_config_t *c, uint32_t hz, uint8_t *lpf, uint8_t *bpf);
/* First missing TX condition as a short label, or NULL when TX is ready. */
const char *RADIO_ConfigTxBlock(const radio_config_t *c, uint32_t hz);
/* TX locked by the storage, whatever the assembly says: the band's calibration
 * could not be read from the card (mod_cal_storage.c). NULL unlocks. The
 * label is RADIO_ConfigTxBlock's while it holds; RADIO_Service refuses TX on it
 * (RF permit off) and no unmute gets past it. */
void RADIO_ConfigSetStorageTxLock(const char *why);
const char *RADIO_ConfigStorageTxLock(void);
/* Why the receiver may not run yet, or NULL. The MCU cannot read the driver,
 * drain or filter assembly, so a board that has not been described stays
 * completely silent: no RX_EN, no QSD clocks, no bias buck, no envelope DAC. */
const char *RADIO_ConfigRxBlock(const radio_config_t *c);
/* PD12 PIN-diode bias buck (QMX+ circuit: PWM high = BSS84 on, 330 uH /
 * 470 uF, ADC_BIAS through 10 k). Called once per 4 kHz ADC scan. */
#define RADIO_BIAS_MA_OFF     5U  /* QDX/QMX: 5 mA disables the bias SMPS */
#define RADIO_BIAS_MA_DEFAULT 30U /* QMX default */
#define RADIO_BIAS_MA_MAX     45U /* ADC_BIAS full scale is the 2.9 V VREFBUF */
/* TX_BIAS voltage for a per-diode current, and the current for a measured
 * TX_BIAS voltage, from the QRP Labs PIN bias model (see .c). */
uint16_t RADIO_BiasTargetMv(uint16_t bias_ma, uint16_t rail5_mv);
float RADIO_BiasCurrentMa(uint16_t bias_mv, uint16_t rail5_mv);
typedef struct { float ref_mv, integral; uint16_t envelope_bad; } radio_bias_loop_t;
void RADIO_BiasLoopStart(radio_bias_loop_t *s, uint16_t measured_mv);
/* Returns TIM4 duty in counts of 1024, never above the VIN-derived ceiling. */
float RADIO_BiasLoopStep(radio_bias_loop_t *s, uint16_t measured_mv, uint16_t target_mv,
                         uint16_t limit_mv, uint16_t vin_mv);
/* QMX-style performance envelope: 1 once ADC_BIAS has stayed under half of
 * duty x VIN for 5 ms (shorted output, open sense, dead switch). */
uint8_t RADIO_BiasEnvelopeFault(radio_bias_loop_t *s, float duty, uint16_t measured_mv, uint16_t vin_mv);
/* ADC_BIAS as a level: an exponential mean over 2^RADIO_BIAS_AVG_SHIFT scans
 * (4 ms at 4 kHz). A single REV1 scan scatters about 27 mV rms (bench,
 * 2026-09-27), the mean about 5 mV. The loop and the protection trips keep
 * using each scan; telemetry, the TX-entry window, the loop start and the self
 * test use the mean. primed = 0 restarts it on the next sample. */
#define RADIO_BIAS_AVG_SHIFT 4U
typedef struct { int32_t acc; uint8_t primed; } radio_bias_avg_t;
uint16_t RADIO_BiasAverageStep(radio_bias_avg_t *a, uint16_t mv);
/* 1 when the mean TX_BIAS is within RADIO_BIAS_WINDOW_MV of the target (about
 * +-0.7 mA per diode at 30 mA), or the target is 0 (buck off). TX starts only
 * after 10 ms of this. */
#define RADIO_BIAS_WINDOW_MV 30U
uint8_t RADIO_BiasSettled(uint16_t mean_mv, uint16_t target_mv);
void RADIO_GetConfig(radio_config_t *c);
void RADIO_SetConfig(const radio_config_t *c);
const radio_config_t *RADIO_Config(void);

#endif

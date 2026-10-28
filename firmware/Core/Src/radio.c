#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio.h"
#include "radio_audio.h"
#include "radio_analog.h"
#include "radio_clocks.h"
#include "radio_pd.h"
#include "radio_bpf_sweep.h"
#include "radio_iq_fit.h"
#include "radio_rx_cal.h"
#include "radio_af_sweep.h"
#include "radio_spectrum.h"
#include "mod_selftest.h"
#include "board_rev1.h"
#include "modulator.h"
#include "sd_audio_player.h"
#include "sd_file_browser.h"
#include "main.h"
#include <string.h>

static radio_config_t config;
static volatile radio_state_t state;
static volatile uint32_t faults;
static volatile uint32_t last_tx_request;
static volatile uint8_t tx_requested;
static uint8_t initialized, ptt, tx_permitted, key_raw[2], key_previous[2], filter_lpf = 255U, filter_bpf = 255U;
static uint32_t transition_at, power_pressed_at[2], key_changed[2], rx_frequency, rx_rejected, bias_ok_since;
static uint8_t rx_mode_planned, rx_retune_mode = 0xFFU;
static uint32_t rx_retune_hz;
static volatile uint8_t calibration;
static uint32_t calibration_until;
static volatile uint8_t panel_request;
/* When the telemetry ADC last (re)started: a scan is due 0.5 ms after it,
 * so until then the newest sample is from before the pause and its age
 * says nothing. The watchdog in RADIO_Service counts from the later of the
 * two -- counting from the sample alone, the first service after a flash
 * write found it 20 ms old and latched FAULT_ADC (bench, first v8.08
 * boot, 2026-10-01). */
static uint32_t analog_restart_tick;
uint32_t rx_retune_tick;
/* Telemetry watchdog trips, split. Counted, never latched; CPU-only so CCM,
 * and 16 bits saturating because flash, SRAM and CCM are all at their policy
 * floors -- these count catastrophes, not events. */
#if MOD_DEEP_DIAG_ENABLE
#define bump16(p) ((void)0)
static volatile uint16_t svc_stall_gap; /* still assigned, never reported */
void RADIO_ServiceStallCounts(uint32_t *dead, uint32_t *late, uint32_t *gap_ms)
{
    if (dead) *dead = 0U;
    if (late) *late = 0U;
    if (gap_ms) *gap_ms = 0U;
}
void RADIO_ServiceRetuneFails(uint32_t *pause, uint32_t *recover)
{
    if (pause) *pause = 0U;
    if (recover) *recover = 0U;
}
#else
static volatile uint16_t svc_stall_dead __attribute__((section(".ccmram_bss")));
static volatile uint16_t svc_stall_late __attribute__((section(".ccmram_bss")));
static volatile uint16_t svc_stall_gap __attribute__((section(".ccmram_bss")));
static volatile uint16_t svc_pause_fail __attribute__((section(".ccmram_bss")));
static volatile uint16_t svc_recover_fail __attribute__((section(".ccmram_bss")));
static void bump16(volatile uint16_t *v) { if (*v < 0xFFFFU) ++*v; }
void RADIO_ServiceStallCounts(uint32_t *dead, uint32_t *late, uint32_t *gap_ms)
{
    if (dead) *dead = svc_stall_dead;
    if (late) *late = svc_stall_late;
    if (gap_ms) *gap_ms = svc_stall_gap;
}
void RADIO_ServiceRetuneFails(uint32_t *pause, uint32_t *recover)
{
    if (pause) *pause = svc_pause_fail;
    if (recover) *recover = svc_recover_fail;
}
#endif

uint8_t RADIO_TakePanelRequest(void)
{
    uint8_t request = panel_request;
    panel_request = RADIO_PANEL_NONE;
    return request;
}

static void rx_spans_update(void);
static void service_tx_exit(uint32_t now);

const radio_config_t *RADIO_Config(void) { return &config; }
void RADIO_GetConfig(radio_config_t *c) { *c = config; }

void RADIO_SetConfig(const radio_config_t *c)
{
    /* Assembly changes require an idle transport; the UI continues to show
     * the accepted value if a running source prevents reconfiguration. */
    if (initialized && (MOD_AudioRealtimeCritical(HAL_GetTick()) || MOD_AUDIO_SOURCE_IS_LOCAL(MOD_GetAudioSource()))) return;
    if (calibration) RADIO_CalStop();
    if (initialized) { RADIO_AudioStop(); MOD_RadioResetTransport(); RADIO_ClocksStop(); }
    radio_config_t next;
    if (RADIO_ConfigValid(c)) next = *c;
    else RADIO_ConfigDefaults(&next);
    uint8_t termination_changed = next.pd_internal_rd != config.pd_internal_rd;
    const uint8_t reference_changed = next.ref_ppb != config.ref_ppb;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    BOARD_PermitRf(0U);
    config = next;
    /* A setting stored at 0 % by v8.01 or earlier would boot to a black panel. */
    if (config.backlight < RADIO_BACKLIGHT_MIN) config.backlight = RADIO_BACKLIGHT_MIN;
    state = faults ? RADIO_FAULT : RADIO_SETUP;
    tx_requested = tx_permitted = 0U;
    if (initialized) RADIO_AnalogStop();
    __set_PRIMASK(pm);
    BOARD_SetRfDrivers((config.driver_a == RADIO_DRIVER_UCC27524 ? 1U : 0U) |
                       (config.driver_b == RADIO_DRIVER_UCC27524 ? 2U : 0U),
                       (config.driver_a == RADIO_DRIVER_ACT541 ? 1U : 0U) |
                       (config.driver_b == RADIO_DRIVER_ACT541 ? 2U : 0U));
    BOARD_SetButtonPolarity(config.buttons_high);
    BOARD_EncoderConfigure(config.enc_a, config.enc_b, config.enc_sw,
                           config.enc_flags, config.enc_detent);
    BOARD_FuncKeyConfigure(RADIO_ConfigFuncKeyPin(&config));
    BOARD_SetBacklight(config.backlight);
    filter_lpf = filter_bpf = 255U;
    rx_spans_update();
    if (termination_changed) RADIO_PdApplyCcTermination();
    RADIO_MeterReconfigure();
    /* A board block with another reference correction (a settings load or
     * restore) moves every frequency: re-plan the carrier on it. */
    if (initialized && reference_changed) MOD_CarrierReplan();
}

void RADIO_SetVolume(uint8_t percent) { config.volume = percent > 100U ? 100U : percent; }

/* DIGI is a sideband mode. CW, AM and FM leave TX AUDIO INPUT and DIGI CLEAN
 * as DIGI set them (mode_digi_apply), and were DIGI to the receiver: the
 * headphones parked -- no receive audio, no sidetone, and a practice keyer
 * clocked by the headphones that never stepped. The Morse trainer and TX
 * TEST change only the transmitter and leave the receive mode as it was, so
 * the transmitter must be a sideband one too: the trainer opened from DIGI
 * had no sidetone and took no keying (bench, 2026-10-06). */
uint8_t RADIO_DigiReceive(void)
{
    return (uint8_t)((config.rx_mode <= RADIO_RX_LSB) && (config.mic_source == RADIO_SOURCE_USB) &&
                     (MOD_GetUsbAudioPolicy() == USB_AUDIO_POLICY_DIGI_CLEAN) &&
                     (MOD_GetModeType(MOD_GetTestConfig()) == MOD_TYPE_SSB));
}

/* The receive channel widths feed the LO planner, the seamless retune and the
 * waterfall (RADIO_RxChannelSpan): a width change re-plans the LO. DIGI
 * receives QMX's 150-3200 Hz whatever SSB RX FILTER says. */
static uint8_t rx_spans_digi;
static void rx_spans_update(void)
{
    int16_t spans[5][2];
    RADIO_ConfigChannelSpans(&config, spans);
    rx_spans_digi = RADIO_DigiReceive();
    if (rx_spans_digi) {
        spans[RADIO_RX_USB][0] = (int16_t)RADIO_DIGI_RX_LOW_HZ;
        spans[RADIO_RX_USB][1] = (int16_t)RADIO_DIGI_RX_HIGH_HZ;
        spans[RADIO_RX_LSB][0] = (int16_t)-(int16_t)RADIO_DIGI_RX_HIGH_HZ;
        spans[RADIO_RX_LSB][1] = (int16_t)-(int16_t)RADIO_DIGI_RX_LOW_HZ;
    }
    RADIO_RxSetChannelSpans(spans);
    rx_frequency = rx_rejected = 0U;
}

uint8_t RADIO_SetCwReceive(uint8_t pitch_10hz, uint8_t bw_10hz, uint8_t flags)
{
    radio_config_t next = config;
    next.cw_pitch_10hz = pitch_10hz;
    next.cw_bw_10hz = bw_10hz;
    next.cw_flags = flags;
    if (!RADIO_ConfigValid(&next)) return 0U;
    const uint8_t width_changed = (uint8_t)(bw_10hz != config.cw_bw_10hz);
    config.cw_pitch_10hz = pitch_10hz;
    config.cw_bw_10hz = bw_10hz;
    config.cw_flags = flags;
    if (width_changed) rx_spans_update();
    return 1U;
}

uint8_t RADIO_SetVoiceReceive(uint8_t ssb_bw_100hz, uint8_t am_bw_khz, uint8_t agc, uint8_t squelch)
{
    radio_config_t next = config;
    next.ssb_bw_100hz = ssb_bw_100hz;
    next.am_bw_khz = am_bw_khz;
    next.rx_agc = agc;
    next.fm_squelch = squelch;
    if (!RADIO_ConfigValid(&next)) return 0U;
    const uint8_t width_changed = (uint8_t)(ssb_bw_100hz != config.ssb_bw_100hz || am_bw_khz != config.am_bw_khz);
    config.ssb_bw_100hz = ssb_bw_100hz;
    config.am_bw_khz = am_bw_khz;
    config.rx_agc = agc;
    config.fm_squelch = squelch;
    if (width_changed) rx_spans_update();
    return 1U;
}

/* The meter's fields hold nothing the hardware uses, so unlike
 * RADIO_SetConfig nothing stops; the ADC interrupt reads the detector table,
 * so the copy is atomic. They run from meter_flags to the end of the block. */
uint8_t RADIO_SetMeterConfig(const radio_config_t *c)
{
    const size_t at = offsetof(radio_config_t, meter_flags);
    radio_config_t next = config;
    memcpy((uint8_t *)&next + at, (const uint8_t *)c + at, sizeof(next) - at);
    if (!RADIO_ConfigValid(&next)) return 0U;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    memcpy((uint8_t *)&config + at, (const uint8_t *)&next + at, sizeof(next) - at);
    __set_PRIMASK(pm);
    RADIO_MeterReconfigure();
    return 1U;
}

/* REF CAL: every frequency is a ratio of the reference, so a new correction
 * re-plans the carrier and restarts the receiver on its new LO offset. */
void RADIO_SetRefPpb(int32_t ppb)
{
    if (ppb > RADIO_REF_PPB_LIMIT) ppb = RADIO_REF_PPB_LIMIT;
    if (ppb < -RADIO_REF_PPB_LIMIT) ppb = -RADIO_REF_PPB_LIMIT;
    if (ppb == config.ref_ppb) return;
    config.ref_ppb = ppb;
    MOD_CarrierReplan();
    rx_frequency = rx_rejected = 0U;
}

void RADIO_SetClockFlags(uint8_t flags)
{
    flags &= RADIO_CLOCK_FLAG_MASK;
    const uint8_t changed = (uint8_t)(flags ^ config.clock_flags);
    if (!changed) return;
    config.clock_flags = flags;
    if (!(flags & RADIO_CLOCK_FLAG_PARK_TX)) MOD_TxTimersPark(0U);
    /* The wide range changes AUTO's candidates and the phase rate the carrier
     * fraction (the master period's cap); the receive-side options take
     * effect when the receiver restarts. */
    if (changed & (RADIO_CLOCK_FLAG_WIDE | RADIO_CLOCK_FLAG_PHASE384)) MOD_CarrierReplan();
    rx_frequency = rx_rejected = 0U;
}

void RADIO_SetMicGain(float gain)
{
    if (!(gain >= 0.1f)) gain = 0.1f; /* also catches NaN */
    if (gain > 100.0f) gain = 100.0f;
    config.mic_gain = gain;
}

void RADIO_SetBacklight(uint8_t percent)
{
    config.backlight = percent > 100U ? 100U : percent < RADIO_BACKLIGHT_MIN ? RADIO_BACKLIGHT_MIN : percent;
    BOARD_SetBacklight(config.backlight);
}

/* RADIO_Service notices the new mode and replans the receive clock, exactly
 * as it does for the SW4/SW5 mode key. A calibration run has planned its
 * clocks around the mode it started with, so it is not changed under it. */
uint8_t RADIO_SetRxMode(uint8_t mode)
{
    if (mode > RADIO_RX_FM || calibration) return 0U;
    config.rx_mode = mode;
    return 1U;
}

/* The FUNC key cannot share a pin with the encoder; 0 turns it off. */
uint8_t RADIO_SetFuncKeyPin(uint8_t pin)
{
    if (pin >= RADIO_ENC_PIN_COUNT) return 0U;
    if (pin && ((pin == config.enc_a) || (pin == config.enc_b) || (pin == config.enc_sw))) return 0U;
    RADIO_ConfigSetFuncKeyPin(&config, pin);
    BOARD_FuncKeyConfigure(pin);
    return 1U;
}

void RADIO_SetBootFlag(uint16_t mask, uint8_t on)
{
    config.boot_flags = (uint16_t)(on ? (config.boot_flags | mask) : (config.boot_flags & (uint16_t)~mask));
}

void RADIO_Init(void)
{
    if (!RADIO_ConfigValid(&config)) RADIO_ConfigDefaults(&config);
    /* Idle button lines float on REV1, so the jumper cannot be read at boot;
     * BOARD_ButtonService learns it from the first press. */
    radio_config_t c = config;
    RADIO_SetConfig(&c);
    RADIO_RefTimerStart();
    initialized = 1U;
    if (!RADIO_AnalogInit() || !MOD_GetVrefReady()) RADIO_Trip(RADIO_FAULT_ADC);
    analog_restart_tick = HAL_GetTick();
}

void RADIO_Trip(uint32_t fault)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    BOARD_EmergencyStop();
    if (initialized) RADIO_AnalogStop();
    if ((faults | fault) != faults) {
        /* A new protection fault, for the SD event log: which, from what
         * state, and the supply and detector readings at the trip. */
        radio_telemetry_t t;
        RADIO_AnalogSnapshot(&t);
        MOD_EventLog(MOD_EVENT_FAULT, (uint8_t)(fault & ~faults), (uint16_t)state,
                     ((uint32_t)t.vin_mv << 16) | t.rail5_mv, ((uint32_t)t.fwd_mv << 16) | t.rev_mv);
    }
    faults |= fault;
    calibration = 0U;
    RADIO_AudioChopStop();
    state = RADIO_FAULT;
    tx_requested = tx_permitted = 0U;
    __set_PRIMASK(pm);
}

void RADIO_RequestTx(void)
{
    if (!RADIO_TxPermitted()) return;
    last_tx_request = HAL_GetTick();
    tx_requested = 1U;
}

/* The radio is still receiving but a transmission is starting: in the last
 * 100 ms the TX renderer produced a radiating frame, a stream started
 * rendering or unmuted USB audio arrived for one (mod_tx_handover_mark), or
 * the transport has asked for TX. The TX request itself only comes once the first such
 * frame reaches the DMA, after the render queue has filled and the main loop
 * has started the stream -- ~100 ms during which the receiver and the
 * renderer shared the CPU and the receiver lost every half. FM fills a
 * deeper queue and then renders nothing until the DMA starts, so 40 ms
 * lapsed there now and then; 100 ms bounds how long a finished or abandoned
 * stream can hold the receiver off, about what leaving TX takes anyway.
 * Likewise while the RF DMA still runs after TX: the two do not fit in the
 * CPU together, and the receiver waits the few milliseconds until the
 * transport stops it (mod_cw_keyer_needs_stream) instead of failing halves. */
#define RADIO_TX_HANDOVER_MS 100U
uint8_t RADIO_TxHandover(void)
{
    return (uint8_t)(state == RADIO_RECEIVE &&
                     (tx_requested || MOD_RfDmaStreamsRunning() ||
                      MOD_TxRfRenderedWithin(HAL_GetTick(), RADIO_TX_HANDOVER_MS)));
}
uint8_t RADIO_TxReady(void) { return state == RADIO_TRANSMIT && !faults; }
uint8_t RADIO_TxPermitted(void) { return tx_permitted && !faults && !BOARD_EmergencyLatched() && state != RADIO_OFF; }
uint8_t RADIO_Ptt(void) { return ptt; }
radio_state_t RADIO_State(void) { return state; }
const char *RADIO_Status(void)
{
    static const char *const labels[] = {"BOARD SETUP", "RECEIVE", "TX SETTLING", "TRANSMIT", "PROTECTION", "POWER OFF"};
    /* Explain the silence of a board that has not been described yet, so an
     * inert first boot does not look like a dead receiver. */
    if (state != RADIO_FAULT && state != RADIO_OFF && RADIO_ConfigRxBlock(&config)) return "SETUP REQUIRED";
    /* An unfitted or uncovered band is a missing filter, not a fault: say which
     * so a silent radio above the top fitted LPF is not mistaken for a dead
     * receiver. */
    if (state != RADIO_FAULT && state != RADIO_OFF &&
        !RADIO_ConfigFilters(&config, MOD_GetCarrierRequestedHz(), NULL, NULL)) return "NO LPF FOR FREQ";
    if (state == RADIO_SETUP && rx_rejected && rx_rejected == MOD_GetCarrierRequestedHz()) return "RX CLOCK N/A";
    if (state == RADIO_RECEIVE && RADIO_RxClockQuality() == 1U) return "RECEIVE (IF NEAR DC)";
    return labels[state <= RADIO_OFF ? state : RADIO_FAULT];
}

void RADIO_GetTelemetry(radio_telemetry_t *t)
{
    RADIO_AnalogSnapshot(t);
    t->faults = faults;
}

void RADIO_ClearFault(void)
{
    RADIO_AudioStop();
    MOD_RadioResetTransport();
    RADIO_ClocksStop();
    tx_requested = tx_permitted = 0U;
    if (!MOD_GetVrefReady()) return;
    if ((faults & RADIO_FAULT_ADC) && !RADIO_AnalogRecover()) return;
    analog_restart_tick = HAL_GetTick();
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    faults = 0U;
    BOARD_ClearEmergency();
    state = RADIO_SETUP;
    rx_frequency = rx_rejected = 0U;
    __set_PRIMASK(pm);
    MOD_RequestPipelineFaultAcknowledge();
}

void RADIO_BeforeRetune(void)
{
    if (!initialized) return;
    /* Fade the headphones out first: a PLL switch or a flash write stops the
     * receiver, and cutting it at full volume is a click. */
    if (state == RADIO_RECEIVE && RADIO_AudioRunning()) RADIO_AudioDuckWait();
    RADIO_AudioStop();
    BOARD_PermitRf(0U);
    if (!RADIO_AnalogPause()) { bump16(&svc_pause_fail); RADIO_Trip(RADIO_FAULT_ADC); }
    RADIO_ClocksStop();
    service_tx_exit(HAL_GetTick());   /* bias, AUX and the TX outputs too */
    tx_requested = tx_permitted = 0U;
    state = faults ? RADIO_FAULT : RADIO_SETUP;
    rx_frequency = rx_rejected = 0U;
}

void RADIO_AfterRetune(void)
{
    if (!initialized) return;
    if (!RADIO_AnalogRecover()) { bump16(&svc_recover_fail); RADIO_Trip(RADIO_FAULT_ADC); }
    analog_restart_tick = HAL_GetTick();
    BOARD_SetBacklight(config.backlight);
    transition_at = HAL_GetTick();
}

void RADIO_SelectFilters(uint8_t lpf, uint8_t bpf)
{
    BOARD_PermitRf(0U);
    RADIO_BiasEnable(0U);
    BOARD_SetRx(0U);
    uint32_t lp = lpf < 5U ? (1UL << (lpf + 2U)) : 0U;
    GPIOE->BSRR = lp | (((31UL << 2U) & ~lp) << 16U);
    uint32_t bp = bpf & 3U;
    GPIOD->BSRR = bp | ((3U & ~bp) << 16U);
    filter_lpf = lpf; filter_bpf = bpf;
    RADIO_MeterHoldDetectors();
    transition_at = HAL_GetTick();
    rx_frequency = 0U; /* RX_EN dropped: the next tune restarts the receiver */
}

uint32_t RADIO_SelectedLpfBits(void)
{
    return (GPIOE->ODR >> 2U) & 31UL;
}

void RADIO_SelectBpf(uint8_t bpf)
{
    uint32_t bp = bpf & 3U;
    GPIOD->BSRR = bp | ((3U & ~bp) << 16U);
    filter_bpf = bpf;
}

static void service_keys(uint32_t now)
{
    uint8_t learned;
    if (BOARD_ButtonService(now, &learned) && learned != config.buttons_high)
        config.buttons_high = learned; /* persisted with the next settings save */
    ptt = 0U;
    for (unsigned i = 0U; i < 2U; ++i) {
        uint8_t raw = BOARD_ButtonPressed((uint8_t)(i + 3U));
        if (raw != key_raw[i]) { key_raw[i] = raw; key_changed[i] = now; }
        uint8_t pressed = now - key_changed[i] >= 20U ? raw : key_previous[i];
        uint8_t action = i ? config.sw5 : config.sw4;
        if (action == RADIO_SW_PTT && pressed) ptt = 1U;
        /* Mode, band, menu and mute belong to the UI: latch the press and let
         * UI_AppHandle run them under its own guards. The mode key moves the
         * whole operating mode (transmit, sideband and receive together), so
         * it is no longer a receive-only toggle here. */
        if (pressed && !key_previous[i]) {
            if (action == RADIO_SW_RX_MODE) panel_request = RADIO_PANEL_MODE_NEXT;
            else if (action == RADIO_SW_BAND_UP) panel_request = RADIO_PANEL_BAND_UP;
            else if (action == RADIO_SW_BAND_DOWN) panel_request = RADIO_PANEL_BAND_DOWN;
            else if (action == RADIO_SW_MENU) panel_request = RADIO_PANEL_MENU;
            else if (action == RADIO_SW_MUTE) panel_request = RADIO_PANEL_MUTE;
        }
        if (action == RADIO_SW_POWER) {
            if (pressed && !key_previous[i]) power_pressed_at[i] = now;
            if (pressed && now - power_pressed_at[i] >= 3000U) RADIO_Shutdown();
        }
        key_previous[i] = pressed;
    }
}

/* Tune a receiver that is running, without restarting it: the NCO follows
 * while the running LO still serves the dial, otherwise the LO moves in place
 * under a short headphone duck. 0 sends the tune down the full restart path
 * (another LO plan, a scanner's LO, a PWM harmonic too close, no receiver). */
static uint8_t rx_retune_seamless(uint32_t hz)
{
    if (state != RADIO_RECEIVE || !RADIO_AudioRunning() || !RADIO_AudioPwmClear(hz)) return 0U;
    uint32_t period = 0U;
    const uint8_t plan = RADIO_RxRetunePlan(hz, (radio_rx_mode_t)config.rx_mode, &period);
    if (plan == 0U) return 0U;
    if (plan == 2U) {
        RADIO_AudioDuckWait();
        if (!RADIO_RxRetuneTo(period)) { RADIO_AudioDuckRelease(); return 0U; }
    }
    RADIO_AudioRetune(hz, (uint8_t)(plan == 2U));
    return 1U;
}

/* An integer HRTIM period cannot reach every frequency near the top of HF.
 * That is a receiver limitation, not a protection event: keep RX off at this
 * frequency and retry only after tuning, reconfiguration or a PLL retune. */
static uint8_t service_rx_clock(uint32_t hz)
{
    /* The LO plan depends on the mode's channel span, so a mode change
     * replans the clock (and restarts the receiver). */
    if (rx_mode_planned != config.rx_mode) { rx_mode_planned = config.rx_mode; rx_frequency = rx_rejected = 0U; }
    if (rx_frequency == hz) return 1U;
    if (rx_frequency && rx_retune_seamless(hz)) { rx_frequency = hz; rx_retune_tick = HAL_GetTick(); return 1U; }
    if (rx_rejected != hz) {
        uint8_t started = RADIO_RxClockStart(hz, (radio_rx_mode_t)config.rx_mode); /* stops old clocks */
        RADIO_AudioSuspend();
        rx_frequency = started ? hz : 0U;
        rx_rejected = started ? 0U : hz;
        /* No DC-free IF at this SYSCLK: ask the carrier clock planner (which
         * checks the receiver plan) for another legal PLL setting once per
         * frequency/mode; it retunes when TX is idle and replans the RX LO. */
        if ((!started || RADIO_RxClockQuality() < 2U) &&
            (rx_retune_hz != hz || rx_retune_mode != config.rx_mode)) {
            rx_retune_hz = hz;
            rx_retune_mode = config.rx_mode;
            MOD_ScheduleCarrierClockRetune(hz, 50U);
        }
        if (started) return 1U;
    }
    state = RADIO_SETUP;
    return 0U;
}

/* Bring the transmitter up: stop the receiver, select the filters, settle the
 * PIN bias, then enable the HRTIM outputs and the RF gate. Split out of
 * RADIO_Service so that function stays inside its complexity budget; noinline
 * because a single-call static is otherwise folded straight back into it. */
static __attribute__((noinline)) void service_tx_entry(uint32_t now, const radio_telemetry_t *t,
                                                       uint8_t lpf, uint8_t bpf)
{
    if (state != RADIO_TX_PREPARE && state != RADIO_TRANSMIT) {
        MOD_TxTimersPark(0U);
        RADIO_ClocksStop();
        rx_frequency = 0U;
        RADIO_SelectFilters(lpf, bpf);
        RADIO_BiasEnable(1U);
        state = RADIO_TX_PREPARE;
        transition_at = bias_ok_since = now;
    }
    /* +-30 mV around the modeled target is about +-0.7 mA per diode. With
     * the buck off (RADIO_BIAS_MA_OFF) the receive bias is already there.
     * t->bias_mv is the 4 ms mean: this loop sees nearly every 4 kHz scan while
     * keyed, and single scans (~27 mV rms) never stayed inside for 10 ms. */
    const uint16_t bias_target = RADIO_BiasTargetMv(config.bias_ma, t->rail5_mv);
    if (!RADIO_BiasSettled(t->bias_mv, bias_target)) bias_ok_since = now;
    if (state == RADIO_TX_PREPARE && now - transition_at >= 5U && now - bias_ok_since >= 10U) {
        /* The latch and the enable in one step: an emergency stop that lands
         * after RADIO_Service checked the latch keeps its ODISR, as the AUX
         * lines and the RF gate below refuse under it; the next pass trips. */
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        if (!BOARD_EmergencyLatched()) HRTIM1->sCommonRegs.OENR = BOARD_TX_OUTPUTS;
        __set_PRIMASK(pm);
        state = RADIO_TRANSMIT;
        BOARD_SetAux(config.aux_tx_mask);
        BOARD_PermitRf(1U);
    }
    if (state == RADIO_TX_PREPARE && now - transition_at > 500U) RADIO_Trip(RADIO_FAULT_BIAS);
}

/* No configured LPF covers this frequency, so there is no RF path in either
 * direction: the same filter carries transmit and, from the antenna side,
 * receive. Drop the one-hot select too, rather than leaving the previous
 * filter's BSS123 on and its PIN diodes drawing the R74/R77 receive bias
 * through a band it does not serve. noinline for the same reason as the
 * transmit helpers. */
static __attribute__((noinline)) void service_no_filter(uint8_t bpf)
{
    if (filter_lpf != 255U) RADIO_SelectFilters(255U, bpf);
    RADIO_ClocksStop();
    RADIO_AudioSuspend();
    state = RADIO_SETUP;
}

/* Leave transmit. The pins are already back to GPIO-low by the time
 * BOARD_PermitRf returns; parking the HRTIM outputs adds the second layer the
 * RX and test clocks have always had. */
static __attribute__((noinline)) void service_tx_exit(uint32_t now)
{
    if (state != RADIO_TRANSMIT && state != RADIO_TX_PREPARE) return;
    BOARD_PermitRf(0U);
    RADIO_BiasEnable(0U);
    BOARD_SetAux(0U);
    HRTIM1->sCommonRegs.ODISR = BOARD_TX_OUTPUTS;
    transition_at = now;
    state = RADIO_SETUP;
}

/* 1 while the running bench routine wants another service pass. The two
 * self-driving routines step themselves; everything else just runs out its
 * timeout. Split out so RADIO_Service stays inside its complexity baseline. */
static __attribute__((noinline)) uint8_t service_calibration(uint32_t now)
{
    if ((int32_t)(now - calibration_until) >= 0) return 0U;
    if (calibration == RADIO_CAL_BPF_SWEEP) return RADIO_SweepService(now);
    if (calibration == RADIO_CAL_SELFTEST) return MOD_SelfTestService(now);
    if (calibration == RADIO_CAL_RX_IQ) return RADIO_IqCalService(now);
    if (calibration == RADIO_CAL_AF_SWEEP) return RADIO_AfSweepService(now);
    return 1U;
}

void RADIO_Service(uint32_t now)
{
    if (!initialized || state == RADIO_OFF) return;
    /* TX AUDIO INPUT or USB AUDIO POLICY moved DIGI in or out. */
    if (RADIO_DigiReceive() != rx_spans_digi) rx_spans_update();
    RADIO_MeterService(now);
    if (BOARD_EmergencyLatched() && !faults) RADIO_Trip(RADIO_FAULT_DMA);
    radio_telemetry_t t;
    RADIO_AnalogSnapshot(&t);
    /* Signed age, deliberately. `now` is sampled in the main loop and the
     * snapshot is read here; the 4 kHz ADC interrupt stamps telemetry.tick
     * from HAL_GetTick() in between, so a millisecond boundary inside that
     * window leaves tick one ahead of now. Unsigned, that subtraction wrapped
     * to 4294967295 and tripped FAULT_ADC on a perfectly healthy converter --
     * intermittently, and only while receiving, because the PendSV audio
     * worker widens the window. Every other deadline in this file already
     * compares signed. */
    const uint32_t fresh = (int32_t)(t.tick - analog_restart_tick) > 0 ? t.tick : analog_restart_tick;
    const int32_t telemetry_age_ms = (int32_t)(now - fresh);
    if (now > 20U && (!t.sequence || telemetry_age_ms > 5)) {
        if (!t.sequence) bump16(&svc_stall_dead);
        else { bump16(&svc_stall_late);
               svc_stall_gap = telemetry_age_ms > 0xFFFF ? 0xFFFFU : (uint16_t)telemetry_age_ms; }
        RADIO_Trip(RADIO_FAULT_ADC);
    }
    service_keys(now);
    if (state == RADIO_OFF || faults) return;
    if (calibration) {
        if (!service_calibration(now)) RADIO_CalStop();
        return;
    }
    uint32_t hz = MOD_GetCarrierRequestedHz();
    uint8_t lpf = 255U, bpf = 0U;
    uint8_t filters = RADIO_ConfigFilters(&config, hz, &lpf, &bpf);
    /* SPEC_TxAllowed: the WATERFALL page's CPU guard (radio_spectrum.h). */
    uint8_t ready = RADIO_ConfigTxReady(&config, hz, NULL, NULL) && MOD_CarrierFrequencyReadyForTx() &&
                    RADIO_PdPowerStable() && RADIO_AnalogRailOk() && SPEC_TxAllowed();
    tx_permitted = ready;
    /* Not ready while transmitting (tuned out of the TX range, PD, rail, the
     * WATERFALL guard): service_tx_exit below leaves TX in full. Dropping to
     * SETUP here first made it return early, leaving the AUX TX lines high
     * and the HRTIM TX outputs enabled into RECEIVE. */
    if (!ready) BOARD_PermitRf(0U);
    /* Signed: RADIO_RequestTx is documented IRQ-safe and stamps its own
     * HAL_GetTick(), so it can land one millisecond ahead of `now`. */
    if (tx_requested && (int32_t)(now - last_tx_request) > 80) tx_requested = 0U;
    if (tx_requested && ready) { service_tx_entry(now, &t, lpf, bpf); return; }
    service_tx_exit(now);
    /* Only a PLL switch in progress (or one that failed without fallback)
     * takes the clocks away. A pending frequency or clock change does not:
     * the receiver plans its own LO on whatever SYSCLK is running, so it keeps
     * playing through the settle time instead of going silent for ~360 ms. */
    if (MOD_CarrierClockUnsettled()) { RADIO_ClocksStop(); RADIO_AudioSuspend(); return; }
    /* Nothing may run on a board whose assembly has not been declared: no
     * RX_EN, no QSD clocks, no filter switching. RADIO_ClocksStop also drops
     * RX_EN and the test clock. */
    if (RADIO_ConfigRxBlock(&config)) {
        RADIO_ClocksStop();
        RADIO_AudioSuspend();
        state = RADIO_SETUP;
        return;
    }
    if (!filters) { service_no_filter(bpf); return; }
    if (filter_bpf != bpf || filter_lpf != lpf) RADIO_SelectFilters(lpf, bpf);
    if (now - transition_at < 5U) return;
    if (!service_rx_clock(hz)) return;
    BOARD_SetRx(1U);
    state = RADIO_RECEIVE;
    /* Receiving with nothing to transmit: park the TX timers (they refuse
     * while a TX stream runs) so they cannot leak into the receiver. */
    MOD_TxTimersPark((config.clock_flags & RADIO_CLOCK_FLAG_PARK_TX) ? 1U : 0U);
}

uint8_t RADIO_CalActive(void) { return calibration; }

void RADIO_CalStop(void)
{
    BOARD_PermitRf(0U);
    RADIO_AnalogStop();
    RADIO_SweepStop();
    RADIO_IqCalStop();
    RADIO_AfSweepStop();
    MOD_SelfTestAbort();
    RADIO_TestClockStop();
    MOD_RadioResetTransport();
    calibration = 0U;
    state = faults ? RADIO_FAULT : RADIO_SETUP;
    rx_frequency = 0U;
}

uint8_t RADIO_CalStart(uint8_t mode, uint16_t code)
{
    if (mode == RADIO_CAL_NONE) { RADIO_CalStop(); return 1U; }
    if (faults || state == RADIO_OFF || mode > RADIO_CAL_LAST ||
        MOD_AudioRealtimeCritical(HAL_GetTick()) || MOD_AUDIO_SOURCE_IS_LOCAL(MOD_GetAudioSource())) return 0U;
    /* Every bench routine drives real hardware: the bias buck, an envelope DAC
     * or the test clock. None may run before the assembly is declared.
     *
     * The self test is the exception, deliberately: a board that has not been
     * described yet is exactly the board an operator needs to test. It touches
     * no RF-path hardware of its own accord -- it re-checks RADIO_ConfigRxBlock
     * before the filter, receive and analog groups and skips them with that
     * reason, leaving the supply, clock, control and storage groups, which
     * drive nothing. */
    if (mode != RADIO_CAL_SELFTEST && RADIO_ConfigRxBlock(&config)) return 0U;
    if ((mode == RADIO_CAL_RX_TEST || mode == RADIO_CAL_BPF_SWEEP ||
         mode == RADIO_CAL_RX_IQ || mode == RADIO_CAL_AF_SWEEP) && state != RADIO_RECEIVE) return 0U;
    if (mode == RADIO_CAL_BIAS && (config.bias_ma <= RADIO_BIAS_MA_OFF || filter_lpf >= 5U)) return 0U;
    if ((mode == RADIO_CAL_ENV_A && config.drain_a != RADIO_DRAIN_ENV) ||
        (mode == RADIO_CAL_ENV_B && config.drain_b != RADIO_DRAIN_ENV) || code > 1024U) return 0U;
    /* Reserve calibration atomically with the RF gate. An ADC/DMA protection
     * IRQ may arrive between UI calls; it must not be followed by a late
     * calibration DAC or test-clock write. Not while keyed either: a routine
     * holds RADIO_Service off, and SETUP after it skips the TX exit, which
     * left the bias, the AUX TX lines and the HRTIM TX outputs on. */
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    if (faults || BOARD_EmergencyLatched() || state == RADIO_OFF || calibration ||
        state == RADIO_TX_PREPARE || state == RADIO_TRANSMIT) {
        __set_PRIMASK(pm);
        return 0U;
    }
    calibration = mode; /* prevent any new RF renderer/DMA start */
    tx_requested = tx_permitted = 0U;
    BOARD_PermitRf(0U);
    __set_PRIMASK(pm);
    MOD_RadioResetTransport();
    RADIO_AnalogStop();
    uint8_t failed = 0U;
    pm = __get_PRIMASK();
    __disable_irq();
    if (faults || BOARD_EmergencyLatched()) {
        failed = 1U;
    }
    if (mode == RADIO_CAL_SELFTEST) {
        /* The self test sequences its own steps and decides per group what it
         * may touch, so starting it only reserves the interlock. In particular
         * the receiver is left running: the RX and FILTERS groups need it, and
         * a board that cannot receive must still complete the other groups. */
        if (!failed) MOD_SelfTestBegin(HAL_GetTick());
    } else if (mode == RADIO_CAL_RX_TEST) {
        /* A nearby low-level signal through the fitted 100 k injection path.
         * A frequency request is quantised to an even HRTIM period; at 40 m
         * that used period LO-2 (about 19 kHz away), near the analogue/front
         * filter skirt, although LO-1 gives the intended clean 9 kHz tone.
         * Use the same bounded exact-period planner as RX calibration. */
        radio_rx_geometry_t g;
        uint32_t test_period = 0U;
        RADIO_RxClockGeometry(&g);
        if (!failed &&
            (!RADIO_IqChoosePeriod(g.period, RADIO_AudioI2sDivider(), RADIO_AudioRate(),
                                   6000.0f, 45000.0f, &test_period) ||
             !RADIO_TestClockStartPeriod(test_period))) failed = 1U;
    } else if (mode == RADIO_CAL_BPF_SWEEP) {
        /* Code selects the axis: the whole-range band-pass sweep, the
         * per-slot low-pass sweep, or QMX's per-band linear RF sweep. */
        if (!failed) RADIO_SweepBegin(HAL_GetTick(), (uint8_t)code);
    } else if (mode == RADIO_CAL_RX_IQ) {
        /* Code 1 adds the opposite-sideband passes. The receiver keeps
         * running: the correlator reads its raw I/Q. */
        if (!failed) RADIO_IqCalBegin(HAL_GetTick(), (uint8_t)(code != 0U));
    } else if (mode == RADIO_CAL_AF_SWEEP) {
        /* Code 1 selects the image sweep. The receiver keeps running: the
         * sweep moves its demodulator and reads the channel level. */
        if (!failed) RADIO_AfSweepBegin(HAL_GetTick(), (uint8_t)(code != 0U));
    } else {
        RADIO_AudioSuspend();
        RADIO_ClocksStop();
        state = RADIO_SETUP;
        if (!failed) {
            if (mode == RADIO_CAL_BIAS) RADIO_BiasEnable(1U);
            else DAC3->DHR12RD = mode == RADIO_CAL_ENV_A ? (uint32_t)code << 16U : code;
        }
    }
    __set_PRIMASK(pm);
    if (failed) { RADIO_CalStop(); return 0U; }
    /* 64 points x 4 filters take ~20 s and x 5 low-pass filters ~35 s, because
     * each of those has to be switched cold and let its PIN diodes settle;
     * the self test can wait on an operator
     * pressing every button; the AF sweep is 152 points at 43 ms and the
     * image sweep half that; the I/Q calibration is up to five 370 ms
     * integrations plus settling. RX_TEST also backs diagnostic USB-I/Q
     * captures, whose re-enumeration and ALSA startup consume several seconds,
     * so keep that low-level 100 k injection alive for the full capture. */
    calibration_until = HAL_GetTick() + (mode == RADIO_CAL_BPF_SWEEP ? 90000U :
                                         mode == RADIO_CAL_SELFTEST ? 300000U :
                                         mode == RADIO_CAL_AF_SWEEP ? 30000U :
                                         mode == RADIO_CAL_RX_IQ ? 15000U :
                                         mode == RADIO_CAL_RX_TEST ? 30000U : 5000U);
    return 1U;
}

void RADIO_Shutdown(void)
{
    BOARD_PermitRf(0U);
    calibration = 0U;
    RADIO_AudioStop();
    MOD_RadioResetTransport();
    RADIO_ClocksStop();
    RADIO_AnalogStop();
    state = RADIO_OFF;
    tx_requested = 0U;
    /* Drain storage from the foreground before releasing the hold latch. A
     * copy or delete in progress is cancelled now (open files closed, the
     * partial copy removed) rather than cut by the power switch. */
    SD_AudioPlayerClose();
    if (SD_FileJobActive()) { SD_FileJobCancel(); SD_FileJobService(); }
    (void)MOD_SaveSettings();
    /* PCM1821 requires clocks absent for at least 10 ms before power removal. */
    HAL_Delay(10U);
    RADIO_PdPrepareShutdown();
    BOARD_PowerOff();
    /* Q13 released: C15 (1000 uF) drains within well under a second. Still
     * running after 3 s means Q2 is held on elsewhere (SW8 pressed, or power
     * fed past it on the bench). SW8 cannot be read by the MCU on REV1, so
     * restart normally instead of sitting dark with the latch released. */
    for (uint32_t start = HAL_GetTick(); HAL_GetTick() - start < 3000U;) {
        APP_WatchdogProgress();
        __WFI();
    }
    NVIC_SystemReset();
}

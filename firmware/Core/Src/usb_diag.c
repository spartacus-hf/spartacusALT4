#pragma GCC optimize ("Os")

#include "usb_diag.h"
#include "usb_diag_mode.h"
#include "usb_diag_pins.h"
#include "usb_diag_keys.h"
#include "usbd_cdc_min.h"
#include "cat.h"
#include "usbd_cat.h"
#include "sctrl_link.h"
#include "sctrl_link_logic.h"
#include "usbd_audio.h"
#include "mod_selftest.h"
#include "radio.h"
#include "radio_analog.h"
#include "radio_audio.h"
#include "radio_digi.h"
#include "radio_bpf_sweep.h"
#include "radio_af_sweep.h"
#include "radio_clocks.h"
#include "radio_rx_cal.h"
#include "radio_pd.h"
#include "board_rev1.h"
#include "modulator.h"
#include "mod_sdcard_service.h"
#include "cw_pack.h"
#include "mod_sdcard.h"
#include "mod_sd_store.h"
#include "card_text.h"
#include "fw_update.h"
#include "mod_crc32.h"
#include "mod_stack_watermark.h"
#include "ui_priv.h"
#include "ui_app.h"
#include "ui_state.h"
#include "ui_format.h"
#include "ui_labels_priv.h"
#include "main.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Output is produced a line at a time, on demand, by a producer function --
 * the same shape as SD_ExportLines. Nothing is ever buffered in full, so a
 * `pins` dump of 86 rows costs the same RAM as a one-line reply. */
typedef uint16_t (*diag_line_fn)(uint16_t index, char *buf, uint16_t cap);

#define DIAG_LINE_MAX  120U
#define DIAG_ARGS_MAX    6U

/* Packets emitted per service pass. The radio always comes first: while the
 * audio path is in its critical window the console emits nothing at all, and
 * outside it the budget still only spends a bounded slice of one main-loop
 * pass. At 64 bytes a packet and a roughly 1 kHz loop, the full budget is
 * about 500 kB/s, well past what a full-speed bulk endpoint will carry. */
#define DIAG_BUDGET_FULL  8U
#define DIAG_BUDGET_BUSY  2U

static struct {
    char     line[DIAG_LINE_MAX];
    uint16_t line_len, line_sent, produce_index;
    diag_line_fn producer;
    uint32_t bytes_in, bytes_out;
    uint8_t  ui_mode;
    uint8_t  ui_screen, ui_repaint, ui_parked;
    uint8_t  ui_enc_cw, ui_enc_ccw, ui_enc_click, ui_enc_hold;
    uint32_t ui_next_frame;
    char     path[96];         /* current sd path argument */
    uint32_t get_offset, get_length;
    uint8_t  mic_source, mic_rows;
    /* `audio` cannot switch personality while it is answering: the CDC
     * endpoint goes away with the switch, so the reply would never be sent.
     * The window is held here and taken once the reply has drained. */
    uint32_t audio_window_s, audio_switch_at;
    /* `link 1` answered: the port goes to the LINK once the reply has left. */
    uint8_t  link_switch;
    const char *tail;          /* the OK/ERR line owed once the producer ends */
    /* Input is taken a packet at a time but consumed a byte at a time, and not
     * past the end of a command until its reply has gone out: a host that
     * sends two commands in one write gets two whole replies, and a person's
     * echo never lands in the middle of one. */
    uint8_t  rx[CDC_MIN_PACKET];
    uint16_t rx_len, rx_pos;
    uint8_t  cmd_ready;
    term_decoder_t term;
    /* The line being typed, and whether a person is typing it
     * (usb_diag_keys.h): echo, line editing and a prompt from the first CR
     * until the port is closed. */
    term_line_t ed;
    uint8_t  prompt_owed, was_open, greet_due, session;
    uint32_t opened_at;
#if !MOD_DEEP_DIAG_ENABLE
    /* Virtual paddle page. */
    vpaddle_t paddle;
    uint8_t  levers;           /* what the keyer is being given now */
    uint8_t  kitty_flags;      /* terminal's reply to CSI ? u, bit 7 once it came */
    uint8_t  releases_seen;    /* the terminal has reported a key release */
    uint8_t  modes_due, modes_on, modes_off_due; /* key reporting and focus reports */
    uint8_t  paddle_tx;        /* the page unmuted TX and mutes it again on leaving */
    uint8_t  held_keys[4], held_count; /* page keys taken while RF was keyed */
#endif
} d;

_Static_assert(sizeof(((term_line_t *)0)->echo) <= DIAG_LINE_MAX,
               "the echo goes out through d.line in one piece");

#define GREETING BOARD_NAME " " FW_VERSION_STRING " console -- help, ui (menu), paddle (CW keys)"

static char *const argv_store[DIAG_ARGS_MAX] = {0};
static const char *argv[DIAG_ARGS_MAX];
static uint8_t argc;

__attribute__((cold)) void USB_DiagReset(void)
{
    memset(&d, 0, sizeof(d));
    (void)argv_store;
    TERM_LineReset(&d.ed, GREETING);
#if !MOD_DEEP_DIAG_ENABLE
    MOD_CwSetVirtualPaddle(0U);
#endif
}

/* The console's port: the DIAG personality's own CDC port, or in the audio
 * personality the USB serial port a line ending made a terminal (cat.c), as
 * QMX's terminal shares its CAT ports. */
static uint8_t port_open(void)
{
    if (SCTRL_LinkActive()) return 1U;
    return USB_DiagModeIsActive() ? USBD_CdcMinIsOpen() : CAT_ConsoleOpen();
}
static uint8_t port_tx_ready(void)
{
    if (SCTRL_LinkActive()) return SCTRL_LinkConsoleReady();
    return USB_DiagModeIsActive() ? USBD_CdcMinTxReady() : CAT_ConsoleTxReady();
}
/* Over the LINK replies go out as CONSOLE frames and lines come in as CMD
 * frames, streamed into the line (USB_DiagLinkByte), never through here. */
static uint16_t port_transmit(const uint8_t *data, uint16_t length)
{
    if (SCTRL_LinkActive()) return SCTRL_LinkConsoleWrite(data, length);
    return USB_DiagModeIsActive() ? USBD_CdcMinTransmit(data, length) : CAT_ConsoleTransmit(data, length);
}
static uint16_t port_receive(uint8_t *out, uint16_t cap)
{
    if (SCTRL_LinkActive()) return 0U;
    return USB_DiagModeIsActive() ? USBD_CdcMinReceive(out, cap) : CAT_ConsoleReceive(out, cap);
}
static uint32_t port_rx_stamp(void) { return USB_DiagModeIsActive() ? USBD_CdcMinRxStamp() : CAT_ConsoleRxStamp(); }

uint8_t USB_DiagIsOpen(void) { return port_open(); }
uint32_t USB_DiagBytesOut(void) { return d.bytes_out; }
uint32_t USB_DiagBytesIn(void) { return d.bytes_in; }

/* ---- reply plumbing ---- */

__attribute__((cold)) static void reply_done(const char *tail)
{
    d.producer = NULL;
    d.produce_index = 0U;
    d.tail = tail;
}

/* A usage line kept on the card (card_text.def): "ERR usage: ..." when the
 * card can say it, "ERR usage" when it cannot. */
static char usage_line[DIAG_LINE_MAX];

__attribute__((cold)) static void reply_usage(card_text_t text)
{
    static const char head[] = "ERR usage: ";
    memcpy(usage_line, head, sizeof(head));
    if (UI_CardText(text, usage_line + sizeof(head) - 1U, (uint16_t)(sizeof(usage_line) - sizeof(head) + 1U)) == 0U)
        usage_line[sizeof(head) - 3U] = '\0';
    reply_done(usage_line);
}

__attribute__((cold)) static void reply_lines(diag_line_fn fn, const char *tail)
{
    d.producer = fn;
    d.produce_index = 0U;
    d.tail = tail;
}

/* One clamping formatter for every producer: snprintf returns the length it
 * wanted, not the length it wrote, so casting its result directly would hand
 * the transport a length past the end of the buffer on a long line. */
__attribute__((cold, format(printf, 3, 4)))
static uint16_t emit(char *buf, uint16_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    if (n < 0) return 0U;
    return (uint16_t)(n >= (int)cap ? (int)cap - 1 : n);
}

__attribute__((cold)) static uint16_t copy_line(char *buf, uint16_t cap, const char *text)
{
    return emit(buf, cap, "%s\r\n", text);
}

/* ---- argument helpers ---- */

__attribute__((cold)) static uint8_t arg_is(uint8_t index, const char *name)
{
    if (index >= argc) return 0U;
    const char *a = argv[index];
    for (;; ++a, ++name) {
        char x = *a, y = *name;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0U;
        if (!x) return 1U;
    }
}

__attribute__((cold)) static int32_t arg_int(uint8_t index, int32_t fallback)
{
    if (index >= argc) return fallback;
    const char *s = argv[index];
    int32_t sign = 1, value = 0;
    if (*s == '-') { sign = -1; ++s; }
    if (!*s) return fallback;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return fallback;
        value = value * 10 + (*s - '0');
    }
    return sign * value;
}

/* Decimal string to float without pulling in strtof's locale machinery. */
__attribute__((cold)) static float arg_float(uint8_t index, float fallback)
{
    if (index >= argc) return fallback;
    const char *s = argv[index];
    float sign = 1.0f, whole = 0.0f, frac = 0.0f, scale = 1.0f;
    if (*s == '-') { sign = -1.0f; ++s; } else if (*s == '+') ++s;
    if (!*s) return fallback;
    for (; *s >= '0' && *s <= '9'; ++s) whole = whole * 10.0f + (float)(*s - '0');
    if (*s == '.') {
        for (++s; *s >= '0' && *s <= '9'; ++s) { frac = frac * 10.0f + (float)(*s - '0'); scale *= 10.0f; }
    }
    if (*s) return fallback;
    return sign * (whole + frac / scale);
}

/* ---- commands ---- */

__attribute__((cold)) static uint16_t line_help(uint16_t index, char *buf, uint16_t cap);

__attribute__((cold)) static uint16_t line_id(uint16_t index, char *buf, uint16_t cap)
{
    switch (index) {
        case 0: return emit(buf, cap, "board=%s\r\n", BOARD_NAME);
        case 1: return emit(buf, cap, "firmware=%s\r\n", FW_VERSION_STRING);
        case 2: return emit(buf, cap, "sysclk_hz=%lu\r\n", (unsigned long)SystemCoreClock);
        case 3: return emit(buf, cap, "reset=%s\r\n", MOD_GetResetReasonLabel());
        case 4: return emit(buf, cap, "uptime_ms=%lu\r\n", (unsigned long)HAL_GetTick());
        case 5: return emit(buf, cap, "state=%s\r\n", RADIO_Status());
        case 6:
            /* Not pd.attached: on a board whose CC analog PHY never came up
             * that reads "dc" with a charger plugged in. */
            return emit(buf, cap, "power=%s\r\n", RADIO_PdSupplyName());
        case 7: {
            const char *why = RADIO_ConfigTxBlock(RADIO_Config(), MOD_GetCarrierRequestedHz());
            return emit(buf, cap, "tx_block=%s\r\n", why ? why : "none");
        }
        case 8: return emit(buf, cap, "deep_diag=%u\r\n", (unsigned)MOD_DEEP_DIAG_ENABLE);
        /* The MCU's unique ID, high word first: the store, its backups and
         * v8.08's flash dump (tools/flash_rev1.py) are tied to it. */
        case 9: return emit(buf, cap, "uid=%08lX%08lX%08lX\r\n", (unsigned long)HAL_GetUIDw2(),
                            (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw0());
        default: return 0U;
    }
}

__attribute__((cold)) static uint16_t line_telem(uint16_t index, char *buf, uint16_t cap)
{
    radio_telemetry_t t;
    RADIO_GetTelemetry(&t);
    switch (index) {
        case 0: return emit(buf, cap, "vin_mv=%u\r\n", (unsigned)t.vin_mv);
        case 1: return emit(buf, cap, "rail5_mv=%u\r\n", (unsigned)t.rail5_mv);
        case 2: return emit(buf, cap, "bias_mv=%u\r\n", (unsigned)t.bias_mv);
        case 3: return emit(buf, cap, "bias_ma_x10=%ld\r\n",
                    (long)(RADIO_BiasCurrentMa(t.bias_mv, t.rail5_mv) * 10.0f));
        case 4: return emit(buf, cap, "bias_duty_1024=%u\r\n", (unsigned)t.bias_duty);
        case 5: return emit(buf, cap, "env_a_mv=%u\r\n", (unsigned)t.env_a_mv);
        case 6: return emit(buf, cap, "env_b_mv=%u\r\n", (unsigned)t.env_b_mv);
        case 7: return emit(buf, cap, "fwd_mv=%u\r\n", (unsigned)t.fwd_mv);
        case 8: return emit(buf, cap, "rev_mv=%u\r\n", (unsigned)t.rev_mv);
        case 9: return emit(buf, cap, "faults=%02lX\r\n", (unsigned long)t.faults);
        case 10: return emit(buf, cap, "seq=%lu\r\n", (unsigned long)t.sequence);
        case 11: return emit(buf, cap, "vref_mv=%u\r\n", (unsigned)MOD_GetVrefMillivolts());
#if MOD_DEEP_DIAG_ENABLE
        case 12: return emit(buf, cap, "cal=%u\r\n", (unsigned)RADIO_CalActive());
#else
        /* Why protection fired, in names rather than a single fault bit: the
         * telemetry watchdog and the ADC DMA handler both raise FAULT_ADC and
         * mean entirely different things. */
        case 12: {
            uint32_t te=0U, late=0U, ovr=0U, dead=0U, slate=0U, gap=0U, pf=0U, rf=0U;
            RADIO_AnalogFailCounts(&te, &late, &ovr);
            RADIO_ServiceStallCounts(&dead, &slate, &gap);
            RADIO_ServiceRetuneFails(&pf, &rf);
            return emit(buf, cap,
                        "cal=%u adc_te=%lu adc_late=%lu adc_ovr=%lu dead=%lu stall=%lu gap_ms=%lu pf=%lu rf=%lu\r\n",
                        (unsigned)RADIO_CalActive(), (unsigned long)te,
                        (unsigned long)late, (unsigned long)ovr, (unsigned long)dead,
                        (unsigned long)slate, (unsigned long)gap, (unsigned long)pf, (unsigned long)rf);
        }
#endif
        /* The power and SWR meter's last reading (`meter` has all of it). */
        case 13: return emit(buf, cap, "pep_w_x100=%ld rev_w_x1000=%ld swr_x100=%ld swr_raw_x100=%ld meter_seq=%lu\r\n",
                             (long)(rf_meter.last.pep_w * 100.0f + 0.5f), (long)(rf_meter.last.ref_w * 1000.0f + 0.5f),
                             (long)(rf_meter.last.swr * 100.0f + 0.5f), (long)(rf_meter.last.swr_raw * 100.0f + 0.5f),
                             (unsigned long)rf_meter.seq);
        default: return 0U;
    }
}

/* Live GPIO state for every pin the REV1 map names: what the board is
 * actually doing with each line, next to what the map says it is for. */
__attribute__((cold)) static uint16_t line_pins(uint16_t index, char *buf, uint16_t cap)
{
    static GPIO_TypeDef *const ports[7] = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG};
    static const char *const modes[4] = {"in", "out", "af", "analog"};
    static const char *const pulls[4] = {"none", "up", "down", "?"};
    if (!index) return copy_line(buf, cap, "pin label signal mode pull level af");
    const uint16_t i = (uint16_t)(index - 1U);
    if (i >= diag_pin_count) return 0U;
    const diag_pin_t *p = &diag_pins[i];
    GPIO_TypeDef *g = ports[p->port];
    const uint8_t mode = (uint8_t)((g->MODER >> (p->pin * 2U)) & 3U);
    const uint8_t pull = (uint8_t)((g->PUPDR >> (p->pin * 2U)) & 3U);
    const uint8_t level = (uint8_t)((g->IDR >> p->pin) & 1U);
    const uint8_t af = (uint8_t)((g->AFR[p->pin >> 3U] >> ((p->pin & 7U) * 4U)) & 15U);
    return emit(buf, cap, "P%c%u %s %s %s %s %u %u\r\n",
                              (char)('A' + p->port), (unsigned)p->pin, p->label, p->signal,
                              modes[mode], pulls[pull], (unsigned)level, (unsigned)af);
}

/* Panel state, polled. QMX's diagnostics screen shows the same rows. */
__attribute__((cold)) static uint16_t line_keys(uint16_t index, char *buf, uint16_t cap)
{
    static const char *const names[5] = {"up", "down", "select", "sw4", "sw5"};
    if (index < 5U)
        return emit(buf, cap, "button.%s=%u\r\n", names[index],
                                  (unsigned)BOARD_ButtonPressed((uint8_t)index));
    if (index == 5U) {
        uint8_t high = 0U;
        const uint8_t locked = BOARD_ButtonService(HAL_GetTick(), &high);
        return emit(buf, cap, "button.polarity=%s\r\n",
                                  !locked ? "learning" : (high ? "active-high" : "active-low"));
    }
    if (index == 6U) {
        int8_t steps = 0;
        uint8_t click = 0U, long_click = 0U;
        const uint8_t fitted = BOARD_EncoderRead(&steps, &click, &long_click);
        return emit(buf, cap, "encoder=%s steps=%d click=%u hold=%u\r\n",
                                  fitted ? "fitted" : "absent", (int)steps,
                                  (unsigned)click, (unsigned)long_click);
    }
    if (index == 7U)
        return emit(buf, cap, "ptt=%u\r\n", (unsigned)RADIO_Ptt());
    return 0U;
}

__attribute__((cold)) static uint16_t line_rx(uint16_t index, char *buf, uint16_t cap)
{
    switch (index) {
        case 0: return emit(buf, cap, "rate_hz=%ld\r\n", (long)RADIO_AudioRate());
        case 1: return emit(buf, cap, "level_ddbfs=%ld\r\n",
                    (long)(RADIO_AudioRxLevelDbfs() * 10.0f));
#if MOD_DEEP_DIAG_ENABLE
        /* This flavour has the full profiler; the summary counters are the
         * poor relation and it has no flash left for both. */
        case 2: return emit(buf, cap, "overruns=%lu\r\n", (unsigned long)RADIO_AudioOverruns());
#else
        /* Which deadline the audio path missed, and whether PendSV ran at
         * all: `workers` should climb at about rate_hz / 64 per second. */
        case 2: {
            uint32_t pending = 0U, working = 0U, overrun = 0U, dma = 0U, workers = 0U;
            RADIO_AudioFailCounts(&pending, &working, &overrun, &dma, &workers);
            return emit(buf, cap, "overruns=%lu pending=%lu working=%lu ovr=%lu dma=%lu workers=%lu\r\n",
                        (unsigned long)RADIO_AudioOverruns(), (unsigned long)pending,
                        (unsigned long)working, (unsigned long)overrun,
                        (unsigned long)dma, (unsigned long)workers);
        }
#endif
#if MOD_DEEP_DIAG_ENABLE
        /* This flavour has the deep profiler instead, and no room for both. */
        case 3: return emit(buf, cap, "i2s_fallbacks=%lu\r\n",
                    (unsigned long)RADIO_AudioI2sFallbacks());
#else
        case 3: {
            radio_audio_cycles_t cyc;
            RADIO_AudioCycleCosts(&cyc);
            return emit(buf, cap,
                        "i2s_fallbacks=%lu wmax=%lu lat=%lu rxmax=%lu rxavg=%lu phmax=%lu phavg=%lu\r\n",
                        (unsigned long)RADIO_AudioI2sFallbacks(),
                        (unsigned long)cyc.worker_max, (unsigned long)cyc.latency_max,
                        (unsigned long)cyc.rx_max, (unsigned long)cyc.rx_avg,
                        (unsigned long)cyc.phone_max, (unsigned long)cyc.phone_avg);
        }
        case 10: {
            radio_audio_cycles_t cyc;
            RADIO_AudioCycleCosts(&cyc);
            return emit(buf, cap, "frontend=%lu demod=%lu\r\n",
                        (unsigned long)cyc.fe_avg, (unsigned long)cyc.demod_avg);
        }
#endif
        case 4: {
            uint8_t lpf = 255U, bpf = 255U;
            (void)RADIO_ConfigFilters(RADIO_Config(), MOD_GetCarrierRequestedHz(), &lpf, &bpf);
            return emit(buf, cap, "lpf=%d bpf=%d\r\n",
                                      lpf < 5U ? (int)lpf : -1, bpf < 4U ? (int)bpf : -1);
        }
        case 5: return emit(buf, cap, "carrier_hz=%lu\r\n",
                    (unsigned long)MOD_GetCarrierRequestedHz());
        /* The LO and the IF behind it: two measurements taken minutes apart
         * only compare if the planner did not move the LO between them. */
        case 6: {
            const uint32_t lo = RADIO_RxClockHz();
            return emit(buf, cap, "lo_hz=%lu if_hz=%ld quality=%u\r\n", (unsigned long)lo,
                        lo ? (long)((int32_t)MOD_GetCarrierRequestedHz() - (int32_t)lo) : 0L,
                        (unsigned)RADIO_RxClockQuality());
        }
        case 7: {
            radio_rx_geometry_t g;
            RADIO_RxClockGeometry(&g);
            return emit(buf, cap, "lo_period=%lu fall=%lu rise=%lu\r\n",
                        (unsigned long)g.period, (unsigned long)g.fall, (unsigned long)g.rise);
        }
        case 8: {
            const radio_config_t *c = RADIO_Config();
            return emit(buf, cap, "i_gain_x1000=%ld q_gain_x1000=%ld skew_x1000=%ld invert=%u\r\n",
                        (long)(c->rx_i_gain * 1000.0f), (long)(c->rx_q_gain * 1000.0f),
                        (long)(c->rx_skew_samples * 1000.0f), (unsigned)c->rx_iq_invert);
        }
        case 9: {
            const char *why = RADIO_ConfigRxBlock(RADIO_Config());
            return emit(buf, cap, "state=%s rx_block=%s usb_iq=%u capture=%u\r\n", RADIO_Status(),
                        why ? why : "none", (unsigned)RADIO_Config()->usb_iq,
                        (unsigned)RADIO_CaptureStreaming());
        }
        default: return 0U;
    }
}

/* DIGI receive health: what the host's capture stream received (RADIO_DigiStats). */
__attribute__((cold)) static uint16_t line_digi(uint16_t index, char *buf, uint16_t cap)
{
    radio_digi_stats_t st;
    RADIO_DigiStats(&st);
    switch (index) {
        case 0: return emit(buf, cap, "streaming=%u sent=%lu padded=%lu dropped=%lu silent=%lu restarts=%lu\r\n",
                            (unsigned)st.streaming, (unsigned long)st.cap_sent, (unsigned long)st.cap_padded,
                            (unsigned long)st.cap_dropped, (unsigned long)st.cap_silent,
                            (unsigned long)st.restarts);
        case 1: return emit(buf, cap, "level_ddbfs=%ld peak_ddbfs=%ld clips=%lu adc_peak_ddbfs=%ld adc_clips=%lu\r\n",
                            (long)(st.level_dbfs * 10.0f), (long)(st.peak_dbfs * 10.0f),
                            (unsigned long)st.cap_clips, (long)(st.adc_peak_dbfs * 10.0f),
                            (unsigned long)st.adc_clips);
        case 2: {
            radio_digi_status_t g;
            RADIO_DigiStatus(&g, HAL_GetTick());
            return emit(buf, cap, "mode_digi=%u live=%u slot_ms=%ld slot_lock=%u depth_ddb=%ld gaps_10min=%lu gaps_slot=%lu\r\n",
                        (unsigned)RADIO_DigiReceive(), (unsigned)g.host_live, (long)g.slot_ms, (unsigned)g.slot_locked,
                        (long)(g.slot_depth_db * 10.0f), (unsigned long)g.gaps_10min, (unsigned long)g.gaps_slot);
        }
        case 3: {
            radio_digi_status_t g;
            RADIO_DigiStatus(&g, HAL_GetTick());
            return emit(buf, cap, "follow=%u ref_state=%u ref_ppb=%ld reacq_10min=%lu load_pct=%u quiet=%u\r\n",
                        (unsigned)g.follow, (unsigned)g.ref_state, (long)(g.ref_ppm * 1000.0f),
                        (unsigned long)g.reacq_10min, (unsigned)g.load_pct, (unsigned)RADIO_DigiQuiet(HAL_GetTick()));
        }
        default: return 0U;
    }
}

__attribute__((cold)) static uint16_t line_pd(uint16_t index, char *buf, uint16_t cap)
{
    radio_pd_info_t pd;
    RADIO_PdSnapshot(&pd);
    if (!index)
        return emit(buf, cap, "attached=%u contract=%u mv=%u ma=%u cc=%u rev=%u\r\n",
                                  (unsigned)pd.attached, (unsigned)pd.contract, (unsigned)pd.mv,
                                  (unsigned)pd.ma, (unsigned)pd.cc, (unsigned)pd.revision);
    if (index == 1U)
        return emit(buf, cap,
                    "pending=%u rp=%u requested_mv=%u requested_ma=%u failures=%lu resets=%lu resyncs=%lu event=%lu status=%s\r\n",
                    (unsigned)pd.pending, (unsigned)pd.rp, (unsigned)pd.requested_mv,
                    (unsigned)pd.requested_ma, (unsigned long)pd.failures,
                    (unsigned long)pd.resets, (unsigned long)pd.resyncs, (unsigned long)pd.last_event,
                    RADIO_PdStatus());
    if (index == 2U) {
        const radio_config_t *c = RADIO_Config();
        radio_telemetry_t t;
        RADIO_GetTelemetry(&t);
        /* VIN is the combined USB/DC rail: a DC supply on the barrel jack
         * masks VBUS, which is not a fault and is easy to miss. */
        return emit(buf, cap,
                    "enable=%u target_mv=%u max_mv=%u limit_ma=%u internal_rd=%u vin_mv=%u vsafe5=%u\r\n",
                    (unsigned)c->pd_enable, (unsigned)c->pd_target_mv, (unsigned)c->pd_max_mv,
                    (unsigned)c->pd_current_ma,
                    (unsigned)c->pd_internal_rd, (unsigned)t.vin_mv,
                    (unsigned)(t.vin_mv >= 4400U && t.vin_mv <= 5500U));
    }
    if (index == 3U)
        return emit(buf, cap, "supply=%s why=%s probe=%s\r\n   %s\r\n",
                    RADIO_PdSupplyName(), RADIO_PdWhyName(), RADIO_PdProbeName(),
                    RADIO_PdWhyDetail());
    if (index - 4U < pd.count) {
        radio_pd_offer_t offer;
        const unsigned i = index - 4U;
        if (!RADIO_PdDecode(pd.pdo[i], &offer))
            return emit(buf, cap, "pdo%u=unsupported\r\n", i + 1U);
        return emit(buf, cap, "pdo%u=%s %u-%u mv %u ma sel=%u\r\n", i + 1U,
                                  offer.type == RADIO_PD_FIXED ? "fixed" :
                                  offer.type == RADIO_PD_PPS ? "pps" :
                                  offer.type == RADIO_PD_BATTERY ? "battery" : "variable",
                                  (unsigned)offer.min_mv, (unsigned)offer.max_mv,
                                  (unsigned)offer.max_ma, (unsigned)offer.selectable);
    }
    return 0U;
}

__attribute__((cold)) static uint16_t line_selftest(uint16_t index, char *buf, uint16_t cap)
{
    if (!index)
        return emit(buf, cap, "id group test verdict value unit low high detail\r\n");
    const uint8_t id = (uint8_t)(index - 1U);
    if (id >= SELFTEST_COUNT) return 0U;
    const selftest_result_t *r = MOD_SelfTestResult(id);
    if (!r || r->verdict == ST_PENDING) return emit(buf, cap, "\r\n");
    int32_t low = 0, high = 0;
    char detail[80];
    MOD_SelfTestRowLimits(id, &low, &high);
    return emit(buf, cap, "%u %s %s %s %ld %s %ld %ld %s\r\n", (unsigned)id,
                              MOD_SelfTestGroupName(MOD_SelfTestGroup(id)), MOD_SelfTestName(id),
                              MOD_SelfTestVerdictName(r->verdict), (long)r->value,
                              MOD_SelfTestUnit(id), (long)low, (long)high,
                              MOD_SelfTestDetail(id, detail, sizeof(detail)));
}

__attribute__((cold)) static uint16_t line_settings(uint16_t index, char *buf, uint16_t cap)
{
    if (index >= SET_COUNT) return 0U;
    const setting_item_t *it = UI_GetSetting((setting_id_t)index);
    if (!it || (!it->get && !it->set && it->style != STY_ACTION)) return emit(buf, cap, "\r\n"); /* not in this build */
    char value[24] = "-";
    if (it->get) (void)UI_AppendF(value, it->get(), it->decimals);
    float lo = 0.0f, hi = 0.0f;
    char range[32] = "";
    if (UI_SettingRange((setting_id_t)index, &lo, &hi)) {
        char *w = range;
        w = UI_AppendF(w, lo, it->decimals);
        *w++ = '.'; *w++ = '.';
        (void)UI_AppendF(w, hi, it->decimals);
    }
    return emit(buf, cap, "%u \"%s\" %s %s %u\r\n", (unsigned)index, UI_SettingName((setting_id_t)index),
                              value, range[0] ? range : "-", (unsigned)it->style);
}


__attribute__((cold)) static uint16_t line_mem(uint16_t index, char *buf, uint16_t cap)
{
    extern uint32_t _sccmram_bss, _eccmram_bss;
    switch (index) {
        case 0: return emit(buf, cap, "sysclk_hz=%lu\r\n", (unsigned long)SystemCoreClock);
        case 1: return emit(buf, cap, "stack_unused_b=%lu\r\n",
                            (unsigned long)MOD_StackWatermarkUnusedBytes());
        case 2: return emit(buf, cap, "ccm_bss_b=%lu\r\n",
                            (unsigned long)((uint8_t *)&_eccmram_bss - (uint8_t *)&_sccmram_bss));
        case 3: return emit(buf, cap, "diag_bytes_in=%lu\r\n", (unsigned long)d.bytes_in);
        case 4: return emit(buf, cap, "diag_bytes_out=%lu\r\n", (unsigned long)d.bytes_out);
        case 5: return emit(buf, cap, "sd_capacity_mib=%lu\r\n", (unsigned long)SD_ServiceCapacityMiB());
        default: return 0U;
    }
}

/* `clock`: the whole clock tree as the radio runs it -- PLL setting, SYSCLK
 * from the corrected reference, the TX carrier grid and what its NCO removes,
 * the receive LO, and the retune machinery. Values are exact integers:
 * micro-hertz SYSCLK, milli-hertz residuals. */
__attribute__((cold)) static uint16_t line_clock(uint16_t index, char *buf, uint16_t cap)
{
    static const char *const states[] = {"idle", "pending", "waiting", "applying",
                                         "failed", "recovered", "failed_fallback"};
    mod_clock_snapshot_t k;
    MOD_GetClockSnapshot(&k);
    const uint64_t uhz = MOD_SysclkMicroHz();
    const uint32_t dial = MOD_GetCarrierRequestedHz();
    const uint8_t flags = RADIO_Config()->clock_flags;
    switch (index) {
        case 0: return emit(buf, cap, "policy=%s pll=%u/%u/%u sysclk_hz=%lu.%06lu ref_ppb=%ld\r\n",
                            MOD_GetCarrierClockPolicyLabel((mod_clock_policy_t)k.policy),
                            (unsigned)k.pll_m, (unsigned)k.pll_n, (unsigned)k.pll_r,
                            (unsigned long)(uhz / 1000000ULL), (unsigned long)(uhz % 1000000ULL),
                            (long)RADIO_Config()->ref_ppb);
        case 1: return emit(buf, cap, "tx dial_hz=%lu period=%u grid_hz=%lu grid_err_mhz=%ld image_dbc=%ld update_hz=%lu\r\n",
                            (unsigned long)dial, (unsigned)k.period_ticks,
                            (unsigned long)MOD_GetActualCarrierFreqHz(),
                            (long)MOD_GetCarrierErrorMilliHz(), (long)MOD_GetCarrierImageDbc(),
                            (unsigned long)MOD_TxPhaseUpdateHz());
        case 2: return emit(buf, cap, "rx lo_hz=%lu if_mhz=%ld quality=%u\r\n",
                            (unsigned long)RADIO_RxClockHz(),
                            (long)(RADIO_RxOffsetHz(dial) * 1000.0f), (unsigned)RADIO_RxClockQuality());
        case 3: return emit(buf, cap, "retune state=%s reason=%u apply_pending=%u upgrade_pending=%u fault=%u "
                            "applied=%lu failed=%lu deferred=%lu rejected=%lu hops=%lu\r\n",
                            k.retune_state < 7U ? states[k.retune_state] : "?", (unsigned)k.last_reason,
                            (unsigned)k.apply_pending, (unsigned)k.upgrade_pending, (unsigned)k.fault_latched,
                            (unsigned long)k.applied, (unsigned long)k.failed,
                            (unsigned long)k.deferred, (unsigned long)k.rejected, (unsigned long)k.tx_hops);
        case 4: return emit(buf, cap, "options park_tx=%u wide=%u phase384=%u qcomb=%u tx_timers=%s\r\n",
                            (unsigned)!!(flags & RADIO_CLOCK_FLAG_PARK_TX), (unsigned)!!(flags & RADIO_CLOCK_FLAG_WIDE),
                            (unsigned)!!(flags & RADIO_CLOCK_FLAG_PHASE384), (unsigned)!!(flags & RADIO_CLOCK_FLAG_QCOMB),
                            k.tx_timers_parked ? "parked" : "running");
        default: return 0U;
    }
}

/* `clock stamp` is the NTP reference measurement's sample: TIM5, a free
 * 32-bit count of SYSCLK, as it stood when this command's USB packet arrived
 * (captured in the OUT interrupt), with the PLL setting and retune count so
 * the host can reject a sample taken across a retune. `clock ppb N` sets the
 * reference correction (REF CAL); `save` keeps it. */
__attribute__((cold)) static void cmd_clock(void)
{
    if (arg_is(1U, "stamp")) {
        mod_clock_snapshot_t k;
        MOD_GetClockSnapshot(&k);
        d.line_len = emit(d.line, DIAG_LINE_MAX, "stamp=%lu pll=%u/%u/%u retunes=%lu ref_ppb=%ld\r\n",
                          (unsigned long)port_rx_stamp(), (unsigned)k.pll_m, (unsigned)k.pll_n,
                          (unsigned)k.pll_r, (unsigned long)(k.applied + k.failed),
                          (long)RADIO_Config()->ref_ppb);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    if (arg_is(1U, "sof")) {
        /* The reference against the USB host's SOF (radio_clocks.c): tracker
         * state (0 idle, 1 acquiring, 2 locked), its error in ppb, what the
         * NCOs follow and add to REF CAL now, groups dropped, groups the step
         * gate coasted, host SOF steps taken as a new origin, acquisitions,
         * the phase share and the gate, and the MCU temperature (milli-C). `clock sof
         * A S` sets those two (radio_clocks.c); `clock usb on|off` switches the following. */
        if (argc >= 4U) RADIO_RefSofTune((uint16_t)arg_int(2U, 0), (uint16_t)arg_int(3U, 0));
        radio_ref_sof_status_t st;
        RADIO_RefSofStatus(&st);
        d.line_len = emit(d.line, DIAG_LINE_MAX, "sof=%u have=%u follow=%u ppb=%ld nco=%ld trim=%ld drop=%lu step=%lu abs=%lu acq=%lu "
                          "a=%u s=%u t=%ld\r\n",
                          (unsigned)st.state, (unsigned)st.have, (unsigned)RADIO_RefFollowEnabled(),
                          (long)st.ppb, (long)st.ppb_nco, (long)RADIO_RefTrimPpb(),
                          (unsigned long)st.dropped, (unsigned long)st.stepped, (unsigned long)st.absorbed,
                          (unsigned long)st.acquisitions, (unsigned)st.alpha_percent, (unsigned)st.step_cycles,
                          (long)st.temp_mc);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    if (arg_is(1U, "adc96")) {
        /* auto (default), on = pin ~96 kHz, off = pin 170-190 kHz; applies at
         * the next retune. Bare: which the running plan uses. */
        if (arg_is(2U, "auto")) RADIO_RxAdcPolicySet(RADIO_RX_ADC_AUTO);
        else if (arg_is(2U, "on")) RADIO_RxAdcPolicySet(RADIO_RX_ADC_96K);
        else if (arg_is(2U, "off")) RADIO_RxAdcPolicySet(RADIO_RX_ADC_HIGH);
        d.line_len = emit(d.line, DIAG_LINE_MAX, "adc96=%u policy=%u rate_hz=%ld\r\n", (unsigned)RADIO_RxAdc96(),
                          (unsigned)RADIO_RxAdcPolicy(), (long)RADIO_AudioRate());
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
#if REF_SOF_BENCH
    if (arg_is(1U, "mark")) {
        RADIO_CaptureMark((uint8_t)(arg_is(2U, "on") || arg_int(2U, 0) == 1));
        reply_done("OK");
        return;
    }
#endif
    if (arg_is(1U, "usb")) {
        RADIO_RefFollowEnable((uint8_t)(arg_is(2U, "on") || arg_int(2U, 0) == 1));
        reply_done("OK");
        return;
    }
    if (arg_is(1U, "ppb")) {
        const int32_t ppb = arg_int(2U, INT32_MIN);
        if (ppb < -RADIO_REF_PPB_LIMIT || ppb > RADIO_REF_PPB_LIMIT) {
            reply_usage(TXT_USAGE_CLOCK);
            return;
        }
        RADIO_SetRefPpb(ppb);
        reply_lines(line_clock, "OK");
        return;
    }
    reply_lines(line_clock, "OK");
}

/* ---- power and SWR meter ---- */

static int32_t x_scaled(float v, float scale) { return (int32_t)(v * scale + (v >= 0.0f ? 0.5f : -0.5f)); }

/* The meter's last reading (radio_rf_meter.h) and the board calibration of
 * the band it was taken on. Integers scaled as named: the console has no
 * float printf. */
__attribute__((cold)) static uint16_t line_meter(uint16_t index, char *buf, uint16_t cap)
{
    const radio_meter_t *m = &rf_meter;
    const rf_meter_reading_t *r = &m->last;
    const radio_config_t *c = RADIO_Config();
    const uint8_t a = RF_MeterAnchor(m->seq ? m->hz : MOD_GetCarrierRequestedHz());
    switch (index) {
        case 0: return emit(buf, cap, "seq=%lu age_ms=%lu hz=%lu scans=%u\r\n", (unsigned long)m->seq,
                            (unsigned long)(m->seq ? HAL_GetTick() - m->tick : 0UL), (unsigned long)m->hz,
                            (unsigned)r->scans);
        case 1: return emit(buf, cap, "pep_w_x100=%ld avg_w_x100=%ld ref_w_x1000=%ld net_w_x100=%ld drain_mv=%ld vin_mv=%ld\r\n",
                            (long)x_scaled(r->pep_w, 100.0f), (long)x_scaled(r->avg_w, 100.0f),
                            (long)x_scaled(r->ref_w, 1000.0f), (long)x_scaled(r->net_w, 100.0f),
                            (long)x_scaled(r->drain_v, 1000.0f), (long)x_scaled(r->vin_v, 1000.0f));
        case 2: return emit(buf, cap, "swr_x100=%ld swr_raw_x100=%ld rho_x10000=%ld rho_raw_x10000=%ld swr_valid=%u\r\n",
                            (long)x_scaled(r->swr, 100.0f), (long)x_scaled(r->swr_raw, 100.0f),
                            (long)x_scaled(r->rho, 10000.0f), (long)x_scaled(r->rho_raw, 10000.0f),
                            (unsigned)r->swr_valid);
        case 5: return emit(buf, cap, "rest_fwd_uv=%ld rest_rev_uv=%ld rest_seen=%lu zero_fwd_uv=%ld zero_rev_uv=%ld\r\n",
                            (long)x_scaled(m->zero_mv[0], 1000.0f), (long)x_scaled(m->zero_mv[1], 1000.0f),
                            (unsigned long)m->zero_seen, (long)x_scaled(c->swr_offset[0], 1000000.0f),
                            (long)x_scaled(c->swr_offset[1], 1000000.0f));
        case 3: return emit(buf, cap, "anchor=%u anchor_hz=%lu gain_x1000=%u leak_x10000=%u floor_100uv=%u tune_pct=%u limit_x100=%u\r\n",
                            (unsigned)a, (unsigned long)rf_meter_anchor_hz[a],
                            (unsigned)(c->meter_gain_x1000[a] ? c->meter_gain_x1000[a] : 1000U),
                            (unsigned)c->meter_leak_x10000[a], (unsigned)c->meter_floor_100uv[a],
                            (unsigned)RADIO_ConfigTunePct(c), (unsigned)c->swr_limit_x100);
        case 4: {
            const uint16_t *t = RF_MeterTable(c->meter_port_mv);
            return emit(buf, cap, "table=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u (%s)\r\n",
                        t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11],
                        (t == c->meter_port_mv) ? "board" : "built-in");
        }
        default: return 0U;
    }
}

/* Per-anchor calibration, one line per HF band anchor; after a change, a
 * reminder that it lives in RAM until `save`. */
static uint8_t metercal_changed;

__attribute__((cold)) static uint16_t line_metercal(uint16_t index, char *buf, uint16_t cap)
{
    const radio_config_t *c = RADIO_Config();
    if (index < 6U) return line_meter(index, buf, cap);
    index = (uint16_t)(index - 6U);
    if (index == RF_METER_ANCHORS && metercal_changed) return copy_line(buf, cap, "changed: `save` keeps it");
    if (index >= RF_METER_ANCHORS) return 0U;
    return emit(buf, cap, "anchor%u hz=%lu gain_x1000=%u leak_x10000=%u floor_100uv=%u\r\n", (unsigned)index,
                (unsigned long)rf_meter_anchor_hz[index], (unsigned)c->meter_gain_x1000[index],
                (unsigned)c->meter_leak_x10000[index], (unsigned)c->meter_floor_100uv[index]);
}

__attribute__((cold)) static void cmd_meter(void) { reply_lines(line_meter, "OK"); }

/* `metercal leak`: the bridge's own reflection on a 50 ohm load for the band
 * of the last reading (a TX TEST carrier into the dummy load, >= 0.2 W, at
 * most 10 s ago). Run it twice on a band, at full drain and at about a third
 * of it (TUNE %): the two captures split the leakage into the part that
 * follows the drive and the floor that does not (RF_MeterLeakFit); once
 * gives the first alone. `metercal ref <W>`: the band's power scale from a reference
 * instrument read on that same carrier (an oscilloscope across the load:
 * P = Vpp^2 / 400). `metercal table a,b,...`: a detector curve fitted on the
 * host (tools/gen_rf_meter_table.py --fit), `table default` the built-in one.
 * `metercal clear [all]`: this band's (or every band's) gain and leakage. */
static struct { float f, rho; uint8_t anchor, have; } metercal_prev;

__attribute__((cold)) static void cmd_metercal(void)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    const radio_meter_t *m = &rf_meter;
    const uint8_t fresh = (uint8_t)(m->seq && (HAL_GetTick() - m->tick) <= 10000UL);
    const uint8_t a = RF_MeterAnchor(m->seq ? m->hz : MOD_GetCarrierRequestedHz());
    metercal_changed = 0U;
    if (argc < 2U || arg_is(1U, "show")) { reply_lines(line_metercal, "OK"); return; }
    if (arg_is(1U, "leak")) {
        if (!fresh || m->last.pep_w < 0.2f || m->last.scans < 20U) {
            reply_done("ERR key a TX TEST carrier of 0.2 W or more into 50 ohm first (10 s)");
            return;
        }
        if (m->last.rho_raw > 0.5f) { reply_done("ERR |gamma| over 0.5: not a 50 ohm load"); return; }
        const float g = c.meter_gain_x1000[a] ? (float)c.meter_gain_x1000[a] * 0.001f : 1.0f;
        const float f = sqrtf(m->last.plateau_w / g);
        float leak, floor_v;
        if (metercal_prev.have && metercal_prev.anchor == a)
            RF_MeterLeakFit(metercal_prev.f, metercal_prev.rho, f, m->last.rho_raw, &leak, &floor_v);
        else
            RF_MeterLeakFit(f, m->last.rho_raw, 0.0f, 0.0f, &leak, &floor_v);
        metercal_prev.f = f;
        metercal_prev.rho = m->last.rho_raw;
        metercal_prev.anchor = a;
        metercal_prev.have = 1U;
        c.meter_leak_x10000[a] = (uint16_t)x_scaled(leak, 10000.0f);
        c.meter_floor_100uv[a] = (uint16_t)x_scaled(floor_v > 0.5f ? 0.5f : floor_v, 10000.0f);
    } else if (arg_is(1U, "ref")) {
        const float w = arg_float(2U, -1.0f);
        if (!(w > 0.0f) || w > 50.0f) { reply_usage(TXT_USAGE_METERCAL); return; }
        if (!fresh || m->last.pep_w < 0.1f) { reply_done("ERR no reading in the last 10 s"); return; }
        const float old = c.meter_gain_x1000[a] ? (float)c.meter_gain_x1000[a] * 0.001f : 1.0f;
        const float g = old * w / m->last.pep_w;
        if (g < 0.25f || g > 4.0f) { reply_done("ERR more than 4x off the bridge's theory; check the reference"); return; }
        c.meter_gain_x1000[a] = (uint16_t)x_scaled(g, 1000.0f);
    } else if (arg_is(1U, "table")) {
        if (arg_is(2U, "default")) {
            memset(c.meter_port_mv, 0, sizeof(c.meter_port_mv));
        } else {
            const char *s = (argc >= 3U) ? argv[2] : "";
            uint32_t n = 0U;
            while (*s && n < RF_METER_POINTS) {
                uint32_t v = 0U;
                if (*s < '0' || *s > '9') break;
                while (*s >= '0' && *s <= '9') v = v * 10U + (uint32_t)(*s++ - '0');
                c.meter_port_mv[n++] = (uint16_t)(v > 65535U ? 65535U : v);
                if (*s == ',') ++s;
            }
            if (n != RF_METER_POINTS || *s || !RF_MeterTableValid(c.meter_port_mv)) {
                reply_done("ERR 12 port mV values, from 0, rising, each >= its detector mV");
                return;
            }
        }
    } else if (arg_is(1U, "clear")) {
        if (arg_is(2U, "all")) {
            memset(c.meter_gain_x1000, 0, sizeof(c.meter_gain_x1000));
            memset(c.meter_leak_x10000, 0, sizeof(c.meter_leak_x10000));
            memset(c.meter_floor_100uv, 0, sizeof(c.meter_floor_100uv));
        } else {
            c.meter_gain_x1000[a] = 0U;
            c.meter_leak_x10000[a] = 0U;
            c.meter_floor_100uv[a] = 0U;
        }
        metercal_prev.have = 0U;
    } else {
        reply_usage(TXT_USAGE_METERCAL);
        return;
    }
    if (!RADIO_SetMeterConfig(&c)) { reply_done("ERR not valid"); return; }
    metercal_changed = 1U;
    reply_lines(line_metercal, "OK");
}

__attribute__((cold)) static uint16_t line_cpu(uint16_t index, char *buf, uint16_t cap)
{
    static const char *const names[MOD_CPU_DIAG_COUNT] = {
        "service_bg", "render_ext", "usb_submit", "dma_pop", "lcd_service",
        "ui_build", "status_full", "storage_save", "retune"
    };
    mod_cpu_diag_snapshot_t cpu;
    MOD_GetCpuDiagSnapshot(&cpu);
    if (!cpu.enabled || !cpu.dwt_available) {
        if (index) return 0U;
        return copy_line(buf, cap, "disabled: enable CPU DIAG in the debug settings");
    }
    if (index >= MOD_CPU_DIAG_COUNT) return 0U;
    const mod_cpu_diag_section_t *s = &cpu.section[index];
    return emit(buf, cap, "%s calls=%lu avg_cyc=%lu max_cyc=%lu\r\n", names[index],
                (unsigned long)s->calls, (unsigned long)s->avg_cycles, (unsigned long)s->max_cycles);
}

/* Directory listing straight from FatFs: no browser state, no name pool, no
 * 192-entry cap -- the host gets the whole directory streamed. */
__attribute__((cold)) static uint16_t line_ls(uint16_t index, char *buf, uint16_t cap)
{
    char name[64];
    uint32_t size = 0UL;
    uint8_t attr = 0U;
    if (!SD_DiagListEntry(d.path, index, name, sizeof(name), &size, &attr)) return 0U;
    return emit(buf, cap, "%s %lu %s\r\n", (attr & 0x10U) ? "dir " : "file",
                (unsigned long)size, name);
}

/* Hex blocks with a per-block CRC32, so a host can pull a file over a stream
 * that has no framing of its own and still know it arrived intact. */
__attribute__((cold)) static uint16_t line_get(uint16_t index, char *buf, uint16_t cap)
{
    uint8_t block[32];
    uint32_t got = 0U;
    const uint32_t offset = d.get_offset + (uint32_t)index * sizeof(block);
    if (d.get_length && offset >= d.get_offset + d.get_length) return 0U;
    uint32_t want = sizeof(block);
    if (d.get_length && offset + want > d.get_offset + d.get_length)
        want = d.get_offset + d.get_length - offset;
    /* A read refused or failed part way (the I/O gate closing, the card) is
     * not the end of the file: the host must not take a short file for it. */
    if (!SD_DiagReadFile(d.path, offset, block, want, &got)) { d.tail = "ERR read failed"; return 0U; }
    if (!got) return 0U;
    uint16_t n = emit(buf, cap, "%lu ", (unsigned long)offset);
    for (uint32_t i = 0U; i < got && n + 2U < cap; ++i)
        n = (uint16_t)(n + (uint16_t)emit(&buf[n], (uint16_t)(cap - n), "%02X", (unsigned)block[i]));
    n = (uint16_t)(n + (uint16_t)emit(&buf[n], (uint16_t)(cap - n), " %08lX\r\n",
                                      (unsigned long)crc32_simple(block, got)));
    return n;
}


/* ---- full-screen view ----
 *
 * Built only in the shipping flavours. The deep-diagnostic build carries its
 * own instrumentation -- the CPU scope, the runtime diagnostics and the extra
 * LCD pages -- and does not fit this as well; a developer running that flavour
 * has a debugger and those pages already. The line protocol, which is what
 * scripts and host tools use, is in every flavour.
 *
 * The QMX terminal applications mode (operating manual section 8) puts the
 * same measurements on a colour screen for a person with a terminal. This is
 * the REV1 equivalent: the numbers are the ones the line protocol serves, laid
 * out in panels and repainted four times a second.
 *
 * Every frame is written with explicit cursor positioning rather than a clear,
 * so the screen does not flicker and a repaint costs about 1.2 kB -- under
 * 5 kB/s, which is nothing next to what the endpoint will carry. */

#if !MOD_DEEP_DIAG_ENABLE
#define ANSI_HOME   "\x1b[H"
#define ANSI_CLEAR  "\x1b[2J\x1b[H"
#define ANSI_RESET  "\x1b[0m"
#define ANSI_TITLE  "\x1b[1;36m"
#define ANSI_LABEL  "\x1b[37m"
#define ANSI_DIM    "\x1b[90m"

#define UI_SCREEN_SELFTEST 4U
#define UI_SCREEN_PADDLE   5U
#define UI_SCREEN_COUNT    6U
/* The last row a screen may use; row 24 parks the cursor. Autowrap is off
 * while the view is up, so a long row is cut at the right margin instead of
 * wrapping into the next one or scrolling the whole screen: the view is laid
 * out for the 80 x 24 window PuTTY and minicom open with. */
#define UI_LAST_ROW        23U
#define ANSI_ENTER  "\x1b[?7l\x1b[?25l"   /* autowrap off, cursor hidden */
#define ANSI_LEAVE  "\x1b[?7h\x1b[?25h"
/* kitty keyboard protocol: push flags 1|2|8 (disambiguate, report press,
 * repeat and release, every key as an escape code), ask which flags the
 * terminal took, and turn on focus reports. Terminals without it ignore all
 * three. */
#define PADDLE_MODES_ON  "\x1b[>11u\x1b[?u\x1b[?1004h"
#define PADDLE_MODES_OFF "\x1b[<u\x1b[?1004l"

__attribute__((cold)) static const char *verdict_colour(uint8_t verdict)
{
    switch (verdict) {
        case ST_PASS: return "\x1b[32m";
        case ST_WARN: return "\x1b[33m";
        case ST_FAIL: return "\x1b[1;31m";
        default:      return ANSI_DIM;
    }
}

/* Green inside the window, red outside: the colour coding QMX uses. */
__attribute__((cold)) static const char *range_colour(int32_t value, int32_t low, int32_t high)
{
    return (value >= low && value <= high) ? "\x1b[32m" : "\x1b[1;31m";
}

__attribute__((cold)) static uint16_t ui_row(char *buf, uint16_t cap, uint8_t row, const char *text)
{
    return emit(buf, cap, "\x1b[%u;1H\x1b[K%s", (unsigned)row, text);
}

__attribute__((cold)) static uint16_t ui_overview(uint16_t i, char *buf, uint16_t cap)
{
    radio_telemetry_t tel;
    RADIO_GetTelemetry(&tel);
    const radio_config_t *c = RADIO_Config();
    const uint8_t row = (uint8_t)(i + 4U);
    switch (i) {
        case 0: return emit(buf, cap, "\x1b[%u;1H\x1b[K" ANSI_LABEL "  SUPPLY" ANSI_RESET, (unsigned)row);
        case 1: return emit(buf, cap, "\x1b[%u;1H\x1b[K    VIN         %s%5u mV" ANSI_RESET
                            "        5V RAIL   %s%5u mV" ANSI_RESET, (unsigned)row,
                            range_colour(tel.vin_mv, 6000, c->vin_limit_mv), (unsigned)tel.vin_mv,
                            range_colour(tel.rail5_mv, 4400, 5500), (unsigned)tel.rail5_mv);
        case 2: return emit(buf, cap, "\x1b[%u;1H\x1b[K    VREF        %5u mV        FAULTS    %s%02lX" ANSI_RESET,
                            (unsigned)row, (unsigned)MOD_GetVrefMillivolts(),
                            tel.faults ? "\x1b[1;31m" : "\x1b[32m", (unsigned long)tel.faults);
        case 3: return emit(buf, cap, "\x1b[%u;1H\x1b[K" ANSI_LABEL "  BIAS SMPS" ANSI_RESET, (unsigned)row);
        case 4: return emit(buf, cap, "\x1b[%u;1H\x1b[K    TX_BIAS     %5u mV   %3ld mA   duty %3u%%",
                            (unsigned)row, (unsigned)tel.bias_mv,
                            (long)RADIO_BiasCurrentMa(tel.bias_mv, tel.rail5_mv),
                            (unsigned)((tel.bias_duty * 100U + 512U) / 1024U));
        case 5: return emit(buf, cap, "\x1b[%u;1H\x1b[K    TARGET      %5u mV   %3u mA (PIN BIAS MA)",
                            (unsigned)row, (unsigned)RADIO_BiasTargetMv(c->bias_ma, tel.rail5_mv),
                            (unsigned)c->bias_ma);
        case 6: return emit(buf, cap, "\x1b[%u;1H\x1b[K" ANSI_LABEL "  PA ENVELOPE" ANSI_RESET, (unsigned)row);
        case 7: return emit(buf, cap, "\x1b[%u;1H\x1b[K    ENV A       %5u mV        ENV B     %5u mV",
                            (unsigned)row, (unsigned)tel.env_a_mv, (unsigned)tel.env_b_mv);
        case 8: return emit(buf, cap, "\x1b[%u;1H\x1b[K    FORWARD     %5u mV        REFLECTED %5u mV",
                            (unsigned)row, (unsigned)tel.fwd_mv, (unsigned)tel.rev_mv);
        case 9: return emit(buf, cap, "\x1b[%u;1H\x1b[K" ANSI_LABEL "  TRANSMITTER" ANSI_RESET, (unsigned)row);
        case 10: {
            const char *why = RADIO_ConfigTxBlock(c, MOD_GetCarrierRequestedHz());
            return emit(buf, cap, "\x1b[%u;1H\x1b[K    %s%s" ANSI_RESET, (unsigned)row,
                        why ? "\x1b[1;31mBLOCKED: " : "\x1b[32mREADY", why ? why : "");
        }
        default: return 0U;
    }
}

__attribute__((cold)) static uint16_t ui_controls(uint16_t i, char *buf, uint16_t cap)
{
    static const char *const names[5] = {"UP      ", "DOWN    ", "SELECT  ", "SW4     ", "SW5     "};
    const uint8_t row = (uint8_t)(i + 4U);
    if (i < 5U) {
        const uint8_t on = BOARD_ButtonPressed((uint8_t)i);
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    %s  %s%s" ANSI_RESET, (unsigned)row, names[i],
                    on ? "\x1b[1;32m" : ANSI_DIM, on ? "PRESSED " : "released");
    }
    if (i == 5U) {
        uint8_t high = 0U;
        const uint8_t locked = BOARD_ButtonService(HAL_GetTick(), &high);
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    POLARITY  %s%s" ANSI_RESET, (unsigned)row,
                    locked ? "\x1b[32m" : "\x1b[33m",
                    !locked ? "learning" : (high ? "active high (R92)" : "active low (R93)"));
    }
    if (i == 6U) {
        int8_t steps = 0;
        uint8_t click = 0U, hold = 0U;
        const uint8_t fitted = BOARD_EncoderRead(&steps, &click, &hold);
        if (steps > 0) d.ui_enc_cw = 1U;
        if (steps < 0) d.ui_enc_ccw = 1U;
        if (click) d.ui_enc_click = 1U;
        if (hold) d.ui_enc_hold = 1U;
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    ENCODER   %s", (unsigned)row,
                    fitted ? "fitted" : ANSI_DIM "not fitted" ANSI_RESET);
    }
    if (i == 7U)
        return emit(buf, cap, "\x1b[%u;1H\x1b[K      CW %s>>>" ANSI_RESET "   CCW %s<<<" ANSI_RESET
                    "   CLICK %s*" ANSI_RESET "   HOLD %s*" ANSI_RESET, (unsigned)row,
                    d.ui_enc_cw ? "\x1b[32m" : "\x1b[1;31m", d.ui_enc_ccw ? "\x1b[32m" : "\x1b[1;31m",
                    d.ui_enc_click ? "\x1b[32m" : "\x1b[1;31m", d.ui_enc_hold ? "\x1b[32m" : "\x1b[1;31m");
    if (i == 8U)
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    PTT       %s", (unsigned)row,
                    RADIO_Ptt() ? "\x1b[1;31mKEYED" ANSI_RESET : ANSI_DIM "open" ANSI_RESET);
    if (i == 9U)
        return ui_row(buf, cap, row, ANSI_DIM "    work every control; rows turn green as they are seen" ANSI_RESET);
    return 0U;
}

__attribute__((cold)) static uint16_t ui_filters(uint16_t i, char *buf, uint16_t cap)
{
    const radio_config_t *c = RADIO_Config();
    const uint8_t row = (uint8_t)(i + 4U);
    if (i < 5U) {
        const selftest_result_t *r = MOD_SelfTestResult((uint8_t)(ST_LPF0 + i));
        const uint32_t edge = c->lpf_upper_hz[i];
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    LPF %u  edge %2lu.%02lu MHz  TX min %2u.%02u  %s%-4s" ANSI_RESET
                    "  %s%s" ANSI_RESET, (unsigned)row, (unsigned)i,
                    (unsigned long)(edge / 1000000UL), (unsigned long)((edge % 1000000UL) / 10000UL),
                    (unsigned)(c->lpf_tx_min_khz[i] / 1000U), (unsigned)((c->lpf_tx_min_khz[i] % 1000U) / 10U),
                    verdict_colour(r ? r->verdict : ST_PENDING),
                    MOD_SelfTestVerdictName(r ? r->verdict : ST_PENDING),
                    (c->qualified_bands & (1UL << i)) ? "\x1b[32m" : ANSI_DIM,
                    (c->qualified_bands & (1UL << i)) ? "qualified" : "not qualified for TX");
    }
    if (i == 5U) return ui_row(buf, cap, row, "");
    if (i < 10U) {
        const unsigned s = i - 6U;
        const uint32_t edge = c->bpf_upper_hz[s];
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    BPF %u  edge %2lu.%02lu MHz", (unsigned)row,
                    s, (unsigned long)(edge / 1000000UL), (unsigned long)((edge % 1000000UL) / 10000UL));
    }
    if (i == 10U) {
        uint8_t lpf = 255U, bpf = 255U;
        (void)RADIO_ConfigFilters(c, MOD_GetCarrierRequestedHz(), &lpf, &bpf);
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    selected for %lu Hz: LPF %d  BPF %d  (PE2..PE6 = %02lX)",
                    (unsigned)row, (unsigned long)MOD_GetCarrierRequestedHz(),
                    lpf < 5U ? (int)lpf : -1, bpf < 4U ? (int)bpf : -1,
                    (unsigned long)RADIO_SelectedLpfBits());
    }
    return 0U;
}

__attribute__((cold)) static uint16_t ui_receive(uint16_t i, char *buf, uint16_t cap)
{
    const uint8_t row = (uint8_t)(i + 4U);
    switch (i) {
        case 0: return emit(buf, cap, "\x1b[%u;1H\x1b[K    STATE       %s", (unsigned)row, RADIO_Status());
        case 1: return emit(buf, cap, "\x1b[%u;1H\x1b[K    CARRIER     %lu Hz", (unsigned)row,
                            (unsigned long)MOD_GetCarrierRequestedHz());
        case 2: return emit(buf, cap, "\x1b[%u;1H\x1b[K    PCM1821     %s%ld Hz" ANSI_RESET "   fallbacks %lu",
                            (unsigned)row, range_colour((int32_t)RADIO_AudioRate(), 171000, 178000),
                            (long)RADIO_AudioRate(), (unsigned long)RADIO_AudioI2sFallbacks());
        case 3: return emit(buf, cap, "\x1b[%u;1H\x1b[K    OVERRUNS    %lu", (unsigned)row,
                            (unsigned long)RADIO_AudioOverruns());
        case 4: {
            /* S-meter: one block per 3 dB from -120 dBFS. */
            char bar[42];
            const int32_t ddb = (int32_t)(RADIO_AudioRxLevelDbfs() * 10.0f);
            int32_t blocks = (ddb + 1200) / 30;
            if (blocks < 0) blocks = 0;
            if (blocks > 40) blocks = 40;
            for (int32_t k = 0; k < 40; ++k) bar[k] = k < blocks ? '#' : '.';
            bar[40] = '\0';
            return emit(buf, cap, "\x1b[%u;1H\x1b[K    LEVEL       %s  %ld.%u dBFS", (unsigned)row,
                        bar, (long)(ddb / 10), (unsigned)((ddb < 0 ? -ddb : ddb) % 10));
        }
        default: return 0U;
    }
}

__attribute__((cold)) static uint16_t ui_selftest(uint16_t i, char *buf, uint16_t cap)
{
    const uint8_t row = (uint8_t)(i + 4U);
    if (!i) {
        char status[48];
        MOD_SelfTestStatus(status, sizeof(status));
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    %s%s" ANSI_RESET "  %u%%  %s", (unsigned)row,
                    verdict_colour(MOD_SelfTestOverall()),
                    MOD_SelfTestVerdictName(MOD_SelfTestOverall()),
                    (unsigned)MOD_SelfTestProgress(), status);
    }
    /* Only what is worth a person's attention: anything not a clean pass. */
    uint16_t seen = 0U;
    for (uint8_t id = 0U; id < SELFTEST_COUNT; ++id) {
        const selftest_result_t *r = MOD_SelfTestResult(id);
        if (!r || r->verdict == ST_PENDING || r->verdict == ST_PASS) continue;
        if (++seen != i) continue;
        return emit(buf, cap, "\x1b[%u;1H\x1b[K    %s%-4s" ANSI_RESET " %-20s %6ld %-5s %s",
                    (unsigned)row, verdict_colour(r->verdict), MOD_SelfTestVerdictName(r->verdict),
                    MOD_SelfTestName(id), (long)r->value, MOD_SelfTestUnit(id),
                    r->detail ? r->detail : ""); /* redrawn live: no card reads (`selftest` has them) */
    }
    if (i == seen + 1U)
        return ui_row(buf, cap, row, ANSI_DIM "    r runs the safe groups, a runs them with the panel" ANSI_RESET);
    return 0U;
}

/* ---- virtual paddle page ----
 *
 * The keyboard as a paddle: , is DIT and . is DAH, the two keys side by side
 * on the bottom row of both German and US layouts. The levers go into the
 * keyer at the same point as the KEY jack (MOD_CwSetVirtualPaddle), so keyer
 * mode, speed, weight, paddle protection, sidetone and break-in all apply.
 * The jack stays live alongside.
 *
 * Opening the page asks the terminal for the kitty keyboard protocol with
 * key release events (kitty, foot, WezTerm, ghostty, alacritty 0.13+). A
 * terminal that takes it gives true press and release, so levers can be held
 * and squeezed; one that ignores it (PuTTY, xterm, minicom) sends a character
 * per press, and the page keys one element per press instead
 * (usb_diag_keys.h).
 *
 * The console entered with TX muted; the page keeps it that way until t is
 * pressed, and mutes it again on the way out, however that happens. Levers
 * are dropped on focus loss, on leaving the page, when the port closes or
 * the console goes away, and after a 10 s hold. */

__attribute__((cold)) static uint8_t paddle_page(void)
{
    return (uint8_t)(d.ui_mode && d.ui_screen == UI_SCREEN_PADDLE);
}

__attribute__((cold)) static void paddle_enter(void)
{
    VPADDLE_Reset(&d.paddle);
    d.kitty_flags = 0U;
    d.releases_seen = 0U;
    d.modes_due = 1U;
    d.modes_off_due = 0U;
}

__attribute__((cold)) static void paddle_leave(void)
{
    VPADDLE_Reset(&d.paddle);
    d.levers = 0U;
    MOD_CwSetVirtualPaddle(0U);
    if (d.paddle_tx) {
        d.paddle_tx = 0U;
        MOD_SetMuted(1U);
    }
    d.modes_due = 0U;
    if (d.modes_on) {
        d.modes_on = 0U;
        d.modes_off_due = 1U;
    }
    d.held_count = 0U;
}

__attribute__((cold)) static void paddle_publish(uint32_t now)
{
    const float wpm = MOD_GetCwWpm();
    vpaddle_keyer_t k;
    k.elements = MOD_CwElementsStarted();
    k.straight = (uint8_t)(MOD_GetCwKeyerMode() == CW_KEYER_STRAIGHT);
    k.dit_ms = (uint16_t)((wpm >= 1.0f && wpm <= 100.0f) ? 1200.0f / wpm : 1200.0f);
    d.levers = VPADDLE_Service(&d.paddle, now, &k);
    MOD_CwSetVirtualPaddle(d.levers);
}

__attribute__((cold)) static uint8_t paddle_hold_mode(void)
{
    return (uint8_t)(d.releases_seen || (d.kitty_flags & 0x02U));
}

__attribute__((cold)) static uint8_t paddle_lever(const term_event_t *ev)
{
    const uint32_t c = ev->kind == TERM_EV_CHAR ? ev->ch : ev->kind == TERM_EV_KEY ? ev->code : 0U;
    return c == ',' ? VPADDLE_DIT : c == '.' ? VPADDLE_DAH : 0U;
}

__attribute__((cold)) static const char *lever_colour(uint8_t down)
{
    return down ? "\x1b[1;30;42m" : "\x1b[7m";
}

/* noinline: every screen producer is called once and would otherwise fold
 * into line_ui, past the complexity budget. */
__attribute__((cold, noinline)) static uint16_t ui_paddle(uint16_t i, char *buf, uint16_t cap)
{
    static const char *const mode_names[MOD_TYPE_COUNT] = {"SSB", "FM", "AM", "CW", "TEST"};
    const uint8_t row = (uint8_t)(i + 4U);
    switch (i) {
        case 0: return ui_row(buf, cap, row, "    , is DIT     . is DAH     t puts TX on or off");
        case 1: return ui_row(buf, cap, row, "");
        case 2: return emit(buf, cap, "\x1b[%u;1H\x1b[K              %s   DIT   " ANSI_RESET
                            "      %s   DAH   " ANSI_RESET, (unsigned)row,
                            lever_colour(d.levers & VPADDLE_DIT), lever_colour(d.levers & VPADDLE_DAH));
        case 3: return ui_row(buf, cap, row, "");
        case 4: {
            const uint8_t dit = (uint8_t)((CW_DIT_GPIO_Port->IDR & CW_DIT_Pin) == 0U);
            const uint8_t dah = (uint8_t)((CW_DAH_GPIO_Port->IDR & CW_DAH_Pin) == 0U);
            return emit(buf, cap, "\x1b[%u;1H\x1b[K    KEY JACK  DIT %s" ANSI_RESET "   DAH %s" ANSI_RESET
                        "   (keys the radio too)", (unsigned)row,
                        dit ? "\x1b[1;31mclosed" : ANSI_DIM "open  ", dah ? "\x1b[1;31mclosed" : ANSI_DIM "open  ");
        }
        case 5: {
            const float wpm = MOD_GetCwWpm();
            return emit(buf, cap, "\x1b[%u;1H\x1b[K    KEYER     %s  %u WPM", (unsigned)row,
                        cw_keyer_label(MOD_GetCwKeyerMode()), (unsigned)(wpm > 0.0f ? wpm + 0.5f : 0.0f));
        }
        case 6: {
            modulator_status_fast_t st;
            MOD_GetStatusFast(&st);
            const uint8_t cw = (uint8_t)(st.mod_type == MOD_TYPE_CW);
            return emit(buf, cap, "\x1b[%u;1H\x1b[K    MODE      %s%s" ANSI_RESET "%s", (unsigned)row,
                        cw ? "\x1b[32m" : "\x1b[1;31m",
                        (unsigned)st.mod_type < MOD_TYPE_COUNT ? mode_names[st.mod_type] : "?",
                        cw ? "" : "   the keyer runs only in CW");
        }
        case 7: {
            /* Someone muted TX from the panel: the page no longer owns it. */
            if (d.paddle_tx && MOD_GetMuted()) d.paddle_tx = 0U;
            if (!d.paddle_tx)
                return ui_row(buf, cap, row, "    TX        \x1b[33mPRACTICE" ANSI_RESET
                              "   sidetone only; t puts the radio on the air");
            const char *why = RADIO_ConfigTxBlock(RADIO_Config(), MOD_GetCarrierRequestedHz());
            if (MOD_GetCwPractice())
                return ui_row(buf, cap, row, "    TX        \x1b[33mCW PRACTICE is on in the CW menu" ANSI_RESET
                              ": sidetone only");
            if (why)
                return emit(buf, cap, "\x1b[%u;1H\x1b[K    TX        \x1b[1;31mARMED, BLOCKED: %s" ANSI_RESET,
                            (unsigned)row, why);
            return ui_row(buf, cap, row, "    TX        \x1b[1;37;41m ON THE AIR " ANSI_RESET
                          "   t mutes it again");
        }
        case 8:
            if (paddle_hold_mode())
                return ui_row(buf, cap, row, "    KEYBOARD  \x1b[32mpress and release" ANSI_RESET
                              ": hold and squeeze as on a paddle");
            return ui_row(buf, cap, row, "    KEYBOARD  \x1b[33mtaps" ANSI_RESET
                          ": no key releases here, so one element per press");
        case 9: return ui_row(buf, cap, row, "");
        case 10: return ui_row(buf, cap, row, ANSI_DIM "    the screen pauses while RF is keyed;"
                               " q leaves and mutes TX" ANSI_RESET);
        default: return 0U;
    }
}

/* Row i of the current screen, below the three header rows. */
__attribute__((cold)) static uint16_t ui_screen_row(uint16_t i, char *buf, uint16_t cap)
{
    switch (d.ui_screen) {
        case 0U: return ui_overview(i, buf, cap);
        case 1U: return ui_controls(i, buf, cap);
        case 2U: return ui_filters(i, buf, cap);
        case 3U: return ui_receive(i, buf, cap);
        case UI_SCREEN_SELFTEST: return ui_selftest(i, buf, cap);
        default: return ui_paddle(i, buf, cap);
    }
}

__attribute__((cold)) static uint16_t line_ui(uint16_t index, char *buf, uint16_t cap)
{
    static const char *const titles[UI_SCREEN_COUNT] = {
        "OVERVIEW", "CONTROLS", "FILTERS", "RECEIVE", "SELF TEST", "VIRTUAL PADDLE"
    };
    if (!index) {
        /* Terminal modes first, then the clear or home. */
        const char *off = d.modes_off_due ? PADDLE_MODES_OFF : "";
        const char *on = (d.modes_due && paddle_page()) ? PADDLE_MODES_ON : "";
        d.modes_off_due = 0U;
        if (*on) { d.modes_due = 0U; d.modes_on = 1U; }
        return emit(buf, cap, "%s%s%s", off, on, d.ui_repaint ? ANSI_ENTER ANSI_CLEAR : ANSI_HOME);
    }
    if (index == 1U)
        return emit(buf, cap, "\x1b[1;1H\x1b[K" ANSI_TITLE " " BOARD_NAME "  " FW_VERSION_STRING "  %s " ANSI_RESET,
                    RADIO_Status());
    if (index == 2U) {
        d.ui_repaint = 0U;
        return emit(buf, cap, "\x1b[2;1H\x1b[K " ANSI_DIM "1 overview  2 controls  3 filters  "
                    "4 receive  5 self test  6 paddle  q quit" ANSI_RESET);
    }
    if (index == 3U) {
        d.ui_parked = 0U;
        return emit(buf, cap, "\x1b[3;1H\x1b[K " ANSI_TITLE "%s" ANSI_RESET, titles[d.ui_screen]);
    }
    const uint16_t i = (uint16_t)(index - 4U);
    const uint16_t n = (i + 4U <= UI_LAST_ROW) ? ui_screen_row(i, buf, cap) : 0U;
    if (n) return n;
    /* The screen is done: one trailing line parks the cursor out of the way,
     * then the producer ends and the frame is complete. */
    if (!d.ui_parked) { d.ui_parked = 1U; return emit(buf, cap, "\x1b[24;1H"); }
    return 0U;
}

/* Leaving the view: undo every terminal mode it set, clear, and say where the
 * operator is now. */
__attribute__((cold)) static uint16_t line_ui_quit(uint16_t index, char *buf, uint16_t cap)
{
    if (index) return 0U;
    const char *off = d.modes_off_due ? PADDLE_MODES_OFF : "";
    d.modes_off_due = 0U;
    return emit(buf, cap, ANSI_RESET "%s" ANSI_LEAVE ANSI_CLEAR "console\r\n", off);
}

__attribute__((cold)) static void ui_quit(void)
{
    paddle_leave();
    d.ui_mode = 0U;
    reply_lines(line_ui_quit, "OK");
    if (d.ed.interactive) d.prompt_owed = 1U;
}

/* Keystrokes, not lines: the full-screen view reacts to single characters. */
__attribute__((cold)) static void ui_key(uint8_t ch)
{
    if (ch >= '1' && ch < (uint8_t)('1' + UI_SCREEN_COUNT)) {
        const uint8_t screen = (uint8_t)(ch - '1');
        if (screen != d.ui_screen) {
            if (d.ui_screen == UI_SCREEN_PADDLE) paddle_leave();
            if (screen == UI_SCREEN_PADDLE) paddle_enter();
        }
        d.ui_screen = screen;
        d.ui_repaint = 1U;
        d.ui_enc_cw = d.ui_enc_ccw = d.ui_enc_click = d.ui_enc_hold = 0U;
        return;
    }
    /* Self-test starts only from its own page, where they are explained. */
    if (d.ui_screen == UI_SCREEN_SELFTEST) {
        if (ch == 'r' || ch == 'R') { (void)MOD_SelfTestStart(ST_GROUP_DEFAULT); return; }
        if (ch == 'a' || ch == 'A') { (void)MOD_SelfTestStart((uint8_t)(ST_GROUP_ALL & ~ST_GROUP_ANALOG)); return; }
    }
    if (d.ui_screen == UI_SCREEN_PADDLE && (ch == 't' || ch == 'T')) {
        d.paddle_tx = (uint8_t)!d.paddle_tx;
        MOD_SetMuted((uint8_t)!d.paddle_tx);
        return;
    }
    /* q, Ctrl-C, and QMX's Ctrl-Q. */
    if (ch == 'q' || ch == 'Q' || ch == 0x03U || ch == 0x11U) ui_quit();
}

/* One decoded input event. `realtime` is set when this runs from the main
 * loop's realtime branch, with RF keyed: levers and focus act at once, and
 * every other key waits for the next ordinary pass. */
__attribute__((cold)) static void ui_event(const term_event_t *ev, uint32_t now, uint8_t realtime)
{
    if (paddle_page()) {
        if (ev->kind == TERM_EV_FOCUS_OUT) { VPADDLE_Reset(&d.paddle); return; }
        if (ev->kind == TERM_EV_KITTY_FLAGS) { d.kitty_flags = (uint8_t)(0x80U | (ev->code & 0x7FU)); return; }
        const uint8_t lever = paddle_lever(ev);
        if (lever) {
            if (ev->kind == TERM_EV_KEY) {
                if (ev->action == TERM_KEY_RELEASE) d.releases_seen = 1U;
                if (paddle_hold_mode()) { VPADDLE_Key(&d.paddle, lever, ev->action, now); return; }
                if (ev->action == TERM_KEY_RELEASE) return;
            }
            VPADDLE_Tap(&d.paddle, lever, now);
            return;
        }
    }
    const uint8_t ch = TERM_KeyChar(ev);
    if (!ch) return;
    if (realtime) {
        if (d.held_count < sizeof(d.held_keys)) d.held_keys[d.held_count++] = ch;
        return;
    }
    ui_key(ch);
}
#endif /* !MOD_DEEP_DIAG_ENABLE */



/* Pipeline health. Conditions that do not latch a screen notice still land in
 * these counters, so this is where a benign-but-persistent condition -- an
 * uncollected USB feedback packet, say -- becomes visible. */
__attribute__((cold)) static uint16_t line_pipeline(uint16_t index, char *buf, uint16_t cap)
{
    static const char *const stages[MOD_PIPELINE_STAGE_COUNT] = {
        "none", "usb", "pcm", "render", "dma", "hrtim", "scheduler", "control"
    };
    mod_pipeline_health_t h;
    MOD_GetPipelineHealthSnapshot(&h);
    if (!index)
        return emit(buf, cap, "latched=%u hard_lockout=%u rearm_pending=%u total=%lu soft=%lu\r\n",
                    (unsigned)h.sticky_latched, (unsigned)h.hard_lockout,
                    (unsigned)h.rearm_pending, (unsigned long)h.total_events,
                    (unsigned long)h.soft_fault_events);
    if (index == 1U)
        return emit(buf, cap, "last stage=%u reason=%u severity=%u value=%lu tick=%lu\r\n",
                    (unsigned)h.last_stage, (unsigned)h.last_reason,
                    (unsigned)h.last_severity, (unsigned long)h.last_value,
                    (unsigned long)h.last_tick_ms);
    if (index == 2U)
        return emit(buf, cap, "first stage=%u reason=%u severity=%u value=%lu tick=%lu\r\n",
                    (unsigned)h.first_stage, (unsigned)h.first_reason,
                    (unsigned)h.first_severity, (unsigned long)h.first_value,
                    (unsigned long)h.first_tick_ms);
    {
        const uint16_t i = (uint16_t)(index - 3U);
        if (i < MOD_PIPELINE_STAGE_COUNT) {
            if (!h.stage_events[i]) return emit(buf, cap, "\r\n");
            return emit(buf, cap, "stage.%s=%lu\r\n", stages[i], (unsigned long)h.stage_events[i]);
        }
        const uint16_t r = (uint16_t)(i - MOD_PIPELINE_STAGE_COUNT);
        if (r < MOD_PIPELINE_REASON_COUNT) {
            if (!h.reason_events[r]) return emit(buf, cap, "\r\n");
            return emit(buf, cap, "reason[%u]=%lu\r\n", (unsigned)r, (unsigned long)h.reason_events[r]);
        }
    }
    return 0U;
}


/* "What is happening, and why" in one round trip, decoded. Everything here is
 * already available piecemeal through telem/rx/pipeline/faults; this is the
 * one command worth asking first, so it answers in names rather than numbers
 * and costs one exchange instead of five. */
__attribute__((cold)) static uint16_t line_status(uint16_t index, char *buf, uint16_t cap)
{
    radio_telemetry_t tel;
    mod_pipeline_health_t h;
    const radio_config_t *c = RADIO_Config();
    const uint32_t hz = MOD_GetCarrierRequestedHz();
    switch (index) {
        case 0: {
            const char *rx = RADIO_ConfigRxBlock(c);
            const char *tx = RADIO_ConfigTxBlock(c, hz);
            return emit(buf, cap, "state=%s rx_block=%s tx_block=%s\r\n",
                        RADIO_Status(), rx ? rx : "none", tx ? tx : "none");
        }
        case 1: {
            RADIO_GetTelemetry(&tel);
            uint16_t n = emit(buf, cap, "faults=%02lX", (unsigned long)tel.faults);
            for (uint8_t bit = 0U; bit < 8U; ++bit)
                if (tel.faults & (1UL << bit))
                    n = (uint16_t)(n + emit(&buf[n], (uint16_t)(cap - n), " %s",
                                            MOD_RadioFaultName(bit)));
            return (uint16_t)(n + emit(&buf[n], (uint16_t)(cap - n), "\r\n"));
        }
        case 2: {
            MOD_GetPipelineHealthSnapshot(&h);
            if (!h.sticky_latched) return copy_line(buf, cap, "pipeline=clean");
            return emit(buf, cap, "pipeline=latched %s stage=%s reason=%s value=%08lX age_ms=%lu\r\n",
                        h.hard_lockout ? "LOCKOUT" : "notice",
                        MOD_PipelineStageName(h.last_stage),
                        MOD_PipelineReasonName(h.last_reason),
                        (unsigned long)h.last_value,
                        (unsigned long)(HAL_GetTick() - h.last_tick_ms));
        }
        case 3: {
            uint8_t lpf = 255U, bpf = 255U;
            (void)RADIO_ConfigFilters(c, hz, &lpf, &bpf);
            return emit(buf, cap, "carrier_hz=%lu lpf=%d bpf=%d qualified=%02lX\r\n",
                        (unsigned long)hz, lpf < 5U ? (int)lpf : -1, bpf < 4U ? (int)bpf : -1,
                        (unsigned long)c->qualified_bands);
        }
        case 4: {
            modulator_status_t st;
            MOD_GetStatus(&st);
            return emit(buf, cap, "usb_audio stream=%u packets=%lu missing=%lu\r\n",
                        (unsigned)st.usb_stream_active, (unsigned long)st.usb_packets,
                        (unsigned long)st.usb_missing_frames);
        }
        case 5: {
            USBD_AUDIO_CaptureStatsTypeDef s;
            USBD_AUDIO_GetCaptureStats(&s);
            return emit(buf, cap, "usb_capture sof=%lu submit=%lu complete=%lu incomplete=%lu fail=%lu frames=%lu\r\n",
                        (unsigned long)s.sofs, (unsigned long)s.submits,
                        (unsigned long)s.completions, (unsigned long)s.incomplete,
                        (unsigned long)s.submit_failures, (unsigned long)s.submitted_frames);
        }
        case 6:
            return emit(buf, cap, "rx rate_hz=%ld level_ddbfs=%ld overruns=%lu\r\n",
                        (long)RADIO_AudioRate(), (long)(RADIO_AudioRxLevelDbfs() * 10.0f),
                        (unsigned long)RADIO_AudioOverruns());
        case 7:
            return emit(buf, cap, "selftest=%s progress=%u export=%s\r\n",
                        MOD_SelfTestVerdictName(MOD_SelfTestOverall()),
                        (unsigned)MOD_SelfTestProgress(),
                        MOD_SelfTestExportName() ? MOD_SelfTestExportName() : "-");
        default: return 0U;
    }
}

/* The last few pipeline events, newest first. The order is usually the
 * diagnosis: health keeps only the first and the last. */
__attribute__((cold)) static uint16_t line_events(uint16_t index, char *buf, uint16_t cap)
{
    mod_pipeline_event_t e;
    if (!MOD_GetPipelineEvent((uint8_t)index, &e))
        return index ? 0U : copy_line(buf, cap, "no events recorded");
    return emit(buf, cap, "%u age_ms=%u %s %s %s %08lX\r\n", (unsigned)index,
                (unsigned)e.age_ms, MOD_PipelineSeverityName(e.severity),
                MOD_PipelineStageName(e.stage), MOD_PipelineReasonName(e.reason),
                (unsigned long)e.value);
}

/* ---- dispatch ---- */

typedef void (*diag_cmd_fn)(void);
/* A command's one-line help: in flash (`help`) for what the console needs
 * with no card -- the store, the card, the firmware -- and on the card's
 * text pack (`text`, HELP.BIN) for the rest. */
typedef struct { const char *name; const char *help; card_text_t text; diag_cmd_fn run; } diag_cmd_t;

__attribute__((cold)) static void cmd_help(void); /* after the command table */
__attribute__((cold)) static void cmd_id(void)    { reply_lines(line_id, "OK"); }
__attribute__((cold)) static void cmd_telem(void) { reply_lines(line_telem, "OK"); }
__attribute__((cold)) static void cmd_pins(void)  { reply_lines(line_pins, "OK"); }
__attribute__((cold)) static void cmd_keys(void)  { reply_lines(line_keys, "OK"); }
__attribute__((cold)) static void cmd_rx(void)    { reply_lines(line_rx, "OK"); }
__attribute__((cold)) static void cmd_digi(void)  { reply_lines(line_digi, "OK"); }

/* Receive CW decoder: its settings, what it has locked onto, and the text. */
__attribute__((cold)) static uint16_t line_cwdec(uint16_t index, char *buf, uint16_t cap)
{
    uint8_t wpm = 0U, locked = 0U;
    float offset = 0.0f, snr = 0.0f;
    RADIO_CwDecodeStatus(&wpm, &offset, &snr, &locked);
    const radio_config_t *c = RADIO_Config();
    switch (index) {
        case 0: return emit(buf, cap, "active=%u decode=%u pitch_hz=%u bw_hz=%u agc=%s\r\n",
                            (unsigned)RADIO_CwDecodeActive(), (unsigned)RADIO_ConfigCwDecode(c),
                            (unsigned)RADIO_ConfigCwPitchHz(c), (unsigned)RADIO_ConfigCwBandwidthHz(c),
                            RADIO_ConfigCwAgc(c) == RADIO_CW_AGC_FAST ? "FAST" : "SLOW");
        case 1: return emit(buf, cap, "locked=%u wpm=%u offset_dhz=%ld snr_ddb=%ld dropped=%lu\r\n",
                            (unsigned)locked, (unsigned)wpm, (long)(offset * 10.0f), (long)(snr * 10.0f),
                            (unsigned long)RADIO_CwDecodeDropped());
        case 2: return emit(buf, cap, "text=%s\r\n", RADIO_CwDecodeText());
        default: return 0U;
    }
}
__attribute__((cold)) static void cmd_cwdec(void) { reply_lines(line_cwdec, "OK"); }
/* The peripheral itself, for a contract that never comes. */
__attribute__((cold)) static uint16_t line_pd_raw(uint16_t index, char *buf, uint16_t cap)
{
    static const char *const vstate[4] = {"open/vRa", "vRd-USB", "vRd-1.5", "vRd-3.0"};
    radio_pd_raw_t r;
    RADIO_PdRawSnapshot(&r);
    if (index == 0U)
        return emit(buf, cap, "ucpden=%u ccenable=%u anamode=%s phyrxen=%u tcdis=%u%u active_cc=%u rd=%s\r\n",
                    (unsigned)r.enabled, (unsigned)r.cc_enable, r.anamode ? "sink" : "source",
                    (unsigned)r.phy_rx_enabled, (unsigned)r.tcdis1, (unsigned)r.tcdis2,
                    (unsigned)r.active_cc, r.internal_rd ? "internal" : "external R3/R4");
    if (index == 1U)
        return emit(buf, cap, "cc1=%u (%s) cc2=%u (%s)\r\n",
                    (unsigned)r.cc1_state, vstate[r.cc1_state & 3U],
                    (unsigned)r.cc2_state, vstate[r.cc2_state & 3U]);
    /* The boot sense: the same detector read with the analog PHY off and again
     * with it on. A source that shows up only in the second pair is the whole
     * REV1 negotiation failure. Two lines, because one would exceed
     * DIAG_LINE_MAX and lose its terminator, which hangs the host reader. */
    if (index == 4U)
        return emit(buf, cap, "sense_off=%u%u sense_on=%u%u probe=%s forced=%u\r\n",
                    (unsigned)(r.cc_off_state & 3U), (unsigned)((r.cc_off_state >> 2U) & 3U),
                    (unsigned)(r.cc_on_state & 3U), (unsigned)((r.cc_on_state >> 2U) & 3U),
                    RADIO_PdProbeName(), (unsigned)r.cc_forced);
    if (index == 5U) {
        const uint8_t off_seen = (uint8_t)(r.cc_off_state != 0U);
        const uint8_t on_seen = (uint8_t)(r.cc_on_state != 0U);
        /* This says what the sense saw, not what the port should do: the
         * production path never holds the internal Rd, so "needs RD" here
         * named a design that was abandoned and read as an instruction on a
         * board that had already negotiated. */
        return emit(buf, cap, "   %s\r\n",
                    (!off_seen && on_seen) ? "source seen only with the PHY on"
                    : (!off_seen && !on_seen) ? "no source on CC"
                                              : "source seen with the PHY off");
    }
    if (index == 2U)
        return emit(buf, cap, "cr=%08lX cfg1=%08lX sr=%08lX imr=%08lX dbdis=%u\r\n",
                    (unsigned long)r.cr, (unsigned long)r.cfg1, (unsigned long)r.sr,
                    (unsigned long)r.imr, (unsigned)r.dead_battery_disabled);
    if (index == 3U)
        return emit(buf, cap, "init=%u failed=%u arena_used=%u/%u\r\n",
                    (unsigned)r.initialized, (unsigned)r.failed,
                    (unsigned)r.arena_used, (unsigned)r.arena_size);
    return 0U;
}

/* The PD event log, oldest first so it reads as a timeline. */
__attribute__((cold)) static uint16_t line_pd_log(uint16_t index, char *buf, uint16_t cap)
{
    const uint16_t n = RADIO_PdLogCount();
    radio_pd_log_t e;
    if (index >= n || !RADIO_PdLogGet((uint16_t)(n - 1U - index), &e)) return 0U;
    return emit(buf, cap, "%lu.%05u %u %u %04X %u\r\n", (unsigned long)e.ms, (unsigned)e.us,
                (unsigned)e.kind, (unsigned)e.a, (unsigned)e.b, (unsigned)e.c);
}

__attribute__((cold)) static void cmd_pd(void)
{
    if (arg_is(1U, "raw")) { reply_lines(line_pd_raw, "OK"); return; }
    if (arg_is(1U, "log")) { reply_lines(line_pd_log, "OK"); return; }
    if (arg_is(1U, "fix")) {
        /* Manual fallback if no source was present during boot sensing. */
        reply_done(RADIO_PdProbeStart() ? "OK PD ARMED" : "ERR no probe");
        return;
    }
    reply_lines(line_pd, "OK");
}

/* cat: the audio personality's USB serial ports (cat.c). */
__attribute__((cold)) static void cmd_cat(void)
{
    uint8_t open = 0U, console = 0U, ptt = 0U;
    uint32_t commands = 0U;
    CAT_Status(&open, &console, &commands, &ptt);
    /* digi_tone: the DIGI TONE carrier's offset, mHz (0 when not rendering). */
    const uint32_t tone_mhz = (uint32_t)(MOD_DigiToneHz() * 1000.0f + 0.5f);
    d.line_len = emit(d.line, DIAG_LINE_MAX,
                      "open=0x%02X console=%d commands=%lu cat_tx=%u usb_vox=%u digi_tone=%u tone_mhz=%lu\r\n",
                      (unsigned)open, (console == 0xFFU) ? -1 : (int)console, (unsigned long)commands,
                      (unsigned)ptt, (unsigned)MOD_GetUsbVox(), (unsigned)MOD_GetDigiTone(),
                      (unsigned long)tone_mhz);
    d.line_sent = 0U;
    reply_done("OK");
}

__attribute__((cold)) static void cmd_faults(void)
{
    if (arg_is(1U, "clear")) {
        RADIO_ClearFault();
        reply_done("OK");
        return;
    }
    reply_lines(line_telem, "OK");
}

__attribute__((cold)) static void cmd_selftest(void)
{
    if (argc < 2U || arg_is(1U, "results") || arg_is(1U, "list")) { reply_lines(line_selftest, "OK"); return; }
    if (arg_is(1U, "status")) {
        char status[48];
        MOD_SelfTestStatus(status, sizeof(status));
        const char *csv = MOD_SelfTestExportName();
        (void)snprintf(d.line, sizeof(d.line),
                       "active=%u progress=%u overall=%s elapsed_ms=%lu export=%s %s\r\n",
                       (unsigned)MOD_SelfTestActive(), (unsigned)MOD_SelfTestProgress(),
                       MOD_SelfTestVerdictName(MOD_SelfTestOverall()),
                       (unsigned long)MOD_SelfTestElapsedMs(), csv ? csv : "-", status);
        d.line_len = (uint16_t)strlen(d.line);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    if (arg_is(1U, "arm")) { MOD_SelfTestArmAnalog(); reply_done("OK"); return; }
    if (arg_is(1U, "abort")) { MOD_SelfTestStop(); reply_done("OK"); return; }
    if (arg_is(1U, "apply")) {
        reply_done(MOD_SelfTestApply((uint8_t)arg_int(2U, 255)) ? "OK" : "ERR nothing to apply");
        return;
    }
    if (arg_is(1U, "run")) {
        uint8_t mask = 0U;
        for (uint8_t i = 2U; i < argc; ++i) mask |= MOD_SelfTestGroupFromName(argv[i]);
        reply_done(MOD_SelfTestStart(mask) ? "OK" : "ERR radio busy or faulted");
        return;
    }
    reply_usage(TXT_USAGE_SELFTEST);
}

__attribute__((cold)) static void cmd_set(void)
{
    if (argc < 2U || arg_is(1U, "list")) { reply_lines(line_settings, "OK"); return; }
    const int32_t id = arg_int(2U, -1);
    const setting_item_t *it = (id >= 0 && id < SET_COUNT) ? UI_GetSetting((setting_id_t)id) : NULL;
    if (!it) { reply_done("ERR unknown setting id"); return; }
    if (arg_is(1U, "get")) {
        char value[24] = "-";
        if (it->get) (void)UI_AppendF(value, it->get(), it->decimals);
        (void)snprintf(d.line, sizeof(d.line), "%s=%s\r\n", UI_SettingName((setting_id_t)id), value);
        d.line_len = (uint16_t)strlen(d.line);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    if (arg_is(1U, "help")) {
        (void)snprintf(d.line, sizeof(d.line), "%s\r\n", UI_SettingHelp((setting_id_t)id));
        d.line_len = (uint16_t)strlen(d.line);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    if (arg_is(1U, "put")) {
        if (!it->set) { reply_done("ERR setting is read-only"); return; }
        /* Assembly changes need an idle transport, the same rule the panel
         * obeys; say so rather than silently dropping the write. */
        if (MOD_AudioRealtimeCritical(HAL_GetTick())) { reply_done("ERR audio is realtime critical"); return; }
        it->set(UI_ClampSettingValue(it, arg_float(3U, 0.0f)));
        char value[24] = "-";
        if (it->get) (void)UI_AppendF(value, it->get(), it->decimals);
        (void)snprintf(d.line, sizeof(d.line), "%s=%s\r\n", UI_SettingName((setting_id_t)id), value);
        d.line_len = (uint16_t)strlen(d.line);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    reply_usage(TXT_USAGE_SET);
}

__attribute__((cold)) static void cmd_save(void) { reply_done(MOD_SaveSettings() ? "OK" : "ERR save failed"); }

/* storage: the settings store on the card (mod_sd_store.c) -- its state,
 * its counters, the event log's, what the last import did, and the
 * backups. With no usable store the radio holds at boot, and `storage ...`
 * is the way on without the panel. */
__attribute__((cold)) static uint16_t line_storage(uint16_t index, char *buf, uint16_t cap)
{
    sd_store_info_t st;
    SD_StoreGetInfo(&st);
    if (index == 0U) {
        return emit(buf, cap, "store=%s status=%s nocard=%u cal_not_loaded=%u inst=%08lX origin=%08lX fw=%lu\r\n",
                    SD_StoreStateName(st.state), MOD_GetStorageStatusLabel(),
                    (unsigned)MOD_StorageRunsWithoutCard(), (unsigned)MOD_CalNotLoaded(),
                    (unsigned long)st.instance, (unsigned long)st.origin, (unsigned long)st.created_fw);
    }
    if (index == 1U) {
        return emit(buf, cap, "reads=%lu writes=%lu verify_fail=%lu bad_copies=%lu repairs=%lu io=%lu degraded=%u\r\n",
                    (unsigned long)st.reads, (unsigned long)st.writes, (unsigned long)st.verify_failures,
                    (unsigned long)st.bad_copies, (unsigned long)st.repairs, (unsigned long)st.io_errors,
                    (unsigned)st.degraded);
    }
    if (index == 2U) {
        uint32_t queued = 0U, lost = 0U, size = 0U;
        uint8_t attr = 0U, nvm = 0U, files = 0U, from = 0U;
        uint16_t cal_flash = 0U, cal_card = 0U, failed = 0U;
        MOD_EventLogCounts(&queued, &lost);
        (void)SD_DiagStat("LOGS/EVENTS.LOG", &size, &attr);
        MOD_StorageImportSummary(&nvm, &files, &from, &cal_flash, &cal_card, &failed);
        return emit(buf, cap, "log_bytes=%lu log_queued=%lu log_lost=%lu import nvm=%u files=%u settings=%u cal=%u/%u failed=%u\r\n",
                    (unsigned long)size, (unsigned long)queued, (unsigned long)lost, (unsigned)nvm,
                    (unsigned)files, (unsigned)from, (unsigned)cal_flash, (unsigned)cal_card, (unsigned)failed);
    }
    if (index == 3U) {
        uint8_t pending = 0U, running = 0U;
        uint16_t slot = 0U;
        uint32_t ok = 0U, failed = 0U;
        MOD_StorageBackupStatus(&pending, &running, &slot, &ok, &failed);
        return emit(buf, cap, "headers=%c%c mirrors=%lu backup pending=%u running=%u slot=%u ok=%lu failed=%lu\r\n",
                    (st.headers & 1U) ? 'A' : '-', (st.headers & 2U) ? 'B' : '-', (unsigned long)st.mirrors,
                    (unsigned)pending, (unsigned)running, (unsigned)slot, (unsigned long)ok, (unsigned long)failed);
    }
    return 0U;
}

/* storage list: every slot of the store, one line each -- for a host to
 * check an import or a restore (tools/flash_rev1.py). Each copy's state
 * (A, B: ok:<seq>, empty, bad, other -- an earlier store's -- or io), then
 * the record read: its length, and for a calibration slot its key (band at
 * byte 8, sideband at 10, analytic backend at 11) and its CRC (the record's
 * last word, over its bytes 4..283 with seq 0); the settings slot gives its
 * blob's CRC (byte 20). */
__attribute__((cold)) static void copy_word(char *out, const sd_store_copy_t *c)
{
    static const char *const names[] = {"empty", "ok", "bad", "other", "io"};
    if (c->state == SD_STORE_COPY_OK) (void)emit(out, 16U, "ok:%lu", (unsigned long)c->seq);
    else (void)emit(out, 16U, "%s", names[(c->state <= SD_STORE_COPY_IO) ? c->state : SD_STORE_COPY_IO]);
}

__attribute__((cold)) static uint16_t line_storage_list(uint16_t index, char *buf, uint16_t cap)
{
    uint8_t rec[1024];
    uint32_t len = 0U, seq = 0U, crc = 0U;
    sd_store_copy_t copies[2];
    char a[16], b[16];
    if (index >= SD_STORE_SLOTS) return 0U;
    SD_StoreSlotCopies(index, copies);
    copy_word(a, &copies[0]);
    copy_word(b, &copies[1]);
    const sd_store_rec_t r = SD_StoreRead(index, rec, sizeof(rec), &len, &seq);
    if (r != SD_STORE_REC_OK) {
        return emit(buf, cap, "slot=%u a=%s b=%s %s\r\n", (unsigned)index, a, b,
                    r == SD_STORE_REC_NONE ? "none" : "error");
    }
    if (index == 0U || len < 288U) {
        if (len >= 24U) memcpy(&crc, &rec[20], sizeof(crc));
        return emit(buf, cap, "slot=%u a=%s b=%s seq=%lu len=%lu crc=%08lX\r\n", (unsigned)index, a, b,
                    (unsigned long)seq, (unsigned long)len, (unsigned long)crc);
    }
    memcpy(&crc, &rec[284], sizeof(crc));
    return emit(buf, cap, "slot=%u a=%s b=%s seq=%lu len=%lu band=%u sb=%u backend=%u crc=%08lX\r\n",
                (unsigned)index, a, b, (unsigned long)seq, (unsigned long)len,
                (unsigned)(rec[8] | (rec[9] << 8)), (unsigned)rec[10], (unsigned)rec[11], (unsigned long)crc);
}

__attribute__((cold)) static void cmd_storage(void)
{
    if (argc >= 2U && arg_is(1U, "list")) {
        reply_lines(line_storage_list, "OK");
        return;
    }
    if (argc >= 2U) {
        uint8_t ok = 0U;
        uint16_t restored = 0U;
        if (arg_is(1U, "nocard")) ok = MOD_StorageRunWithoutCard();
        else if (arg_is(1U, "new")) {
            /* This radio's own store is only replaced on purpose. */
            if (SD_StoreState() == SD_STORE_READY && !(argc >= 3U && arg_is(2U, "wipe"))) {
                reply_done("ERR the card holds this radio's store; `storage new wipe` replaces it");
                return;
            }
            ok = MOD_StorageCreate();
        }
        else if (arg_is(1U, "adopt")) ok = MOD_StorageAdopt();
        else if (arg_is(1U, "keep")) ok = MOD_StorageKeep();
        else if (arg_is(1U, "import")) ok = MOD_StorageImport((uint8_t)(argc >= 3U && arg_is(2U, "nvm")));
        else if (arg_is(1U, "backup")) ok = MOD_StorageBackup();
        else if (arg_is(1U, "restore")) ok = MOD_StorageRestore(argc >= 3U ? argv[2] : NULL, &restored);
        else {
            reply_done("ERR usage: storage [list|nocard|new [wipe]|adopt|keep|import [nvm]|backup|restore [path]]");
            return;
        }
        if (!ok) { reply_done("ERR storage operation failed"); return; }
    }
    reply_lines(line_storage, "OK");
}

/* card: the files this image needs on the SD card (the settings help and
 * the CW trainer's pack), as NAME:ID:STATE after
 * `need` -- the ID is the u32 the image expects in bytes 4..7 of the file,
 * the state ok, old (another build's) or missing. tools/flash_rev1.py reads
 * it after every flash and copies what is not ok from the tree. */
__attribute__((cold)) static void cmd_card(void)
{
    static const char *const state[3] = {"missing", "old", "ok"};
    d.line_len = emit(d.line, DIAG_LINE_MAX, "card=%u store=%s import=%u need HELP.BIN:%08lX:%s CWPACK.BIN:%08lX:%s\r\n",
                      (unsigned)SD_IsMounted(), SD_StoreStateName(SD_StoreState()),
                      (unsigned)MOD_StorageImportAvailable(),
                      (unsigned long)UI_HelpCardId(), state[UI_HelpCardState()],
                      (unsigned long)CWPACK_CardId(), state[CWPACK_CardState()]);
    d.line_sent = 0U;
    reply_done("OK");
}

__attribute__((cold)) static void cmd_exit(void) { reply_done("OK"); USB_DiagModeExit(); }

#if MOD_DEEP_DIAG_ENABLE
__attribute__((cold)) static void cmd_ui(void)
{
    reply_done("ERR full-screen view is not built into the diagnostic flavour");
}

__attribute__((cold)) static void cmd_paddle(void) { cmd_ui(); }
#else
__attribute__((cold)) static void ui_open(uint8_t screen)
{
    d.ui_mode = 1U;
    d.ui_repaint = 1U;
    d.ui_screen = screen;
    d.ui_next_frame = HAL_GetTick();
    if (screen == UI_SCREEN_PADDLE) paddle_enter();
    reply_done(NULL);
}

__attribute__((cold)) static void cmd_ui(void) { ui_open(0U); }
/* Straight into the virtual paddle page of the full-screen view. */
__attribute__((cold)) static void cmd_paddle(void) { ui_open(UI_SCREEN_PADDLE); }
#endif

/* knob <steps>: queue encoder detents as the knob does (+ clockwise, 24 at
 * most either way), for bench runs that tune without a hand on the radio;
 * `knob click` presses it. `knob` alone reports the last tune: how long the
 * carrier apply took, and the ticks of the detent, its apply and the
 * receiver following it. */
static uint32_t knob_tick;
__attribute__((cold)) static void cmd_knob(void)
{
    if (arg_is(1U, "click")) { ui_ev.click = 1U; reply_done("OK"); return; }
    if (arg_is(1U, "hold")) { ui_ev.hold = 1U; reply_done("OK"); return; }
    if (argc < 2U) {
        d.line_len = emit(d.line, DIAG_LINE_MAX, "tune_apply_us=%lu max_us=%lu knob=%lu turn=%lu apply=%lu rx=%lu\r\n",
                          (unsigned long)ui_tune_apply_us, (unsigned long)ui_tune_apply_max_us,
                          (unsigned long)knob_tick, (unsigned long)ui_tune_turn_tick, (unsigned long)ui_tune_apply_tick,
                          (unsigned long)rx_retune_tick);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    int32_t n = arg_int(1U, 0) + (int32_t)ui_ev.turn;
    if (n == (int32_t)ui_ev.turn) { reply_usage(TXT_USAGE_KNOB); return; }
    knob_tick = HAL_GetTick();
    ui_ev.turn = (int8_t)(n > 24 ? 24 : (n < -24 ? -24 : n));
    reply_done("OK");
}

__attribute__((cold)) static void cmd_status(void) { reply_lines(line_status, "OK"); }
__attribute__((cold)) static void cmd_events(void) { reply_lines(line_events, "OK"); }

__attribute__((cold)) static void cmd_pipeline(void)
{
    if (arg_is(1U, "ack")) {
        MOD_RequestPipelineFaultAcknowledge();
        reply_done("OK");
        return;
    }
    reply_lines(line_pipeline, "OK");
}

__attribute__((cold)) static void cmd_mem(void) { reply_lines(line_mem, "OK"); }
__attribute__((cold)) static void cmd_cpu(void) { reply_lines(line_cpu, "OK"); }

/* Force a filter slot for bench work. Receive only: RADIO_SelectFilters drops
 * RX_EN and the bias buck before it moves anything, and RADIO_Service puts the
 * covering filter back on the next pass unless a bench routine holds it. */
__attribute__((cold)) static void cmd_lpf(void)
{
    const int32_t slot = arg_int(1U, -1);
    if (slot < 0 || slot > 5) { reply_usage(TXT_USAGE_LPF); return; }
    if (RADIO_ConfigRxBlock(RADIO_Config())) { reply_done("ERR board not confirmed"); return; }
    RADIO_SelectFilters(slot > 4 ? 255U : (uint8_t)slot, 0U);
    reply_done("OK");
}

__attribute__((cold)) static void cmd_bpf(void)
{
    const int32_t slot = arg_int(1U, -1);
    if (slot < 0 || slot > 3) { reply_usage(TXT_USAGE_BPF); return; }
    if (RADIO_ConfigRxBlock(RADIO_Config())) { reply_done("ERR board not confirmed"); return; }
    RADIO_SelectBpf((uint8_t)slot);
    reply_done("OK");
}

__attribute__((cold)) static void cmd_sd(void)
{
    uint32_t size = 0UL, free_mib = 0UL, total_mib = 0UL;
    uint8_t attr = 0U;
    if (!MOD_BackgroundIoAllowed(HAL_GetTick())) { reply_done("ERR storage busy"); return; }
    if (argc < 2U || arg_is(1U, "status")) {
        (void)SD_DiagFreeMiB(&free_mib, &total_mib);
        d.line_len = emit(d.line, DIAG_LINE_MAX,
                          "mounted=%u capacity_mib=%lu free_mib=%lu status=%s err=%u last=%s:%u\r\n",
                          (unsigned)SD_IsMounted(), (unsigned long)total_mib,
                          (unsigned long)free_mib, SD_ServiceStatusString(), (unsigned)SD_GetLastError(),
                          SD_ServiceLastFatFsOp(), (unsigned)SD_ServiceLastFResult());
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    /* What the block driver has met since boot: CRC checks on (CMD59), and
     * every error, retry and re-initialisation. */
    if (arg_is(1U, "stats")) {
        sd_stats_t st;
        SD_GetStats(&st);
        d.line_len = emit(d.line, DIAG_LINE_MAX,
                          "crc_on=%u inits=%lu crc=%lu timeouts=%lu io=%lu retries=%lu reinits=%lu failed=%lu "
                          "aborts=%lu reject=%lX@%lu write_fail_run=%u\r\n",
                          (unsigned)st.crc_on, (unsigned long)st.inits, (unsigned long)st.crc_errors,
                          (unsigned long)st.timeouts, (unsigned long)st.io_errors,
                          (unsigned long)st.retries, (unsigned long)st.reinits, (unsigned long)st.failures,
                          (unsigned long)st.aborts, (unsigned long)st.reject,
                          (unsigned long)st.reject_lba, (unsigned)st.write_fail_run);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    if (argc >= 3U) (void)snprintf(d.path, sizeof(d.path), "%s", argv[2]);
    if (arg_is(1U, "ls")) {
        if (argc < 3U) d.path[0] = '\0';
        reply_lines(line_ls, "OK");
        return;
    }
    if (argc < 3U) { reply_done("ERR usage: sd ls|stat|get|rm|mkdir <path>"); return; }
    if (arg_is(1U, "stat")) {
        if (!SD_DiagStat(d.path, &size, &attr)) { reply_done("ERR no such path"); return; }
        d.line_len = emit(d.line, DIAG_LINE_MAX, "size=%lu attr=%02X\r\n",
                          (unsigned long)size, (unsigned)attr);
        d.line_sent = 0U;
        reply_done("OK");
        return;
    }
    if (arg_is(1U, "get")) {
        if (!SD_DiagStat(d.path, &size, &attr)) { reply_done("ERR no such file"); return; }
        d.get_offset = (uint32_t)arg_int(3U, 0);
        d.get_length = (uint32_t)arg_int(4U, 0);
        reply_lines(line_get, "OK");
        return;
    }
    if (arg_is(1U, "rm")) { reply_done(SD_DiagDelete(d.path) ? "OK" : "ERR delete failed"); return; }
    if (arg_is(1U, "mkdir")) { reply_done(SD_DiagMkdir(d.path) ? "OK" : "ERR mkdir failed"); return; }
    /* sd put <path> <offset> <hex>: a block of a file from the host (HELP.BIN
     * for one: tools/rev1_diag.py sdput). Offset 0 starts the file anew. */
    if (arg_is(1U, "put") && argc >= 5U) {
        uint8_t block[64];
        unsigned n = 0U;
        for (const char *h = argv[4]; h[0] && h[1] && n < sizeof(block); h += 2) {
            unsigned v = 0U;
            for (unsigned k = 0U; k < 2U; ++k) {
                const char c = h[k];
                v = (v << 4) | (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            }
            block[n++] = (uint8_t)v;
        }
        const uint32_t offset = (uint32_t)arg_int(3U, 0);
        reply_done(SD_DiagWriteFile(d.path, offset, block, n, offset == 0U) ? "OK" : "ERR write failed");
        return;
    }
    reply_done("ERR usage: sd status|stats|ls [path]|stat <p>|get <p> [off] [len]|put <p> <off> <hex>|rm <p>|mkdir <p>");
}

__attribute__((cold)) static void cmd_fw(void)
{
    if (argc < 2U || arg_is(1U, "status")) {
        d.line_len = emit(d.line, DIAG_LINE_MAX, "firmware=%s loader=%s\r\n",
                          FW_VERSION_STRING, FW_UpdateStateText());
        d.line_sent = 0U;
        reply_done("OK");
    } else if (arg_is(1U, "verify")) {
        fw_update_result_t result = FW_UpdateVerify();
        if (result == FW_UPDATE_OK) reply_done("OK");
        else {
            d.line_len = emit(d.line, DIAG_LINE_MAX, "reason=%s\r\n", FW_UpdateResultText(result));
            d.line_sent = 0U;
            reply_done("ERR firmware verify failed");
        }
    } else if (arg_is(1U, "install")) {
        fw_update_result_t result = FW_UpdateInstall();
        d.line_len = emit(d.line, DIAG_LINE_MAX, "reason=%s\r\n", FW_UpdateResultText(result));
        d.line_sent = 0U;
        reply_done("ERR firmware install failed");
    } else {
        reply_done("ERR usage: fw status|verify|install");
    }
}

/* The receive bench is not built into the deep-diagnostic flavour, which
 * carries its own pipeline instrumentation and has no room left. Same trade
 * the full-screen `ui` view already makes, and for the same reason: the
 * shipping images are the ones a receiver is calibrated on. */
#if !MOD_DEEP_DIAG_ENABLE
__attribute__((cold)) static const char *iqcal_status_name(uint8_t status)
{
    static const char *const names[] = {"idle", "running", "done", "failed", "stopped"};
    return status < 5U ? names[status] : "?";
}

/* QSD I/Q imbalance, its correction, and what the correction measured out at.
 * Levels are deci-dB and the coefficients are the thousandths the RX I GAIN /
 * RX Q GAIN / RX SKEW SAMPLES settings step in, so `set put` takes them back
 * unchanged. No %f anywhere near the radio. */
__attribute__((cold)) static uint16_t line_rxcal(uint16_t index, char *buf, uint16_t cap)
{
    radio_iqcal_info_t info;
    RADIO_IqCalInfo(&info);
    const radio_iq_result_t *m = &info.measured;
    switch (index) {
        case 0: return emit(buf, cap, "active=%u status=%s result=%u message=%s\r\n",
                            (unsigned)(RADIO_CalActive() == RADIO_CAL_RX_IQ),
                            iqcal_status_name(info.status), (unsigned)info.have_result,
                            info.message);
        case 1: return emit(buf, cap, "rf_hz=%lu lo_hz=%lu delta_hz=%ld\r\n",
                            (unsigned long)info.rf_hz, (unsigned long)info.lo_hz,
                            (long)info.delta_hz);
        case 2: return emit(buf, cap, "lo_period=%lu test_period=%lu floor_ddbfs=%ld\r\n",
                            (unsigned long)info.lo_period, (unsigned long)info.test_period,
                            (long)(info.floor_dbfs * 10.0f));
        /* Where a collect has got to. Printed before the result rows, so a run
         * that is waiting on the correlator shows whether it is moving at all
         * rather than only the message it ends with. */
        case 3: { uint32_t count = 0U, samples = 0U; uint16_t skip = 0U;
                  RADIO_AudioIqCalProgress(&count, &samples, &skip);
                  return emit(buf, cap, "collect=%lu/%lu skip_halves=%u\r\n",
                              (unsigned long)count, (unsigned long)samples, (unsigned)skip); }
        /* Returning 0 ends the producer, so with nothing measured the reply
         * stops after the message and the geometry rather than printing a
         * screen of zeros. */
        case 4: if (!info.have_result) return 0U;
                return emit(buf, cap, "wanted_ddbfs=%ld image_ddbfs=%ld image_ddb=%ld\r\n",
                            (long)(m->wanted_dbfs * 10.0f), (long)(m->image_dbfs * 10.0f),
                            (long)(m->image_db * 10.0f));
        case 5: return emit(buf, cap, "corrected_image_ddb=%ld clipped=%u orientation=%s\r\n",
                            (long)(info.corrected_image_db * 10.0f), (unsigned)m->clipped,
                            m->inverted ? "inverted" : "normal");
        case 6: return emit(buf, cap, "amp_ddb=%ld phase_mdeg=%ld\r\n",
                            (long)(m->amp_db * 10.0f), (long)(m->phase_deg * 1000.0f));
        case 7: return emit(buf, cap, "i_gain_x1000=%ld q_gain_x1000=%ld skew_x1000=%ld invert=%u\r\n",
                            (long)(m->i_gain * 1000.0f), (long)(m->q_gain * 1000.0f),
                            (long)(m->skew_samples * 1000.0f), (unsigned)m->inverted);
        case 8: return emit(buf, cap, "dc_i_x1e6=%ld dc_q_x1e6=%ld\r\n",
                            (long)(m->dc_i * 1000000.0f), (long)(m->dc_q * 1000000.0f));
        case 9: if (!info.sideband_valid) return copy_line(buf, cap, "opposite_ddb=-");
                return emit(buf, cap, "opposite_ddb=%ld in_band_ddbfs=%ld opposite_ddbfs=%ld\r\n",
                            (long)(info.opposite_db * 10.0f), (long)(info.in_band_dbfs * 10.0f),
                            (long)(info.opposite_dbfs * 10.0f));
        default: return 0U;
    }
}

#endif /* !MOD_DEEP_DIAG_ENABLE */

#if !MOD_DEEP_DIAG_ENABLE
/* The injected-carrier bench routines (rxcal, iq/RX_TEST, the AF and image
 * sweeps) put the test clock a whole number of HRTIM period ticks from the
 * LO's own period. One tick is about f^2 / f_HRTIM wide -- 9 kHz at 7 MHz,
 * 37 kHz at 14 MHz, 60 kHz at 18 MHz -- so above roughly 15.6 MHz even the
 * nearest period is further from the LO than the planner's 45 kHz window, and
 * there is no injected carrier to be had on that band at all. That is a
 * property of the band, not a busy radio, so say which one it is. */
__attribute__((cold)) static uint8_t injection_available(void)
{
    radio_rx_geometry_t g;
    RADIO_RxClockGeometry(&g);
    return RADIO_IqChoosePeriod(g.period, RADIO_AudioI2sDivider(), RADIO_AudioRate(),
                                6000.0f, 45000.0f, NULL);
}

#define ERR_NO_INJECTION "ERR no injected carrier above ~15.6 MHz"

__attribute__((cold)) static void cmd_rxcal(void)
{
    if (arg_is(1U, "run") || arg_is(1U, "sb")) {
        const uint16_t sideband = arg_is(1U, "sb") || arg_is(2U, "sb");
        if (RADIO_CalStart(RADIO_CAL_RX_IQ, sideband)) { reply_done("OK"); return; }
        reply_done(injection_available() ? "ERR needs receive, or the radio is busy"
                                         : ERR_NO_INJECTION);
        return;
    }
    if (arg_is(1U, "apply")) {
        reply_done(RADIO_IqCalApply() ? "OK" : "ERR no usable result to apply");
        return;
    }
    if (arg_is(1U, "stop")) { RADIO_CalStop(); reply_done("OK"); return; }
    if (argc < 2U || arg_is(1U, "status")) { reply_lines(line_rxcal, "OK"); return; }
    reply_usage(TXT_USAGE_RXCAL);
}

#if !MOD_DEEP_DIAG_ENABLE
__attribute__((cold)) static uint16_t line_sweep(uint16_t index, char *buf, uint16_t cap)
{
    radio_sweep_info_t info;
    RADIO_SweepInfo(&info);
    const uint32_t *fitted = RADIO_SweepEdges();
    const uint32_t *current = RADIO_Config()->bpf_upper_hz;
    if (index == 0U)
        return emit(buf, cap,
                    "active=%u axis=%s point=%u/%u slots=%u fit=%u clipped=%u floor_ddbfs=%ld export=%s message=%s\r\n",
                    (unsigned)(RADIO_CalActive() == RADIO_CAL_BPF_SWEEP),
                    info.axis == RADIO_SWEEP_LPF ? "lpf" : info.axis == RADIO_SWEEP_BAND ? "band" : "bpf",
                    (unsigned)info.point, (unsigned)RADIO_SWEEP_POINTS, (unsigned)info.slots,
                    (unsigned)info.fit_ok, (unsigned)info.clipped,
                    (long)(info.floor_dbfs * 10.0f), info.export_name ? info.export_name : "-",
                    info.message);
    if (index == 1U)
        return emit(buf, cap, "edges_hz=%lu,%lu,%lu,%lu\r\n", (unsigned long)current[0],
                    (unsigned long)current[1], (unsigned long)current[2], (unsigned long)current[3]);
    if (index == 2U)
        return fitted ? emit(buf, cap, "fitted_hz=%lu,%lu,%lu,%lu\r\n", (unsigned long)fitted[0],
                             (unsigned long)fitted[1], (unsigned long)fitted[2], (unsigned long)fitted[3])
                      : copy_line(buf, cap, "fitted_hz=none");
    return 0U;
}

/* The curves themselves, so a host does not need the SD card to see them.
 * A level of 0 means the point was never measured. */
__attribute__((cold)) static uint16_t line_sweep_table(uint16_t index, char *buf, uint16_t cap)
{
    radio_sweep_info_t info;
    RADIO_SweepInfo(&info);
    if (!index)
        return copy_line(buf, cap, info.axis == RADIO_SWEEP_LPF
            ? "hz lpf0_ddbfs lpf1_ddbfs lpf2_ddbfs lpf3_ddbfs lpf4_ddbfs"
            : "hz bpf0_ddbfs bpf1_ddbfs bpf2_ddbfs bpf3_ddbfs");
    const unsigned k = index - 1U;
    if (k >= RADIO_SWEEP_POINTS) return 0U;
    uint16_t n = emit(buf, cap, "%lu", (unsigned long)RADIO_SweepPointHz(k));
    for (unsigned slot = 0U; slot < info.slots && n + 12U < cap; ++slot) {
        const uint8_t code = RADIO_SweepLevel(k, slot);
        n += emit(buf + n, (uint16_t)(cap - n), " %ld",
                  code ? (long)(RADIO_SweepCodeDb(code) * 10.0f) : -2000L);
    }
    return n + emit(buf + n, (uint16_t)(cap - n), "\r\n");
}

/* QMX 8.6.1 audio filter sweep and 8.6.3 image sweep (radio_af_sweep.h). */
__attribute__((cold)) static uint16_t line_afsweep(uint16_t index, char *buf, uint16_t cap)
{
    radio_af_info_t info;
    RADIO_AfSweepInfo(&info);
    if (index == 0U)
        return emit(buf, cap,
                    "active=%u mode=%s point=%u/%u clipped=%u export=%s message=%s\r\n",
                    (unsigned)(RADIO_CalActive() == RADIO_CAL_AF_SWEEP),
                    info.image_mode ? "image" : "af", (unsigned)info.point,
                    (unsigned)RADIO_AF_POINTS, (unsigned)info.clipped,
                    info.export_name ? info.export_name : "-", info.message);
    if (index == 1U)
        return emit(buf, cap, "rf_hz=%lu lo_hz=%lu delta_hz=%ld tone_hz=%ld image_hz=%ld\r\n",
                    (unsigned long)info.rf_hz, (unsigned long)info.lo_hz, (long)info.delta_hz,
                    (long)info.tone_hz, (long)info.image_hz);
    if (index == 2U)
        return emit(buf, cap, "floor_ddbfs=%ld best_ddbfs=%ld ratio_ddb=%ld\r\n",
                    (long)(info.floor_dbfs * 10.0f), (long)(info.best_dbfs * 10.0f),
                    (long)(info.worst_ratio_db * 10.0f));
    return 0U;
}

/* The curves themselves, so a host does not need the SD card to see them. */
__attribute__((cold)) static uint16_t line_afsweep_table(uint16_t index, char *buf, uint16_t cap)
{
    radio_af_info_t info;
    RADIO_AfSweepInfo(&info);
    if (!index)
        return copy_line(buf, cap, info.image_mode ? "offset_hz ddbfs"
                                                   : "audio_hz wanted_ddbfs opposite_ddbfs");
    const unsigned k = index - 1U;
    if (k >= RADIO_AF_POINTS) return 0U;
    const long x = info.image_mode
        ? (long)RADIO_ImagePointHz(info.first_hz, info.step_hz, k) : (long)RADIO_AfPointHz(k);
    int32_t db[RADIO_AF_CURVES];
    for (unsigned c = 0U; c < RADIO_AF_CURVES; ++c) {
        const uint8_t code = RADIO_AfSweepLevel(k, c);
        db[c] = code ? (int32_t)(RADIO_SweepCodeDb(code) * 10.0f) : -2000;
    }
    return info.image_mode ? emit(buf, cap, "%ld %ld\r\n", x, (long)db[0])
                           : emit(buf, cap, "%ld %ld %ld\r\n", x, (long)db[0], (long)db[1]);
}

#endif /* !MOD_DEEP_DIAG_ENABLE */

/* One entry point for both sweeps: they are the same engine with a different
 * axis, and `image` is the calibration start code. */
__attribute__((cold)) static void af_sweep_command(uint8_t image)
{
#if MOD_DEEP_DIAG_ENABLE
    (void)image;
    reply_done("ERR not built into the diagnostic flavour");
#else
    if (arg_is(1U, "run")) {
        reply_done(RADIO_CalStart(RADIO_CAL_AF_SWEEP, image) ? "OK"
                                                             : "ERR needs receive, or the radio is busy");
        return;
    }
    if (arg_is(1U, "table")) { reply_lines(line_afsweep_table, "OK"); return; }
    if (arg_is(1U, "stop")) { RADIO_CalStop(); reply_done("OK"); return; }
    if (argc < 2U || arg_is(1U, "status")) { reply_lines(line_afsweep, "OK"); return; }
    reply_usage(image ? TXT_USAGE_IMAGESWEEP : TXT_USAGE_AFSWEEP);
#endif
}

#if !MOD_DEEP_DIAG_ENABLE
/* QMX 8.6.7 "Test ADC I/Q": one I2S half of raw PCM1821 pairs and what they
 * say about the quadrature. QMX collects 64 samples with a tone injected at
 * the band centre and has the operator judge equal amplitude, a clean 90
 * degrees and symmetric peaks; these are the same judgements as numbers, with
 * the samples themselves printed so an odd-looking one can be seen. */
static radio_iq_snapshot_t iq_stats;
static uint8_t iq_ready;

__attribute__((cold)) static uint16_t line_iq(uint16_t index, char *buf, uint16_t cap)
{
    if (index == 0U) {
        int32_t pairs[64][2];
        const unsigned frames = RADIO_AudioIqSnapshotFrames();
        const unsigned got = RADIO_AudioIqSnapshotRead(&pairs[0][0], 0U,
                                                       frames < 64U ? frames : 64U);
        iq_ready = got != 0U;
        if (iq_ready) RADIO_IqSnapshotStats(&pairs[0][0], got, &iq_stats);
        return emit(buf, cap, "frames=%u inject=%u ready=%u%s\r\n",
                    (unsigned)(iq_ready ? got : 0U),
                    (unsigned)(RADIO_CalActive() == RADIO_CAL_RX_TEST), (unsigned)iq_ready,
                    iq_ready ? "" : " (the injection is settling: run iq again)");
    }
    if (!iq_ready) return 0U;
    if (index == 1U)
        return emit(buf, cap, "amp_ddb=%ld phase_ddeg=%ld i_rms_ppm=%lu q_rms_ppm=%lu valid=%u clipped=%u\r\n",
                    (long)(iq_stats.amp_db * 10.0f), (long)(iq_stats.phase_deg * 10.0f),
                    (unsigned long)(iq_stats.i_rms * 1000000.0f),
                    (unsigned long)(iq_stats.q_rms * 1000000.0f),
                    (unsigned)iq_stats.valid, (unsigned)iq_stats.clipped);
    if (index == 2U)
        return emit(buf, cap, "i_max=%ld i_min=%ld q_max=%ld q_min=%ld\r\n",
                    (long)iq_stats.i_max, (long)iq_stats.i_min,
                    (long)iq_stats.q_max, (long)iq_stats.q_min);
    if (index == 3U) return copy_line(buf, cap, "n i q");
    const unsigned k = index - 4U;
    int32_t pair[2];
    if (!RADIO_AudioIqSnapshotRead(pair, k, 1U)) return 0U;
    return emit(buf, cap, "%u %ld %ld\r\n", k, (long)pair[0], (long)pair[1]);
}
#endif /* !MOD_DEEP_DIAG_ENABLE */

__attribute__((cold)) static void cmd_iq(void)
{
#if MOD_DEEP_DIAG_ENABLE
    reply_done("ERR not built into the diagnostic flavour");
#else
    if (arg_is(1U, "stop")) { RADIO_CalStop(); reply_done("OK"); return; }
    const uint8_t raw = arg_is(1U, "raw");
    uint8_t need_arm = arg_is(1U, "rearm") || arg_is(2U, "rearm");
    /* Inject unless asked not to, so a single command does what QMX's screen
     * does. `raw` also stops a preceding injected capture. The first reply
     * after arming is expected to say ready=0; a later `iq` must read that
     * completed half instead of arming over it. `rearm` explicitly requests
     * a fresh half for host tools that repeat the command. */
    if (raw && RADIO_CalActive() == RADIO_CAL_RX_TEST) {
        RADIO_CalStop();
        need_arm = 1U;
    }
    if (!raw && RADIO_CalActive() != RADIO_CAL_RX_TEST) {
        if (!RADIO_CalStart(RADIO_CAL_RX_TEST, 0U)) {
            reply_done(injection_available() ? "ERR needs receive, or the radio is busy"
                                             : ERR_NO_INJECTION);
            return;
        }
        need_arm = 1U;
    }
    int32_t probe[2];
    if (need_arm || !RADIO_AudioIqSnapshotRead(probe, 0U, 1U))
        RADIO_AudioIqSnapshotArm();
    reply_lines(line_iq, "OK");
#endif
}

/* QMX 8.6.9 microphone test: QMX scrolls ten rows a second with the average
 * and peak level so the operator can set MIC GAIN by talking into it. Each row
 * here is one RADIO_MicProbe, which polls the ADC for about 12 ms without
 * touching the audio pipeline, so this works with the receiver running. */
__attribute__((cold)) static uint16_t line_mic(uint16_t index, char *buf, uint16_t cap)
{
    const uint8_t source = (uint8_t)(RADIO_SOURCE_MIC1 + (d.mic_source - 1U));
    if (index == 0U)
        return emit(buf, cap, "source=MIC%u qmx_gain_db=50 trim_x100=%lu ref_pct=%u split=%u vref_mv=%u rows=%u\r\n",
                    (unsigned)d.mic_source,
                    (unsigned long)(RADIO_Config()->mic_gain * 100.0f),
                    (unsigned)RADIO_MicrophoneReferencePercent(),
                    (unsigned)RADIO_MicMonitorSplitEnabled(),
                    (unsigned)MOD_GetVrefMillivolts(), (unsigned)d.mic_rows);
    /* Raw ADC codes: an electret on this passive network moves the input by
     * single codes, and rounding that to millivolts would report zero for
     * exactly the signal this test is looking for. rms_x16 is the sensitive
     * column; min/max only report the loudest impulse in the 12 ms window. */
    if (index == 1U) return copy_line(buf, cap, "n min max pp mean_x16 rms_x16");
    if (index - 2U >= d.mic_rows) return 0U;
    radio_mic_probe_t probe;
    if (!RADIO_MicProbe(source, &probe))
        return copy_line(buf, cap, "ERR mic adc busy or unavailable");
    return emit(buf, cap, "%u %u %u %u %u %u\r\n", (unsigned)(index - 2U),
                (unsigned)probe.min, (unsigned)probe.max,
                (unsigned)(probe.max - probe.min),
                (unsigned)probe.mean_x16, (unsigned)probe.rms_x16);
}

__attribute__((cold)) static void cmd_mic(void)
{
    if (arg_is(1U, "ref")) {
        const int32_t percent = arg_int(2U, 68);
        if (percent < 0 || percent > 100) {
            reply_usage(TXT_USAGE_MIC);
            return;
        }
        reply_done(RADIO_MicrophoneReference((uint8_t)percent) ? "OK" : "ERR mic ref");
        return;
    }
    /* `mic split 1` makes the next monitor carry the uncancelled microphone
     * and its reference as left and right, so a host can measure the
     * cancellation coefficient instead of trusting QRP Labs' 0.68. */
    if (arg_is(1U, "split")) {
        const int32_t on = arg_int(2U, 1);
        if (on < 0 || on > 1) {
            reply_usage(TXT_USAGE_MIC_2);
            return;
        }
        reply_done(RADIO_MicMonitorSplit((uint8_t)on) ? "OK" : "ERR mic split");
        return;
    }
    /* `mic mon N [seconds]` routes a microphone into the USB audio capture
     * endpoint instead of printing levels, so a host can record it and an
     * operator can judge what the microphone actually sounds like. Pair it
     * with `audio`: the monitor lives in the audio service, not here, so it
     * survives the switch to the audio personality. `mic mon 0` ends it. */
    if (arg_is(1U, "mon")) {
        const int32_t src = arg_int(2U, 0);
        const int32_t secs = arg_int(3U, 30);
        if (src == 0) { RADIO_MicMonitorStop(); reply_done("OK"); return; }
        if (src < 1 || src > 3 || secs < 1 || secs > 600) {
            reply_usage(TXT_USAGE_MIC_3);
            return;
        }
        reply_done(RADIO_MicMonitorStart((uint8_t)(RADIO_SOURCE_MIC1 + src - 1),
                                         (uint32_t)secs)
                   ? "OK" : "ERR mic mon: needs RECEIVE and a free ADC");
        return;
    }
    const int32_t source = arg_int(1U, 1);
    const int32_t rows = arg_int(2U, 20);
    if (source < 1 || source > 3 || rows < 1 || rows > 200) {
        reply_usage(TXT_USAGE_MIC_4);
        return;
    }
    d.mic_source = (uint8_t)source;
    d.mic_rows = (uint8_t)rows;
    reply_lines(line_mic, "OK");
}

/* QMX 8.6.10 diagnostics, in QMX's own order and grouping so the two screens
 * can be read side by side. REV1 has an LM1084 linear 5 V rather than QMX's
 * 3V3 and 5V bucks, so those rows are the measured rails; QMX's Transmitter
 * block becomes a Receiver block, with the transmit-block reason printed where
 * QMX prints its transmitter-disabled reason. */
__attribute__((cold)) static uint16_t line_diag(uint16_t index, char *buf, uint16_t cap)
{
    radio_telemetry_t t;
    RADIO_GetTelemetry(&t);
    switch (index) {
        case 0: /* Supply. QMX calls 6.0-12.5 V OK. */
            return emit(buf, cap, "supply vin_mv=%u ok=%u\r\n", (unsigned)t.vin_mv,
                        (unsigned)(t.vin_mv >= 6000U && t.vin_mv <= 12500U));
        case 1:
            return emit(buf, cap, "rail5 mv=%u ok=%u vref_mv=%u\r\n", (unsigned)t.rail5_mv,
                        (unsigned)(t.rail5_mv >= 4400U && t.rail5_mv <= 5500U),
                        (unsigned)MOD_GetVrefMillivolts());
        case 2: /* BIAS SMPS: on only while transmitting, as on QMX. */
            return emit(buf, cap, "bias mv=%u ma_x10=%ld duty_1024=%u max_duty_1024=512\r\n",
                        (unsigned)t.bias_mv,
                        (long)(RADIO_BiasCurrentMa(t.bias_mv, t.rail5_mv) * 10.0f),
                        (unsigned)t.bias_duty);
        case 3: {
            uint8_t high = 0U;
            const uint8_t locked = BOARD_ButtonService(HAL_GetTick(), &high);
            return emit(buf, cap, "controls up=%u down=%u select=%u sw4=%u sw5=%u polarity=%s\r\n",
                        (unsigned)BOARD_ButtonPressed(0U), (unsigned)BOARD_ButtonPressed(1U),
                        (unsigned)BOARD_ButtonPressed(2U), (unsigned)BOARD_ButtonPressed(3U),
                        (unsigned)BOARD_ButtonPressed(4U),
                        !locked ? "learning" : (high ? "active-high" : "active-low"));
        }
        case 4: {
            int8_t steps = 0;
            uint8_t click = 0U, hold = 0U;
            const uint8_t fitted = BOARD_EncoderRead(&steps, &click, &hold);
            return emit(buf, cap, "inputs encoder=%s steps=%d click=%u hold=%u ptt=%u\r\n",
                        fitted ? "fitted" : "off", (int)steps, (unsigned)click, (unsigned)hold,
                        (unsigned)BOARD_ButtonPressed(3U));
        }
        case 5: {
            uint8_t lpf = 255U, bpf = 255U;
            (void)RADIO_ConfigFilters(RADIO_Config(), MOD_GetCarrierRequestedHz(), &lpf, &bpf);
            return emit(buf, cap, "receiver state=%s rate_hz=%ld level_ddbfs=%ld lpf=%d bpf=%d overruns=%lu\r\n",
                        RADIO_Status(), (long)RADIO_AudioRate(),
                        (long)(RADIO_AudioRxLevelDbfs() * 10.0f),
                        lpf < 5U ? (int)lpf : -1, bpf < 4U ? (int)bpf : -1,
                        (unsigned long)RADIO_AudioOverruns());
        }
        case 6: {
            const char *why = RADIO_ConfigTxBlock(RADIO_Config(), MOD_GetCarrierRequestedHz());
            return emit(buf, cap, "transmitter blocked=%s\r\n", why ? why : "no");
        }
        case 7:
            return emit(buf, cap, "faults=%02lX cal=%u seq=%lu\r\n",
                        (unsigned long)t.faults, (unsigned)RADIO_CalActive(),
                        (unsigned long)t.sequence);
        default: return 0U;
    }
}

__attribute__((cold)) static void cmd_diag(void) { reply_lines(line_diag, "OK"); }

__attribute__((cold)) static void cmd_afsweep(void) { af_sweep_command(0U); }
__attribute__((cold)) static void cmd_imagesweep(void) { af_sweep_command(1U); }

__attribute__((cold)) static void cmd_sweep(void)
{
    if (arg_is(1U, "run") || arg_is(1U, "all") || arg_is(1U, "lpf") || arg_is(1U, "rf")) {
        const uint16_t axis = arg_is(1U, "lpf") ? RADIO_SWEEP_LPF
                            : arg_is(1U, "rf") ? RADIO_SWEEP_BAND : RADIO_SWEEP_BPF;
        reply_done(RADIO_CalStart(RADIO_CAL_BPF_SWEEP, axis) ? "OK"
                                                             : "ERR needs receive, or the radio is busy");
        return;
    }
    if (arg_is(1U, "table")) { reply_lines(line_sweep_table, "OK"); return; }
    if (arg_is(1U, "apply")) {
        /* The panel action, so there is one implementation of the apply. */
        const setting_item_t *it = UI_GetSetting(SET_REV1_BPF_APPLY);
        if (!it || !it->set) { reply_done("ERR apply unavailable"); return; }
        it->set(1.0f);
        reply_lines(line_sweep, "OK");
        return;
    }
    if (arg_is(1U, "stop")) { RADIO_CalStop(); reply_done("OK"); return; }
    if (argc < 2U || arg_is(1U, "status")) { reply_lines(line_sweep, "OK"); return; }
    reply_usage(TXT_USAGE_SWEEP);
}

/* Come up in the console after a reset instead of in USB audio. Bench
 * scaffolding, so it lives here rather than in the settings map: a board being
 * driven over this port otherwise needs the LCD menu walked after every flash.
 * Not built into the deep-diagnostic flavour, which has no flash left. */
/* Turn the receive cycle instrument on or off, so its own cost can be
 * measured by watching the miss rate with it both ways. */
__attribute__((cold)) static void cmd_rxprof(void)
{
#if MOD_DEEP_DIAG_ENABLE
    reply_done("ERR not built into the diagnostic flavour");
#else
    if (argc >= 2U) RADIO_AudioProfileEnable(arg_is(1U, "on") || arg_int(1U, 0) == 1);
    d.line_len = emit(d.line, DIAG_LINE_MAX, "rxprof=%u\r\n",
                      (unsigned)RADIO_AudioProfileEnabled());
    d.line_sent = 0U;
    reply_done("OK");
#endif
}

__attribute__((cold)) static void cmd_diagboot(void)
{
    radio_config_t c;
    RADIO_GetConfig(&c);
    if (argc >= 2U) {
        const uint8_t on = arg_is(1U, "on") || arg_int(1U, 0) == 1;
        if (!on && !arg_is(1U, "off") && arg_int(1U, -1) != 0) {
            reply_done("ERR usage: diagboot [on|off]");
            return;
        }
        c.boot_flags = (uint16_t)((c.boot_flags & ~1U) | (on ? 1U : 0U));
        if (!RADIO_ConfigValid(&c)) { reply_done("ERR config invalid"); return; }
        RADIO_SetConfig(&c);
        RADIO_GetConfig(&c);
    }
    d.line_len = emit(d.line, DIAG_LINE_MAX, "diagboot=%u (save to keep)\r\n",
                      (unsigned)(c.boot_flags & 1U));
    d.line_sent = 0U;
    reply_done("OK");
}

/* Hand the port to USB audio for a while and take it back automatically. */
__attribute__((cold)) static void cmd_audio(void)
{
    const int32_t seconds = arg_int(1U, -1);
    if (seconds < 1 || seconds > 600) { reply_usage(TXT_USAGE_AUDIO); return; }
    if (!USB_DiagModeIsActive()) { reply_done("ERR console is not the active personality"); return; }
    d.audio_window_s = (uint32_t)seconds;
    d.audio_switch_at = 0U;
    d.line_len = emit(d.line, DIAG_LINE_MAX, "audio_window_s=%ld\r\n", (long)seconds);
    d.line_sent = 0U;
    reply_done("OK");
}
#endif /* !MOD_DEEP_DIAG_ENABLE */

#if !MOD_DEEP_DIAG_ENABLE
/* `link 1`: this port carries the S-CTRL LINK (docs/SCTRL_LINK.md) once
 * this OK has left; HELLO then gives the caps and the image CRC. A CAT port
 * of the USB audio personality only, one LINK at a time. */
__attribute__((cold)) static void cmd_link(void)
{
    const uint8_t ok = (uint8_t)((arg_int(1U, 0) == (int32_t)SL_PROTO) && !USB_DiagModeIsActive() && !SCTRL_LinkActive());
    d.link_switch = ok;
    reply_done(ok ? "OK" : "ERR link");
}
#endif

static const diag_cmd_t commands[] = {
    {"help", "list commands, or explain one", TXT_NONE, cmd_help},
    {"id", "board, firmware, clock, reset cause, tx block", TXT_NONE, cmd_id},
    {"status", "what the radio is doing and why, decoded; ask first", TXT_NONE, cmd_status},
    {"events", NULL, TXT_CMD_EVENTS, cmd_events},
    {"telem", NULL, TXT_CMD_TELEM, cmd_telem},
    {"pins", NULL, TXT_CMD_PINS, cmd_pins},
    {"keys", NULL, TXT_CMD_KEYS, cmd_keys},
    {"rx", NULL, TXT_CMD_RX, cmd_rx},
    {"digi", "host capture stream: sent, gaps, level, ADC peak", TXT_NONE, cmd_digi},
    {"cwdec", NULL, TXT_CMD_CWDEC, cmd_cwdec},
    {"pd", NULL, TXT_CMD_PD, cmd_pd},
    {"cat", NULL, TXT_CMD_CAT, cmd_cat},
    {"faults", NULL, TXT_CMD_FAULTS, cmd_faults},
    {"selftest", NULL, TXT_CMD_SELFTEST, cmd_selftest},
    {"set", NULL, TXT_CMD_SET, cmd_set},
    {"save", "persist settings to storage", TXT_NONE, cmd_save},
    {"ui", NULL, TXT_CMD_UI, cmd_ui},
    {"paddle", NULL, TXT_CMD_PADDLE, cmd_paddle},
    {"knob", NULL, TXT_CMD_KNOB, cmd_knob},
    {"pipeline", NULL, TXT_CMD_PIPELINE, cmd_pipeline},
    {"mem", NULL, TXT_CMD_MEM, cmd_mem},
    {"clock", NULL, TXT_CMD_CLOCK, cmd_clock},
    {"meter", NULL, TXT_CMD_METER, cmd_meter},
    {"metercal", NULL, TXT_CMD_METERCAL, cmd_metercal},
    {"cpu", NULL, TXT_CMD_CPU, cmd_cpu},
    {"sd", "card status, listing, stat, get, put, rm, mkdir", TXT_NONE, cmd_sd},
    {"storage", "settings store on the card: state, list, nocard, new, adopt, import, backup, restore", TXT_NONE, cmd_storage},
    {"card", "files this image needs on the SD card, and their state", TXT_NONE, cmd_card},
    {"fw", "status, verify or install REV1FW.UPD", TXT_NONE, cmd_fw},
#if !MOD_DEEP_DIAG_ENABLE
    {"rxcal", NULL, TXT_CMD_RXCAL, cmd_rxcal},
    {"sweep", NULL, TXT_CMD_SWEEP, cmd_sweep},
    {"afsweep", NULL, TXT_CMD_AFSWEEP, cmd_afsweep},
    {"imagesweep", NULL, TXT_CMD_IMAGESWEEP, cmd_imagesweep},
    {"iq", NULL, TXT_CMD_IQ, cmd_iq},
    {"mic", NULL, TXT_CMD_MIC, cmd_mic},
    {"diag", NULL, TXT_CMD_DIAG, cmd_diag},
    {"audio", NULL, TXT_CMD_AUDIO, cmd_audio},
    {"diagboot", "start in this console after a reset; needs save", TXT_NONE, cmd_diagboot},
    {"rxprof", NULL, TXT_CMD_RXPROF, cmd_rxprof},
#endif
    {"lpf", NULL, TXT_CMD_LPF, cmd_lpf},
    {"bpf", NULL, TXT_CMD_BPF, cmd_bpf},
    {"exit", "leave the console and re-enumerate as USB audio", TXT_NONE, cmd_exit},
#if !MOD_DEEP_DIAG_ENABLE
    {"link", NULL, TXT_CMD_LINK, cmd_link},
#endif
};
#define DIAG_CMD_COUNT (sizeof(commands) / sizeof(commands[0]))

__attribute__((cold)) static uint16_t line_help(uint16_t index, char *buf, uint16_t cap)
{
    char text[DIAG_LINE_MAX];
    if (index >= DIAG_CMD_COUNT) return 0U;
    const char *help = commands[index].help;
    if (help == NULL) {
        (void)UI_CardText(commands[index].text, text, (uint16_t)sizeof(text));
        help = text; /* "" without the card: the name alone */
    }
    return emit(buf, cap, "%-9s %s\r\n", commands[index].name, help);
}

/* `help <command>`: that command's line alone (it printed the whole list). */
static uint8_t help_index;

__attribute__((cold)) static uint16_t line_help_one(uint16_t index, char *buf, uint16_t cap)
{
    return index ? 0U : line_help(help_index, buf, cap);
}

__attribute__((cold)) static void cmd_help(void)
{
    if (argc < 2U) { reply_lines(line_help, "OK"); return; }
    for (uint8_t i = 0U; i < DIAG_CMD_COUNT; ++i) {
        if (!arg_is(1U, commands[i].name)) continue;
        help_index = i;
        reply_lines(line_help_one, "OK");
        return;
    }
    reply_done("ERR unknown command; try help");
}

__attribute__((cold)) static void dispatch(void)
{
    for (uint8_t i = 0U; i < DIAG_CMD_COUNT; ++i) {
        if (!arg_is(0U, commands[i].name)) continue;
        commands[i].run();
        return;
    }
    reply_done("ERR unknown command; try help");
}

__attribute__((cold)) static void parse_and_run(void)
{
    argc = 0U;
    char *const cmd = d.ed.text;
    const uint16_t len = d.ed.len;
    for (uint16_t i = 0U; i < len && argc < DIAG_ARGS_MAX; ) {
        while (i < len && cmd[i] == ' ') cmd[i++] = '\0';
        if (i >= len) break;
        argv[argc++] = &cmd[i];
        while (i < len && cmd[i] != ' ') ++i;
    }
    cmd[len] = '\0';
    for (uint16_t i = 0U; i < len; ++i) if (cmd[i] == ' ') cmd[i] = '\0';
    if (!argc) { reply_done("OK"); return; }
    if (d.ed.overflow) { reply_done("ERR command too long"); return; }
    dispatch();
}

/* ---- service ---- */

/* One byte of the line protocol, or of a person typing it. */
__attribute__((cold)) static void line_char(uint8_t ch)
{
    const uint8_t r = TERM_LineInput(&d.ed, ch, (uint8_t)(d.producer != NULL));
    if (r == TERM_LINE_READY) {
        d.cmd_ready = 1U;
    } else if (r == TERM_LINE_INTERRUPT) {
        d.producer = NULL;
        d.tail = "ERR interrupted";
        d.prompt_owed = d.ed.interactive;
    }
}

/* Everything before the next command's reply has left: the previous reply,
 * the prompt, and the echo of the line itself. */
__attribute__((cold)) static uint8_t output_idle(void)
{
    return (uint8_t)(!d.producer && !d.tail && !d.prompt_owed && !d.ed.echo_len &&
                     d.line_sent >= d.line_len);
}

__attribute__((cold)) static void take_input(uint32_t now, uint8_t realtime)
{
    (void)now;
    uint8_t packets = 0U;
    for (;;) {
        if (d.rx_pos >= d.rx_len) {
            /* From the realtime branch: one packet per call, no more. */
            if (realtime && packets) return;
            packets++;
            const uint16_t n = port_receive(d.rx, sizeof(d.rx));
            if (!n) return;
            d.bytes_in += n;
            d.rx_len = n;
            d.rx_pos = 0U;
            d.greet_due = 0U;
        }
        while (d.rx_pos < d.rx_len) {
            /* A whole command is waiting for its turn: leave the rest. */
            if (d.cmd_ready) return;
            term_event_t ev;
            if (!TERM_Decode(&d.term, d.rx[d.rx_pos++], &ev)) continue;
#if !MOD_DEEP_DIAG_ENABLE
            if (d.ui_mode) { ui_event(&ev, now, realtime); continue; }
#endif
            if (ev.kind == TERM_EV_CHAR) line_char(ev.ch);
        }
    }
}

__attribute__((cold)) static uint8_t next_line(void)
{
    if (d.line_sent < d.line_len) return 1U;
    d.line_len = 0U;
    d.line_sent = 0U;
    if (d.producer) {
        const uint16_t n = d.producer(d.produce_index++, d.line, DIAG_LINE_MAX);
        if (n) { d.line_len = n > DIAG_LINE_MAX - 1U ? (uint16_t)(DIAG_LINE_MAX - 1U) : n; return 1U; }
        d.producer = NULL;
    }
    if (d.tail) {
        d.line_len = copy_line(d.line, DIAG_LINE_MAX, d.tail);
        d.tail = NULL;
        return 1U;
    }
    if (d.prompt_owed) {
        d.prompt_owed = 0U;
        if (!d.ui_mode) {
            d.line_len = emit(d.line, DIAG_LINE_MAX, "> ");
            return 1U;
        }
    }
    /* Echo last: typed ahead during a reply, it shows after the prompt. */
    if (d.ed.echo_len) {
        memcpy(d.line, d.ed.echo, d.ed.echo_len);
        d.line_len = d.ed.echo_len;
        d.ed.echo_len = 0U;
        return 1U;
    }
    return 0U;
}

/* The port opened or closed (DTR). Each opening is a fresh session: nothing
 * left over from the last host, no echo until this one presses Enter. */
__attribute__((cold)) static void session_edge(uint8_t open, uint32_t now)
{
#if !MOD_DEEP_DIAG_ENABLE
    paddle_leave();
    d.modes_off_due = 0U;
#endif
    if (!open) return;
    d.producer = NULL;
    d.tail = NULL;
    d.line_len = d.line_sent = 0U;
    d.rx_len = d.rx_pos = 0U;
    d.cmd_ready = 0U;
    d.ui_mode = 0U;
    d.prompt_owed = 0U;
    TERM_LineReset(&d.ed, GREETING);
    TERM_DecoderReset(&d.term);
    d.opened_at = now;
    d.greet_due = 1U;
}

#if !MOD_DEEP_DIAG_ENABLE
/* The LINK's console lines (sctrl_link.c): a line may start only when the
 * last command's reply has gone and nothing is typed half-way. */
__attribute__((cold)) uint8_t USB_DiagLinkIdle(void)
{
    return (uint8_t)(!d.cmd_ready && output_idle() && (d.ed.len == 0U));
}

__attribute__((cold)) void USB_DiagLinkByte(uint8_t ch)
{
    if ((ch >= 0x20U) && (ch < 0x7FU)) line_char(ch); /* text only: no editing keys */
}

__attribute__((cold)) void USB_DiagLinkEnd(uint8_t ok)
{
    if (ok) line_char('\n');
    else TERM_LineClear(&d.ed);
}
#endif

__attribute__((cold)) void USB_DiagRelease(void)
{
#if !MOD_DEEP_DIAG_ENABLE
    paddle_leave();
    d.ui_mode = 0U;
#endif
}

/* Main loop, realtime branch only. While RF is keyed USB_DiagService does
 * nothing at all, which would leave a lever down until the transmission
 * ended by itself; with the paddle page open this takes the input instead
 * (one packet at most, no output) so a key release is seen at once. */
__attribute__((cold)) void USB_DiagPaddlePoll(uint32_t now)
{
#if !MOD_DEEP_DIAG_ENABLE
    if (!paddle_page()) return;
    if (!port_open()) { paddle_leave(); return; }
    take_input(now, 1U);
    paddle_publish(now);
#else
    (void)now;
#endif
}

/* cold: this returns immediately on every pass except while a host is
 * actually attached, so it is optimised for size, not for the hot path. */
__attribute__((cold)) void USB_DiagService(uint32_t now)
{
    if (!USB_DiagModeIsActive() && !CAT_ConsoleOpen() && !d.was_open) return;
#if !MOD_DEEP_DIAG_ENABLE
    /* Under the LINK the console only answers its CMD frames: with none
     * arriving and nothing owed it has nothing to do (the session's edges
     * come after the LINK has ended). */
    if (SCTRL_LinkActive() && USB_DiagLinkIdle() && !d.audio_window_s) return;
#endif
    /* The radio comes first: inside the audio path's critical window the
     * console does nothing at all, not even parse. */
    if (MOD_AudioRealtimeCritical(now)) return;
    const uint8_t open = port_open(), session = CAT_ConsoleSession();
    if ((open != d.was_open) || (session != d.session)) {
        /* The port changed hands under an open session (cat.c): that one
         * ends first, its line and output with it. */
        if (open && d.was_open) session_edge(0U, now);
        d.was_open = open;
        d.session = session;
        session_edge(open, now);
    }
    if (!open) return;

    take_input(now, 0U);
#if !MOD_DEEP_DIAG_ENABLE
    for (uint8_t i = 0U; i < d.held_count && d.ui_mode; ++i) ui_key(d.held_keys[i]);
    d.held_count = 0U;
    if (paddle_page()) paddle_publish(now);
#endif
    if (d.cmd_ready && output_idle()) {
        d.cmd_ready = 0U;
        parse_and_run();
        TERM_LineClear(&d.ed);
        if (d.ed.interactive && !d.ui_mode) d.prompt_owed = 1U;
        take_input(now, 0U);
    }
    /* A terminal program that opened the port and said nothing: tell the
     * person what this is. A script has sent its command long before. The
     * leading # marks it as commentary for hosts that parse replies. */
    if (d.greet_due && (uint32_t)(now - d.opened_at) >= 300U && output_idle()) {
        d.greet_due = 0U;
        d.tail = "# " BOARD_NAME " " FW_VERSION_STRING " console. Press Enter.";
    }

    /* A frame every 250 ms (the paddle page every 100 ms, so the levers
     * follow the keys), and only once the previous one has drained, so a slow
     * host can never make frames queue up. */
#if !MOD_DEEP_DIAG_ENABLE
    if (d.ui_mode && !d.producer && d.line_sent >= d.line_len &&
        (int32_t)(now - d.ui_next_frame) >= 0) {
        d.ui_next_frame = now + (paddle_page() ? 100U : 250U);
        reply_lines(line_ui, NULL);
    }
#endif

    uint8_t budget = MOD_NoncriticalUiAllowed(now) ? DIAG_BUDGET_FULL : DIAG_BUDGET_BUSY;
    while (budget-- && port_tx_ready()) {
        if (!next_line()) break;
        const uint16_t sent = port_transmit((const uint8_t *)&d.line[d.line_sent],
                                                  (uint16_t)(d.line_len - d.line_sent));
        if (!sent) break;
        d.line_sent = (uint16_t)(d.line_sent + sent);
        d.bytes_out += sent;
    }

#if !MOD_DEEP_DIAG_ENABLE
    /* `link 1`: the port goes to the LINK once the reply has left it. */
    if (d.link_switch && !d.producer && !d.tail && d.line_sent >= d.line_len && CAT_ConsoleTxReady()) {
        uint8_t mask = 0U, port = 0xFFU, ptt = 0U;
        uint32_t commands_seen = 0U;
        CAT_Status(&mask, &port, &commands_seen, &ptt);
        d.link_switch = 0U;
        if (port < USBD_CAT_PORTS) SCTRL_LinkStart(port);
    }
#endif

    /* Switch to USB audio only once the confirmation has left, plus a moment
     * for the last packet to reach the host: re-enumerating takes the endpoint
     * with it. USB_DiagModeTick brings the console back when the window ends. */
#if !MOD_DEEP_DIAG_ENABLE
    if (d.audio_window_s) {
        if (d.producer || d.tail || d.line_sent < d.line_len) d.audio_switch_at = 0U;
        else if (!d.audio_switch_at) d.audio_switch_at = now + 50U;
        else if ((int32_t)(now - d.audio_switch_at) >= 0) {
            const uint32_t seconds = d.audio_window_s;
            d.audio_window_s = 0U;
            (void)USB_DiagModeAudioWindow(seconds);
        }
    }
#endif
}

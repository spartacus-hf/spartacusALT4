#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/* The S-CTRL LINK on the radio (sctrl_link.h, docs/SCTRL_LINK.md). */

#include "sctrl_link.h"
#include "sctrl_link_logic.h"

#include "cat.h"
#include "fw_update_format.h"
#include "main.h"
#include "mod_morse_tables.h"
#include "mod_priv.h"
#include "modulator.h"
#include "radio.h"
#include "radio_analog.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "radio_digi.h"
#include "radio_pd.h"
#include "radio_spectrum.h"
#include "ui_gfx.h"
#include "ui_priv.h"
#include "ui_state.h"
#include "usb_diag.h"
#include "usb_diag_mode.h"
#include "usbd_cat.h"

#include <stddef.h>
#include <string.h>

#if !MOD_DEEP_DIAG_ENABLE

#define NO_PORT          0xFFU
#define CONSOLE_CHUNK    48U   /* console bytes per CONSOLE frame */
#define ACK_SLOTS        4U
#define SOF_WINDOW_US    600U  /* re-arm after an IN packet only this early in a frame */
#define CAPS (SL_CAP_SCREEN | SL_CAP_SPECTRUM | SL_CAP_CWDEC | \
              SL_CAP_CAT | SL_CAP_TX | SL_CAP_MONITOR | SL_CAP_INPUT)

enum { OPEN_NONE = 0U, OPEN_SCREEN, OPEN_SPECTRUM };
enum { SPEC_SEC_LINE = 0U, SPEC_SEC_TRACE, SPEC_SEC_PEAK, SPEC_SEC_DONE };

typedef struct { uint8_t type, seq, code, detail; } ack_t;

static struct {
    sl_ring_t ring;
    sl_rx_t rx;
    sl_tx_t tx;
    sl_writer_t w;
    sl_rle_t rle;
    ack_t ack[ACK_SLOTS];
    uint8_t acks;
    uint8_t active, hello_due, bye_due, seq;
    uint8_t open, open_page, spec_sec;
    uint16_t spec_idx;
    uint32_t open_hash;
    uint16_t sub, txevents;
    uint8_t status_ms;          /* STATUS period / 4 */
    uint8_t cmd_busy, cmd_started, cmd_seq, cat_pending, cat_seq;
    uint32_t dirty;             /* screen pages to send */
    uint8_t band_page, band_pages, refresh_last, bg_ms;
    uint32_t status_at;
    uint32_t tick_ms;           /* the guard's last tick: once a millisecond is plenty */
    uint32_t settings_hash, hash_acc;
    uint8_t hash_id;
    uint32_t cw_seen;
    uint16_t spec_seq;
    const uint8_t *spec_line;
    uint32_t spec_version;
} L;

/* Interrupt side. */
static volatile uint8_t link_port = NO_PORT;
static volatile uint8_t packets_per_frame = 2U, sent_this_frame;
static volatile uint32_t sof_stamp, sof_window;

/* ---- interrupt: SOF-paced IN packets ---- */

static void send_next(void)
{
    const uint8_t *p = NULL;
    const uint16_t n = SL_RingChunk(&L.ring, &p);
    if (n && USBD_CatInSend(link_port, p, n)) {
        SL_RingConsume(&L.ring, n); /* the packet is in packet memory already */
        sent_this_frame++;
    }
}

void SCTRL_LinkUsbSof(void)
{
    if (link_port == NO_PORT) return;
    sof_stamp = TIM5->CNT;
    sent_this_frame = 0U;
    send_next();
}

void SCTRL_LinkUsbInDone(uint8_t port)
{
    if (port != link_port) return;
    if ((sent_this_frame < packets_per_frame) && ((uint32_t)(TIM5->CNT - sof_stamp) < sof_window)) send_next();
}

/* ---- small frames ---- */

static uint8_t whole(uint8_t type, const void *p, uint16_t n)
{
    if (L.open != OPEN_NONE) return 0U;
    if (!SL_FrameWhole(&L.ring, type, L.seq, (const uint8_t *)p, n)) return 0U;
    L.seq++;
    return 1U;
}

static void ack(uint8_t type, uint8_t seq, uint8_t code)
{
    if (L.acks < ACK_SLOTS) L.ack[L.acks++] = (ack_t){type, seq, code, 0U};
}

/* ---- the radio as the guard and STATUS see it ---- */

static uint8_t transmitting(void)
{
    const radio_state_t st = RADIO_State();
    return (uint8_t)((st == RADIO_TRANSMIT) || (st == RADIO_TX_PREPARE));
}

static void view(uint32_t now, sl_radio_view_t *v)
{
    (void)now;
    v->diag = USB_DiagModeIsActive();
    v->fault = (uint8_t)((RADIO_State() == RADIO_FAULT) || MOD_PipelineRfLockedOut());
    v->cannot_radiate = cw_keyer_monitor_only();
    /* Someone else keys: CAT PTT, or TX the LINK did not cause. */
    v->other_owner = (uint8_t)(MOD_GetUsbCatPtt() || (transmitting() && !SL_TxActive(&L.tx)));
    v->pending = (uint8_t)((ui.mode_pending != UI_UMODE_COUNT) || ui.band_pending);
    v->memory_busy = MOD_CwMemoryBusy();
    v->tx_test = (uint8_t)((active_config == (uint32_t)MOD_CFG_TX_CARRIER_TEST) &&
                           (requested_config == (uint32_t)MOD_CFG_TX_CARRIER_TEST));
}

static void apply_levers(void)
{
    MOD_CwSetVirtualPaddle((uint8_t)(L.tx.levers | (L.tx.tune ? 1U : 0U)));
    if (L.tx.stop_memory) {
        MOD_CwMemoryStop();
        L.tx.stop_memory = 0U;
    }
}

static uint8_t tx_owner(void)
{
    if (MOD_GetUsbCatPtt()) {
        const uint8_t p = CAT_PttPort();
        return (p < USBD_CAT_PORTS) ? (uint8_t)(p + 1U) : 0xFFU;
    }
    if (SL_TxActive(&L.tx)) return L.tx.memory ? 4U : 3U;
    if (!transmitting()) return 0U;
    return (UI_NavModeShown() == UI_UMODE_CW) ? 5U : 6U;
}

/* A slice of the setting values folded into the hash each STATUS tick:
 * the whole table about once a second, no hooks in the setters. */
static void hash_step(void)
{
    for (uint8_t k = 0U; k < 16U; ++k) {
        if (L.hash_id >= SET_COUNT) {
            L.settings_hash = L.hash_acc;
            L.hash_acc = 2166136261U;
            L.hash_id = 0U;
            return;
        }
        const setting_item_t *it = UI_GetSetting((setting_id_t)L.hash_id++);
        if (!it || !it->get) continue;
        const float v = it->get();
        uint32_t bits;
        memcpy(&bits, &v, 4U);
        L.hash_acc = (L.hash_acc ^ bits) * 16777619U;
    }
}

static void status_frame(uint32_t now)
{
    sl_status_t s;
    memset(&s, 0, sizeof(s));
    const radio_config_t *c = RADIO_Config();
    radio_telemetry_t t;
    RADIO_GetTelemetry(&t);
    rf_meter_reading_t r;
    radio_digi_status_t dg;
    RADIO_DigiStatus(&dg, now);
    radio_ref_sof_status_t rs;
    RADIO_RefSofStatus(&rs);
    uint8_t open_mask = 0U, console = 0U, ptt = 0U;
    uint32_t commands = 0U;
    CAT_Status(&open_mask, &console, &commands, &ptt);

    s.uptime_ms = now;
    s.dial_hz = UI_NavTuneHz();
    s.state = (uint8_t)RADIO_State();
    s.mode = (uint8_t)UI_NavModeShown();
    s.page = (uint8_t)ui.page;
    s.ui = (uint8_t)((ui.home_level & 3U) | ((ui.zone & 3U) << 2) | ((ui.menu_level & 3U) << 4) |
                     (ui.editing ? 0x40U : 0U) | (ui.boot_hold ? 0x80U : 0U));
    s.flags0 = (uint8_t)((MOD_GetMuted() ? 0x01U : 0U) | (c->usb_iq ? 0x02U : 0U) | (MOD_GetUsbVox() ? 0x04U : 0U) |
                         ((MOD_GetSideband() == SIDEBAND_LSB) ? 0x08U : 0U) |
                         (MOD_AudioRealtimeCritical(now) ? 0x10U : 0U) | (MOD_BackgroundIoAllowed(now) ? 0U : 0x20U) |
                         (uint8_t)(RADIO_AudioPcMonitorFlags() << 6));
    sl_radio_view_t v;
    view(now, &v);
    s.flags1 = (uint8_t)((L.tx.armed ? 0x01U : 0U) | (ptt ? 0x02U : 0U) | (v.memory_busy ? 0x04U : 0U) |
                         (v.pending ? 0x08U : 0U) | (MOD_PipelineRfLockedOut() ? 0x10U : 0U) | (v.diag ? 0x20U : 0U) |
                         (v.cannot_radiate ? 0x40U : 0U) | ((MOD_CwBootArmStage() < 3U) ? 0x80U : 0U));
    s.tx_owner = tx_owner();
    s.load_pct = RADIO_AudioLoadPct();
    s.faults = (uint16_t)t.faults;
    s.vin_mv = t.vin_mv;
    if (RADIO_MeterRecent(now, 3000U, &r)) {
        const float cw = r.pep_w * 100.0f + 0.5f;
        s.pep_cw = (uint16_t)(cw > 65535.0f ? 65535.0f : cw);
        if (r.swr_valid) s.swr_x100 = (uint16_t)(r.swr > 655.0f ? 65535U : (uint16_t)(r.swr * 100.0f + 0.5f));
    }
    s.rx_dbfs_x10 = (int16_t)(RADIO_AudioRxLevelDbfs() * 10.0f);
    s.wpm = (uint8_t)(cfg_cw_wpm + 0.5f);
    s.cat = (uint8_t)((open_mask & 3U) | ((console < USBD_CAT_PORTS) ? (uint8_t)(console << 4) : 0xF0U));
    s.cat0_age_ds = CAT_PortAgeDs(0U, now);
    /* New pipeline events: the PC reads them with the console's `events`,
     * names and all, when this moves. */
    s.event_seq = MOD_PipelineEventSeq();
    s.settings_hash = L.settings_hash;
    s.pd_mv = RADIO_PdVoltage();
    s.rx_overruns = (uint16_t)RADIO_AudioOverruns();
    s.cap_gaps = (uint16_t)dg.gaps_10min;
    s.ref_state = rs.state;
    s.link = (uint8_t)((L.tx.hb_ok ? 1U : 0U) | ((L.sub & SL_SUB_SCREEN) ? 2U : 0U));
    s.ref_ppm_x100 = (int16_t)(rs.ppb / 10);
    s.band = MOD_GetActiveBandIndex();
    s.arm_left_s = SL_TxArmLeftS(&L.tx, now);
    if (whole(SL_T_STATUS, &s, sizeof(s))) {
        L.status_at = now;
        hash_step();
    }
}

/* ---- inbound ---- */

static void cmd_byte(void *ctx, uint8_t b)
{
    (void)ctx;
    if (!L.cmd_started) {
        L.cmd_started = 1U;
        L.cmd_seq = L.rx.buf[1];
        L.cmd_busy = (uint8_t)!USB_DiagLinkIdle();
    }
    if (!L.cmd_busy) USB_DiagLinkByte(b);
}

static void cmd_end(void *ctx, uint8_t ok)
{
    if (!L.cmd_started) {
        L.cmd_seq = L.rx.buf[1];
        L.cmd_busy = (uint8_t)!USB_DiagLinkIdle();
    }
    if (L.cmd_busy) ack(SL_H_CMD, L.cmd_seq, SL_ACK_BUSY);
    else USB_DiagLinkEnd(ok);
    L.cmd_started = 0U;
    L.cmd_busy = 0U;
    (void)ctx;
}

static const sl_rx_sink_t sink = {cmd_byte, cmd_end, NULL};

/* INPUT code 14: the pages a PC may open. */
#define OPENABLE ((1UL << PAGE_HOME) | (1UL << PAGE_FREQ) | (1UL << PAGE_DIAG) | (1UL << PAGE_TX_TEST) | \
                  (1UL << PAGE_CW_TRAINER) | (1UL << PAGE_SD) | (1UL << PAGE_PD) | (1UL << PAGE_WATERFALL))

/* INPUT codes 3..12: the ui_events_t flag each one sets. */
static const uint8_t input_flag[10] = {
    offsetof(ui_events_t, click), offsetof(ui_events_t, hold), offsetof(ui_events_t, up),
    offsetof(ui_events_t, dn), offsetof(ui_events_t, sel), offsetof(ui_events_t, up_long),
    offsetof(ui_events_t, dn_long), offsetof(ui_events_t, sel_long), offsetof(ui_events_t, fkey),
    offsetof(ui_events_t, fkey_long),
};

static uint8_t input(uint8_t code, int8_t value)
{
    uint8_t *ev = (uint8_t *)&ui_ev;
    if ((code == 1U) || (code == 2U) || (code == 13U)) {
        /* Detents accumulate as the knob's do, clamped as `knob` clamps. */
        int8_t *acc = (int8_t *)&ev[(code == 1U) ? offsetof(ui_events_t, turn)
                                   : (code == 2U) ? offsetof(ui_events_t, turn_held) : offsetof(ui_events_t, fkey_turn)];
        const int32_t n = *acc + value;
        *acc = (int8_t)(n > 24 ? 24 : (n < -24 ? -24 : n));
        return SL_ACK_OK;
    }
    if (code == 14U) {
        /* Open a page as its menu entry does (TX TEST switches the mode and
         * restores it on leaving); only the ones a PC has a use for. */
        if (((uint8_t)value >= 32U) || !((OPENABLE >> (uint8_t)value) & 1U)) return SL_ACK_RANGE;
        UI_NavOpenPage((ui_page_t)value);
        return SL_ACK_OK;
    }
    if ((code < 3U) || (code > 12U)) return SL_ACK_BAD;
    ev[input_flag[code - 3U]] = 1U;
    return SL_ACK_OK;
}

static void frame_in(uint8_t len, uint32_t now)
{
    const uint8_t type = L.rx.buf[0], seq = L.rx.buf[1];
    const uint8_t *p = &L.rx.buf[2];
    const uint8_t n = (uint8_t)(len - 2U);
    uint8_t code = SL_ACK_OK, reply = 1U;
    sl_radio_view_t v;
    view(now, &v);
    switch (type) {
        case SL_H_HEARTBEAT:
            SL_TxHeartbeat(&L.tx, now);
            return;
        case SL_H_PADDLE:
            code = SL_TxPaddle(&L.tx, now, n ? p[0] : 0U, &v);
            reply = (uint8_t)(code != SL_ACK_OK); /* refusals only */
            break;
        case SL_H_TX_STOP:
            SL_TxStop(&L.tx);
            L.txevents |= 1U << SL_TXE_STOPPED;
            break;
        case SL_H_TX_ARM: {
            if (n < 4U) { code = SL_ACK_BAD; break; }
            const uint8_t was = L.tx.armed;
            code = SL_TxArm(&L.tx, now, p[0], p[1], p[2], p[3], &v);
            if (L.tx.armed != was) L.txevents |= (uint16_t)(1U << (L.tx.armed ? SL_TXE_ARMED : SL_TXE_DISARMED));
            break;
        }
        case SL_H_MEMSEND:
            code = SL_TxMemory(&L.tx, now, &v);
            if (code == SL_ACK_OK) {
                /* The text in place, NUL where its CRC was: it has been checked. */
                L.rx.buf[len] = 0U;
                uint8_t codes[48];
                const uint8_t count = MORSE_ParseText((const char *)p, codes, sizeof(codes));
                code = !count ? SL_ACK_BAD : (!MOD_CwMemorySend(codes, count) ? SL_ACK_BUSY : SL_ACK_OK);
                if (code != SL_ACK_OK) L.tx.memory = 0U;
            }
            break;
        case SL_H_SUB:
            if (n < 4U) { code = SL_ACK_BAD; break; }
            L.sub = (uint16_t)(p[0] | (p[1] << 8));
            packets_per_frame = (p[2] < 1U) ? 1U : (p[2] > 4U ? 4U : p[2]);
            L.status_ms = (uint8_t)(250U / ((p[3] < 1U) ? 1U : (p[3] > 20U ? 20U : p[3])));
            L.dirty = 0x3FFFFFFFUL;
            L.cw_seen = RADIO_CwDecodeCount();
            break;
        case SL_H_REFRESH: {
            const uint8_t what = n ? p[0] : 0U;
            if (what & 0x01U) { L.dirty = 0x3FFFFFFFUL; L.refresh_last = 1U; }
            if (what & 0x08U) L.cw_seen = 0U;
            if (what & 0x10U) L.status_at = now - 1000U;
            break;
        }
        case SL_H_INPUT:
            code = (n < 2U) ? SL_ACK_BAD : (MOD_AudioRealtimeCritical(now) ? SL_ACK_BUSY_RT : input(p[0], (int8_t)p[1]));
            break;
        case SL_H_CAT:
            /* One TX path: the LINK keys through the guard, never CAT PTT. */
            if (L.cat_pending) code = SL_ACK_BUSY;
            else if ((n >= 2U) && (((p[0] == 'T') && ((p[1] == 'X') || (p[1] == 'Q'))) || ((p[0] == 'R') && (p[1] == 'X'))))
                code = SL_ACK_UNSUPPORTED;
            else { L.cat_pending = CAT_LinkRun(link_port, p, n); reply = 0U; }
            break;
        case SL_H_LINK_OFF:
            L.bye_due = 1U;
            reply = 0U;
            break;
        case SL_H_TUNE:
            code = n ? SL_TxTune(&L.tx, now, p[0], &v) : SL_ACK_BAD;
            break;
        case SL_H_MONITOR:
            /* Heartbeat-bound like TX: the tick turns it off when the PC goes. */
            if (n < 2U) code = SL_ACK_BAD;
            else RADIO_AudioPcMonitor(p[0], p[1]);
            break;
        default:
            code = SL_ACK_UNSUPPORTED;
            break;
    }
    apply_levers();
    if (reply) ack(type, seq, code);
}

void SCTRL_LinkRx(const uint8_t *data, uint16_t length)
{
    if (!L.active) return;
    const uint32_t now = HAL_GetTick();
    for (uint16_t i = 0U; i < length; ++i) {
        const uint8_t len = SL_RxByte(&L.rx, data[i], &sink);
        if (len >= 2U) frame_in(len, now);
    }
}

/* ---- start and end ---- */

uint8_t SCTRL_LinkActive(void) { return L.active; }

void SCTRL_LinkStart(uint8_t port)
{
    const uint32_t now = HAL_GetTick();
    link_port = NO_PORT;
    memset(&L, 0, sizeof(L));
    SL_TxInit(&L.tx, now);
    L.active = 1U;
    L.hello_due = 1U;
    L.status_ms = 25U;
    L.hash_acc = 2166136261U;
    packets_per_frame = 2U;
    sof_window = (SystemCoreClock / 1000000U) * SOF_WINDOW_US;
    CAT_LinkClaim(port);
    link_port = port; /* last: the interrupt starts sending from here */
}

/* Everything the LINK started stops; the port goes back to CAT. */
static void stop(void)
{
    link_port = NO_PORT;
    SL_TxStop(&L.tx);
    apply_levers();
    RADIO_AudioPcMonitor(0U, 128U);
    L.active = 0U;
}

/* HOME's badge: 0 no LINK, 1 a PC is linked, 2 it holds the transmitter. */
uint8_t SCTRL_LinkBadge(void)
{
    if (!L.active) return 0U;
    return SL_TxActive(&L.tx) ? 2U : 1U;
}

void SCTRL_LinkClosed(void)
{
    if (L.active) stop();
}

/* ---- console ---- */

uint8_t SCTRL_LinkConsoleReady(void)
{
    return (uint8_t)(L.active && (L.open == OPEN_NONE) && !L.hello_due &&
                     (SL_RingFree(&L.ring) >= 2U * CONSOLE_CHUNK + 10U));
}

uint16_t SCTRL_LinkConsoleWrite(const uint8_t *data, uint16_t length)
{
    if (!SCTRL_LinkConsoleReady()) return 0U;
    const uint16_t n = (length > CONSOLE_CHUNK) ? CONSOLE_CHUNK : length;
    return whole(SL_T_CONSOLE, data, n) ? n : 0U;
}

/* ---- screen ---- */

void SCTRL_LinkScreenChanged(uint32_t pages)
{
    /* Not the band's pages: each waterfall line redraws them all, and their
     * flags, sent lowest page first, starved the scale and footer below for
     * seconds. Their flag goes when the band comes, goes or moves. */
    if (L.active) L.dirty |= pages & ~(((1UL << L.band_pages) - 1UL) << L.band_page);
}

static void screen_continue(void)
{
    const uint8_t *fb = GFX_FrameBits(), *at = GFX_TileAttrs();
    const sl_src2_t src = {&fb[(uint32_t)L.open_page * GFX_W], GFX_W, &at[(uint32_t)L.open_page * GFX_TCOLS], GFX_TCOLS};
    if (!SL_RlePut(&L.rle, &src, &L.w, &L.ring)) return;
    if (!SL_FrameEnd(&L.w, &L.ring)) return;
    L.open = OPEN_NONE;
    L.seq++;
    /* Redrawn while it went out: send it again. */
    if (GFX_PageHash(L.open_page) != L.open_hash) L.dirty |= 1UL << L.open_page;
}

static void screen_start(void)
{
    const uint8_t page = (uint8_t)__builtin_ctz(L.dirty);
    const uint8_t in_band = (uint8_t)((uint8_t)(page - L.band_page) < L.band_pages);
    const uint8_t last = (uint8_t)(L.refresh_last && !(L.dirty & (L.dirty - 1U)));
    const uint8_t head[2] = {page, (uint8_t)(in_band | (last << 1))};
    if (in_band) {
        if (!whole(SL_T_SCREEN, head, sizeof(head))) return;
    } else {
        if (SL_RingFree(&L.ring) < 16U || !SL_FrameBegin(&L.w, &L.ring, SL_T_SCREEN, L.seq)) return;
        (void)SL_FramePut(&L.w, &L.ring, head, sizeof(head));
        L.open = OPEN_SCREEN;
        L.open_page = page;
        L.open_hash = GFX_PageHash(page);
        L.rle.pos = 0U;
        L.rle.lit_left = 0U;
    }
    L.dirty &= ~(1UL << page);
    if (last) L.refresh_last = 0U;
    if (!in_band) screen_continue();
}

/* ---- waterfall ---- */

static uint8_t trace_byte(const int16_t *t, uint16_t x)
{
    const int32_t v = t[x] / 8 + 160;
    return (uint8_t)(t[x] == INT16_MIN ? 0 : (v < 1 ? 1 : (v > 255 ? 255 : v)));
}

static void spectrum_continue(void)
{
    while (L.spec_sec != SPEC_SEC_DONE) {
        const int16_t *trace = SPEC_Trace();
        const uint8_t *peak = SPEC_Peak();
        if (L.spec_sec == SPEC_SEC_LINE) {
            const uint16_t left = (uint16_t)(SPEC_LINE_BYTES - L.spec_idx);
            L.spec_idx = (uint16_t)(L.spec_idx + SL_FramePut(&L.w, &L.ring, &L.spec_line[L.spec_idx], left));
            if (L.spec_idx < SPEC_LINE_BYTES) return;
        } else {
            while (L.spec_idx < SPEC_W) {
                const uint8_t b = (L.spec_sec == SPEC_SEC_TRACE) ? (trace ? trace_byte(trace, L.spec_idx) : 0U)
                                                                 : (peak ? peak[L.spec_idx] : 0U);
                if (!SL_FramePut(&L.w, &L.ring, &b, 1U)) return;
                L.spec_idx++;
            }
        }
        L.spec_idx = 0U;
        L.spec_sec++;
        if ((L.spec_sec == SPEC_SEC_TRACE) && !((L.sub & SL_SUB_SPECTRUM_TRACE) && trace)) L.spec_sec = SPEC_SEC_DONE;
        if ((L.spec_sec == SPEC_SEC_PEAK) && !peak) L.spec_sec = SPEC_SEC_DONE;
    }
    if (!SL_FrameEnd(&L.w, &L.ring)) return;
    L.open = OPEN_NONE;
    L.seq++;
}

static uint8_t spectrum_due(void)
{
    if (!(L.sub & SL_SUB_SPECTRUM) || (ui.page != PAGE_WATERFALL)) return 0U;
    const uint8_t *line = SPEC_HistoryLine(0U);
    return (uint8_t)(line && (SPEC_Version() != L.spec_version));
}

_Static_assert(sizeof(spec_view_t) == 32U && offsetof(spec_view_t, valid) == 18U &&
               offsetof(spec_view_t, bottom_db) == 26U && offsetof(spec_view_t, rate10) == 30U,
               "SPECTRUM carries spec_view_t as it is: change docs/SCTRL_LINK.md with it");

static void spectrum_start(void)
{
    if (SL_RingFree(&L.ring) < 2U * (sizeof(sl_spectrum_pre_t) + sizeof(spec_view_t)) + 8U) return;
    spec_view_t v;
    SPEC_GetView(&v);
    const uint8_t *line = SPEC_HistoryLine(0U);
    const uint8_t trace = (uint8_t)((L.sub & SL_SUB_SPECTRUM_TRACE) && SPEC_Trace());
    const spec_config_t *c = SPEC_Config();
    if (line != L.spec_line) L.spec_seq++;
    const sl_spectrum_pre_t pre = {
        L.spec_seq, (uint8_t)((trace ? 0x01U : 0U) | ((trace && SPEC_Peak()) ? 0x02U : 0U)),
        (uint8_t)(c->palette | (c->layout << 4)), SPEC_W,
    };
    (void)SL_FrameBegin(&L.w, &L.ring, SL_T_SPECTRUM, L.seq);
    (void)SL_FramePut(&L.w, &L.ring, (const uint8_t *)&pre, sizeof(pre));
    (void)SL_FramePut(&L.w, &L.ring, (const uint8_t *)&v, sizeof(v));
    L.spec_line = line;
    L.spec_version = SPEC_Version();
    L.open = OPEN_SPECTRUM;
    L.spec_sec = SPEC_SEC_LINE;
    L.spec_idx = 0U;
    spectrum_continue();
}

/* ---- the producer ---- */

static void cwdec_step(void)
{
    if (!(L.sub & SL_SUB_CWDEC)) return;
    const uint32_t count = RADIO_CwDecodeCount();
    if (count == L.cw_seen) return;
    const char *text = RADIO_CwDecodeText();
    const uint32_t have = (uint32_t)strlen(text);
    uint32_t fresh = (count < L.cw_seen) ? count : count - L.cw_seen; /* cleared: all of it */
    if (fresh > have) fresh = have;
    if (fresh > 40U) fresh = 40U;
    uint8_t b[44];
    memcpy(b, &count, 4U);
    memcpy(&b[4], &text[have - fresh], fresh);
    if (whole(SL_T_CWDEC, b, (uint16_t)(4U + fresh))) L.cw_seen = count;
}

__attribute__((noinline)) static uint8_t hello(void)
{
    static const sl_hello_t base = {
        SL_PROTO, FW_VERSION_MAJOR, FW_VERSION_MINOR, 0U, CAPS, 0U, GFX_W, GFX_H, GFX_PAGES, GFX_TCOLS,
        SL_TX_BODY_MAX, 0U, 0U,
    };
    sl_hello_t h = base;
    h.image_crc = ((const fw_image_footer_t *)FW_FOOTER_ADDR)->image_crc32;
    h.port = link_port;
    h.packets_per_frame = packets_per_frame;
    if (!SL_FrameWhole(&L.ring, SL_T_HELLO, L.seq, (const uint8_t *)&h, sizeof(h))) return 0U;
    L.seq++;
    L.hello_due = 0U;
    return 1U;
}

/* ACKs, TXEVENTs and BYE: 1 when all of them went out. */
__attribute__((noinline)) static uint8_t control(void)
{
    while (L.acks) {
        if (!whole(SL_T_ACK, &L.ack[0], sizeof(ack_t))) return 0U;
        memmove(&L.ack[0], &L.ack[1], (size_t)(--L.acks) * sizeof(ack_t));
    }
    while (L.txevents) {
        const uint8_t k = (uint8_t)__builtin_ctz(L.txevents);
        const uint8_t b[4] = {k, 0U, 0U, 0U};
        if (!whole(SL_T_TXEVENT, b, sizeof(b))) return 0U;
        L.txevents &= (uint16_t)~(1U << k);
    }
    if (L.bye_due == 1U) { /* once: 2 waits for it to drain */
        const uint8_t reason = 0U;
        if (!whole(SL_T_BYE, &reason, 1U)) return 0U;
        L.bye_due = 2U;
    }
    if (L.bye_due == 2U) {
        /* The port is CAT again only once the ring has drained. */
        if (SL_RingUsed(&L.ring)) return 0U;
        const uint8_t port = link_port;
        stop();
        CAT_LinkRelease(port);
        return 0U;
    }
    return 1U;
}

/* What waits while the radio is keyed, or while the console has a reply to
 * send (that goes first). */
__attribute__((noinline)) static void background(uint32_t now)
{
    /* Every 4 ms is plenty for what waits here (a page, a line, decoded
     * text). Its gates cost ~3000 cycles each from a cold cache: once a
     * millisecond they were 1.5 % of the core on HOME (bench, 2026-10-06). */
    if ((uint8_t)((uint8_t)now - L.bg_ms) < 4U) return;
    L.bg_ms = (uint8_t)now;
    if (MOD_AudioRealtimeCritical(now) || !USB_DiagLinkIdle()) return;
    cwdec_step();
    if (spectrum_due()) { spectrum_start(); if (L.open != OPEN_NONE) return; }
    if (!(L.sub & SL_SUB_SCREEN) || !MOD_NoncriticalUiAllowed(now)) return;
    /* The waterfall's band came or went: every page again, flagged anew (a
     * band page goes as its flag alone, SCTRL_LINK.md SCREEN). */
    uint8_t page = 0U, pages = 0U;
    GFX_BandSpan(&page, &pages);
    if (page != L.band_page || pages != L.band_pages) {
        L.band_page = page;
        L.band_pages = pages;
        L.dirty = 0x3FFFFFFFUL;
    }
    if (L.dirty) screen_start();
}

void SCTRL_LinkService(uint32_t now)
{
    if (!L.active) return;
    if (L.hello_due && !hello()) return;
    /* A frame being sent fills the ring every pass; everything else runs once
     * a millisecond at most (its times are in milliseconds), not every pass:
     * the main loop has a tenth of the core while receiving, and its other
     * services share it. */
    if (L.open == OPEN_SCREEN) screen_continue();
    else if (L.open == OPEN_SPECTRUM) spectrum_continue();
    if (now == L.tick_ms) return;
    L.tick_ms = now;
    /* Every millisecond while the guard holds anything; idle it only watches
     * the heartbeat (1.5 s), and frames apply their levers as they arrive. */
    if (L.tx.armed || SL_TxActive(&L.tx) || !(now & 15U)) {
        sl_radio_view_t v;
        view(now, &v);
        L.txevents |= SL_TxTick(&L.tx, now, &v);
        apply_levers();
        /* The PC went quiet: its monitor and phones-off go with it. */
        if (!L.tx.hb_ok && RADIO_AudioPcMonitorFlags()) RADIO_AudioPcMonitor(0U, 128U);
    }
    if ((L.open != OPEN_NONE) || !control()) return;
    if ((L.sub & SL_SUB_STATUS) && ((uint32_t)(now - L.status_at) >= (uint32_t)L.status_ms * 4U)) status_frame(now);
    if (L.cat_pending) {
        uint16_t n = 0U;
        const char *r = CAT_LinkReply(link_port, &n);
        if (!whole(SL_T_CATREPLY, r, n)) return;
        CAT_LinkReplyDone(link_port);
        L.cat_pending = 0U;
    }
    background(now);
}

#else /* the deep-diagnostic flavour has no RAM for a LINK */

void SCTRL_LinkStart(uint8_t port) { (void)port; }
uint8_t SCTRL_LinkActive(void) { return 0U; }
uint8_t SCTRL_LinkBadge(void) { return 0U; }
uint8_t SCTRL_LinkConsoleReady(void) { return 0U; }
uint16_t SCTRL_LinkConsoleWrite(const uint8_t *data, uint16_t length) { (void)data; (void)length; return 0U; }
void SCTRL_LinkRx(const uint8_t *data, uint16_t length) { (void)data; (void)length; }
void SCTRL_LinkClosed(void) {}
void SCTRL_LinkService(uint32_t now) { (void)now; }
void SCTRL_LinkScreenChanged(uint32_t pages) { (void)pages; }
void SCTRL_LinkUsbSof(void) {}
void SCTRL_LinkUsbInDone(uint8_t port) { (void)port; }

#endif

#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/**
 * @file    cat.c
 * @brief   CAT and terminal on the USB serial ports (see cat.h).
 */

#include "cat.h"
#include "cat_logic.h"
#include "sctrl_link.h"
#include "usbd_cat.h"
#include "main.h"
#include "modulator.h"
#include "radio.h"
#include "radio_analog.h"
#include "radio_audio.h"
#include "radio_config.h"
#include "radio_digi.h"
#include "ui_format.h"
#include "ui_priv.h"
#include "ui_state.h"

#include <stdio.h>
#include <string.h>

/* Replies waiting for the IN endpoint. A command is taken only once the
 * last reply has gone, so every reply has the whole buffer and a host that
 * never reads stops being read. */
#define CAT_OUT_MAX          96U
/* A mode the radio cannot take within this (a page that owns it, a stream
 * that will not stop) is dropped, and MD reads the radio's again. */
#define CAT_MD_PENDING_MS    5000U
#define CAT_TIMEOUT_S_DEFAULT 120U  /* QMX's CAT timeout */
#define CAT_NO_PORT          0xFFU

typedef struct {
    cat_parser_t parser;
    uint8_t      was_open;
    uint8_t      terminal;          /* this port is the console's */
    uint8_t      link;              /* this port carries the S-CTRL LINK (sctrl_link.c) */
    uint32_t     last_ms;           /* its last CAT command */
    uint8_t      in[USBD_CAT_OUT_PACKET];
    uint8_t      in_len, in_pos;
    char         out[CAT_OUT_MAX];
    uint16_t     out_len, out_sent;
} cat_port_state_t;

static cat_port_state_t ports[USBD_CAT_PORTS];
static uint8_t  console_port = CAT_NO_PORT;
/* Moves whenever the console's port changes hands: the console starts a
 * fresh session on it, even for a close and a reopen it never saw (it waits
 * while the radio is realtime-critical). */
static uint8_t  console_session;
/* The console reads what made a port its own where it lies: the line typed
 * so far (the port's parser.cmd, from hand_pos), the line ending, then the
 * rest of that packet (in[in_pos..in_len]). */
static uint8_t  hand_pos;
static char     hand_ending;
static uint8_t  current_port;               /* whose command is running */
static uint32_t service_now;                /* CAT_Service's `now`, for the ops */
static uint8_t  tx_port = CAT_NO_PORT;      /* who asked for transmit */
static uint8_t  md_pending;                 /* CAT_MD_* still to apply, 0 none */
static uint32_t md_pending_at;
static uint32_t last_cmd_ms;
static uint32_t command_count;
static uint8_t  timeout_on = 1U;
static uint16_t timeout_s = CAT_TIMEOUT_S_DEFAULT;

/* ---- settings, the console's way (`set put`) ---- */

static float setting_get(setting_id_t id)
{
    const setting_item_t *it = UI_GetSetting(id);
    return (it && it->get) ? it->get() : 0.0f;
}

/* Nothing to change is not a change: TX AUDIO INPUT and USB RAW IQ restart
 * the audio path when written. Like the console, an assembly change waits
 * for an idle transport. */
static uint8_t setting_put(setting_id_t id, float v)
{
    const setting_item_t *it = UI_GetSetting(id);
    if (!it || !it->set) return 0U;
    v = UI_ClampSettingValue(it, v);
    if (it->get && (it->get() == v)) return 1U;
    if (MOD_AudioRealtimeCritical(HAL_GetTick())) return 0U;
    it->set(v);
    return 1U;
}

/* ---- the radio as cat_logic sees it ---- */

static uint8_t data_mode(void) { return (uint8_t)(MOD_GetUsbAudioPolicy() == USB_AUDIO_POLICY_DIGI_CLEAN); }

static uint8_t md_of(ui_umode_t m, uint8_t data)
{
    switch (m) {
        case UI_UMODE_DIGI: return (MOD_GetSideband() == SIDEBAND_LSB) ? CAT_MD_LSB_DATA : CAT_MD_USB_DATA;
        case UI_UMODE_LSB: return data ? CAT_MD_LSB_DATA : CAT_MD_LSB;
        case UI_UMODE_CW:  return CAT_MD_CW;
        case UI_UMODE_AM:  return CAT_MD_AM;
        case UI_UMODE_FM:  return CAT_MD_FM;
        default:           return data ? CAT_MD_USB_DATA : CAT_MD_USB;
    }
}

static uint32_t op_get_freq(void) { return UI_NavTuneHz(); }
static uint8_t  op_set_freq(uint32_t hz) { return UI_NavTuneTo(hz); }
static uint8_t  op_get_mode(void) { return md_pending ? md_pending : md_of(UI_NavModeShown(), data_mode()); }

static uint8_t op_set_mode(uint8_t md)
{
    if ((md == 0U) || (md == 8U) || (md > 9U)) return 0U;
    md_pending = md;
    md_pending_at = service_now;
    return 1U;
}

/* The mode a client asked for, once the radio can take it: like the
 * panel's, a mode change waits for the audio path to be idle. DIGI (MD6,
 * MD9; Hamlib's PKTLSB/PKTUSB) is the sideband with USB audio as its clean
 * source, QMX's DiGi; plain LSB/USB put the voice processing back. */
static void md_service(uint32_t now)
{
    if (!md_pending) return;
    /* Signed: one time base (service_now), but never trust a wrap. */
    if ((int32_t)(now - md_pending_at) > (int32_t)CAT_MD_PENDING_MS) { md_pending = 0U; return; }
    if (MOD_BackgroundIoAllowed(now) == 0U) return;
    const uint8_t md = md_pending;
    /* Data/Pkt is the DIGI mode (QMX's DiGi), in one step on either
     * sideband; plain USB/LSB put the voice processing back (UI_ModeApply). */
    uint8_t ok;
    if ((md == CAT_MD_USB_DATA) || (md == CAT_MD_LSB_DATA)) {
        ok = UI_ModeApplyDigi((md == CAT_MD_LSB_DATA) ? SIDEBAND_LSB : SIDEBAND_USB);
    } else {
        ui_umode_t m = UI_UMODE_USB;
        if (md == CAT_MD_LSB) m = UI_UMODE_LSB;
        else if ((md == CAT_MD_CW) || (md == CAT_MD_CWR)) m = UI_UMODE_CW;
        else if (md == CAT_MD_FM) m = UI_UMODE_FM;
        else if (md == CAT_MD_AM) m = UI_UMODE_AM;
        ok = UI_ModeApply(m);
    }
    if (!ok) return;
    md_pending = 0U;
}

static uint8_t op_get_tx(void)
{
    const radio_state_t st = RADIO_State();
    return (uint8_t)(MOD_GetUsbCatPtt() || (st == RADIO_TRANSMIT) || (st == RADIO_TX_PREPARE));
}

static void op_set_tx(uint8_t on)
{
    if (on && !MOD_GetUsbCatPtt()) RADIO_DigiHostKey(HAL_GetTick());
    MOD_SetUsbCatPtt(on);
    tx_port = on ? current_port : CAT_NO_PORT;
}

static int32_t op_get(cat_param_t id)
{
    const radio_config_t *c = RADIO_Config();
    const uint32_t now = HAL_GetTick();
    rf_meter_reading_t r;
    switch (id) {
        case CAT_P_WPM:        return (int32_t)(setting_get(SET_CW_WPM) + 0.5f);
        case CAT_P_VOLUME:     return (int32_t)(((uint32_t)c->volume * 255U + 50U) / 100U);
        case CAT_P_IQ:         return c->usb_iq ? 1 : 0;
        case CAT_P_VOX:        return MOD_GetUsbVox();
        case CAT_P_SOURCE:     return (c->mic_source == RADIO_SOURCE_USB) ? 0 : 2;
        case CAT_P_SIDEBAND:   return (UI_NavModeShown() == UI_UMODE_LSB) ? 1 : 0;
        case CAT_P_TIMEOUT_ON: return timeout_on;
        case CAT_P_TIMEOUT_S:  return timeout_s;
        case CAT_P_POWER_DW:
            return RADIO_MeterRecent(now, 3000U, &r) ? (int32_t)(r.pep_w * 10.0f + 0.5f) : 0;
        case CAT_P_SWR_X100:
            if (!op_get_tx() || !RADIO_MeterRecent(now, 1000U, &r) || !r.swr_valid) return -1;
            return (int32_t)(r.swr * 100.0f + 0.5f);
        case CAT_P_SMETER_DB: {
            /* dB above -140 dBFS at the demodulator: monotonic, uncalibrated. */
            const float db = RADIO_AudioRxLevelDbfs() + 140.0f;
            return (db > 0.0f) ? (int32_t)(db + 0.5f) : 0;
        }
        case CAT_P_FILTER_HZ:
            if (UI_NavModeShown() == UI_UMODE_CW)
                return c->cw_bw_10hz ? (int32_t)c->cw_bw_10hz * 10 : (int32_t)RADIO_CW_BW_DEFAULT_HZ;
            return c->ssb_bw_100hz ? (int32_t)c->ssb_bw_100hz * 100 : (int32_t)RADIO_SSB_BW_DEFAULT_HZ;
        default:
            return CAT_PARAM_NONE;
    }
}

static uint8_t op_set(cat_param_t id, int32_t v)
{
    switch (id) {
        case CAT_P_WPM:    return setting_put(SET_CW_WPM, (float)v);
        case CAT_P_VOLUME:
            if (v > 255) return 0U;
            RADIO_SetVolume((uint8_t)(((uint32_t)v * 100U + 127U) / 255U));
            return 1U;
        case CAT_P_IQ:     return (v <= 1) ? setting_put(SET_REV1_USB_IQ, (float)v) : 0U;
        case CAT_P_VOX:
            if (v > 1) return 0U;
            MOD_SetUsbVox((uint8_t)v);
            return 1U;
        case CAT_P_SOURCE:
            /* 0 USB, 2 the microphone in use (MIC1 if USB was); no two-tone. */
            if (v == 0) return setting_put(SET_REV1_MIC, (float)RADIO_SOURCE_USB);
            if (v != 2) return 0U;
            if (RADIO_Config()->mic_source != RADIO_SOURCE_USB) return 1U;
            return setting_put(SET_REV1_MIC, (float)RADIO_SOURCE_MIC1);
        case CAT_P_SIDEBAND: {
            const uint8_t data = data_mode();
            return op_set_mode((v == 1) ? (data ? CAT_MD_LSB_DATA : CAT_MD_LSB)
                                        : (data ? CAT_MD_USB_DATA : CAT_MD_USB));
        }
        case CAT_P_TIMEOUT_ON:
            if (v > 1) return 0U;
            timeout_on = (uint8_t)v;
            return 1U;
        case CAT_P_TIMEOUT_S:
            if ((v < 1) || (v > 3600)) return 0U;
            timeout_s = (uint16_t)v;
            return 1U;
        default:
            return 0U;
    }
}

/* ---- menu manager: MM path / path=value / path? and ML, over this radio's
 * menu (categories, then their rows; a linked group is a sub-menu), names
 * case-insensitive or indexes from 0, section titles not counted -- QMX's
 * scheme with this radio's names. Types as QMX's: 0 sub-menu, 1 page or
 * action, 3 number (field width), 5 list (list type = ML argument). ---- */

static uint8_t name_eq(const char *a, const char *b)
{
    while (*a == ' ') ++a;
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return 0U;
    }
    while (*a == ' ') ++a;
    return (uint8_t)(!*a && !*b);
}

static uint8_t as_index(const char *s, uint8_t *out)
{
    uint32_t v = 0U;
    if (!*s) return 0U;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return 0U;
        v = v * 10U + (uint32_t)(*s - '0');
        if (v > 255U) return 0U;
    }
    *out = (uint8_t)v;
    return 1U;
}

/* Decimal to float without strtof's locale machinery (as the console). */
static uint8_t as_float(const char *s, float *out)
{
    float sign = 1.0f, whole = 0.0f, frac = 0.0f, scale = 1.0f;
    if (*s == '-') { sign = -1.0f; ++s; } else if (*s == '+') ++s;
    if (!*s) return 0U;
    for (; *s >= '0' && *s <= '9'; ++s) whole = whole * 10.0f + (float)(*s - '0');
    if (*s == '.') {
        for (++s; *s >= '0' && *s <= '9'; ++s) { frac = frac * 10.0f + (float)(*s - '0'); scale *= 10.0f; }
    }
    if (*s) return 0U;
    *out = sign * (whole + frac / scale);
    return 1U;
}

typedef struct {
    uint8_t  cat;
    uint8_t  group;
    uint8_t  have_row;
    ui_row_t row;
} mm_node_t;

static uint8_t mm_find_row(uint8_t cat, uint8_t group, const char *seg, ui_row_t *out)
{
    uint8_t want = 0U;
    const uint8_t numeric = as_index(seg, &want);
    const uint8_t total = UI_MenuRowCount(cat, group);
    uint8_t k = 0U;
    for (uint8_t i = 0U; i < total; ++i) {
        ui_row_t row;
        if (!UI_MenuRowAt(cat, group, i, &row) || (row.kind == UI_ROW_HEADER)) continue;
        if (numeric ? (k == want) : name_eq(seg, UI_MenuRowLabel(&row))) { *out = row; return 1U; }
        ++k;
    }
    return 0U;
}

/* A section title inside a list (AUDIO's "USB INPUT"): QMX would have a
 * sub-menu there, so a path may name it; it adds no level. */
static uint8_t mm_is_section(uint8_t cat, uint8_t group, const char *seg)
{
    const uint8_t total = UI_MenuRowCount(cat, group);
    for (uint8_t i = 0U; i < total; ++i) {
        ui_row_t row;
        if (UI_MenuRowAt(cat, group, i, &row) && (row.kind == UI_ROW_HEADER) &&
            name_eq(seg, UI_MenuRowLabel(&row))) return 1U;
    }
    return 0U;
}

static uint8_t mm_walk(char *path, mm_node_t *n)
{
    n->cat = CAT_NO_PORT;
    n->group = AUDIO_GROUP_NONE;
    n->have_row = 0U;
    for (char *seg = path; seg; ) {
        char *next = strchr(seg, '|');
        if (next) *next++ = '\0';
        if (n->cat == CAT_NO_PORT) {
            uint8_t idx = 0U;
            const uint8_t numeric = as_index(seg, &idx);
            for (uint8_t c = 0U; c < UI_MenuCategoryCount(); ++c) {
                if (numeric ? (c == idx) : name_eq(seg, UI_MenuCategoryLabel(c))) { n->cat = c; break; }
            }
            if (n->cat == CAT_NO_PORT) return 0U;
        } else {
            if (n->have_row) {
                if (n->row.kind != UI_ROW_GROUP) return 0U; /* a setting has no rows */
                n->group = (uint8_t)n->row.id;
                n->have_row = 0U;
            }
            if (mm_find_row(n->cat, n->group, seg, &n->row)) n->have_row = 1U;
            else if (!mm_is_section(n->cat, n->group, seg)) return 0U;
        }
        seg = next;
    }
    return (uint8_t)(n->cat != CAT_NO_PORT);
}

/* The setting a node names, if it has a value to read. */
static const setting_item_t *mm_setting(const mm_node_t *n)
{
    if (!n->have_row || (n->row.kind != UI_ROW_SETTING)) return NULL;
    const setting_item_t *it = UI_GetSetting((setting_id_t)n->row.id);
    return (it && it->get && (it->style != STY_ACTION)) ? it : NULL;
}

static void mm_value(const setting_item_t *it, float v, char *out)
{
    out[0] = '\0';
    if (UI_SettingIsDiscrete(it->style)) UI_FormatSettingValue(it, v, out);
    else (void)UI_AppendF(out, v, it->decimals);
}

static uint8_t op_menu(const char *arg, char *body, uint16_t cap, uint8_t *has_reply)
{
    char path[CAT_CMD_MAX + 1U];
    (void)snprintf(path, sizeof(path), "%s", arg);
    char *value = strchr(path, '=');
    uint8_t query = 0U;
    if (value) *value++ = '\0';
    else {
        const size_t l = strlen(path);
        if (l && (path[l - 1U] == '?')) { path[l - 1U] = '\0'; query = 1U; }
    }
    mm_node_t n;
    if (!mm_walk(path, &n)) return 0U;
    const setting_item_t *it = mm_setting(&n);

    if (query) {
        uint32_t type = 1U, sub = 0U;
        const char *name = n.have_row ? UI_MenuRowLabel(&n.row) : UI_MenuCategoryLabel(n.cat);
        if (!n.have_row || (n.row.kind == UI_ROW_GROUP)) type = 0U;
        else if (it && UI_SettingIsDiscrete(it->style)) { type = 5U; sub = (uint32_t)it->style; }
        else if (it) { type = 3U; sub = 8U; }
        (void)snprintf(body, cap, "%lu|%lu|%s", (unsigned long)type, (unsigned long)sub, name);
        *has_reply = 1U;
        return 1U;
    }
    if (!it) return 0U; /* a sub-menu, page or action has no value: ?; as QMX */
    if (!value) {
        char v[32];
        mm_value(it, it->get(), v);
        (void)snprintf(body, cap, "%s", v);
        *has_reply = 1U;
        return 1U;
    }
    /* Set: a list takes its label or its index, a number its value. Saved,
     * like QMX's MM set, by the settings autosave. */
    if (!it->set) return 0U;
    float v = 0.0f;
    uint8_t ok = 0U;
    if (UI_SettingIsDiscrete(it->style)) {
        const int32_t count = UI_SettingDiscreteCount(it->style);
        for (int32_t i = 0; (i < count) && !ok; ++i) {
            char label[32];
            mm_value(it, (float)i, label);
            if (name_eq(value, label)) { v = (float)i; ok = 1U; }
        }
        uint8_t idx = 0U;
        if (!ok && as_index(value, &idx) && ((int32_t)idx < count)) { v = (float)idx; ok = 1U; }
    } else {
        ok = as_float(value, &v);
    }
    if (!ok || MOD_AudioRealtimeCritical(HAL_GetTick())) return 0U;
    it->set(UI_ClampSettingValue(it, v));
    *has_reply = 0U;
    return 1U;
}

static uint8_t op_menu_list(uint32_t list, char *body, uint16_t cap)
{
    if ((list > 255U) || !UI_SettingIsDiscrete((setting_style_t)list)) return 0U;
    const setting_item_t it = { .get = NULL, .set = NULL, .step = STEP_1,
                                .decimals = 0U, .style = (setting_style_t)list };
    const int32_t count = UI_SettingDiscreteCount(it.style);
    uint16_t n = 0U;
    body[0] = '\0';
    for (int32_t i = 0; i < count; ++i) {
        char label[32];
        mm_value(&it, (float)i, label);
        const uint16_t l = (uint16_t)strlen(label);
        if ((uint32_t)n + l + 2U >= cap) break;
        if (i) body[n++] = '|';
        memcpy(&body[n], label, l);
        n = (uint16_t)(n + l);
        body[n] = '\0';
    }
    return 1U;
}

static const cat_ops_t ops = {
    op_get_freq, op_set_freq, op_get_mode, op_set_mode, op_get_tx, op_set_tx,
    op_get, op_set, op_menu, op_menu_list,
    FW_VERSION_STRINGIFY(FW_VERSION_MAJOR) "_" FW_VERSION_MINOR_STRING "_000SPARTACUS",
};

/* ---- the ports ---- */

static void port_reset(cat_port_state_t *s)
{
    CAT_ParserReset(&s->parser);
    s->in_len = s->in_pos = 0U;
    s->out_len = s->out_sent = 0U;
    s->terminal = 0U;
    s->link = 0U;
}

/* A line ending: the port is the console's from here, with the line that
 * made it one -- unless the other port already is. */
static void to_terminal(uint8_t p, cat_port_state_t *s, char ending)
{
    if ((console_port != CAT_NO_PORT) && (console_port != p)) {
        CAT_ParserReset(&s->parser);
        memcpy(s->out, "?;", 2U);
        s->out_len = 2U;
        return;
    }
    hand_pos = 0U;
    hand_ending = ending;
    s->terminal = 1U;
    console_port = p;
    ++console_session;
}

static void port_service(uint8_t p, uint32_t now)
{
    cat_port_state_t *s = &ports[p];
    const uint8_t open = USBD_CatIsOpen(p);
    if (open != s->was_open) {
        s->was_open = open;
        if (console_port == p) { console_port = CAT_NO_PORT; hand_ending = 0; ++console_session; }
        if (s->link) SCTRL_LinkClosed();
        port_reset(s);
        /* The program that keyed the radio went away. */
        if (!open && (tx_port == p)) op_set_tx(0U);
    }
    if (!open) return;
    /* The LINK's port: what the host sends goes to the LINK; what goes back
     * leaves from the USB interrupt (sctrl_link.c), never from here. */
    if (s->link) {
        for (uint8_t packets = 0U; packets < 4U; ++packets) {
            uint8_t in[USBD_CAT_OUT_PACKET];
            const uint16_t n = USBD_CatReceive(p, in, sizeof(in));
            if (!n) break;
            SCTRL_LinkRx(in, n);
        }
        return;
    }

    /* Replies first -- also the last ones a port owed before it became a
     * terminal, which the console waits for (CAT_ConsoleTxReady). */
    if ((s->out_sent < s->out_len) && USBD_CatTxReady(p)) {
        const uint16_t n = USBD_CatTransmit(p, (const uint8_t *)&s->out[s->out_sent],
                                            (uint16_t)(s->out_len - s->out_sent));
        s->out_sent = (uint16_t)(s->out_sent + n);
    }
    if (s->out_sent >= s->out_len) s->out_len = s->out_sent = 0U;
    if (s->terminal) return;

    for (uint8_t packets = 0U; packets < 4U; ++packets) {
        if (s->in_pos >= s->in_len) {
            const uint16_t n = USBD_CatReceive(p, s->in, sizeof(s->in));
            if (!n) return;
            s->in_len = (uint8_t)n;
            s->in_pos = 0U;
        }
        while (s->in_pos < s->in_len) {
            if (s->out_len) return; /* the host reads the last reply first */
            const char c = (char)s->in[s->in_pos++];
            uint16_t rl = 0U;
            current_port = p;
            const cat_push_t k = CAT_ParserPush(&s->parser, &ops, c, s->out, CAT_OUT_MAX, &rl);
            if (k == CAT_PUSH_TERMINAL) { to_terminal(p, s, c); return; }
            if (k != CAT_PUSH_DONE) continue;
            last_cmd_ms = now;
            s->last_ms = now;
            ++command_count;
            s->out_len = rl;
        }
    }
}

void CAT_Service(uint32_t now)
{
    service_now = now;
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) port_service(p, now);
    md_service(now);
    /* QMX's CAT timeout: a transmit no CAT command has followed for this
     * long ends by itself (WSPR's 110.6 s over fits the default). Commands
     * from the port that keyed count, not another program polling the other
     * port or the S-CTRL LINK's tunnel: either could otherwise hold a hung
     * program's transmission open. */
    const uint32_t last = (tx_port < USBD_CAT_PORTS) ? ports[tx_port].last_ms : last_cmd_ms;
    if (MOD_GetUsbCatPtt() && timeout_on && ((uint32_t)(now - last) >= (uint32_t)timeout_s * 1000U)) op_set_tx(0U);
}

/* ---- the console's side ---- */

uint8_t CAT_ConsoleSession(void) { return console_session; }

uint8_t CAT_ConsoleOpen(void)
{
    return (uint8_t)((console_port != CAT_NO_PORT) && USBD_CatIsOpen(console_port));
}

uint8_t CAT_ConsoleTxReady(void)
{
    return (uint8_t)(CAT_ConsoleOpen() && (ports[console_port].out_len == 0U) && USBD_CatTxReady(console_port));
}

uint16_t CAT_ConsoleTransmit(const uint8_t *data, uint16_t length)
{
    return CAT_ConsoleTxReady() ? USBD_CatTransmit(console_port, data, length) : 0U;
}

uint16_t CAT_ConsoleReceive(uint8_t *out, uint16_t cap)
{
    if (!CAT_ConsoleOpen() || !out || !cap) return 0U;
    cat_port_state_t *s = &ports[console_port];
    uint16_t n = 0U;
    while ((n < cap) && (hand_pos < s->parser.len)) out[n++] = (uint8_t)s->parser.cmd[hand_pos++];
    if ((n < cap) && hand_ending) { out[n++] = (uint8_t)hand_ending; hand_ending = 0; }
    while ((n < cap) && (s->in_pos < s->in_len)) out[n++] = s->in[s->in_pos++];
    return n ? n : USBD_CatReceive(console_port, out, cap);
}

uint32_t CAT_ConsoleRxStamp(void)
{
    return (console_port != CAT_NO_PORT) ? USBD_CatRxStamp(console_port) : 0U;
}

void CAT_Status(uint8_t *open_mask, uint8_t *port, uint32_t *commands, uint8_t *ptt)
{
    uint8_t mask = 0U;
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) if (USBD_CatIsOpen(p)) mask = (uint8_t)(mask | (1U << p));
    *open_mask = mask;
    *port = console_port;
    *commands = command_count;
    *ptt = MOD_GetUsbCatPtt();
}

/* ---- the S-CTRL LINK (sctrl_link.c) ---- */

uint8_t CAT_PttPort(void) { return tx_port; }

uint16_t CAT_PortAgeDs(uint8_t port, uint32_t now)
{
    if (port >= USBD_CAT_PORTS || !ports[port].last_ms) return 0xFFFFU;
    const uint32_t ds = (uint32_t)(now - ports[port].last_ms) / 100U;
    return (uint16_t)(ds > 0xFFFFU ? 0xFFFFU : ds);
}

/* The console's port becomes the LINK's: its parser and reply buffer serve
 * the LINK's CAT tunnel from now on. */
void CAT_LinkClaim(uint8_t port)
{
    if (port >= USBD_CAT_PORTS) return;
    cat_port_state_t *s = &ports[port];
    CAT_ParserReset(&s->parser);
    s->in_len = s->in_pos = 0U;
    s->out_len = s->out_sent = 0U;
    s->link = 1U;
}

/* LINK_OFF: a fresh CAT port again, as if just opened. */
void CAT_LinkRelease(uint8_t port)
{
    if (port >= USBD_CAT_PORTS) return;
    if (console_port == port) { console_port = CAT_NO_PORT; hand_ending = 0; ++console_session; }
    port_reset(&ports[port]);
}

/* One CAT command from the LINK, run as its port: 1 when a reply (maybe
 * empty) waits in CAT_LinkReply. */
uint8_t CAT_LinkRun(uint8_t port, const uint8_t *cmd, uint8_t len)
{
    if (port >= USBD_CAT_PORTS) return 0U;
    cat_port_state_t *s = &ports[port];
    CAT_ParserReset(&s->parser);
    s->out_len = 0U;
    current_port = port;
    for (uint8_t i = 0U; i < len; ++i) {
        uint16_t rl = 0U;
        const cat_push_t k = CAT_ParserPush(&s->parser, &ops, (char)cmd[i], s->out, CAT_OUT_MAX, &rl);
        if (k == CAT_PUSH_DONE) {
            s->last_ms = service_now;
            ++command_count;
            s->out_len = rl;
            break;
        }
        if (k == CAT_PUSH_TERMINAL) break;
    }
    CAT_ParserReset(&s->parser);
    return 1U;
}

const char *CAT_LinkReply(uint8_t port, uint16_t *len)
{
    *len = (port < USBD_CAT_PORTS) ? ports[port].out_len : 0U;
    return (port < USBD_CAT_PORTS) ? ports[port].out : "";
}

void CAT_LinkReplyDone(uint8_t port)
{
    if (port < USBD_CAT_PORTS) ports[port].out_len = 0U;
}

#pragma GCC optimize ("Os") /* off the real-time paths: built for size */

#include "usb_diag_keys.h"
#include <string.h>

enum { TERM_GROUND = 0, TERM_ESC, TERM_CSI, TERM_SS3 };

void TERM_DecoderReset(term_decoder_t *t)
{
    memset(t, 0, sizeof(*t));
}

static uint32_t take_number(const char **s)
{
    uint32_t v = 0U;
    for (; **s >= '0' && **s <= '9'; ++*s)
        if (v < 100000000U) v = v * 10U + (uint32_t)(**s - '0');
    return v;
}

/* CSI parameters are in t->seq; `final` ends the sequence. */
static uint8_t csi_event(term_decoder_t *t, uint8_t final, term_event_t *ev)
{
    const char *s = t->seq;
    t->seq[t->len] = '\0';
    if (t->overflow) return 0U;
    if (!t->len && final == 'I') { ev->kind = TERM_EV_FOCUS_IN; return 1U; }
    if (!t->len && final == 'O') { ev->kind = TERM_EV_FOCUS_OUT; return 1U; }
    if (final != 'u') return 0U;   /* cursor and function keys: nothing to do */
    if (*s == '?') {
        ++s;
        ev->kind = TERM_EV_KITTY_FLAGS;
        ev->code = take_number(&s);
        return 1U;
    }
    if (*s < '0' || *s > '9') return 0U;
    /* code[:shifted[:base]] [; mods[:action] [; text]] */
    ev->code = take_number(&s);
    while (*s == ':') { ++s; (void)take_number(&s); }
    uint32_t mods = 1U, action = TERM_KEY_PRESS;
    if (*s == ';') {
        ++s;
        if (*s >= '0' && *s <= '9') mods = take_number(&s);
        if (*s == ':') { ++s; action = take_number(&s); }
    }
    ev->kind = TERM_EV_KEY;
    ev->mods = (uint8_t)((mods ? mods - 1U : 0U) & 0xFFU);
    ev->action = (uint8_t)((action >= TERM_KEY_PRESS && action <= TERM_KEY_RELEASE)
                           ? action : TERM_KEY_PRESS);
    return 1U;
}

uint8_t TERM_Decode(term_decoder_t *t, uint8_t byte, term_event_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->action = TERM_KEY_PRESS;
    switch (t->state) {
        case TERM_ESC:
            if (byte == '[') {
                t->state = TERM_CSI;
                t->len = 0U;
                t->overflow = 0U;
                return 0U;
            }
            if (byte == 'O') { t->state = TERM_SS3; return 0U; }
            /* A lone ESC (the Esc key, or Alt with a key): drop the ESC and
             * let the byte through as itself. */
            t->state = TERM_GROUND;
            break;
        case TERM_SS3:
            /* ESC O x: application-mode cursor and F1-F4 keys. */
            t->state = TERM_GROUND;
            return 0U;
        case TERM_CSI:
            if (byte >= 0x40U && byte <= 0x7EU) {
                t->state = TERM_GROUND;
                return csi_event(t, byte, ev);
            }
            if (byte >= 0x20U && byte <= 0x3FU) {
                if (t->len + 1U < TERM_SEQ_MAX) t->seq[t->len++] = (char)byte;
                else t->overflow = 1U;
                return 0U;
            }
            /* Any other control byte cancels the sequence (ECMA-48 CAN/SUB);
             * ESC starts a new one. */
            t->state = (byte == 0x1BU) ? (uint8_t)TERM_ESC : (uint8_t)TERM_GROUND;
            return 0U;
        default:
            break;
    }
    if (byte == 0x1BU) { t->state = TERM_ESC; return 0U; }
    ev->kind = TERM_EV_CHAR;
    ev->ch = byte;
    return 1U;
}

uint8_t TERM_KeyChar(const term_event_t *ev)
{
    if (ev->kind == TERM_EV_CHAR) return ev->ch;
    if (ev->kind != TERM_EV_KEY || ev->action == TERM_KEY_RELEASE) return 0U;
    const uint32_t c = ev->code;
    if (c == 8U || c == 9U || c == 13U || c == 27U || c == 127U) return (uint8_t)c;
    if (c < 32U || c > 126U) return 0U;   /* functional keys, modifiers, non-ASCII */
    if (ev->mods & TERM_MOD_CTRL) {
        if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 1U);
        if (c >= '@' && c <= '_') return (uint8_t)(c - '@');
    }
    if ((ev->mods & TERM_MOD_SHIFT) && c >= 'a' && c <= 'z') return (uint8_t)(c - 32U);
    return (uint8_t)c;
}

/* ---- line editor ---- */

void TERM_LineReset(term_line_t *l, const char *greeting)
{
    memset(l, 0, sizeof(*l));
    l->greeting = greeting;
}

void TERM_LineClear(term_line_t *l)
{
    l->len = 0U;
    l->overflow = 0U;
}

static void echo_n(term_line_t *l, const char *text, uint16_t n)
{
    if (!l->interactive) return;
    const uint16_t room = (uint16_t)(sizeof(l->echo) - l->echo_len);
    if (n > room) n = room;
    memcpy(&l->echo[l->echo_len], text, n);
    l->echo_len = (uint16_t)(l->echo_len + n);
}

static void echo_s(term_line_t *l, const char *text)
{
    echo_n(l, text, (uint16_t)strlen(text));
}

uint8_t TERM_LineInput(term_line_t *l, uint8_t ch, uint8_t reply_busy)
{
    /* CR LF from a terminal program is one Enter, not an Enter and a blank line. */
    const uint8_t after_cr = l->last_cr;
    l->last_cr = (uint8_t)(ch == '\r');
    if (ch == '\n' && after_cr) return TERM_LINE_MORE;
    if (ch == '\r' || ch == '\n') {
        if (ch == '\r' && !l->interactive) {
            l->interactive = 1U;
            echo_n(l, l->text, l->len);
            if (!l->len && !l->overflow && l->greeting) {
                echo_s(l, "\r\n");
                echo_s(l, l->greeting);
                echo_s(l, "\r\n> ");
                return TERM_LINE_MORE;
            }
        }
        if (l->len || l->overflow) {
            echo_s(l, "\r\n");
            return TERM_LINE_READY;
        }
        echo_s(l, "\r\n> ");
        return TERM_LINE_MORE;
    }
    if (ch == 0x7FU || ch == 0x08U) {
        /* Backspace: PuTTY sends DEL, others BS. Only people press it. */
        l->interactive = 1U;
        if (l->len) {
            l->len--;
            echo_s(l, "\b \b");
        }
        return TERM_LINE_MORE;
    }
    if (ch == 0x03U || ch == 0x15U) {
        TERM_LineClear(l);
        if (ch == 0x03U && reply_busy) return TERM_LINE_INTERRUPT;
        echo_s(l, ch == 0x03U ? "^C\r\n> " : "^U\r\n> ");
        return TERM_LINE_MORE;
    }
    if (ch < 0x20U) return TERM_LINE_MORE;   /* tab, XON/XOFF and the rest of C0 */
    if (l->len + 1U >= TERM_LINE_MAX) { l->overflow = 1U; return TERM_LINE_MORE; }
    l->text[l->len++] = (char)ch;
    echo_n(l, (const char *)&ch, 1U);
    return TERM_LINE_MORE;
}

/* ---- virtual paddle ---- */

void VPADDLE_Reset(vpaddle_t *p)
{
    memset(p, 0, sizeof(*p));
}

static uint8_t valid_lever(uint8_t lever)
{
    return (uint8_t)(lever == VPADDLE_DIT || lever == VPADDLE_DAH);
}

void VPADDLE_Key(vpaddle_t *p, uint8_t lever, uint8_t action, uint32_t now)
{
    if (!valid_lever(lever)) return;
    if (action == TERM_KEY_RELEASE) {
        p->held &= (uint8_t)~lever;
        p->capped &= (uint8_t)~lever;
        return;
    }
    /* Only a fresh press rearms a lever the hold cap dropped; its autorepeat
     * does not. */
    if (action == TERM_KEY_PRESS) p->capped &= (uint8_t)~lever;
    if ((p->capped & lever) || (p->held & lever)) return;
    p->held |= lever;
    p->held_since[lever == VPADDLE_DAH ? 1U : 0U] = now;
}

void VPADDLE_Tap(vpaddle_t *p, uint8_t lever, uint32_t now)
{
    (void)now;
    if (!valid_lever(lever)) return;
    /* One press may wait behind the one in progress; more would only run on
     * after the key is let go. Two can arrive in one USB packet before the
     * first has started, so the slot counts the one in progress. */
    if ((uint8_t)(p->npending + (p->tap ? 1U : 0U)) < 2U) p->pending[p->npending++] = lever;
}

uint8_t VPADDLE_Service(vpaddle_t *p, uint32_t now, const vpaddle_keyer_t *k)
{
    for (uint8_t i = 0U; i < 2U; ++i) {
        const uint8_t lever = (uint8_t)(1U << i);
        if ((p->held & lever) && (uint32_t)(now - p->held_since[i]) >= VPADDLE_HOLD_CAP_MS) {
            p->held &= (uint8_t)~lever;
            p->capped |= lever;
        }
    }
    const uint32_t dit_ms = k->dit_ms ? k->dit_ms : 1U;
    if (p->tap) {
        /* A keyer takes the lever when it starts the element, and the element
         * then runs to its end on its own. A straight key has no elements:
         * the tap is the key-down, a dit or a dah long. */
        const uint32_t limit = k->straight ? (p->tap == VPADDLE_DAH ? 3U : 1U) * dit_ms
                                           : VPADDLE_TAP_TIMEOUT_MS;
        const uint8_t taken = (uint8_t)(!k->straight && k->elements != p->tap_elements);
        if (taken || (uint32_t)(now - p->tap_since) >= limit) {
            p->tap = 0U;
            p->tap_released_at = now;
            /* Two straight-key taps in a row are two key-downs, not one long
             * one: leave a dit of key-up between them. */
            p->tap_gap_ms = (uint16_t)(k->straight ? (dit_ms > 0xFFFFU ? 0xFFFFU : dit_ms) : 0U);
        }
    }
    if (!p->tap && p->npending && (uint32_t)(now - p->tap_released_at) >= p->tap_gap_ms) {
        p->tap = p->pending[0];
        p->pending[0] = p->pending[1];
        p->npending--;
        p->tap_since = now;
        p->tap_elements = k->elements;
    }
    return (uint8_t)(p->held | p->tap);
}

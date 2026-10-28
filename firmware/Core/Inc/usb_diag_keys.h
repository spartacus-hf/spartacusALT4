#ifndef USB_DIAG_KEYS_H
#define USB_DIAG_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Keyboard input for the USB console, and the virtual paddle it drives.
 *
 * No HAL here: the host tests feed this the exact bytes PuTTY, minicom and a
 * kitty-protocol terminal send, and usb_diag.c only moves bytes in and levers
 * out.
 *
 * The decoder takes one byte at a time. Plain bytes come out as characters;
 * escape sequences are consumed whole, so an arrow key (ESC [ A) can never
 * reach a command as the letter A. The sequences worth something are decoded:
 * kitty keyboard protocol key events (CSI code ; mods : action u), its flag
 * report (CSI ? flags u) and focus reports (CSI I, CSI O). */

typedef enum {
    TERM_EV_CHAR = 1,      /* a plain byte, printable or control */
    TERM_EV_KEY,           /* a kitty key event */
    TERM_EV_FOCUS_IN,
    TERM_EV_FOCUS_OUT,
    TERM_EV_KITTY_FLAGS    /* reply to CSI ? u */
} term_event_kind_t;

#define TERM_KEY_PRESS    1U
#define TERM_KEY_REPEAT   2U
#define TERM_KEY_RELEASE  3U

/* Kitty modifier bits, after the protocol's +1 offset is removed. */
#define TERM_MOD_SHIFT  0x01U
#define TERM_MOD_CTRL   0x04U

typedef struct {
    uint8_t  kind;         /* term_event_kind_t */
    uint8_t  action;       /* TERM_KEY_*; press for everything but TERM_EV_KEY */
    uint8_t  mods;         /* TERM_MOD_* bits */
    uint8_t  ch;           /* TERM_EV_CHAR byte */
    uint32_t code;         /* TERM_EV_KEY unicode key code, TERM_EV_KITTY_FLAGS flags */
} term_event_t;

#define TERM_SEQ_MAX 24U

typedef struct {
    uint8_t state;
    uint8_t len;
    uint8_t overflow;
    char    seq[TERM_SEQ_MAX];
} term_decoder_t;

void    TERM_DecoderReset(term_decoder_t *t);
/* Returns 1 and fills *ev when the byte completes something, 0 while a
 * sequence is still arriving or when it was one nobody here needs. */
uint8_t TERM_Decode(term_decoder_t *t, uint8_t byte, term_event_t *ev);
/* The character a key press stands for, the way a plain terminal would have
 * sent it (Ctrl folds letters to control codes), or 0 for keys that have none. */
uint8_t TERM_KeyChar(const term_event_t *ev);

/* ---- line editor ----
 *
 * The typing side of the line protocol, for scripts and people alike. A
 * script ends its lines with LF and gets nothing back but the reply. A
 * terminal program sends CR for Enter (PuTTY, minicom, picocom, screen): the
 * first CR makes the session interactive, like QMX entering its terminal
 * applications on Enter, and from then on every key is echoed, Backspace
 * (DEL or BS) rubs out, Ctrl-U drops the line, Ctrl-C drops it or stops a
 * reply, and an empty Enter prompts again. What was typed before that first
 * Enter is echoed with it, and an empty first Enter shows the greeting.
 *
 * The echo stream is the caller's to send, in order, before anything it
 * sends later; a command's own prompt is the caller's too, after its reply. */

#define TERM_LINE_MAX 120U

typedef struct {
    char        text[TERM_LINE_MAX];   /* the line so far; not NUL-terminated */
    uint16_t    len;
    uint8_t     overflow;              /* the line outgrew text; it will be refused */
    uint8_t     interactive;
    uint8_t     last_cr;
    uint16_t    echo_len;
    char        echo[TERM_LINE_MAX];
    const char *greeting;
} term_line_t;

#define TERM_LINE_MORE       0U
#define TERM_LINE_READY      1U   /* a line is complete (text, len, overflow) */
#define TERM_LINE_INTERRUPT  2U   /* Ctrl-C while a reply was still coming */

void    TERM_LineReset(term_line_t *l, const char *greeting);
/* One plain byte (escape sequences already removed by TERM_Decode).
 * reply_busy: a reply is still being sent, so Ctrl-C interrupts it. */
uint8_t TERM_LineInput(term_line_t *l, uint8_t ch, uint8_t reply_busy);
/* The ready line has been run: start the next one. */
void    TERM_LineClear(term_line_t *l);

/* ---- virtual paddle ----
 *
 * Two ways in, because terminals differ in what they report:
 *
 *  - hold: the terminal reports key releases (kitty keyboard protocol with
 *    event types). A lever is down from press to release, exactly as long as
 *    the key is held, so the keyer sees squeezes and held levers as it would
 *    from the KEY jack.
 *  - tap: the terminal sends a character per key press and nothing on release
 *    (PuTTY, xterm, minicom). Each press becomes one element: the lever stays
 *    down until the keyer has started an element, then lets go. A held key
 *    autorepeats into a stream after the OS repeat delay. One more press can
 *    wait behind the current one, so letting go of a held key stops the stream
 *    within an element or two.
 *
 * A lever never stays down on its own: focus loss, the page closing or the
 * port going away release everything, and a hold that lasts VPADDLE_HOLD_CAP_MS
 * is dropped until the key is pressed again. */

#define VPADDLE_DIT 0x01U
#define VPADDLE_DAH 0x02U

#define VPADDLE_HOLD_CAP_MS     10000U
/* A tap nobody takes (not in CW, keyer busy with something else) lets go
 * after this long. */
#define VPADDLE_TAP_TIMEOUT_MS   1500U

typedef struct {
    uint8_t  held;              /* hold-mode levers */
    uint8_t  capped;            /* held past the cap; ignored until pressed again */
    uint8_t  tap;               /* tap-mode lever currently down, 0 when none */
    uint8_t  pending[2];        /* presses not yet started, oldest first */
    uint8_t  npending;
    uint32_t held_since[2];
    uint32_t tap_since;
    uint32_t tap_elements;      /* keyer element count when the tap went down */
    uint32_t tap_released_at;
    uint16_t tap_gap_ms;        /* straight key: key-up before the next tap */
} vpaddle_t;

typedef struct {
    uint32_t elements;          /* keyer elements started so far (wraps) */
    uint8_t  straight;          /* straight key: a tap is a timed key-down */
    uint16_t dit_ms;            /* straight key: one dit at the keyer speed */
} vpaddle_keyer_t;

void    VPADDLE_Reset(vpaddle_t *p);
/* Hold mode: one kitty key event for a lever. */
void    VPADDLE_Key(vpaddle_t *p, uint8_t lever, uint8_t action, uint32_t now);
/* Tap mode: one key press for a lever. */
void    VPADDLE_Tap(vpaddle_t *p, uint8_t lever, uint32_t now);
/* Advance the timers and return the levers that are down now. */
uint8_t VPADDLE_Service(vpaddle_t *p, uint32_t now, const vpaddle_keyer_t *k);

#ifdef __cplusplus
}
#endif

#endif

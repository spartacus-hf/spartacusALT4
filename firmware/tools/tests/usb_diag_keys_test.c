#include "usb_diag_keys.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

/* Feed a byte string; collect up to `cap` events. */
static unsigned feed(term_decoder_t *t, const char *bytes, size_t n, term_event_t *out, unsigned cap)
{
    unsigned got = 0U;
    for (size_t i = 0U; i < n; ++i) {
        term_event_t ev;
        if (TERM_Decode(t, (uint8_t)bytes[i], &ev) && got < cap) out[got++] = ev;
    }
    return got;
}

#define FEED(t, lit, out) feed((t), (lit), sizeof(lit) - 1U, (out), (unsigned)(sizeof(out) / sizeof((out)[0])))

/* The bytes a person's terminal sends must reach the console as the keys they
 * mean, and escape sequences must never leak out as letters. */
static void test_plain_terminals(void)
{
    term_decoder_t t;
    term_event_t ev[16];
    TERM_DecoderReset(&t);

    /* PuTTY: Enter is CR, Backspace is DEL. */
    unsigned n = FEED(&t, "id\r\x7f", ev);
    CHECK(n == 4U && ev[0].ch == 'i' && ev[2].ch == '\r' && ev[3].ch == 0x7FU,
          "plain bytes: got %u events", n);

    /* Arrow keys in normal and application cursor mode, F1, F5 and Home:
     * nothing comes out. The up arrow used to reach the full-screen view as
     * 'A', which started the self test. */
    n = FEED(&t, "\x1b[A\x1bOA\x1bOP\x1b[15~\x1b[1;5H", ev);
    CHECK(n == 0U, "cursor/function keys leaked %u events", n);

    /* Esc on its own, then a key: the key survives. */
    n = FEED(&t, "\x1bq", ev);
    CHECK(n == 1U && ev[0].kind == TERM_EV_CHAR && ev[0].ch == 'q', "lone ESC swallowed the key");

    /* Two ESCs, then a sequence: the second ESC starts the sequence. */
    n = FEED(&t, "\x1b\x1b[B.", ev);
    CHECK(n == 1U && ev[0].ch == '.', "ESC ESC [ B . gave %u events", n);

    /* An over-long sequence is dropped whole. */
    n = FEED(&t, "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13uX", ev);
    CHECK(n == 1U && ev[0].ch == 'X', "over-long CSI: %u events", n);

    /* CAN inside a sequence cancels it; the next byte is plain again. */
    n = FEED(&t, "\x1b[12\x18" "a", ev);
    CHECK(n == 1U && ev[0].ch == 'a', "CAN did not cancel the sequence");
}

/* What alacritty sends with CSI > 11 u pushed (disambiguate, event types,
 * all keys as escape codes). */
static void test_kitty_events(void)
{
    term_decoder_t t;
    term_event_t ev[16];
    TERM_DecoderReset(&t);

    unsigned n = FEED(&t, "\x1b[44u\x1b[44;1:2u\x1b[44;1:3u", ev);
    CHECK(n == 3U, "comma press/repeat/release: %u events", n);
    CHECK(ev[0].kind == TERM_EV_KEY && ev[0].code == 44U && ev[0].action == TERM_KEY_PRESS &&
          ev[0].mods == 0U, "comma press decoded wrong");
    CHECK(ev[1].action == TERM_KEY_REPEAT && ev[2].action == TERM_KEY_RELEASE, "event types wrong");

    /* Caps lock on (bit 64) and alternate keys present. */
    n = FEED(&t, "\x1b[46:62:46;65:1u", ev);
    CHECK(n == 1U && ev[0].code == 46U && ev[0].mods == 64U && ev[0].action == TERM_KEY_PRESS,
          "period with alternates and caps lock decoded wrong");

    /* Ctrl-C and shifted q fold to what a plain terminal sends. */
    n = FEED(&t, "\x1b[99;5u\x1b[113;2u\x1b[113;1:3u", ev);
    CHECK(n == 3U && TERM_KeyChar(&ev[0]) == 0x03U && TERM_KeyChar(&ev[1]) == 'Q' &&
          TERM_KeyChar(&ev[2]) == 0U, "key characters wrong");

    /* Left Ctrl on its own, Enter, and an empty modifier field with text. */
    n = FEED(&t, "\x1b[57442;5u\x1b[13u\x1b[97;;97u", ev);
    CHECK(n == 3U && TERM_KeyChar(&ev[0]) == 0U && TERM_KeyChar(&ev[1]) == '\r' &&
          TERM_KeyChar(&ev[2]) == 'a', "modifier / enter / text field wrong");

    /* Flag report and focus. */
    n = FEED(&t, "\x1b[?11u\x1b[O\x1b[I", ev);
    CHECK(n == 3U && ev[0].kind == TERM_EV_KITTY_FLAGS && ev[0].code == 11U &&
          ev[1].kind == TERM_EV_FOCUS_OUT && ev[2].kind == TERM_EV_FOCUS_IN, "flags/focus wrong");

    /* Release of an arrow key in kitty form is still not a character. */
    n = FEED(&t, "\x1b[1;1:3A", ev);
    CHECK(n == 0U, "arrow release leaked");
}

/* A console session in miniature: bytes in through the decoder and the line
 * editor, echo collected, finished lines collected as the console would run
 * them. */
typedef struct {
    term_decoder_t dec;
    term_line_t ed;
    char echo[512];
    size_t echo_len;
    char lines[8][TERM_LINE_MAX + 1];
    unsigned nlines;
    unsigned interrupts;
    unsigned overflowed;
} session_t;

static void session_open(session_t *s)
{
    memset(s, 0, sizeof(*s));
    TERM_DecoderReset(&s->dec);
    TERM_LineReset(&s->ed, "GREETING");
}

static void session_feed(session_t *s, const char *bytes, size_t n, uint8_t busy)
{
    for (size_t i = 0U; i < n; ++i) {
        term_event_t ev;
        if (!TERM_Decode(&s->dec, (uint8_t)bytes[i], &ev) || ev.kind != TERM_EV_CHAR) continue;
        const uint8_t r = TERM_LineInput(&s->ed, ev.ch, busy);
        if (r == TERM_LINE_READY) {
            if (s->ed.overflow) s->overflowed++;
            else if (s->nlines < 8U) {
                memcpy(s->lines[s->nlines], s->ed.text, s->ed.len);
                s->lines[s->nlines++][s->ed.len] = '\0';
            }
            TERM_LineClear(&s->ed);
        } else if (r == TERM_LINE_INTERRUPT) {
            s->interrupts++;
        }
        /* The console sends the echo as it goes. */
        memcpy(&s->echo[s->echo_len], s->ed.echo, s->ed.echo_len);
        s->echo_len += s->ed.echo_len;
        s->ed.echo_len = 0U;
    }
    s->echo[s->echo_len] = '\0';
}

#define SEND(s, lit) session_feed((s), (lit), sizeof(lit) - 1U, 0U)

static void check_echo(const session_t *s, const char *expected, const char *what)
{
    CHECK(strcmp(s->echo, expected) == 0, "%s: echo \"%s\", expected \"%s\"", what, s->echo, expected);
}

static void test_line_editor(void)
{
    session_t s;

    /* rev1_diag.py: LF-terminated lines, no echo, two commands in one write. */
    session_open(&s);
    SEND(&s, "id\nset get 42\n");
    CHECK(s.nlines == 2U && !strcmp(s.lines[0], "id") && !strcmp(s.lines[1], "set get 42"),
          "script lines wrong (%u)", s.nlines);
    check_echo(&s, "", "script");
    CHECK(!s.ed.interactive, "a script became interactive");

    /* PuTTY, first thing: Enter on its own greets and prompts. */
    session_open(&s);
    SEND(&s, "\r");
    check_echo(&s, "\r\nGREETING\r\n> ", "first Enter");
    CHECK(s.nlines == 0U && s.ed.interactive, "first Enter ran something");

    /* Then a command with a typo fixed by Backspace (PuTTY sends DEL). */
    s.echo_len = 0U;
    SEND(&s, "ix\x7f" "d\r");
    check_echo(&s, "ix\b \bd\r\n", "typing with backspace");
    CHECK(s.nlines == 1U && !strcmp(s.lines[0], "id"), "edited line wrong");

    /* An empty Enter prompts again; Backspace on an empty line does nothing. */
    s.echo_len = 0U;
    SEND(&s, "\x7f\r");
    check_echo(&s, "\r\n> ", "empty Enter");

    /* A command typed blind before the first Enter is shown with it. */
    session_open(&s);
    SEND(&s, "help\r");
    check_echo(&s, "help\r\n", "blind first command");
    CHECK(s.nlines == 1U && !strcmp(s.lines[0], "help"), "blind command lost");

    /* CR LF (miniterm, Windows terminals) is one Enter. */
    session_open(&s);
    SEND(&s, "id\r\n\r\n");
    CHECK(s.nlines == 1U, "CR LF ran %u lines", s.nlines);
    check_echo(&s, "id\r\n\r\n> ", "CR LF");

    /* Arrow keys, XON/XOFF and Tab never reach the line. */
    session_open(&s);
    SEND(&s, "\ri\x1b[A\x1b[D\x11\x13\td\r");
    CHECK(s.nlines == 1U && !strcmp(s.lines[0], "id"), "keys leaked into the line: \"%s\"", s.lines[0]);

    /* Ctrl-U drops the line; Ctrl-C drops it too, or interrupts a reply. */
    session_open(&s);
    SEND(&s, "\rabc\x15");
    CHECK(s.ed.len == 0U, "Ctrl-U kept the line");
    CHECK(strstr(s.echo, "abc^U\r\n> ") != NULL, "Ctrl-U echo wrong: \"%s\"", s.echo);
    SEND(&s, "xy\x03");
    CHECK(s.ed.len == 0U && strstr(s.echo, "xy^C\r\n> ") != NULL, "Ctrl-C idle wrong");
    s.echo_len = 0U;
    session_feed(&s, "\x03", 1U, 1U);
    CHECK(s.interrupts == 1U && s.echo_len == 0U, "Ctrl-C during a reply did not interrupt");

    /* A line longer than the buffer is refused whole, never cut and run. */
    session_open(&s);
    char longline[TERM_LINE_MAX + 20U];
    memset(longline, 'a', sizeof(longline));
    longline[sizeof(longline) - 1U] = '\n';
    session_feed(&s, longline, sizeof(longline), 0U);
    CHECK(s.overflowed == 1U && s.nlines == 0U, "overflow not refused");
    SEND(&s, "id\n");
    CHECK(s.nlines == 1U && !strcmp(s.lines[0], "id"), "line after overflow wrong");

    /* The echo never runs past its buffer, however much is typed blind. */
    session_open(&s);
    session_feed(&s, longline, sizeof(longline) - 1U, 0U);
    SEND(&s, "\r");
    CHECK(s.echo_len <= TERM_LINE_MAX, "echo overran: %zu", s.echo_len);
}

static void test_hold(void)
{
    vpaddle_t p;
    const vpaddle_keyer_t k = {0U, 0U, 60U};
    VPADDLE_Reset(&p);

    VPADDLE_Key(&p, VPADDLE_DIT, TERM_KEY_PRESS, 100U);
    CHECK(VPADDLE_Service(&p, 101U, &k) == VPADDLE_DIT, "dit not down after press");
    VPADDLE_Key(&p, VPADDLE_DAH, TERM_KEY_PRESS, 150U);
    CHECK(VPADDLE_Service(&p, 151U, &k) == (VPADDLE_DIT | VPADDLE_DAH), "squeeze not both levers");
    VPADDLE_Key(&p, VPADDLE_DIT, TERM_KEY_REPEAT, 800U);
    VPADDLE_Key(&p, VPADDLE_DIT, TERM_KEY_RELEASE, 900U);
    CHECK(VPADDLE_Service(&p, 901U, &k) == VPADDLE_DAH, "release did not lift dit");

    /* The hold cap drops a lever, a repeat does not bring it back, a press does. */
    CHECK(VPADDLE_Service(&p, 150U + VPADDLE_HOLD_CAP_MS, &k) == 0U, "hold cap did not release");
    VPADDLE_Key(&p, VPADDLE_DAH, TERM_KEY_REPEAT, 150U + VPADDLE_HOLD_CAP_MS + 40U);
    CHECK(VPADDLE_Service(&p, 150U + VPADDLE_HOLD_CAP_MS + 41U, &k) == 0U, "repeat rearmed a capped lever");
    VPADDLE_Key(&p, VPADDLE_DAH, TERM_KEY_PRESS, 20000U);
    CHECK(VPADDLE_Service(&p, 20001U, &k) == VPADDLE_DAH, "press did not rearm");

    /* Across the 32-bit tick wrap. */
    VPADDLE_Reset(&p);
    VPADDLE_Key(&p, VPADDLE_DIT, TERM_KEY_PRESS, 0xFFFFFF00U);
    CHECK(VPADDLE_Service(&p, 0x00000010U, &k) == VPADDLE_DIT, "hold dropped across tick wrap");
    CHECK(VPADDLE_Service(&p, 0xFFFFFF00U + VPADDLE_HOLD_CAP_MS, &k) == 0U, "cap missed across wrap");
}

static void test_tap_keyer(void)
{
    vpaddle_t p;
    vpaddle_keyer_t k = {7U, 0U, 60U};
    VPADDLE_Reset(&p);

    /* One press: the lever goes down and stays down until the keyer starts
     * an element, however long that takes. */
    VPADDLE_Tap(&p, VPADDLE_DIT, 0U);
    CHECK(VPADDLE_Service(&p, 0U, &k) == VPADDLE_DIT, "tap lever not down");
    CHECK(VPADDLE_Service(&p, 200U, &k) == VPADDLE_DIT, "tap let go before the keyer took it");
    k.elements++;
    CHECK(VPADDLE_Service(&p, 201U, &k) == 0U, "tap still down after the element started");
    CHECK(VPADDLE_Service(&p, 400U, &k) == 0U, "tap came back");

    /* A dah pressed during that dit waits, then takes over at once. */
    VPADDLE_Tap(&p, VPADDLE_DIT, 500U);
    VPADDLE_Tap(&p, VPADDLE_DAH, 510U);
    CHECK(VPADDLE_Service(&p, 511U, &k) == VPADDLE_DIT, "first tap not first");
    k.elements++;
    CHECK(VPADDLE_Service(&p, 520U, &k) == VPADDLE_DAH, "queued dah not next");
    /* A third press while one is queued and one is down is dropped: holding
     * a key must not leave a long run-on after it is let go. */
    VPADDLE_Tap(&p, VPADDLE_DIT, 521U);
    VPADDLE_Tap(&p, VPADDLE_DIT, 522U);
    k.elements++;
    CHECK(VPADDLE_Service(&p, 600U, &k) == VPADDLE_DIT, "queued dit missing");
    k.elements++;
    CHECK(VPADDLE_Service(&p, 700U, &k) == 0U, "dropped tap ran on");

    /* Nobody takes it (not in CW): it lets go by itself. */
    VPADDLE_Tap(&p, VPADDLE_DAH, 1000U);
    CHECK(VPADDLE_Service(&p, 1000U, &k) == VPADDLE_DAH, "tap not down");
    CHECK(VPADDLE_Service(&p, 1000U + VPADDLE_TAP_TIMEOUT_MS, &k) == 0U, "untaken tap stuck");
}

static void test_tap_straight(void)
{
    vpaddle_t p;
    const vpaddle_keyer_t k = {0U, 1U, 60U};
    VPADDLE_Reset(&p);

    VPADDLE_Tap(&p, VPADDLE_DAH, 1000U);
    VPADDLE_Tap(&p, VPADDLE_DAH, 1001U);
    CHECK(VPADDLE_Service(&p, 1001U, &k) == VPADDLE_DAH, "straight dah not down");
    CHECK(VPADDLE_Service(&p, 1179U, &k) == VPADDLE_DAH, "straight dah short");
    CHECK(VPADDLE_Service(&p, 1181U, &k) == 0U, "straight dah long");
    /* The second one waits a dit of key-up. */
    CHECK(VPADDLE_Service(&p, 1230U, &k) == 0U, "no gap between straight taps");
    CHECK(VPADDLE_Service(&p, 1241U, &k) == VPADDLE_DAH, "second straight tap lost");
}

int main(void)
{
    test_plain_terminals();
    test_kitty_events();
    test_line_editor();
    test_hold();
    test_tap_keyer();
    test_tap_straight();
    if (failures) {
        fprintf(stderr, "usb_diag_keys: %u failure(s)\n", failures);
        return 1;
    }
    printf("usb_diag_keys: terminal decoding and virtual paddle OK\n");
    return 0;
}

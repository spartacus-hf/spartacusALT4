/* The S-CTRL LINK's pure logic (sctrl_link_logic.h): framing through the
 * ring as the USB interrupt drains it, page coding, the inbound decoder with
 * its streamed console lines, and the transmit guard's rules
 * (SCTRL_LINK.md section 6).
 *
 * With SCTRL_VECTORS=<path> in the environment it also writes the vectors
 * the PC side (spartacusControl, crates/proto) is tested against. */
#include "sctrl_link_logic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

static FILE *vectors;

static void hex(FILE *f, const uint8_t *p, size_t n)
{
    if (n == 0U) { fputs("-", f); return; }
    for (size_t i = 0U; i < n; ++i) fprintf(f, "%02x", p[i]);
}

/* ---- the PC side's decoder, as small as the test needs ---- */

typedef struct {
    uint8_t body[2048];
    size_t len;
    int esc, bad;
} host_rx_t;

/* Returns the body length (with CRC) of a frame that ended, 0 otherwise. */
static size_t host_byte(host_rx_t *h, uint8_t b)
{
    if (b == SL_END) {
        const size_t n = h->bad ? 0U : h->len;
        h->len = 0U; h->esc = 0; h->bad = 0;
        return n;
    }
    if (h->esc) {
        h->esc = 0;
        if (b == SL_ESC_END) b = SL_END;
        else if (b == SL_ESC_ESC) b = SL_ESC;
        else { h->bad = 1; return 0U; }
    } else if (b == SL_ESC) { h->esc = 1; return 0U; }
    if (h->len < sizeof(h->body)) h->body[h->len++] = b;
    else h->bad = 1;
    return 0U;
}

/* Everything in the ring, the way the interrupt takes it: packet by packet,
 * checking the chunk rule on the way. */
static size_t drain(sl_ring_t *r, uint8_t *out, size_t cap)
{
    size_t n = 0U;
    for (;;) {
        const uint8_t *p = NULL;
        const uint16_t used = SL_RingUsed(r);
        const uint16_t c = SL_RingChunk(r, &p);
        if (c == 0U) break;
        CHECK(c <= SL_PACKET);
        if (c == used) CHECK(c < SL_PACKET); /* never a full last packet */
        if (n + c <= cap) memcpy(&out[n], p, c);
        n += c;
        SL_RingConsume(r, c);
    }
    return n;
}

/* One wire frame decoded: body (type, seq, payload) into `body`, CRC
 * checked. Returns the payload length, -1 on failure. */
static int host_frame(const uint8_t *wire, size_t n, uint8_t *type, uint8_t *seq, uint8_t *payload, size_t cap)
{
    host_rx_t h;
    memset(&h, 0, sizeof(h));
    for (size_t i = 0U; i < n; ++i) {
        const size_t got = host_byte(&h, wire[i]);
        if (got == 0U) continue;
        if (got < 4U) return -1;
        const uint16_t crc = (uint16_t)(h.body[got - 2U] | (h.body[got - 1U] << 8));
        if (SL_Crc16Buf(0xFFFFU, h.body, (uint32_t)(got - 2U)) != crc) return -1;
        *type = h.body[0];
        *seq = h.body[1];
        if (got - 4U > cap) return -1;
        memcpy(payload, &h.body[2], got - 4U);
        return (int)(got - 4U);
    }
    return -1;
}

/* ---- CRC and framing ---- */

static void test_crc(void)
{
    const uint8_t check[] = "123456789";
    CHECK(SL_Crc16Buf(0xFFFFU, check, 9U) == 0x29B1U); /* the CCITT-FALSE check value */
    if (vectors) fprintf(vectors, "crc 313233343536373839 29b1\n");
}

static void frame_vector(const char *name, uint8_t type, uint8_t seq, const uint8_t *p, uint16_t n)
{
    static sl_ring_t r;
    memset(&r, 0, sizeof(r));
    CHECK(SL_FrameWhole(&r, type, seq, p, n));
    uint8_t wire[600];
    const size_t w = drain(&r, wire, sizeof(wire));
    uint8_t t = 0U, s = 0U, back[300];
    CHECK(host_frame(wire, w, &t, &s, back, sizeof(back)) == (int)n);
    CHECK(t == type && s == seq && (n == 0U || memcmp(back, p, n) == 0));
    if (vectors) {
        fprintf(vectors, "frame %s %02x %02x ", name, type, seq);
        hex(vectors, p, n);
        fputc(' ', vectors);
        hex(vectors, wire, w);
        fputc('\n', vectors);
    }
}

static void test_framing(void)
{
    const uint8_t plain[] = {1, 2, 3, 4};
    const uint8_t nasty[] = {SL_END, SL_ESC, SL_ESC_END, SL_ESC_ESC, SL_END, SL_END, 0x00, 0xFF};
    frame_vector("plain", SL_T_ACK, 7, plain, sizeof(plain));
    frame_vector("stuffed", SL_T_CONSOLE, 0xC0, nasty, sizeof(nasty)); /* the seq is stuffed too */
    frame_vector("empty", SL_H_HEARTBEAT, 0xDB, NULL, 0U);
    uint8_t console[] = "OK\r\n";
    frame_vector("console", SL_T_CONSOLE, 3, console, 4U);

    /* A frame bigger than the ring: written as the interrupt drains it. */
    static sl_ring_t r;
    memset(&r, 0, sizeof(r));
    uint8_t big[900];
    for (unsigned i = 0U; i < sizeof(big); ++i) big[i] = (uint8_t)(i * 37U + (i >> 3));
    big[10] = SL_END; big[500] = SL_ESC;
    sl_writer_t w;
    CHECK(SL_FrameBegin(&w, &r, SL_T_SPECTRUM, 9));
    static uint8_t wire[4096];
    size_t wn = 0U, put = 0U;
    unsigned passes = 0U;
    while (put < sizeof(big) || w.open) {
        if (put < sizeof(big)) put += SL_FramePut(&w, &r, &big[put], (uint16_t)(sizeof(big) - put));
        else (void)SL_FrameEnd(&w, &r);
        /* The interrupt takes at most two packets per pass. */
        for (unsigned k = 0U; k < 2U; ++k) {
            const uint8_t *p = NULL;
            const uint16_t c = SL_RingChunk(&r, &p);
            if (!c) break;
            memcpy(&wire[wn], p, c);
            wn += c;
            SL_RingConsume(&r, c);
        }
        CHECK(++passes < 1000U);
    }
    wn += drain(&r, &wire[wn], sizeof(wire) - wn);
    uint8_t t = 0U, s = 0U;
    static uint8_t back[1024];
    CHECK(host_frame(wire, wn, &t, &s, back, sizeof(back)) == (int)sizeof(big));
    CHECK(t == SL_T_SPECTRUM && s == 9U && memcmp(back, big, sizeof(big)) == 0);

    /* No room: nothing half-written. */
    memset(&r, 0, sizeof(r));
    r.head = (uint16_t)(SL_RING_SIZE - 3U);
    CHECK(!SL_FrameBegin(&w, &r, SL_T_STATUS, 0));
    CHECK(SL_RingUsed(&r) == SL_RING_SIZE - 3U);
    CHECK(!SL_FrameWhole(&r, SL_T_ACK, 0, plain, sizeof(plain)));
}

static void test_chunks(void)
{
    static sl_ring_t r;
    const uint8_t *p = NULL;
    memset(&r, 0, sizeof(r));
    CHECK(SL_RingChunk(&r, &p) == 0U);
    r.head = 32U;
    CHECK(SL_RingChunk(&r, &p) == 31U); /* the whole ring in one full packet: no */
    r.head = 33U;
    CHECK(SL_RingChunk(&r, &p) == 32U);
    /* Wrapping: the chunk stops at the end of the buffer. */
    r.tail = (uint16_t)(SL_RING_SIZE - 10U);
    r.head = (uint16_t)(SL_RING_SIZE + 40U);
    CHECK(SL_RingChunk(&r, &p) == 10U && p == &r.buf[SL_RING_SIZE - 10U]);
    /* Free-running indexes across the 16-bit wrap. */
    r.tail = 65530U;
    r.head = (uint16_t)(65530U + 50U);
    CHECK(SL_RingUsed(&r) == 50U && SL_RingFree(&r) == SL_RING_SIZE - 50U);
}

/* ---- Page coding ---- */

static size_t encode_page(const uint8_t *fb, const uint8_t *attr, uint8_t *payload, size_t cap,
                          unsigned packets_per_pass, uint8_t *mutate_at_pass)
{
    static sl_ring_t r;
    static uint8_t wire[4096];
    memset(&r, 0, sizeof(r));
    sl_writer_t w;
    sl_rle_t e = {0U, 0U};
    sl_src2_t s = {fb, 320U, attr, 40U};
    CHECK(SL_FrameBegin(&w, &r, SL_T_SCREEN, 1));
    const uint8_t head[2] = {5U, 0U};
    CHECK(SL_FramePut(&w, &r, head, 2U) == 2U);
    size_t wn = 0U;
    unsigned pass = 0U;
    uint8_t done = 0U;
    while (!done || w.open) {
        if (!done) done = SL_RlePut(&e, &s, &w, &r);
        else (void)SL_FrameEnd(&w, &r);
        if (mutate_at_pass && pass == *mutate_at_pass) ((uint8_t *)fb)[100] ^= 0x5AU;
        for (unsigned k = 0U; k < packets_per_pass; ++k) {
            const uint8_t *p = NULL;
            const uint16_t c = SL_RingChunk(&r, &p);
            if (!c) break;
            memcpy(&wire[wn], p, c);
            wn += c;
            SL_RingConsume(&r, c);
        }
        CHECK(++pass < 5000U);
    }
    wn += drain(&r, &wire[wn], sizeof(wire) - wn);
    uint8_t t = 0U, sq = 0U;
    const int n = host_frame(wire, wn, &t, &sq, payload, cap);
    CHECK(n >= 2 && t == SL_T_SCREEN && payload[0] == 5U);
    return n < 0 ? 0U : (size_t)n;
}

static void page_case(const char *name, const uint8_t *fb, const uint8_t *attr)
{
    uint8_t payload[1024], out[SL_PAGE_BYTES];
    const size_t n = encode_page(fb, attr, payload, sizeof(payload), 2U, NULL);
    CHECK(n >= 2U && n - 2U <= SL_PAGE_BYTES + 3U); /* worst case: three literal headers */
    CHECK(SL_RleDecode(&payload[2], (uint16_t)(n - 2U), out, SL_PAGE_BYTES));
    CHECK(memcmp(out, fb, 320U) == 0 && memcmp(&out[320], attr, 40U) == 0);
    if (vectors) {
        fprintf(vectors, "rle %s ", name);
        hex(vectors, out, SL_PAGE_BYTES);
        fputc(' ', vectors);
        hex(vectors, &payload[2], n - 2U);
        fputc('\n', vectors);
    }
}

static void test_pages(void)
{
    static uint8_t fb[320], attr[40];
    memset(fb, 0, sizeof(fb));
    memset(attr, 0x40, sizeof(attr));
    page_case("blank", fb, attr);
    size_t blank = 0U;
    {
        uint8_t payload[1024];
        blank = encode_page(fb, attr, payload, sizeof(payload), 4U, NULL);
        CHECK(blank <= 12U); /* a blank page is a handful of bytes */
    }
    memset(fb, 0xFF, sizeof(fb));
    page_case("full", fb, attr);
    uint32_t x = 12345U;
    for (unsigned i = 0U; i < sizeof(fb); ++i) { x = x * 1103515245U + 12345U; fb[i] = (uint8_t)(x >> 16); }
    for (unsigned i = 0U; i < sizeof(attr); ++i) attr[i] = (uint8_t)(i * 7U);
    page_case("noise", fb, attr);
    /* Text-like: columns of glyph bits between blank runs, a few colours. */
    memset(fb, 0, sizeof(fb));
    for (unsigned c = 8U; c < 300U; ++c) if ((c % 9U) < 6U) fb[c] = (uint8_t)(0x3CU ^ (c * 13U));
    for (unsigned i = 0U; i < sizeof(attr); ++i) attr[i] = (i < 20U) ? 0x40U : 0x91U;
    fb[50] = SL_END; fb[51] = SL_ESC; fb[52] = SL_END; /* bytes that need stuffing */
    page_case("text", fb, attr);
    /* Runs of exactly 2 and 3 and 129+ at the edges. */
    memset(fb, 0xAA, sizeof(fb));
    fb[0] = 1; fb[1] = 1; fb[2] = 2; fb[3] = 2; fb[4] = 2; fb[319] = 9;
    page_case("edges", fb, attr);

    /* The page redrawn while it is being sent: still exactly 360 bytes. */
    uint8_t payload[1024], out[SL_PAGE_BYTES];
    uint8_t at = 3U;
    const size_t n = encode_page(fb, attr, payload, sizeof(payload), 1U, &at);
    CHECK(SL_RleDecode(&payload[2], (uint16_t)(n - 2U), out, SL_PAGE_BYTES));

    /* The decoder refuses short, long and truncated codings. */
    const uint8_t short_run[] = {0x80U, 7U};
    CHECK(!SL_RleDecode(short_run, sizeof(short_run), out, SL_PAGE_BYTES));
    const uint8_t truncated[] = {0x05U, 1U, 2U};
    CHECK(!SL_RleDecode(truncated, sizeof(truncated), out, 6U));
    const uint8_t over[] = {0xFFU, 1U, 0xFFU, 1U, 0xFFU, 1U};
    CHECK(!SL_RleDecode(over, sizeof(over), out, SL_PAGE_BYTES));
}

/* ---- Inbound ---- */

typedef struct {
    char line[200];
    unsigned len, ends, oks;
} sink_t;

static void s_byte(void *ctx, uint8_t b) { sink_t *s = ctx; if (s->len < sizeof(s->line) - 1U) s->line[s->len++] = (char)b; }
static void s_end(void *ctx, uint8_t ok) { sink_t *s = ctx; s->ends++; s->oks += ok; s->line[s->len] = '\0'; }

/* Wire bytes of a host frame. */
static size_t host_wire(uint8_t type, uint8_t seq, const uint8_t *p, size_t n, uint8_t *out)
{
    static sl_ring_t r;
    memset(&r, 0, sizeof(r));
    CHECK(SL_FrameWhole(&r, type, seq, p, (uint16_t)n));
    return drain(&r, out, 600U);
}

static void test_inbound(void)
{
    sl_rx_t x;
    memset(&x, 0, sizeof(x));
    sink_t s;
    memset(&s, 0, sizeof(s));
    const sl_rx_sink_t sink = {s_byte, s_end, &s};
    uint8_t wire[600];

    /* A control frame, with noise before it. */
    const uint8_t input[2] = {1U, (uint8_t)-3};
    size_t n = host_wire(SL_H_INPUT, SL_END, input, 2U, wire);
    uint8_t got = 0U;
    CHECK(SL_RxByte(&x, 0x55U, &sink) == 0U);
    for (size_t i = 0U; i < n; ++i) got = got ? got : SL_RxByte(&x, wire[i], &sink);
    CHECK(got == 4U && x.buf[0] == SL_H_INPUT && x.buf[1] == SL_END && x.buf[2] == 1U && (int8_t)x.buf[3] == -3);
    if (vectors) { fprintf(vectors, "hostframe input "); hex(vectors, wire, n); fputc('\n', vectors); }
    const uint16_t errors_after_noise = x.errors;
    CHECK(errors_after_noise == 1U); /* the stray byte made a frame of its own */

    /* A console line: streamed to the sink, CRC never in it, committed. */
    const char *cmd = "sd put CW.TXT 0 48454c4c4f20c0db";
    n = host_wire(SL_H_CMD, 2U, (const uint8_t *)cmd, strlen(cmd), wire);
    for (size_t i = 0U; i < n; ++i) CHECK(SL_RxByte(&x, wire[i], &sink) == 0U);
    CHECK(s.ends == 1U && s.oks == 1U && strcmp(s.line, cmd) == 0);
    if (vectors) { fprintf(vectors, "hostframe cmd "); hex(vectors, wire, n); fputc('\n', vectors); }

    /* A 120-byte line (the console's longest). */
    char longline[121];
    memset(longline, 'a', 120U);
    longline[120] = '\0';
    memset(&s, 0, sizeof(s));
    n = host_wire(SL_H_CMD, 3U, (const uint8_t *)longline, 120U, wire);
    for (size_t i = 0U; i < n; ++i) (void)SL_RxByte(&x, wire[i], &sink);
    CHECK(s.ends == 1U && s.oks == 1U && s.len == 120U);

    /* A corrupted line is dropped, never committed. */
    memset(&s, 0, sizeof(s));
    n = host_wire(SL_H_CMD, 4U, (const uint8_t *)"save", 4U, wire);
    wire[4] ^= 0x01U;
    for (size_t i = 0U; i < n; ++i) (void)SL_RxByte(&x, wire[i], &sink);
    CHECK(s.ends == 1U && s.oks == 0U);

    /* A bad escape inside a line ends it as not ok at once. */
    memset(&s, 0, sizeof(s));
    const uint8_t bad_esc[] = {SL_END, SL_H_CMD, 5U, 'a', 'b', SL_ESC, 0x01U, 'c', 0x00U, 0x00U, SL_END};
    for (size_t i = 0U; i < sizeof(bad_esc); ++i) (void)SL_RxByte(&x, bad_esc[i], &sink);
    CHECK(s.ends == 1U && s.oks == 0U);

    /* An oversize control frame is dropped and counted. */
    uint8_t big[80];
    memset(big, 7, sizeof(big));
    const uint16_t before = x.errors;
    n = host_wire(SL_H_GETVAL, 6U, big, sizeof(big), wire);
    got = 0U;
    for (size_t i = 0U; i < n; ++i) got = got ? got : SL_RxByte(&x, wire[i], &sink);
    CHECK(got == 0U && x.errors == before + 1U);

    /* And the decoder recovers for the next one. */
    n = host_wire(SL_H_HEARTBEAT, 7U, NULL, 0U, wire);
    got = 0U;
    for (size_t i = 0U; i < n; ++i) got = got ? got : SL_RxByte(&x, wire[i], &sink);
    CHECK(got == 2U && x.buf[0] == SL_H_HEARTBEAT);
}

/* ---- Transmit guard ---- */

static const sl_radio_view_t idle = {0};

static void test_guard_arm(void)
{
    sl_tx_t t;
    SL_TxInit(&t, 0U);
    sl_radio_view_t v = idle;
    /* Nothing without a heartbeat first. */
    CHECK(SL_TxArm(&t, 0U, 1U, 30U, 60U, 60U, &v) == SL_ACK_NOT_ARMED);
    SL_TxHeartbeat(&t, 10U);
    /* Not armed: the paddle may not key RF. */
    CHECK(SL_TxPaddle(&t, 10U, 1U, &v) == SL_ACK_NOT_ARMED && t.levers == 0U);
    CHECK(SL_TxTune(&t, 10U, 1U, &v) == SL_ACK_NOT_ARMED && t.tune == 0U);
    CHECK(SL_TxMemory(&t, 10U, &v) == SL_ACK_NOT_ARMED && t.memory == 0U);
    /* Practice: sidetone only, no ARM needed -- but never TUNE. */
    v.cannot_radiate = 1U;
    CHECK(SL_TxPaddle(&t, 10U, 2U, &v) == SL_ACK_OK && t.levers == 2U);
    CHECK(SL_TxTune(&t, 10U, 1U, &v) == SL_ACK_MUTED);
    CHECK(SL_TxPaddle(&t, 20U, 0U, &v) == SL_ACK_OK && t.levers == 0U);
    v = idle;
    /* Refusals. */
    v.diag = 1U;
    CHECK(SL_TxArm(&t, 20U, 1U, 30U, 60U, 60U, &v) == SL_ACK_DIAG);
    v = idle; v.fault = 1U;
    CHECK(SL_TxArm(&t, 20U, 1U, 30U, 60U, 60U, &v) == SL_ACK_FAULT);
    v = idle; v.other_owner = 1U;
    CHECK(SL_TxArm(&t, 20U, 1U, 30U, 60U, 60U, &v) == SL_ACK_BUSY_OWNER);
    v = idle;
    CHECK(SL_TxArm(&t, 20U, 1U, 0U, 60U, 60U, &v) == SL_ACK_RANGE);
    CHECK(SL_TxArm(&t, 20U, 1U, 30U, 60U, 2U, &v) == SL_ACK_OK && t.armed && t.ttl_s == SL_ARM_TTL_MIN_S);
    CHECK(t.key_max_ms == 3000U && t.mem_max_s == 60U);
    CHECK(SL_TxArmLeftS(&t, 20U) == 5U);
    /* A pending mode change refuses keying and ends the ARM. */
    v.pending = 1U;
    CHECK(SL_TxPaddle(&t, 30U, 1U, &v) == SL_ACK_PENDING);
    SL_TxHeartbeat(&t, 30U);
    CHECK(SL_TxTick(&t, 30U, &v) & (1U << SL_TXE_DISARMED));
    CHECK(!t.armed);
    /* The TTL runs out with nothing sent. */
    v = idle;
    CHECK(SL_TxArm(&t, 100U, 1U, 30U, 60U, 10U, &v) == SL_ACK_OK);
    SL_TxHeartbeat(&t, 5000U); CHECK(SL_TxTick(&t, 5000U, &v) == 0U);
    SL_TxHeartbeat(&t, 10000U); CHECK(SL_TxTick(&t, 10000U, &v) == 0U && t.armed);
    SL_TxHeartbeat(&t, 10101U);
    CHECK(SL_TxTick(&t, 10101U, &v) == (1U << SL_TXE_DISARMED) && !t.armed);
}

static void test_guard_releases(void)
{
    sl_tx_t t;
    sl_radio_view_t v = idle;
    SL_TxInit(&t, 0U);
    SL_TxHeartbeat(&t, 0U);
    CHECK(SL_TxArm(&t, 0U, 1U, 30U, 5U, 60U, &v) == SL_ACK_OK);

    /* Levers held by refreshes every 100 ms: up to the 3 s key-down cap. */
    uint32_t now = 0U;
    CHECK(SL_TxPaddle(&t, now, 1U, &v) == SL_ACK_OK);
    uint16_t ev = 0U;
    while (!ev && now < 10000U) {
        now += 100U;
        SL_TxHeartbeat(&t, now);
        (void)SL_TxPaddle(&t, now, 1U, &v);
        ev = SL_TxTick(&t, now, &v);
    }
    CHECK(ev == (1U << SL_TXE_KEY_CAP) && now == 3100U && t.levers == 0U);
    /* Further refreshes start a new key-down; let go and press: fine. */
    CHECK(SL_TxPaddle(&t, now, 0U, &v) == SL_ACK_OK);

    /* A lever not refreshed for 250 ms is let go. */
    CHECK(SL_TxPaddle(&t, now, 2U, &v) == SL_ACK_OK);
    SL_TxHeartbeat(&t, now + 200U);
    CHECK(SL_TxTick(&t, now + 200U, &v) == 0U && t.levers == 2U);
    SL_TxHeartbeat(&t, now + 260U);
    CHECK(SL_TxTick(&t, now + 260U, &v) == (1U << SL_TXE_LEVER_STALE) && t.levers == 0U);
    now += 300U;

    /* The PC goes quiet mid key-down: released and disarmed. */
    SL_TxHeartbeat(&t, now);
    CHECK(SL_TxPaddle(&t, now, 1U, &v) == SL_ACK_OK);
    for (uint32_t k = 0U; k < 15U; ++k) { (void)SL_TxPaddle(&t, now + k * 100U, 1U, &v); }
    CHECK(SL_TxTick(&t, now + 1400U, &v) == 0U);
    ev = SL_TxTick(&t, now + 1501U, &v);
    CHECK((ev & (1U << SL_TXE_HEARTBEAT)) && t.levers == 0U && !t.armed && !t.hb_ok);
    /* ... and nothing keys until a heartbeat and a new ARM. */
    CHECK(SL_TxPaddle(&t, now + 1600U, 1U, &v) == SL_ACK_NOT_ARMED);
    now += 2000U;
    SL_TxHeartbeat(&t, now);
    CHECK(SL_TxArm(&t, now, 1U, 50U, 2U, 60U, &v) == SL_ACK_OK);

    /* Memory: done when the keyer says so, stopped at its cap otherwise. */
    CHECK(SL_TxMemory(&t, now, &v) == SL_ACK_OK && t.memory);
    v.memory_busy = 1U;
    CHECK(SL_TxMemory(&t, now, &v) == SL_ACK_BUSY);
    SL_TxHeartbeat(&t, now + 1000U);
    CHECK(SL_TxTick(&t, now + 1000U, &v) == 0U);
    v.memory_busy = 0U;
    CHECK(SL_TxTick(&t, now + 1100U, &v) == (1U << SL_TXE_MEMORY_DONE) && !t.memory && !t.stop_memory);
    v.memory_busy = 1U;
    CHECK(SL_TxMemory(&t, now + 1200U, &v) == SL_ACK_BUSY); /* the keyer is still busy */
    v.memory_busy = 0U;
    CHECK(SL_TxMemory(&t, now + 1200U, &v) == SL_ACK_OK);
    v.memory_busy = 1U;
    SL_TxHeartbeat(&t, now + 3201U);
    ev = SL_TxTick(&t, now + 3201U, &v);
    CHECK(ev == (1U << SL_TXE_MEMORY_CAP) && t.stop_memory && !t.memory);
    t.stop_memory = 0U;
    v.memory_busy = 0U;

    /* Frames are stamped on arrival, the tick with its pass's start: a key-down
     * or a heartbeat 1 ms "in the future" is no cap and no heartbeat loss. */
    now += 4000U;
    v.tx_test = 1U;
    SL_TxHeartbeat(&t, now + 1U);
    CHECK(SL_TxTune(&t, now + 1U, 1U, &v) == SL_ACK_OK);
    CHECK(SL_TxPaddle(&t, now + 1U, 1U, &v) == SL_ACK_OK);
    CHECK(SL_TxTick(&t, now, &v) == 0U && t.tune && t.levers && t.hb_ok);
    CHECK(SL_TxTune(&t, now + 2U, 0U, &v) == SL_ACK_OK && SL_TxPaddle(&t, now + 2U, 0U, &v) == SL_ACK_OK);

    /* TUNE is a carrier only on TX TEST: refused elsewhere, released when TX
     * TEST goes away under it. */
    now += 4000U;
    SL_TxHeartbeat(&t, now);
    v.tx_test = 0U;
    CHECK(SL_TxTune(&t, now, 1U, &v) == SL_ACK_PAGE && !t.tune);
    v.tx_test = 1U;
    CHECK(SL_TxTune(&t, now, 1U, &v) == SL_ACK_OK && t.tune);
    v.tx_test = 0U;
    CHECK(SL_TxTick(&t, now + 5U, &v) == (1U << SL_TXE_TUNE_LEFT) && !t.tune && !SL_TxActive(&t));
    v.tx_test = 1U;

    /* Another owner keys during a LINK key-down: LINK lets go. */
    SL_TxHeartbeat(&t, now);
    CHECK(SL_TxTune(&t, now, 1U, &v) == SL_ACK_OK && t.tune);
    v.other_owner = 1U;
    CHECK(SL_TxTick(&t, now + 10U, &v) & (1U << SL_TXE_OWNER));
    CHECK(!t.tune && !SL_TxActive(&t));
    v.other_owner = 0U;

    /* TUNE is capped like a lever. */
    SL_TxHeartbeat(&t, now + 20U);
    CHECK(SL_TxTune(&t, now + 20U, 1U, &v) == SL_ACK_OK);
    SL_TxHeartbeat(&t, now + 1000U);
    CHECK(SL_TxTick(&t, now + 1000U, &v) == 0U);
    SL_TxHeartbeat(&t, now + 5000U);
    CHECK(SL_TxTick(&t, now + 5020U, &v) == 0U && t.tune);
    CHECK(SL_TxTick(&t, now + 5021U, &v) == (1U << SL_TXE_KEY_CAP) && !t.tune);

    /* A fault while transmitting: released and disarmed. */
    SL_TxHeartbeat(&t, now + 5100U);
    CHECK(SL_TxArm(&t, now + 5100U, 1U, 50U, 2U, 60U, &v) == SL_ACK_OK);
    CHECK(SL_TxPaddle(&t, now + 5100U, 3U, &v) == SL_ACK_OK);
    v.fault = 1U;
    ev = SL_TxTick(&t, now + 5110U, &v);
    CHECK(!SL_TxActive(&t) && !t.armed && (ev & (1U << SL_TXE_DISARMED)));

    /* TX_STOP: everything, always. */
    v = idle;
    SL_TxHeartbeat(&t, now + 5200U);
    CHECK(SL_TxArm(&t, now + 5200U, 1U, 50U, 2U, 60U, &v) == SL_ACK_OK);
    CHECK(SL_TxMemory(&t, now + 5200U, &v) == SL_ACK_OK);
    CHECK(SL_TxPaddle(&t, now + 5200U, 1U, &v) == SL_ACK_OK);
    SL_TxStop(&t);
    CHECK(!SL_TxActive(&t) && !t.armed && t.stop_memory);
}

static void test_status_layout(void)
{
    sl_status_t s;
    memset(&s, 0, sizeof(s));
    s.uptime_ms = 0x01020304U;
    s.dial_hz = 14074000U;
    s.state = 1U;
    s.mode = 5U;
    s.page = 15U;
    s.faults = 0x0040U;
    s.vin_mv = 8800U;
    s.rx_dbfs_x10 = -723;
    s.settings_hash = 0xDEADBEEFU;
    s.ref_ppm_x100 = -1720;
    s.band = 6U;
    s.arm_left_s = 42U;
    s.cat0_age_ds = 0x1234U;
    s.event_seq = 0x5AU;
    const uint8_t *b = (const uint8_t *)&s;
    CHECK(b[0] == 0x04U && b[3] == 0x01U && b[8] == 1U && b[9] == 5U && b[10] == 15U);
    CHECK(b[16] == 0x40U && b[18] == (uint8_t)(8800U & 0xFFU) && b[32] == 0xEFU && b[46] == 6U && b[47] == 42U);
    CHECK(b[28] == 0x34U && b[29] == 0x12U && b[30] == 0x5AU && b[31] == 0U);
    if (vectors) { fprintf(vectors, "status sample "); hex(vectors, b, sizeof(s)); fputc('\n', vectors); }
}

int main(void)
{
    const char *path = getenv("SCTRL_VECTORS");
    if (path && *path) {
        vectors = fopen(path, "w");
        if (!vectors) { perror(path); return 1; }
        fprintf(vectors, "# SCTRL LINK v%u test vectors, from alt2 tools/tests/sctrl_link_logic_test.c\n", SL_PROTO);
        fprintf(vectors, "# crc <ascii hex> <crc>; frame <name> <type> <seq> <payload> <wire>;\n");
        fprintf(vectors, "# rle <name> <360 decoded bytes> <coding>; hostframe <name> <wire>; status <name> <48 bytes>\n");
    }
    test_crc();
    test_framing();
    test_chunks();
    test_pages();
    test_inbound();
    test_guard_arm();
    test_guard_releases();
    test_status_layout();
    if (vectors) fclose(vectors);
    if (failures) {
        fprintf(stderr, "sctrl_link_logic: %u failure(s)\n", failures);
        return 1;
    }
    printf("sctrl_link_logic: ok\n");
    return 0;
}

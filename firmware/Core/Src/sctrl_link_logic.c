#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/* The S-CTRL LINK's pure logic: framing, the ring's producer side, screen
 * page coding, the inbound decoder and the transmit guard (see the header). */

#include "sctrl_link_logic.h"

#include <string.h>

#ifndef SL_BARRIER
#if defined(__arm__)
#define SL_BARRIER() __asm volatile ("dmb" ::: "memory")
#else
#define SL_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
#endif

/* ---- CRC-16/CCITT-FALSE, a nibble at a time: 32 bytes of table ---- */

static const uint16_t crc_nibble[16] = {
    0x0000U, 0x1021U, 0x2042U, 0x3063U, 0x4084U, 0x50A5U, 0x60C6U, 0x70E7U,
    0x8108U, 0x9129U, 0xA14AU, 0xB16BU, 0xC18CU, 0xD1ADU, 0xE1CEU, 0xF1EFU,
};

uint16_t SL_Crc16(uint16_t crc, uint8_t byte)
{
    crc = (uint16_t)((crc << 4) ^ crc_nibble[((crc >> 12) ^ (byte >> 4)) & 0x0FU]);
    crc = (uint16_t)((crc << 4) ^ crc_nibble[((crc >> 12) ^ (byte & 0x0FU)) & 0x0FU]);
    return crc;
}

uint16_t SL_Crc16Buf(uint16_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) crc = SL_Crc16(crc, *p++);
    return crc;
}

/* ---- Ring, producer side ---- */

static inline void ring_put(sl_ring_t *r, uint16_t *head, uint8_t b)
{
    r->buf[*head & (SL_RING_SIZE - 1U)] = b;
    *head = (uint16_t)(*head + 1U);
}

/* One byte, stuffed, at *head (not yet published). */
static inline void put_stuffed(sl_ring_t *r, uint16_t *head, uint8_t b)
{
    if (b == SL_END) { ring_put(r, head, SL_ESC); ring_put(r, head, SL_ESC_END); }
    else if (b == SL_ESC) { ring_put(r, head, SL_ESC); ring_put(r, head, SL_ESC_ESC); }
    else ring_put(r, head, b);
}

static inline void publish(sl_ring_t *r, uint16_t head)
{
    SL_BARRIER(); /* the bytes before the index the interrupt reads */
    r->head = head;
}

uint8_t SL_FrameBegin(sl_writer_t *w, sl_ring_t *r, uint8_t type, uint8_t seq)
{
    if (SL_RingFree(r) < 5U) return 0U;
    uint16_t head = r->head;
    ring_put(r, &head, SL_END);
    put_stuffed(r, &head, type);
    put_stuffed(r, &head, seq);
    publish(r, head);
    w->crc = SL_Crc16(SL_Crc16(0xFFFFU, type), seq);
    w->open = 1U;
    return 1U;
}

uint16_t SL_FramePut(sl_writer_t *w, sl_ring_t *r, const uint8_t *p, uint16_t n)
{
    uint16_t head = r->head;
    uint16_t room = SL_RingFree(r);
    uint16_t done = 0U;
    uint16_t crc = w->crc;
    while ((done < n) && (room >= 2U)) {
        const uint8_t b = p[done++];
        crc = SL_Crc16(crc, b);
        const uint16_t before = head;
        put_stuffed(r, &head, b);
        room = (uint16_t)(room - (uint16_t)(head - before));
    }
    w->crc = crc;
    publish(r, head);
    return done;
}

uint8_t SL_FrameEnd(sl_writer_t *w, sl_ring_t *r)
{
    if (!w->open) return 1U;
    if (SL_RingFree(r) < 5U) return 0U;
    uint16_t head = r->head;
    put_stuffed(r, &head, (uint8_t)(w->crc & 0xFFU));
    put_stuffed(r, &head, (uint8_t)(w->crc >> 8));
    ring_put(r, &head, SL_END);
    publish(r, head);
    w->open = 0U;
    return 1U;
}

uint8_t SL_FrameWhole(sl_ring_t *r, uint8_t type, uint8_t seq, const uint8_t *p, uint16_t n)
{
    /* Worst case: every byte stuffed, plus END, type, seq, CRC, END. */
    if ((uint32_t)SL_RingFree(r) < 2U * (uint32_t)n + 10U) return 0U;
    sl_writer_t w;
    (void)SL_FrameBegin(&w, r, type, seq);
    (void)SL_FramePut(&w, r, p, n);
    return SL_FrameEnd(&w, r);
}

/* ---- PackBits ---- */

static inline uint8_t src_at(const sl_src2_t *s, uint16_t i)
{
    return (i < s->na) ? s->a[i] : s->b[i - s->na];
}

/* Identical bytes from i on, at most 129. */
static uint16_t run_at(const sl_src2_t *s, uint16_t i, uint16_t total)
{
    const uint8_t v = src_at(s, i);
    uint16_t n = 1U;
    while ((n < 129U) && ((uint16_t)(i + n) < total) && (src_at(s, (uint16_t)(i + n)) == v)) ++n;
    return n;
}

uint8_t SL_RlePut(sl_rle_t *e, const sl_src2_t *s, sl_writer_t *w, sl_ring_t *r)
{
    const uint16_t total = (uint16_t)(s->na + s->nb);
    while (e->pos < total) {
        /* A token is put only with room for it (four bytes stuffed). */
        if (e->lit_left) {
            const uint8_t b = src_at(s, e->pos);
            if (SL_FramePut(w, r, &b, 1U) == 0U) return 0U;
            ++e->pos;
            --e->lit_left;
            continue;
        }
        if (SL_RingFree(r) < 4U) return 0U;
        const uint16_t run = run_at(s, e->pos, total);
        if (run >= 3U) {
            const uint8_t tok[2] = {(uint8_t)(0x80U + run - 2U), src_at(s, e->pos)};
            (void)SL_FramePut(w, r, tok, 2U);
            e->pos = (uint16_t)(e->pos + run);
            continue;
        }
        /* A literal: up to 128 bytes, ending where a run of three starts. */
        uint16_t len = 0U;
        while ((len < 128U) && ((uint16_t)(e->pos + len) < total)) {
            const uint16_t i = (uint16_t)(e->pos + len);
            if (((uint16_t)(i + 2U) < total) && (src_at(s, i) == src_at(s, (uint16_t)(i + 1U))) &&
                (src_at(s, i) == src_at(s, (uint16_t)(i + 2U)))) break;
            ++len;
        }
        if (len == 0U) len = 1U; /* cannot happen: a run of three would have been taken */
        const uint8_t tok = (uint8_t)(len - 1U);
        (void)SL_FramePut(w, r, &tok, 1U);
        e->lit_left = (uint8_t)len;
    }
    return 1U;
}

uint8_t SL_RleDecode(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t out_len)
{
    uint16_t i = 0U, o = 0U;
    while (i < n) {
        const uint8_t c = in[i++];
        if (c < 0x80U) {
            const uint16_t len = (uint16_t)(c + 1U);
            if (((uint16_t)(i + len) > n) || ((uint16_t)(o + len) > out_len)) return 0U;
            memcpy(&out[o], &in[i], len);
            i = (uint16_t)(i + len);
            o = (uint16_t)(o + len);
        } else {
            const uint16_t len = (uint16_t)(c - 0x80U + 2U);
            if ((i >= n) || ((uint16_t)(o + len) > out_len)) return 0U;
            memset(&out[o], in[i++], len);
            o = (uint16_t)(o + len);
        }
    }
    return (uint8_t)(o == out_len);
}

/* ---- Inbound ---- */

void SL_RxReset(sl_rx_t *x)
{
    const uint16_t errors = x->errors;
    memset(x, 0, sizeof(*x));
    x->errors = errors;
}

static void rx_restart(sl_rx_t *x)
{
    x->len = 0U;
    x->esc = 0U;
    x->drop = 0U;
    x->stream = 0U;
    x->held = 0U;
    x->stream_len = 0U;
}

/* One unstuffed body byte. */
static void rx_body(sl_rx_t *x, uint8_t b, const sl_rx_sink_t *sink)
{
    if (x->drop) return;
    if (x->stream) {
        /* Two bytes behind: whatever is held when END comes is the CRC. */
        if (x->held == 2U) {
            const uint8_t out = x->hold[0];
            if (x->stream_len >= (uint8_t)(SL_RX_BODY_MAX - 4U)) { x->drop = 1U; if (sink && sink->cmd_end) sink->cmd_end(sink->ctx, 0U); return; }
            x->crc = SL_Crc16(x->crc, out);
            if (sink && sink->cmd_byte) sink->cmd_byte(sink->ctx, out);
            ++x->stream_len;
            x->hold[0] = x->hold[1];
            x->held = 1U;
        }
        x->hold[x->held++] = b;
        return;
    }
    if (x->len >= SL_RX_MAX) { x->drop = 1U; return; }
    x->buf[x->len++] = b;
    /* type and seq known: a console line goes to the console from here. */
    if ((x->len == 2U) && (x->buf[0] == SL_H_CMD) && sink && sink->cmd_byte) {
        x->stream = 1U;
        x->crc = SL_Crc16(SL_Crc16(0xFFFFU, x->buf[0]), x->buf[1]);
        x->held = 0U;
        x->stream_len = 0U;
    }
}

uint8_t SL_RxByte(sl_rx_t *x, uint8_t byte, const sl_rx_sink_t *sink)
{
    if (byte == SL_END) {
        uint8_t result = 0U;
        if (x->stream) {
            if (!x->drop) {
                const uint16_t got = (uint16_t)(x->hold[0] | ((uint16_t)x->hold[1] << 8));
                const uint8_t ok = (uint8_t)((x->held == 2U) && (got == x->crc));
                if (!ok) ++x->errors;
                if (sink && sink->cmd_end) sink->cmd_end(sink->ctx, ok);
            } else {
                ++x->errors;
            }
        } else if (x->len || x->drop) {
            if (x->drop || x->esc || (x->len < 4U)) {
                ++x->errors;
            } else {
                const uint8_t body = (uint8_t)(x->len - 2U);
                const uint16_t got = (uint16_t)(x->buf[body] | ((uint16_t)x->buf[body + 1U] << 8));
                if (SL_Crc16Buf(0xFFFFU, x->buf, body) == got) result = body;
                else ++x->errors;
            }
        }
        rx_restart(x);
        return result;
    }
    if (x->esc) {
        x->esc = 0U;
        if (byte == SL_ESC_END) rx_body(x, SL_END, sink);
        else if (byte == SL_ESC_ESC) rx_body(x, SL_ESC, sink);
        else {
            /* A bad escape spoils the frame; a console line already under way
             * must not be committed. */
            if (x->stream && !x->drop && sink && sink->cmd_end) sink->cmd_end(sink->ctx, 0U);
            x->drop = 1U;
        }
        return 0U;
    }
    if (byte == SL_ESC) { x->esc = 1U; return 0U; }
    rx_body(x, byte, sink);
    return 0U;
}

/* ---- Transmit guard ---- */

void SL_TxInit(sl_tx_t *t, uint32_t now)
{
    memset(t, 0, sizeof(*t));
    t->hb_at = now;
    t->key_max_ms = 1000U;
    t->mem_max_s = 60U;
    t->ttl_s = 60U;
}

void SL_TxHeartbeat(sl_tx_t *t, uint32_t now)
{
    t->hb_at = now;
    t->hb_ok = 1U;
}

static void release(sl_tx_t *t)
{
    t->levers = 0U;
    t->tune = 0U;
    t->key_since = 0U;
    if (t->memory) t->stop_memory = 1U;
    t->memory = 0U;
}

static void disarm(sl_tx_t *t)
{
    release(t);
    t->armed = 0U;
    t->arm_until = 0U;
}

static inline void touch_arm(sl_tx_t *t, uint32_t now)
{
    if (t->armed) t->arm_until = now + (uint32_t)t->ttl_s * 1000U;
}

/* The checks every RF request shares; SL_ACK_OK when it may key. */
static uint8_t may_key(const sl_tx_t *t, const sl_radio_view_t *v)
{
    if (v->diag) return SL_ACK_DIAG;
    if (v->fault) return SL_ACK_FAULT;
    if (v->other_owner) return SL_ACK_BUSY_OWNER;
    if (v->pending) return SL_ACK_PENDING;
    if (!t->hb_ok) return SL_ACK_NOT_ARMED;
    if (!t->armed) return SL_ACK_NOT_ARMED;
    return SL_ACK_OK;
}

uint8_t SL_TxArm(sl_tx_t *t, uint32_t now, uint8_t on, uint8_t key_max_ds, uint8_t mem_max_s,
                 uint8_t ttl_s, const sl_radio_view_t *v)
{
    if (!on) { disarm(t); return SL_ACK_OK; }
    if (v->diag) return SL_ACK_DIAG;
    if (v->fault) return SL_ACK_FAULT;
    if (v->other_owner) return SL_ACK_BUSY_OWNER;
    if (!t->hb_ok) return SL_ACK_NOT_ARMED;
    if ((key_max_ds == 0U) || (mem_max_s == 0U)) return SL_ACK_RANGE;
    t->key_max_ms = (uint16_t)(key_max_ds * 100U); /* at most SL_KEY_MAX_CEIL_MS */
    t->mem_max_s = mem_max_s;
    t->ttl_s = (ttl_s < SL_ARM_TTL_MIN_S) ? (uint8_t)SL_ARM_TTL_MIN_S : ttl_s;
    t->armed = 1U;
    touch_arm(t, now);
    return SL_ACK_OK;
}

uint8_t SL_TxPaddle(sl_tx_t *t, uint32_t now, uint8_t levers, const sl_radio_view_t *v)
{
    levers &= 3U;
    if (!levers) {
        t->levers = 0U;
        if (!t->tune) t->key_since = 0U;
        return SL_ACK_OK;
    }
    /* Sidetone only (practice, TX MUTE, the trainer): nothing can radiate,
     * so no ARM -- but never in DIAG, never past a fault. */
    if (!v->cannot_radiate) {
        const uint8_t why = may_key(t, v);
        if (why != SL_ACK_OK) { t->levers = 0U; return why; }
    } else if (v->fault) {
        t->levers = 0U;
        return SL_ACK_FAULT;
    }
    if (!t->levers && !t->tune) t->key_since = now;
    t->levers = levers;
    t->lever_at = now;
    touch_arm(t, now);
    return SL_ACK_OK;
}

uint8_t SL_TxMemory(sl_tx_t *t, uint32_t now, const sl_radio_view_t *v)
{
    if (v->memory_busy || t->memory) return SL_ACK_BUSY;
    if (!v->cannot_radiate) {
        const uint8_t why = may_key(t, v);
        if (why != SL_ACK_OK) return why;
    } else if (v->fault) {
        return SL_ACK_FAULT;
    }
    t->memory = 1U;
    t->mem_since = now;
    touch_arm(t, now);
    return SL_ACK_OK;
}

uint8_t SL_TxTune(sl_tx_t *t, uint32_t now, uint8_t on, const sl_radio_view_t *v)
{
    if (!on) {
        t->tune = 0U;
        if (!t->levers) t->key_since = 0U;
        return SL_ACK_OK;
    }
    if (v->cannot_radiate) return SL_ACK_MUTED; /* a carrier with no RF tunes nothing */
    const uint8_t why = may_key(t, v);
    if (why != SL_ACK_OK) return why;
    /* TUNE holds a lever: a carrier only on TX TEST, dits anywhere else. */
    if (!v->tx_test) return SL_ACK_PAGE;
    if (!t->levers && !t->tune) t->key_since = now;
    t->tune = 1U;
    touch_arm(t, now);
    return SL_ACK_OK;
}

void SL_TxStop(sl_tx_t *t) { disarm(t); }

/* Milliseconds from `t` to `now`; 0 when `t` is later. The frames are
 * stamped when they arrive, the tick with its main-loop pass's start: one
 * landing in the same pass is a millisecond "in the future", which unsigned
 * arithmetic made four billion -- a false heartbeat loss or key-down cap. */
static uint32_t since(uint32_t now, uint32_t t)
{
    const int32_t d = (int32_t)(now - t);
    return (d > 0) ? (uint32_t)d : 0U;
}

uint16_t SL_TxTick(sl_tx_t *t, uint32_t now, const sl_radio_view_t *v)
{
    uint16_t ev = 0U;
    /* The PC went quiet: nothing it started may go on. */
    if (t->hb_ok && since(now, t->hb_at) > SL_HEARTBEAT_MS) {
        t->hb_ok = 0U;
        if (t->armed || SL_TxActive(t)) { disarm(t); ev |= 1U << SL_TXE_HEARTBEAT; }
    }
    if (t->levers && since(now, t->lever_at) > SL_LEVER_REFRESH_MS) {
        t->levers = 0U;
        if (!t->tune) t->key_since = 0U;
        ev |= 1U << SL_TXE_LEVER_STALE;
    }
    /* key_since is set whenever a key-down starts (0 is a valid tick). */
    if ((t->levers || t->tune) && since(now, t->key_since) > (uint32_t)t->key_max_ms) {
        t->levers = 0U;
        t->tune = 0U;
        t->key_since = 0U;
        ev |= 1U << SL_TXE_KEY_CAP;
    }
    if (t->memory) {
        if (since(now, t->mem_since) > (uint32_t)t->mem_max_s * 1000U) {
            t->memory = 0U;
            t->stop_memory = 1U;
            ev |= 1U << SL_TXE_MEMORY_CAP;
        } else if (!v->memory_busy && since(now, t->mem_since) > 50U) {
            t->memory = 0U;
            ev |= 1U << SL_TXE_MEMORY_DONE;
        }
    }
    /* TX TEST left (its page closed, a mode change): a held TUNE would be a
     * lever down in another mode. */
    if (t->tune && !v->tx_test) {
        t->tune = 0U;
        if (!t->levers) t->key_since = 0U;
        ev |= 1U << SL_TXE_TUNE_LEFT;
    }
    /* Someone else keyed, or the radio stopped being able to: let go. */
    if (SL_TxActive(t) && !v->cannot_radiate && (v->other_owner || v->diag)) {
        release(t);
        ev |= 1U << SL_TXE_OWNER;
    }
    if (t->armed) {
        if (v->fault || v->diag || v->pending) {
            disarm(t);
            ev |= 1U << SL_TXE_DISARMED;
        } else if (!SL_TxActive(t) && (int32_t)(now - t->arm_until) > 0) {
            disarm(t);
            ev |= 1U << SL_TXE_DISARMED;
        }
    }
    if (SL_TxActive(t) && v->fault) {
        release(t);
        ev |= 1U << SL_TXE_OWNER;
    }
    return ev;
}

uint8_t SL_TxArmLeftS(const sl_tx_t *t, uint32_t now)
{
    if (!t->armed) return 0U;
    const int32_t left = (int32_t)(t->arm_until - now);
    if (left <= 0) return 1U; /* armed while a transmission holds it */
    const uint32_t s = ((uint32_t)left + 999U) / 1000U;
    return (uint8_t)(s > 255U ? 255U : s);
}

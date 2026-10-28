#ifndef SCTRL_LINK_LOGIC_H
#define SCTRL_LINK_LOGIC_H

/* The S-CTRL LINK (docs/SCTRL_LINK.md): the framed channel a PC runs on one
 * USB serial port once the console command `link 1` has switched it over.
 * This file is the part with no hardware in it, so the host suite
 * (tools/tests/sctrl_link_logic_test.c) runs it as it is:
 *
 *   - CRC-16/CCITT-FALSE and SLIP framing, written into a 256-byte ring a
 *     few bytes at a time: a frame longer than the ring (a screen page, a
 *     waterfall line) is produced over several main-loop passes from the
 *     memory it describes, never copied whole;
 *   - the ring's consumer side, which the USB interrupt calls to load the
 *     next IN packet;
 *   - PackBits run-length coding of one screen page (320 bitmap bytes and 40
 *     tile colours), streamed the same way;
 *   - the inbound decoder: control frames into a 56-byte buffer, console
 *     lines (up to the console's own 120 bytes) streamed into the console's
 *     line as they arrive, two bytes behind so the CRC is never taken for
 *     text, and committed only when it checks;
 *   - the transmit guard: ARM, heartbeat, lever refresh, key-down and memory
 *     caps, one owner. Whatever the PC does, a LINK transmission ends by
 *     these rules (SCTRL_LINK.md section 6).
 *
 * RAM: the ring and the decoder are the LINK's whole buffer budget (about
 * 330 bytes with the guard); nothing here allocates. */

#include <stddef.h>
#include <stdint.h>

#define SL_PROTO        1U
#define SL_RING_SIZE    256U   /* a power of two that divides 65536 */
#define SL_RX_MAX       52U    /* control frames, CAT and memory text (48) */
#define SL_PACKET       32U    /* the CAT ports' IN packet */
#define SL_TX_BODY_MAX  1024U  /* radio -> host */
#define SL_RX_BODY_MAX  136U   /* host -> radio: a 120-byte console line + type, seq, CRC */
#define SL_PAGE_BYTES   360U   /* 320 bitmap + 40 tile colours */

#define SL_END      0xC0U
#define SL_ESC      0xDBU
#define SL_ESC_END  0xDCU
#define SL_ESC_ESC  0xDDU

/* Radio -> host. */
enum {
    SL_T_HELLO = 0x01U, SL_T_STATUS, SL_T_SCREEN, SL_T_BAND, SL_T_SPECTRUM, SL_T_EVENT,
    SL_T_CWDEC, SL_T_CONSOLE, SL_T_CATREPLY, SL_T_SETVAL, SL_T_ACK, SL_T_BYE, SL_T_TXEVENT
};
/* Host -> radio. */
enum {
    SL_H_HEARTBEAT = 0x40U, SL_H_SUB, SL_H_REFRESH, SL_H_INPUT, SL_H_PADDLE, SL_H_TX_ARM,
    SL_H_TX_STOP, SL_H_MEMSEND, SL_H_CMD, SL_H_CAT, SL_H_GETVAL, SL_H_MONITOR, SL_H_LINK_OFF,
    SL_H_TUNE
};
/* ACK codes. */
enum {
    SL_ACK_OK = 0U, SL_ACK_BUSY_TX, SL_ACK_BUSY_OWNER, SL_ACK_BUSY_RT, SL_ACK_NOT_ARMED,
    SL_ACK_FAULT, SL_ACK_BAD, SL_ACK_UNSUPPORTED, SL_ACK_BUSY, SL_ACK_CAPPED, SL_ACK_MUTED,
    SL_ACK_RANGE, SL_ACK_DIAG, SL_ACK_PENDING, SL_ACK_PAGE
};
/* TXEVENT kinds, also the bits SL_TxTick returns (1 << kind). */
enum {
    SL_TXE_ARMED = 1U, SL_TXE_DISARMED, SL_TXE_KEY_CAP, SL_TXE_HEARTBEAT, SL_TXE_STOPPED,
    SL_TXE_MEMORY_DONE, SL_TXE_LEVER_STALE, SL_TXE_MEMORY_CAP, SL_TXE_OWNER, SL_TXE_TUNE_LEFT
};
/* SUB channel bits. */
enum {
    SL_SUB_STATUS = 1U << 0, SL_SUB_SCREEN = 1U << 1, SL_SUB_SPECTRUM = 1U << 2,
    SL_SUB_SPECTRUM_TRACE = 1U << 3, SL_SUB_EVENTS = 1U << 4, SL_SUB_CWDEC = 1U << 5,
    SL_SUB_SETVAL = 1U << 6
};
/* HELLO caps bits. */
enum {
    SL_CAP_SCREEN = 1U << 0, SL_CAP_SPECTRUM = 1U << 1, SL_CAP_CWDEC = 1U << 2,
    SL_CAP_EVENTS = 1U << 3, SL_CAP_SETVAL = 1U << 4, SL_CAP_CAT = 1U << 5,
    SL_CAP_TX = 1U << 6, SL_CAP_MONITOR = 1U << 7, SL_CAP_INPUT = 1U << 8, SL_CAP_BAND = 1U << 9
};

/* ---- Fixed payloads, as they go on the wire (little-endian) ---- */

typedef struct __attribute__((packed)) {
    uint8_t  proto, fw_major, fw_minor, personality;
    uint32_t caps, image_crc;
    uint16_t fb_w, fb_h;
    uint8_t  pages, tcols;
    uint16_t max_body;
    uint8_t  port, packets_per_frame;
} sl_hello_t;
_Static_assert(sizeof(sl_hello_t) == 22U, "HELLO payload");

typedef struct __attribute__((packed)) {
    uint32_t uptime_ms, dial_hz;
    uint8_t  state, mode, page, ui, flags0, flags1, tx_owner, load_pct;
    uint16_t faults, vin_mv, pep_cw, swr_x100;
    int16_t  rx_dbfs_x10;
    uint8_t  wpm, cat;
    uint16_t cat0_age_ds;
    uint8_t  event_seq, reserved31;
    uint32_t settings_hash;
    uint16_t pd_mv, rx_overruns, cap_gaps;
    uint8_t  ref_state, link;
    int16_t  ref_ppm_x100;
    uint8_t  band, arm_left_s;
} sl_status_t;
_Static_assert(sizeof(sl_status_t) == 48U, "STATUS payload");
_Static_assert(offsetof(sl_status_t, faults) == 16U && offsetof(sl_status_t, settings_hash) == 32U &&
               offsetof(sl_status_t, ref_ppm_x100) == 44U, "STATUS layout (SCTRL_LINK.md)");

/* SPECTRUM: this, then the engine's spec_view_t as it is (32 bytes, see
 * SCTRL_LINK.md), then the line, trace and peak. */
typedef struct __attribute__((packed)) {
    uint16_t line_seq;
    uint8_t  flags, palette;
    uint16_t width;
} sl_spectrum_pre_t;
_Static_assert(sizeof(sl_spectrum_pre_t) == 6U, "SPECTRUM prefix");

typedef struct __attribute__((packed)) {
    uint16_t age_ms;
    uint8_t  stage, reason, severity, reserved;
    uint32_t value;
} sl_event_t;
_Static_assert(sizeof(sl_event_t) == 10U, "EVENT payload");

/* ---- CRC ---- */

uint16_t SL_Crc16(uint16_t crc, uint8_t byte);
uint16_t SL_Crc16Buf(uint16_t crc, const uint8_t *p, uint32_t n);

/* ---- The transmit ring: one producer (main loop), one consumer (USB IRQ) ---- */

typedef struct {
    uint8_t buf[SL_RING_SIZE];
    volatile uint16_t head;  /* free-running; written by the producer only */
    volatile uint16_t tail;  /* free-running; written by the consumer only */
} sl_ring_t;

static inline uint16_t SL_RingUsed(const sl_ring_t *r) { return (uint16_t)(r->head - r->tail); }
static inline uint16_t SL_RingFree(const sl_ring_t *r) { return (uint16_t)(SL_RING_SIZE - SL_RingUsed(r)); }

/* The next IN packet: up to SL_PACKET contiguous bytes, never a full packet
 * that empties the ring (Linux cdc-acm holds a URB that ends on a full
 * packet until more comes: a STATUS would wait for the next one). 0 when
 * there is nothing. Interrupt side; SL_RingConsume after the packet is
 * loaded. */
static inline uint16_t SL_RingChunk(const sl_ring_t *r, const uint8_t **p)
{
    const uint16_t used = SL_RingUsed(r);
    if (used == 0U) return 0U;
    const uint16_t off = (uint16_t)(r->tail & (SL_RING_SIZE - 1U));
    uint16_t n = (uint16_t)(SL_RING_SIZE - off);
    if (n > used) n = used;
    if (n > SL_PACKET) n = SL_PACKET;
    if ((n == SL_PACKET) && (used == SL_PACKET)) n = SL_PACKET - 1U;
    *p = &r->buf[off];
    return n;
}

static inline void SL_RingConsume(sl_ring_t *r, uint16_t n) { r->tail = (uint16_t)(r->tail + n); }

/* ---- Frames out ---- */

typedef struct {
    uint16_t crc;
    uint8_t  open;
} sl_writer_t;

/* Start a frame (END, type, seq): needs 5 free bytes, 0 when not. */
uint8_t  SL_FrameBegin(sl_writer_t *w, sl_ring_t *r, uint8_t type, uint8_t seq);
/* Body bytes, stuffed; returns how many of `n` went in (stops with < 2 free). */
uint16_t SL_FramePut(sl_writer_t *w, sl_ring_t *r, const uint8_t *p, uint16_t n);
/* CRC and END: needs 5 free bytes; 1 once the frame is closed. */
uint8_t  SL_FrameEnd(sl_writer_t *w, sl_ring_t *r);
/* A whole small frame in one go, or nothing: 1 when it went in. */
uint8_t  SL_FrameWhole(sl_ring_t *r, uint8_t type, uint8_t seq, const uint8_t *p, uint16_t n);

/* ---- Screen pages: PackBits, streamed ---- */

/* Control byte c < 0x80: c + 1 literal bytes follow; c >= 0x80: the next
 * byte repeats c - 0x80 + 2 times. Runs of three or more are coded as runs. */
typedef struct {
    uint16_t pos;       /* source bytes coded so far */
    uint8_t  lit_left;  /* literal bytes still owed to the open token */
} sl_rle_t;

/* The page as two spans: the 320 bitmap bytes, then its 40 tile colours. */
typedef struct {
    const uint8_t *a;
    uint16_t na;
    const uint8_t *b;
    uint16_t nb;
} sl_src2_t;

/* Code as much as fits; 1 when the whole source is coded. The source may
 * change between calls (the frame is redrawn): the output stays a valid
 * coding of exactly na + nb bytes, and the caller sends the page again if
 * its hash moved meanwhile. */
uint8_t  SL_RlePut(sl_rle_t *e, const sl_src2_t *s, sl_writer_t *w, sl_ring_t *r);
/* Decode (host suite, and the reference for the PC side): 1 when `in`
 * decodes to exactly `out_len` bytes. */
uint8_t  SL_RleDecode(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t out_len);

/* ---- Frames in ---- */

/* Console lines are not buffered here: their bytes go to the console's own
 * line as they arrive (two behind, so the CRC is never text), then the line
 * is committed (ok = 1) or dropped. */
typedef struct {
    void (*cmd_byte)(void *ctx, uint8_t byte);
    void (*cmd_end)(void *ctx, uint8_t ok);
    void *ctx;
} sl_rx_sink_t;

typedef struct {
    uint8_t  buf[SL_RX_MAX];
    uint8_t  len;       /* body bytes in buf (type, seq, payload, CRC) */
    uint8_t  esc;       /* the last byte was ESC */
    uint8_t  drop;      /* this frame is being thrown away */
    uint8_t  stream;    /* this frame is a console line going to the sink */
    uint8_t  held;      /* stream: bytes held back (0..2) */
    uint8_t  hold[2];
    uint8_t  stream_len;/* stream: line bytes passed on */
    uint16_t crc;       /* stream: CRC of what was passed on */
    uint16_t errors;    /* frames dropped: CRC, size, stuffing */
} sl_rx_t;

/* One byte from the host. Returns the body length (type, seq, payload; the
 * CRC checked and removed) of a buffered frame that just completed, else 0.
 * A streamed console line ends through the sink instead. */
uint8_t  SL_RxByte(sl_rx_t *x, uint8_t byte, const sl_rx_sink_t *sink);
void     SL_RxReset(sl_rx_t *x);

/* ---- The transmit guard ---- */

#define SL_HEARTBEAT_MS      1500U
#define SL_LEVER_REFRESH_MS  250U
#define SL_KEY_MAX_CEIL_MS   25500U
#define SL_ARM_TTL_MIN_S     5U

/* What the radio is doing, gathered by the caller each time. */
typedef struct {
    uint8_t diag;            /* DIAG personality: TX muted */
    uint8_t fault;           /* a fault or pipeline lockout is latched */
    uint8_t cannot_radiate;  /* the keyer is sidetone-only: practice, TX MUTE, trainer */
    uint8_t other_owner;     /* CAT PTT, the radio's own key or microphone transmits */
    uint8_t pending;         /* a mode or band change is in flight */
    uint8_t memory_busy;     /* MOD_CwMemoryBusy */
    uint8_t tx_test;         /* TX TEST is the mode: a lever down is the TUNE carrier */
} sl_radio_view_t;

typedef struct {
    uint32_t arm_until, hb_at, lever_at, key_since, mem_since;
    uint16_t key_max_ms;
    uint8_t  mem_max_s, ttl_s;
    uint8_t  armed, hb_ok;
    uint8_t  levers;         /* what the LINK presses now (bit 0 DIT, bit 1 DAH) */
    uint8_t  tune;           /* TUNE carrier held */
    uint8_t  memory;         /* a LINK memory is sending */
    uint8_t  stop_memory;    /* one-shot for the caller: MOD_CwMemoryStop */
} sl_tx_t;

void    SL_TxInit(sl_tx_t *t, uint32_t now);
void    SL_TxHeartbeat(sl_tx_t *t, uint32_t now);
/* Each returns an SL_ACK_* code. */
uint8_t SL_TxArm(sl_tx_t *t, uint32_t now, uint8_t on, uint8_t key_max_ds, uint8_t mem_max_s,
                 uint8_t ttl_s, const sl_radio_view_t *v);
uint8_t SL_TxPaddle(sl_tx_t *t, uint32_t now, uint8_t levers, const sl_radio_view_t *v);
uint8_t SL_TxMemory(sl_tx_t *t, uint32_t now, const sl_radio_view_t *v);
uint8_t SL_TxTune(sl_tx_t *t, uint32_t now, uint8_t on, const sl_radio_view_t *v);
/* TX_STOP: everything released and disarmed. */
void    SL_TxStop(sl_tx_t *t);
/* Every main-loop pass, the realtime branch included. Returns the TXEVENT
 * kinds that happened as bits (1 << SL_TXE_*). */
uint16_t SL_TxTick(sl_tx_t *t, uint32_t now, const sl_radio_view_t *v);
/* LINK transmission held now: the levers or TUNE down, or a memory sending. */
static inline uint8_t SL_TxActive(const sl_tx_t *t)
{
    return (uint8_t)((t->levers != 0U) || (t->tune != 0U) || (t->memory != 0U));
}
/* Seconds of ARM left, for STATUS. */
uint8_t SL_TxArmLeftS(const sl_tx_t *t, uint32_t now);

#endif

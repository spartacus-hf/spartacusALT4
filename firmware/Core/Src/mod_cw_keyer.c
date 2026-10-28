/* The CW keyer's state machine (mod_cw_keyer.h): hardware-free, one step per
 * 48 kHz sample, so render_cw, the headphone renderer and the host suite
 * (tools/tests/cw_keyer_test.c) run the same code. */
#include "mod_cw_keyer.h"

#if defined(__GNUC__)
#define CWK_HOT __attribute__((optimize("O3"), hot))
#define CWK_COLD __attribute__((cold))
#else
#define CWK_HOT
#define CWK_COLD
#endif

#define BIT(e) ((uint8_t)(e))   /* CWK_DIT is bit 0, CWK_DAH bit 1 */

static inline uint8_t other(uint8_t e) { return (uint8_t)(e ^ 3U); }

static float clampf_(float v, float lo, float hi)
{
    if (!(v == v)) return lo;
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static uint32_t round_u(float v) { return (v <= 0.0f) ? 0U : (uint32_t)(v + 0.5f); }

/* Configuration time only: the floats stay out of the per-sample path. */
CWK_COLD void CWK_TimingInit(cwk_timing_t *t, float wpm, float weight, float curtis_dit_pct,
                             float curtis_dah_pct, uint8_t latency_eighths, uint8_t acs_dits, uint8_t mode)
{
    wpm = clampf_(wpm, 5.0f, 60.0f);
    weight = clampf_(weight, 35.0f, 65.0f);
    /* 1.2 s / wpm per dit at 48 kHz. The weight's share is rounded on its
     * own, so dit + gap and dah + gap are 2 and 4 whole units exactly. */
    const uint32_t u = round_u(57600.0f / wpm);
    const float d = (float)u * (weight - 50.0f) / 50.0f;
    const int32_t delta = (d < 0.0f) ? -(int32_t)round_u(-d) : (int32_t)round_u(d);
    t->unit = u;
    t->mark[CWK_NONE] = u;
    t->mark[CWK_DIT] = (uint32_t)((int32_t)u + delta);
    t->mark[CWK_DAH] = (uint32_t)((int32_t)(3U * u) + delta);
    t->gap = (uint32_t)((int32_t)u - delta);
    if (acs_dits == 1U) acs_dits = 3U;
    /* The space after a character's last mark is gap + extra = n u - delta,
     * the n-dit space the weighted mark leaves: extra is (n - 1) u. */
    t->char_extra = (acs_dits >= 2U && acs_dits <= 4U) ? (uint32_t)(acs_dits - 1U) * u : 0U;
    t->curtis[CWK_NONE] = 0U;
    t->curtis[CWK_DIT] = round_u((float)t->mark[CWK_DIT] * clampf_(curtis_dit_pct, 0.0f, 100.0f) * 0.01f);
    t->curtis[CWK_DAH] = round_u((float)t->mark[CWK_DAH] * clampf_(curtis_dah_pct, 0.0f, 100.0f) * 0.01f);
    if (latency_eighths > 8U) latency_eighths = 8U;
    t->latency = (u * latency_eighths) / 8U;
    t->mode = (mode < CWK_MODE_COUNT) ? mode : CWK_IAMBIC_B;
}

/* SysTick, every ms: a handful of compares, no loops over time. */
uint32_t CWK_SamplerTick(cwk_sampler_t *p, uint8_t raw, uint8_t debounce_ms)
{
    uint8_t rose = 0U;
    if (debounce_ms == 0U) debounce_ms = 1U;
    for (uint8_t i = 0U; i < 2U; ++i) {
        const uint8_t bit = (uint8_t)(1U << i);
        if ((uint8_t)(raw & bit) == (uint8_t)(p->stable & bit)) {
            p->count[i] = 0U;
            continue;
        }
        if (++p->count[i] < debounce_ms) continue;
        p->count[i] = 0U;
        p->stable ^= bit;
        if (p->stable & bit) {
            ++p->closures[i];
            rose |= bit;
        }
    }
    /* Two closures in the same ms: the DAH counts as the later, so a squeeze
     * from rest starts with the DIT (Morserino-32's convention). */
    if (rose & BIT(CWK_DAH)) p->last = CWK_DAH;
    else if (rose) p->last = CWK_DIT;
    return (uint32_t)(raw & 3U) | ((uint32_t)p->stable << 2) | ((uint32_t)(p->last & 3U) << 4) |
           ((uint32_t)p->closures[0] << 8) | ((uint32_t)p->closures[1] << 16);
}

void CWK_ReadInput(cwk_reader_t *r, uint32_t word, cwk_input_t *in)
{
    const uint8_t dits = CWK_WORD_DITS(word), dahs = CWK_WORD_DAHS(word);
    in->level = CWK_WORD_LEVEL(word);
    in->closed = (uint8_t)(((dits != r->seen[0]) ? BIT(CWK_DIT) : 0U) |
                           ((dahs != r->seen[1]) ? BIT(CWK_DAH) : 0U));
    in->last = CWK_WORD_LAST(word);
    r->seen[0] = dits;
    r->seen[1] = dahs;
}

uint8_t CWK_Pending(const cwk_reader_t *r, uint32_t word)
{
    return (uint8_t)((CWK_WORD_DITS(word) != r->seen[0]) || (CWK_WORD_DAHS(word) != r->seen[1]));
}

void CWK_Discard(cwk_reader_t *r, uint32_t word)
{
    r->seen[0] = CWK_WORD_DITS(word);
    r->seen[1] = CWK_WORD_DAHS(word);
}

CWK_COLD void CWK_Reset(cwk_state_t *s)
{
    const uint32_t now = s->now;
    *s = (cwk_state_t){0};
    s->now = now;
    s->first = CWK_DIT;
    s->last = CWK_DIT;
}

uint8_t CWK_Idle(const cwk_state_t *s)
{
    return (uint8_t)(s->phase == CWK_IDLE && s->mem == 0U);
}

/* The lever the operator meant first, of two wanted together. */
static inline uint8_t pick_start(const cwk_state_t *s, uint8_t want)
{
    if (want == BIT(CWK_DIT)) return CWK_DIT;
    if (want == BIT(CWK_DAH)) return CWK_DAH;
    return (s->first == CWK_DAH) ? CWK_DAH : CWK_DIT;
}

/* Morserino-32's ultimaticSelect (QST 5/1955): a remembered closure is
 * guaranteed its element, the later of two wins; with nothing remembered,
 * the lever held -- the later closed of two -- keeps control. */
static inline uint8_t ultimatic_next(const cwk_state_t *s, uint8_t level)
{
    const uint8_t pick = s->mem ? s->mem : level;
    if (pick == 3U) return (s->last == CWK_DAH) ? CWK_DAH : CWK_DIT;
    return (pick == BIT(CWK_DAH)) ? CWK_DAH : ((pick == BIT(CWK_DIT)) ? CWK_DIT : CWK_NONE);
}

/* What follows an element gap that has run out. */
static inline uint8_t gap_next(const cwk_state_t *s, const cwk_timing_t *t, uint8_t level)
{
    if (t->mode == CWK_ULTIMATIC) return ultimatic_next(s, level);
    /* The gap's lever: remembered, or still held now. The other lever was
     * sampled into the memory through the gap already. */
    const uint8_t want = (uint8_t)(s->mem | (level & BIT(s->elem)));
    if (want == 3U) {
        if (t->mode == CWK_NONSQUEEZE) return (s->first == CWK_DAH) ? CWK_DAH : CWK_DIT;
        return other(s->elem);
    }
    return (want == BIT(CWK_DAH)) ? CWK_DAH : ((want == BIT(CWK_DIT)) ? CWK_DIT : CWK_NONE);
}

static inline uint8_t start_mark(cwk_state_t *s, const cwk_timing_t *t, uint8_t e)
{
    s->phase = CWK_MARK;
    s->elem = e;
    s->left = t->mark[e];
    s->elapsed = 0U;
    /* Ultimatic spends only the memory of the element it sends. */
    s->mem = (t->mode == CWK_ULTIMATIC) ? (uint8_t)(s->mem & ~BIT(e)) : 0U;
    return (uint8_t)(CWK_EV_START | ((e == CWK_DAH) ? CWK_EV_DAH : 0U));
}

/* Ultimatic: every closure is remembered, except the sending lever coming
 * back within the chatter guard of its opening. */
static inline void ultimatic_remember(cwk_state_t *s, uint8_t closed)
{
    for (uint8_t e = CWK_DIT; e <= CWK_DAH; ++e) {
        if ((closed & BIT(e)) == 0U) continue;
        if (e == s->elem && (uint32_t)(s->now - s->opened[e]) < CWK_CHATTER_SAMPLES) continue;
        s->mem |= BIT(e);
    }
}

/* One mark sample: what the other lever does counts once its window is open. */
static inline uint8_t mark_sample(cwk_state_t *s, const cwk_timing_t *t, uint8_t level, uint8_t closed)
{
    const uint8_t opp = BIT(other(s->elem));
    if (t->mode == CWK_ULTIMATIC) {
        ultimatic_remember(s, closed);
    } else if ((t->mode == CWK_IAMBIC_B && s->elapsed >= t->curtis[s->elem]) || t->mode == CWK_NONSQUEEZE) {
        s->mem |= (uint8_t)((level | closed) & opp);
    }
    ++s->elapsed;
    if (s->left > 1U) { --s->left; return CWK_EV_KEY; }
    s->phase = CWK_GAP;
    s->left = t->gap;
    s->elapsed = 0U;
    return (uint8_t)(CWK_EV_KEY | CWK_EV_END);
}

/* One gap sample: the other lever counts throughout, the same lever only as
 * a fresh closure past the latency window. Between characters (ACS) any
 * lever counts. */
static inline void gap_sample(cwk_state_t *s, const cwk_timing_t *t, uint8_t level, uint8_t closed)
{
    if (t->mode == CWK_ULTIMATIC) {
        ultimatic_remember(s, closed);
    } else if (s->phase == CWK_CHAR_GAP) {
        s->mem |= (uint8_t)(level | closed);
    } else {
        s->mem |= (uint8_t)((level | closed) & BIT(other(s->elem)));
        if (s->elapsed >= t->latency) s->mem |= (uint8_t)(closed & BIT(s->elem));
    }
    ++s->elapsed;
    if (s->left) --s->left;
}

/* Which lever was down first, which closed last, and when each opened. */
static inline void track_levers(cwk_state_t *s, const cwk_input_t *in, uint8_t level, uint8_t closed)
{
    const uint8_t fell = (uint8_t)(s->prev & ~level);
    if (fell & BIT(CWK_DIT)) s->opened[CWK_DIT] = s->now;
    if (fell & BIT(CWK_DAH)) s->opened[CWK_DAH] = s->now;
    if (closed == 3U) s->last = (in->last == CWK_DIT) ? CWK_DIT : CWK_DAH;
    else if (closed) s->last = (closed == BIT(CWK_DAH)) ? CWK_DAH : CWK_DIT;
    if (level == 3U) {
        /* Both down: the first is whichever was down already, or of two
         * closed together the earlier (the sampler names the later). */
        if (s->prev == BIT(CWK_DIT) || s->prev == BIT(CWK_DAH)) s->first = s->prev;
        else if (s->prev == 0U) s->first = (closed == 3U) ? other(s->last) : CWK_DIT;
    } else if (level) {
        s->first = level;
    } else if (closed == 3U) {
        s->first = other(s->last);
    } else if (closed) {
        s->first = closed;
    }
    s->prev = level;
}

/* Non-squeeze: with both levers down only the first counts, and the other's
 * closure is no closure. */
static inline void nonsqueeze_filter(const cwk_state_t *s, uint8_t *level, uint8_t *closed)
{
    if (*level != 3U) return;
    const uint8_t keep = (s->first == CWK_DAH) ? BIT(CWK_DAH) : BIT(CWK_DIT);
    *level = keep;
    *closed &= keep;
}

/* A gap has run out: the next element, the space between characters, or
 * rest. Out of line: once per element, and it keeps CWK_Step small. */
static __attribute__((noinline)) uint8_t gap_end(cwk_state_t *s, const cwk_timing_t *t, uint8_t level)
{
    const uint8_t was_char_gap = (uint8_t)(s->phase == CWK_CHAR_GAP);
    uint8_t next;
    if (t->mode == CWK_ULTIMATIC) next = ultimatic_next(s, level);
    else if (was_char_gap) next = (uint8_t)((s->mem | level) ? pick_start(s, (uint8_t)(s->mem | level)) : CWK_NONE);
    else next = gap_next(s, t, level);
    if (next != CWK_NONE) return start_mark(s, t, next);
    if (!was_char_gap && t->char_extra) {
        s->phase = CWK_CHAR_GAP;
        s->left = t->char_extra;
        s->elapsed = 0U;
        return CWK_EV_CHAR_END;
    }
    s->phase = CWK_IDLE;
    s->mem = 0U;
    return was_char_gap ? 0U : CWK_EV_CHAR_END;
}

CWK_HOT uint8_t CWK_Step(cwk_state_t *s, const cwk_timing_t *t, const cwk_input_t *in)
{
    uint8_t level = (uint8_t)(in->level & 3U);
    uint8_t closed = (uint8_t)(in->closed & 3U);
    uint8_t ev = 0U;
    ++s->now;
    track_levers(s, in, level, closed);
    if (t->mode == CWK_NONSQUEEZE) nonsqueeze_filter(s, &level, &closed);

    if ((s->phase == CWK_GAP || s->phase == CWK_CHAR_GAP) && s->left == 0U) ev = gap_end(s, t, level);
    if (s->phase == CWK_IDLE) {
        const uint8_t want = (uint8_t)(level | closed | s->mem);
        if (want == 0U) return ev;
        if (t->mode == CWK_ULTIMATIC) s->mem |= closed;
        ev |= start_mark(s, t, (t->mode == CWK_ULTIMATIC && (s->mem | level) == 3U)
                                   ? CWK_DIT : pick_start(s, want));
        closed = 0U; /* the closure that started it is spent */
    }
    if (s->phase == CWK_MARK) return (uint8_t)(ev | mark_sample(s, t, level, closed));
    gap_sample(s, t, level, closed);
    return ev;
}

/* ---- Messages (radio memories) ---------------------------------------------- */

/* The next element of the code sounding, CWK_NONE once it is spent. */
static uint8_t seq_elem(cwk_seq_t *q)
{
    if (q->bits == 0U) return CWK_NONE;
    --q->bits;
    return (uint8_t)(((q->code[q->pos] >> q->bits) & 1U) ? CWK_DAH : CWK_DIT);
}

static uint8_t seq_load(cwk_seq_t *q)
{
    uint8_t n = 0U;
    for (uint8_t c = q->code[q->pos]; c > 1U; c >>= 1) ++n;
    q->bits = n;
    return seq_elem(q);
}

CWK_COLD uint8_t CWK_SeqStart(cwk_seq_t *q, const uint8_t *code, uint8_t len)
{
    q->code = code;
    q->len = len;
    q->pos = 0U;
    q->wait = 0U;
    while (q->pos < len && code[q->pos] < 2U) ++q->pos;
    q->hold = (q->pos < len) ? BIT(seq_load(q)) : 0U;
    return (uint8_t)(q->hold != 0U);
}

uint8_t CWK_SeqLevel(cwk_seq_t *q)
{
    if (q->wait && --q->wait == 0U) q->hold = BIT(seq_load(q));
    return q->hold;
}

uint8_t CWK_SeqAfter(cwk_seq_t *q, const cwk_timing_t *t, uint8_t ev)
{
    if (ev & CWK_EV_START) q->hold = 0U; /* taken */
    if ((ev & CWK_EV_END) == 0U) return 1U;
    const uint8_t e = seq_elem(q);
    if (e != CWK_NONE) {
        q->hold = BIT(e); /* through the gap: taken at its end */
        return 1U;
    }
    /* The character is out: the next one after its space, from this key-up
     * (the wait counts from the next sample, so +1). */
    uint8_t word = 0U;
    while (++q->pos < q->len && q->code[q->pos] < 2U) word = 1U;
    if (q->pos >= q->len) return 0U;
    q->wait = t->gap + (word ? 6U : 2U) * t->unit + 1U;
    return 1U;
}


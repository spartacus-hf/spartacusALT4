#ifndef MOD_CW_KEYER_H
#define MOD_CW_KEYER_H

/* The CW keyer's state machine, hardware-free: one step per 48 kHz sample,
 * from the debounced paddle levels and the closures the 1 ms sampler counted
 * (mod_cw.c). render_cw (transmit) and the headphone renderer (sidetone,
 * practice, the trainer) both run it, and tools/tests/cw_keyer_test.c drives
 * this same code.
 *
 * Timing is integer samples, worked out when a setting changes
 * (CWK_TimingInit): no float and no division per sample.
 *
 * What a lever does while an element is being sent is the keyer's memory,
 * per mode (Morserino-32 is the reference):
 *  - IAMBIC A: the other lever is remembered from the end of the mark, in
 *    the element gap; a squeeze let go during a mark stops after it.
 *  - IAMBIC B: the other lever is remembered from CURTIS DIT/DAH percent into
 *    the mark (0 % is the original Curtis B: from the start). Letting a
 *    squeeze go before that point stops like A, after it adds the opposite
 *    element -- the same rule for a held lever and a squeeze release.
 *  - The lever of the element just sent repeats on its level at the end of
 *    the gap (holding it repeats), or on a fresh closure after KEY LATENCY
 *    eighths of a dit into the gap: a quick re-tap counts, a slow release
 *    does not add an element.
 *  - ULTIMATIC: every closure is remembered and guaranteed its element; with
 *    both levers held the one closed last has control. Curtis and latency do
 *    not apply; a re-closure of the sending lever within 5 ms of its opening
 *    is contact bounce.
 *  - NON-SQUEEZE: a squeeze is ignored -- the lever held first keeps
 *    control, as on a single-lever paddle.
 *  - AUTO SPACE (ACS): after a character, the next one waits until the space
 *    since key-up is N dits; levers closed meanwhile are kept.
 *
 * Weight lengthens every mark by d = u (w - 50) / 50 and shortens the gap
 * after it by the same, so dit + gap and dah + gap stay 2 and 4 dits. */

#include <stdint.h>

enum { CWK_NONE = 0U, CWK_DIT = 1U, CWK_DAH = 2U };
enum { CWK_IDLE = 0U, CWK_MARK, CWK_GAP, CWK_CHAR_GAP };
/* Same values as cw_keyer_mode_t (modulator.h). */
enum { CWK_STRAIGHT = 0U, CWK_IAMBIC_A, CWK_IAMBIC_B, CWK_ULTIMATIC, CWK_NONSQUEEZE, CWK_MODE_COUNT };

/* Events cwk_step reports for the sample. */
#define CWK_EV_KEY       0x01U  /* the key is down this sample */
#define CWK_EV_START     0x02U  /* a mark started this sample */
#define CWK_EV_END       0x04U  /* a mark ended this sample (key-up) */
#define CWK_EV_DAH       0x08U  /* with START: the mark is a dah */
#define CWK_EV_CHAR_END  0x10U  /* the character ended: no element follows the gap */

/* A ms of the 1 ms sampler, in samples. */
#define CWK_SAMPLES_PER_MS      48U
#define CWK_CHATTER_SAMPLES     (5U * CWK_SAMPLES_PER_MS)

typedef struct {
    uint32_t mark[3];      /* [CWK_DIT], [CWK_DAH]: weighted mark lengths */
    uint32_t gap;          /* element gap after a mark */
    uint32_t char_extra;   /* ACS: more space when a character ends; 0 off */
    uint32_t curtis[3];    /* B: samples into a mark before the other lever counts */
    uint32_t latency;      /* samples into a gap before the same lever's closure counts */
    uint32_t unit;         /* one dit, unweighted */
    uint8_t  mode;
} cwk_timing_t;

typedef struct {
    uint8_t level;         /* bit 0 DIT, bit 1 DAH: debounced and gated */
    uint8_t closed;        /* levers closed since the last step (same bits) */
    uint8_t last;          /* CWK_DIT/CWK_DAH: the lever closed most recently */
} cwk_input_t;

typedef struct {
    uint32_t left;         /* samples left in the phase */
    uint32_t elapsed;      /* samples into the phase */
    uint32_t now;          /* free-running sample count (chatter guard) */
    uint32_t opened[3];    /* `now` when each lever last opened */
    uint8_t  phase;
    uint8_t  elem;         /* element being or last sent */
    uint8_t  mem;          /* levers remembered for after the gap */
    uint8_t  first;        /* lever held first (squeeze order) */
    uint8_t  prev;         /* last step's levels */
    uint8_t  last;         /* lever closed most recently */
} cwk_state_t;

/* The 1 ms paddle sampler (SysTick, MOD_CwInputTick): debounces each lever
 * on press and on release and counts closures, so a tap the keyer is not
 * stepping for -- an idle keyer, a renderer that steps in bursts -- is
 * still there when it looks. It publishes one 32-bit word:
 * bits 0-1 raw levels, 2-3 debounced levels, 4-5 the lever closed last,
 * 8-15 DIT closures, 16-23 DAH closures. */
typedef struct {
    uint8_t stable;        /* debounced levels */
    uint8_t last;          /* lever closed most recently */
    uint8_t count[2];      /* ms each lever's raw level has differed from stable */
    uint8_t closures[2];   /* running closure counts */
} cwk_sampler_t;

#define CWK_WORD_RAW(w)     ((uint8_t)((w) & 3U))
#define CWK_WORD_LEVEL(w)   ((uint8_t)(((w) >> 2) & 3U))
#define CWK_WORD_LAST(w)    ((uint8_t)(((w) >> 4) & 3U))
#define CWK_WORD_DITS(w)    ((uint8_t)((w) >> 8))
#define CWK_WORD_DAHS(w)    ((uint8_t)((w) >> 16))

/* One ms: `raw` levels (bit 0 DIT, bit 1 DAH, after SWAP); a lever changes
 * once its raw level has held `debounce_ms` ms (0 and 1: the next ms). */
uint32_t CWK_SamplerTick(cwk_sampler_t *p, uint8_t raw, uint8_t debounce_ms);

/* The keyer's side: the closures it has taken from the word. */
typedef struct {
    uint8_t seen[2];
} cwk_reader_t;

/* The word as one step's input; closures since the last call. */
void CWK_ReadInput(cwk_reader_t *r, uint32_t word, cwk_input_t *in);
/* Closures in the word the reader has not taken. */
uint8_t CWK_Pending(const cwk_reader_t *r, uint32_t word);
/* Take every closure in the word without acting on it (a gated keyer). */
void CWK_Discard(cwk_reader_t *r, uint32_t word);

/* Timing for `wpm` (5..60), weight percent (35..65), Curtis percents
 * (0..100), latency eighths (0..8), ACS dits (0 off, 2..4), mode. */
void CWK_TimingInit(cwk_timing_t *t, float wpm, float weight, float curtis_dit_pct,
                    float curtis_dah_pct, uint8_t latency_eighths, uint8_t acs_dits, uint8_t mode);
void CWK_Reset(cwk_state_t *s);
/* One sample. Straight key is not stepped here: the key follows the level. */
uint8_t CWK_Step(cwk_state_t *s, const cwk_timing_t *t, const cwk_input_t *in);
/* Nothing in progress and nothing remembered: stepping would change nothing
 * until a lever moves. */
uint8_t CWK_Idle(const cwk_state_t *s);

/* A message the keyer sends by itself (the radio memories): the sequencer
 * holds the lever of each element from the key-up before it until the
 * keyer starts it -- a lever held through the gap is taken at its end in
 * every mode -- and waits out the space between characters (gap + 2 dits)
 * and words (gap + 6) from the key-up. So weight, ACS and break-in work as
 * with the paddle. Codes as mod_morse_tables.h's, 0 a word space; the
 * array must outlive the message. */
typedef struct {
    const uint8_t *code;
    uint32_t wait;         /* samples until the next character's lever, 0: none */
    uint8_t  len, pos;     /* codes, and the one sounding */
    uint8_t  bits;         /* elements of it still to come */
    uint8_t  hold;         /* lever held now (CWK_DIT/CWK_DAH bit), 0 none */
} cwk_seq_t;

/* Start: 0 if there is nothing to send. */
uint8_t CWK_SeqStart(cwk_seq_t *q, const uint8_t *code, uint8_t len);
/* Before each CWK_Step: the levers the message holds. */
uint8_t CWK_SeqLevel(cwk_seq_t *q);
/* After it, with its events: 0 once the last element has started its
 * key-up (the keyer finishes it on its own). */
uint8_t CWK_SeqAfter(cwk_seq_t *q, const cwk_timing_t *t, uint8_t ev);

#endif /* MOD_CW_KEYER_H */

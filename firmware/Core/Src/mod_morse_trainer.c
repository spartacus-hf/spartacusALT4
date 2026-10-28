#pragma GCC optimize ("Os") /* off the real-time paths: built for size */

/* The Morse trainer (mod_morse_trainer.h).
 *
 * The keyer reports each element as it happens -- a paddle element's start
 * with its kind, a straight key's press, and every key-up -- timestamped in
 * ms (MTRAIN_KeyEvent). The main loop reads them in order and classifies the
 * silence before each key-down against the character and word gaps before
 * it takes the element, so a main loop held up by the display or the card
 * still decodes E E as E E, never as I. Silence that is still going on is
 * judged only once every event so far has been read.
 *
 * What it teaches follows Morserino-32: its Koch orders, its draw (two
 * thirds over the lesson, one third from its newest third), its adaptive
 * weights for weak characters, its echo trainer's flow. What is new is the
 * course START LEARNING runs -- a script in the pack (tools/cw_pack/
 * steps.txt) over these activities, which moves on to the next lesson by
 * itself at 90 % -- grading that lines the answer up with the prompt, and
 * first-try statistics. The activities on their own only recommend the
 * next character. */

#include "mod_morse_trainer.h"
#include "mod_cw_player.h"
#include "cw_pack.h"
#include "modulator.h"
#include <string.h>

#if defined(__GNUC__) && !defined(MTRAIN_HOST)
#define MT_CCM __attribute__((section(".ccmram_bss")))
#else
#define MT_CCM
#endif

#define MT_Q          32U         /* key events; a power of two */
#define MT_TIME_MASK  0x3FFFFFFFUL
#define MT_ERASE      0xFEU       /* seven dits or more: the answer so far is wrong */
#define MT_SK_NOISE   8U          /* ms: a straight-key press shorter than this is a bounce */
#define MT_RAMP       240U        /* 5 ms key-edge ramp on what the trainer plays */
#define MT_WIN        50U         /* first attempts the lesson's score is taken over */
#define MT_NEWEST_WIN 20U
#define MT_LEARN_RUN  3U          /* right in a row to learn a character */
#define MT_RETRY_MS   2000U       /* an error checks the card again this often */

typedef struct {
    uint32_t up_ms, down_ms;
    uint16_t sk_dit, sk_dah;      /* a straight key's dit and dah, ms, as keyed */
    uint8_t code;                 /* 1, then the elements so far (dah 1) */
    uint8_t n, dahs;
    uint8_t down, straight;
    uint8_t spaced;               /* the word space after the last symbol is taken */
    uint8_t any;                  /* an element since the levers were given back */
} mt_dec_t;

typedef struct {
    mtrain_view_t v;
    mt_dec_t d;
    uint32_t rng, now, t_state, t_last, played_ms;
    uint32_t session_ms;
    uint64_t unseen;              /* adaptive: lesson places not drawn yet */
    uint8_t stops[8];             /* where the course's lessons end: bit i, i + 1 characters */
    uint8_t course[MORSE_SYMBOLS];
    uint8_t weight[MORSE_SYMBOLS];
    uint8_t words[CWPACK_WORDS_MAX / 8U];
    uint8_t abbr[CWPACK_ABBR_MAX / 8U];
    int16_t n_words, n_abbr;      /* eligible; -1 not scanned for this lesson */
    uint8_t course_key;           /* course and carousel loaded, 0xFF none */
    uint8_t lesson_min;
    uint8_t learn;                /* LEARN: the character */
    uint8_t intro;                /* LEARN: the first playing repeats it */
    uint8_t run;                  /* LEARN: right in a row */
    uint8_t trials;               /* course LISTEN, ECHO: prompts left (ECHO 0: no end) */
    uint8_t step;                 /* course: the step running (index) */
    uint8_t op, a;                /* its op and first argument (CWPACK_STEP_*) */
    uint8_t src, len, show;       /* what it plays: source, group, how an echo prompt is given */
    uint8_t intro_at;             /* INTRO: the course place being taught */
    uint8_t explore;              /* EXPLORE: 1 dits held, 2 dahs held, 4 a squeeze */
    uint8_t fetch;                /* LOAD: read the step first */
    uint8_t act;                  /* the activity running */
    uint8_t resume;               /* the state a pause interrupted */
    uint8_t hold, submit, submit_wr, lev_prev;
    uint8_t open;                 /* THINK: the levers were given back */
    uint8_t replay;               /* PLAY: start the prompt once the player is free */
    uint8_t begin;                /* LOAD: start the activity once the course is in */
    uint8_t wpm;                  /* listen speed this session (adaptation) */
    uint8_t dirty;
    /* How the learner spaces, in sixteenths of a dit, learnt this session
     * as the receive decoder learns a sender (radio_cw_decoder.c): the gap
     * inside a letter and the gap between letters. */
    uint8_t space16, char16;
    uint8_t gap8[MTRAIN_ANSWER_MAX]; /* the gap before answer[i], 8 ms units */
    uint8_t sight;                   /* the course's prompt is shown, not played */
} mt_t;

static mt_t mt MT_CCM;
static mtrain_prefs_t prefs;
static mtrain_progress_t prog;

/* The event queue: the keyer (render or headphone context) writes, the main
 * loop reads. Kind in bits 30-31, ms in 0-29. */
static volatile uint32_t mt_q[MT_Q] MT_CCM;
static volatile uint8_t mt_wr, mt_rd, mt_lost, mt_capture;

/* ---- Keyer side ----------------------------------------------------------- */

uint8_t MTRAIN_Capturing(void) { return mt_capture; }

void MTRAIN_KeyEvent(uint8_t ev, uint32_t ms)
{
    if (!mt_capture) return;
    const uint8_t wr = mt_wr, next = (uint8_t)((wr + 1U) & (MT_Q - 1U));
    if (next == mt_rd) {
        mt_lost = 1U;
        return;
    }
    mt_q[wr] = ((uint32_t)ev << 30) | (ms & MT_TIME_MASK);
    mt_wr = next;
}

/* ---- Small things ----------------------------------------------------------- */

static uint32_t rnd(void)
{
    uint32_t x = mt.rng ? mt.rng : 0x6D2B79F5UL;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    mt.rng = x;
    return x;
}

static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0U; }

static uint8_t clamp8(uint32_t v, uint8_t lo, uint8_t hi) { return (uint8_t)(v < lo ? lo : (v > hi ? hi : v)); }

static void enter(uint8_t s)
{
    mt.v.state = s;
    mt.t_state = mt.now;
}

static uint32_t since(void) { return mt.now - mt.t_state; }

static uint8_t popcount64(uint64_t x)
{
    uint8_t n = 0U;
    for (; x; x &= x - 1U) ++n;
    return n;
}

/* ---- Course and lesson ------------------------------------------------------ */

/* LICW's carousel (Morserino-32's Koch::setupLICWkochChars): basic course 1
 * is the first 18 characters entered at `start`; basic course 2 keeps them
 * and enters the next 26 at `start`. Returns the course's length. */
static uint8_t licw_rotate(uint8_t *c, uint8_t start)
{
    uint8_t t[44];
    uint32_t pos = 0U;
    const uint32_t s = 3U * start;
    if (start < 6U) {
        memcpy(t, &c[s], 18U - s);
        pos = 18U - s;
        memcpy(&t[pos], c, s);
        memcpy(c, t, 18U);
        return 18U;
    }
    memcpy(t, c, 18U);
    pos = 18U;
    memcpy(&t[pos], &c[s], 44U - s);
    pos += 44U - s;
    memcpy(&t[pos], &c[18], s - 18U);
    memcpy(c, t, 44U);
    return 44U;
}

uint8_t MTRAIN_LessonMax(void) { return mt.v.lesson_max; }

static uint8_t *lesson_slot(void) { return &prog.lesson[prefs.course < MTRAIN_COURSES ? prefs.course : 0U]; }

/* Morserino-32's starting weights: the five newest characters 2..6, the rest 1. */
static void weights_init(void)
{
    const uint8_t n = mt.v.lesson;
    for (uint8_t i = 0U; i < MORSE_SYMBOLS; ++i)
        mt.weight[i] = (i >= n) ? 0U : ((uint8_t)(i + 6U) > n ? (uint8_t)(7U + i - n) : 1U);
    mt.unseen = (n >= 64U) ? ~0ULL : ((1ULL << n) - 1U);
}

/* The lesson's score starts again. */
static void window_clear(void)
{
    prog.win = 0U;
    prog.win_n = 0U;
    prog.newest_win = 0U;
    prog.newest_n = 0U;
    mt.v.window = 0U;
    mt.dirty = 1U;
}

/* A lesson of the course ends with n characters (n >= 1). */
static uint8_t ends(uint8_t n)
{
    --n;
    return (uint8_t)((mt.stops[n >> 3] >> (n & 7U)) & 1U);
}

/* The first lesson end at `n` characters or more (the course's own lesson
 * groups: Koch one at a time, CW Academy its sessions', LICW three); the
 * whole course at the latest. */
static uint8_t stop_from(uint8_t n)
{
    while (n < mt.v.lesson_max && !ends(n)) ++n;
    return n;
}

/* Where the lesson before this one ended: its first new character. */
static uint8_t stop_before(void)
{
    uint8_t k = (uint8_t)(mt.v.lesson - 1U);
    while (k && !ends(k)) --k;
    return k;
}

static void lesson_set(uint8_t lesson)
{
    lesson = clamp8(lesson, mt.lesson_min, mt.v.lesson_max);
    if (lesson != *lesson_slot()) {
        *lesson_slot() = lesson;
        window_clear();
    }
    mt.v.lesson = lesson;
    mt.v.newest = mt.course[lesson - 1U];
    mt.v.recommend = 0U;
    mt.n_words = mt.n_abbr = -1;
    weights_init();
}

/* The course and carousel of prefs, from the pack or CW/CUSTOM.TXT. */
static uint8_t course_load(void)
{
    const uint8_t key = (uint8_t)(prefs.course * 16U + prefs.carousel);
    if (mt.course_key == key) return 1U;
    uint8_t n;
    mt.lesson_min = 1U;
    memset(mt.stops, 0xFF, sizeof(mt.stops));
    mt.stops[0] = 0xFEU; /* CUSTOM: two, then one more each lesson */
    if (prefs.course == MTRAIN_COURSE_CUSTOM) {
        n = CWPACK_Custom(mt.course);
    } else {
        n = CWPACK_Course((uint8_t)(prefs.course < MTRAIN_COURSE_CUSTOM ? prefs.course : 0U), mt.course, mt.stops);
        if (n && prefs.course == MTRAIN_COURSE_LICW) {
            n = licw_rotate(mt.course, prefs.carousel);
            if (n > 18U) mt.lesson_min = 19U;
        }
    }
    if (n == 0U) return 0U;
    mt.v.lesson_max = n;
    mt.course_key = key;
    const uint8_t want = *lesson_slot();
    lesson_set(want ? want : stop_from(mt.lesson_min)); /* no lesson ends with one character */
    return 1U;
}

/* The lesson's characters as a mask of MORSE_Index bits. */
static uint64_t lesson_mask(void)
{
    uint64_t m = 0U;
    for (uint8_t i = 0U; i < mt.v.lesson; ++i) m |= 1ULL << MORSE_Index(mt.course[i]);
    return m;
}

/* ---- Drawing prompts ------------------------------------------------------- */

static uint8_t draw_char(void)
{
    const uint32_t n = mt.v.lesson;
    uint32_t i;
    if (prefs.boost || mt.v.guided) { /* the course always draws adaptively */
        if (mt.unseen) {
            /* Morserino's opening sequence: each character once, in random order. */
            uint32_t k = rnd_below(popcount64(mt.unseen));
            for (i = 0U; i < n; ++i)
                if (((mt.unseen >> i) & 1U) && k-- == 0U) break;
            mt.unseen &= ~(1ULL << i);
        } else {
            uint32_t sum = 0U;
            for (i = 0U; i < n; ++i) sum += mt.weight[i];
            uint32_t r = rnd_below(sum);
            for (i = 0U; i + 1U < n && r >= mt.weight[i]; ++i) r -= mt.weight[i];
        }
    } else if (rnd_below(3U)) {
        i = rnd_below(n);
    } else {
        /* The newest third a little more often (Koch::getRandomChar). */
        i = 2U * n / 3U + rnd_below(n - 2U * n / 3U);
    }
    return mt.course[i < n ? i : n - 1U];
}

/* Record r of the eligible ones, favouring the front: the pack lists the
 * most frequent words first. */
static int32_t pick_listed(const uint8_t *bitmap, int16_t count)
{
    if (count <= 0) return -1;
    uint32_t a = rnd_below((uint32_t)count), b = rnd_below((uint32_t)count);
    uint32_t r = a < b ? a : b;
    for (uint32_t i = 0U; i < CWPACK_WORDS_MAX; ++i)
        if (((bitmap[i >> 3] >> (i & 7U)) & 1U) && r-- == 0U) return (int32_t)i;
    return -1;
}

/* A word or an abbreviation made of the lesson's characters. -1: the card
 * could not be read now; 0: none fits. */
static int8_t pick_text(cwpack_list_t list)
{
    int16_t *count = (list == CWPACK_ABBR) ? &mt.n_abbr : &mt.n_words;
    uint8_t *bitmap = (list == CWPACK_ABBR) ? mt.abbr : mt.words;
    if (*count < 0) {
        const int32_t n = CWPACK_Scan(list, lesson_mask(), CWPACK_TEXT_MAX, bitmap);
        if (n < 0) return -1;
        *count = (int16_t)n;
    }
    const int32_t at = pick_listed(bitmap, *count);
    if (at < 0) return 0;
    const uint8_t n = CWPACK_Text(list, (uint16_t)at, mt.v.prompt);
    if (n == 0U) return -1;
    mt.v.prompt_len = n;
    return 1;
}

/* A call sign: a prefix as often as it is heard, a digit unless the prefix
 * ends in one (and is longer than two), one to three letters
 * (getRandomCall, unlimited length). */
static int8_t make_call(void)
{
    uint8_t *c = mt.v.prompt;
    uint8_t n = CWPACK_Prefix(rnd(), c);
    if (n == 0U) return -1;
    if (!(c[n - 1U] >= '0' && c[n - 1U] <= '9' && n > 2U)) c[n++] = (uint8_t)('0' + rnd_below(10U));
    uint32_t k = 1U + rnd_below(3U);
    if (k == 1U && rnd_below(3U)) k = 2U + rnd_below(2U);
    while (k--) c[n++] = (uint8_t)('A' + rnd_below(26U));
    c[n] = 0U;
    mt.v.prompt_len = n;
    return 1;
}

static void make_group(uint8_t len)
{
    for (uint8_t i = 0U; i < len; ++i) mt.v.prompt[i] = draw_char();
    mt.v.prompt[len] = 0U;
    mt.v.prompt_len = len;
}

/* The next prompt. -1: the card could not be read now (try again). */
static int8_t make_prompt(void)
{
    if (mt.act == MTRAIN_LEARN) {
        mt.v.prompt[0] = mt.learn;
        mt.v.prompt[1] = 0U;
        mt.v.prompt_len = 1U;
        return 1;
    }
    uint8_t src = mt.v.guided ? mt.src : prefs.src;
    const uint8_t len = mt.v.guided ? mt.len : clamp8(prefs.group, 1U, MTRAIN_GROUP_MAX);
    if (src == CWPACK_SRC_DRILL) {
        /* CW Academy's sending drill: each of the lesson's newest (up to
         * four) in turn, `len` times: KKKKK MMMMM. */
        const uint8_t k = mt.v.lesson < 4U ? mt.v.lesson : 4U;
        memset(mt.v.prompt, mt.course[mt.v.lesson - 1U - mt.trials % k], len);
        mt.v.prompt[len] = 0U;
        mt.v.prompt_len = len;
        return 1;
    }
    if (src == MTRAIN_MIXED) src = (uint8_t)rnd_below(4U);
    int8_t r = 0;
    if (src == MTRAIN_WORDS || src == MTRAIN_ABBR) r = pick_text(src == MTRAIN_ABBR ? CWPACK_ABBR : CWPACK_WORDS);
    else if (src == MTRAIN_CALLS) r = make_call();
    if (r < 0) return -1;
    if (r == 0) make_group(len);
    return 1;
}

/* ---- Playing ----------------------------------------------------------------- */

/* What was played, into the record: listening counts as exposure. */
static void heard(void)
{
    for (uint8_t i = 0U; i < mt.v.prompt_len; ++i) {
        const uint8_t x = MORSE_Index(mt.v.prompt[i]);
        if (x < MORSE_SYMBOLS && prog.heard[x] != 0xFFFFU) ++prog.heard[x];
    }
    mt.dirty = 1U;
}


static void gate(uint8_t on) { MOD_CwSetTrainGate(on); }

static void play_prompt(void)
{
    cwp_prog_t p;
    uint8_t text[2U * MTRAIN_PROMPT_MAX];
    uint8_t n = 0U;
    memset(&p, 0, sizeof(p));
    const float wpm = (float)mt.wpm;
    float eff = (float)prefs.pace_wpm;
    if (eff > wpm) eff = wpm;
    CWP_Timing(&p, wpm, eff, prefs.manual ? prefs.ics : 0U, prefs.manual ? prefs.iws : 0U, MT_RAMP);
    for (uint8_t k = 0U; k < (mt.intro ? 3U : 1U); ++k) {
        if (k) text[n++] = ' ';
        for (uint8_t i = 0U; i < mt.v.prompt_len && n < sizeof(text); ++i) text[n++] = mt.v.prompt[i];
    }
    mt.intro = 0U;
    CWP_SetText(&p, text, n);
    (void)CWP_Start(&p);
    mt.played_ms = CWP_Duration(&p) / 48U;
    heard();
}

/* Feedback: a quick double beep for right, one long beep for wrong, at the
 * sidetone's pitch (Morserino's rising and falling pairs need a second
 * oscillator). */
static void beep(uint8_t right)
{
    cwp_prog_t p;
    if (!prefs.tone) return;
    memset(&p, 0, sizeof(p));
    p.text[0] = p.text[1] = right ? MORSE_Code('E') : MORSE_Code('T');
    p.len = right ? 2U : 1U;
    p.dit = 2880U;                 /* 60 ms */
    p.dah = 19200U;                /* 400 ms */
    p.elem_gap = p.dit;
    p.char_gap = p.word_gap = 3360U;
    p.lead = 48U;
    p.ramp = MT_RAMP;
    p.level = 200U;
    (void)CWP_Start(&p);
}

/* ---- Decoding ---------------------------------------------------------------- */

static uint32_t dit_ms(void)
{
    if (mt.d.straight) return mt.d.sk_dit;
    const uint32_t wpm = prefs.key_wpm ? prefs.key_wpm : 15U;
    return (1200U + wpm / 2U) / wpm;
}

/* Move a spacing toward a gap just keyed: a quarter of the way, or three
 * quarters for one a prompt showed was inside a letter. */
static __attribute__((noinline)) void learn(uint8_t *est, uint32_t gap_ms, uint8_t proven)
{
    uint32_t g = gap_ms * 16U / dit_ms();
    if (g > 255U) g = 255U;
    const int32_t d = (int32_t)g - (int32_t)*est;
    *est = (uint8_t)((int32_t)*est + (proven ? d * 3 / 4 : d / 4));
}

/* Silence that ends a letter: half way between the learner's two gaps, as
 * the receive decoder splits at a dit plus its sender's space, taking the
 * gap between letters as at least twice the one inside until letters were
 * keyed; never under the standard 2.5 dits, never over 5. A newcomer who
 * leaves long pauses inside a letter -- M as two dahs with a breath between
 * -- was read as two letters at any speed. With `word`, the silence that
 * ends a word: two dits more, at least the standard 5. */
static uint32_t gap_ms(uint8_t word)
{
    const uint32_t in = mt.space16, between = mt.char16 > 2U * in ? mt.char16 : 2U * in;
    uint32_t g = (in + between) / 2U;
    if (g < MTRAIN_CHAR_GAP_TENTHS * 16U / 10U) g = MTRAIN_CHAR_GAP_TENTHS * 16U / 10U;
    if (g > 80U) g = 80U;
    if (word) g = (g + 32U > MTRAIN_WORD_GAP_TENTHS * 16U / 10U) ? g + 32U : MTRAIN_WORD_GAP_TENTHS * 16U / 10U;
    return g * dit_ms() / 16U;
}

static void dec_reset(void)
{
    const uint8_t straight = mt.d.straight;
    const uint16_t dit = (uint16_t)dit_ms(); /* a straight key's as keyed, else the keyer's */
    memset(&mt.d, 0, sizeof(mt.d));
    mt.d.code = 1U;
    mt.d.straight = straight;
    mt.d.sk_dit = dit;
    mt.d.sk_dah = (uint16_t)(3U * dit);
}

static void line_add(uint8_t sym)
{
    if (sym == ' ' && (mt.v.line_len == 0U || mt.v.line[mt.v.line_len - 1U] == ' ')) return;
    if (mt.v.line_len >= MTRAIN_LINE_MAX) {
        memmove(mt.v.line, &mt.v.line[1], MTRAIN_LINE_MAX - 1U);
        --mt.v.line_len;
    }
    mt.v.line[mt.v.line_len++] = sym;
    mt.v.line[mt.v.line_len] = 0U;
}

static void on_symbol(uint8_t sym)
{
    if (mt.v.state == MTRAIN_KEYING) {
        if (sym != MT_ERASE) {
            line_add(sym);
            return;
        }
        /* The error prosign takes the word back. */
        while (mt.v.line_len && mt.v.line[mt.v.line_len - 1U] == ' ') --mt.v.line_len;
        while (mt.v.line_len && mt.v.line[mt.v.line_len - 1U] != ' ') --mt.v.line_len;
        mt.v.line[mt.v.line_len] = 0U;
        return;
    }
    if (sym == MT_ERASE) {
        mt.v.answer_len = 0U;
        mt.v.answer[0] = 0U;
        enter(MTRAIN_THINK);
        return;
    }
    if (mt.v.answer_len < MTRAIN_ANSWER_MAX) {
        mt.v.answer[mt.v.answer_len++] = sym;
        mt.v.answer[mt.v.answer_len] = 0U;
    }
}

/* The elements keyed so far are a symbol. */
static void sym_commit(void)
{
    const uint8_t n = mt.d.n, dahs = mt.d.dahs;
    if (n == 0U) return;
    /* What the keyer was tried with: a run of dits, of dahs, both. */
    if (n >= 3U) mt.explore |= (uint8_t)(dahs == 0U ? 1U : (dahs == n ? 2U : 4U));
    uint8_t sym;
    if (mt.d.n >= 7U && mt.d.dahs == 0U) sym = MT_ERASE;
    else if (mt.d.n > MORSE_ELEMENTS_MAX || (sym = MORSE_Symbol(mt.d.code)) == 0U) sym = '*';
    mt.d.code = 1U;
    mt.d.n = mt.d.dahs = 0U;
    mt.d.spaced = 0U;
    on_symbol(sym);
}

/* The silence from the last key-up until `t`. */
static void gaps_until(uint32_t t)
{
    if (mt.d.down || !mt.d.any) return;
    const uint32_t silence = t - mt.d.up_ms;
    if (mt.d.n && silence >= gap_ms(0U)) sym_commit();
    if (!mt.d.n && !mt.d.spaced && silence >= gap_ms(1U)) {
        mt.d.spaced = 1U;
        if (mt.v.state == MTRAIN_KEYING) line_add(' ');
    }
}

static void element(uint8_t dah)
{
    if (mt.d.n < MORSE_ELEMENTS_MAX) mt.d.code = (uint8_t)((mt.d.code << 1) | dah);
    if (mt.d.n < 0xFFU) ++mt.d.n;
    mt.d.dahs = (uint8_t)(mt.d.dahs + dah);
}

/* A straight key's element by its length; the averages follow the hand. */
static uint8_t sk_dah(uint32_t ms)
{
    const uint32_t dit = mt.d.sk_dit, dah = mt.d.sk_dah;
    const uint8_t is_dah = (uint8_t)(ms >= dit + ((dah > dit ? dah - dit : dit) * 3U) / 8U);
    if (is_dah) {
        const uint32_t v = (dah * 7U + ms) / 8U;
        mt.d.sk_dah = (uint16_t)(v < 40U ? 40U : (v > 2000U ? 2000U : v));
    } else {
        const uint32_t v = (dit * 7U + ms) / 8U;
        mt.d.sk_dit = (uint16_t)(v < 15U ? 15U : (v > 1000U ? 1000U : v));
    }
    return is_dah;
}

static void key_event(uint8_t ev, uint32_t t)
{
    if (ev == MTRAIN_EV_UP) {
        if (!mt.d.down) return;
        mt.d.down = 0U;
        if (mt.d.straight) {
            if (t - mt.d.down_ms < MT_SK_NOISE) return; /* a bounce: the gap goes on */
            element(sk_dah(t - mt.d.down_ms));
        }
        mt.d.up_ms = t;
        return;
    }
    gaps_until(t);
    /* The gap this key-down ends: inside a letter it is learnt now; before
     * a letter of the answer it is kept for judge(). */
    if (mt.d.any && !mt.d.down) {
        const uint32_t gap = t - mt.d.up_ms;
        if (mt.d.n) learn(&mt.space16, gap, 0U);
        else if (mt.v.answer_len < MTRAIN_ANSWER_MAX)
            mt.gap8[mt.v.answer_len] = (uint8_t)(gap >= 2040U ? 255U : gap / 8U);
    }
    mt.d.down = 1U;
    mt.d.straight = (uint8_t)(ev == MTRAIN_EV_DOWN);
    mt.d.down_ms = t;
    mt.d.any = 1U;
    if (!mt.d.straight) element((uint8_t)(ev == MTRAIN_EV_DAH));
    if (mt.v.state == MTRAIN_THINK) enter(MTRAIN_ANSWER);
}

static uint8_t keying_state(void)
{
    const uint8_t s = mt.v.state;
    return (uint8_t)(s == MTRAIN_THINK || s == MTRAIN_ANSWER || s == MTRAIN_KEYING);
}

static void drain_one(void)
{
    const uint32_t e = mt_q[mt_rd];
    mt_rd = (uint8_t)((mt_rd + 1U) & (MT_Q - 1U));
    /* Back to a full ms count: the event is younger than 2^29 ms. The keyer's
     * interrupt may stamp one after this service took its time; that one is
     * taken as now. Read as 2^30 ms old, it ended the letter and the word:
     * M keyed as two dahs was T T at random (bench, 2026-10-07). */
    const uint32_t back = ((mt.now & MT_TIME_MASK) - (e & MT_TIME_MASK)) & MT_TIME_MASK;
    const uint32_t t = mt.now - (back > (MT_TIME_MASK >> 1) ? 0U : back);
    if (keying_state()) key_event((uint8_t)(e >> 30), t);
}

/* Events up to queue index `until` (all of them: MT_Q), in order. */
static void drain(uint32_t until)
{
    while (mt_rd != mt_wr && mt_rd != until) drain_one();
}

/* ---- Grading ------------------------------------------------------------------- */

/* The answer against the prompt by edit distance: which prompt symbols were
 * missed, and the first difference as expected/heard. */
static void grade(void)
{
    uint8_t dist[MTRAIN_PROMPT_MAX + 1U][MTRAIN_ANSWER_MAX + 1U];
    const uint8_t *p = mt.v.prompt, *a = mt.v.answer;
    const uint32_t n = mt.v.prompt_len, m = mt.v.answer_len;
    for (uint32_t i = 0U; i <= n; ++i)
        for (uint32_t j = 0U; j <= m; ++j) {
            uint32_t best = i + j;
            if (i && j) {
                const uint32_t sub = dist[i - 1U][j - 1U] + (uint32_t)(p[i - 1U] != a[j - 1U]);
                const uint32_t del = dist[i - 1U][j] + 1U, ins = dist[i][j - 1U] + 1U;
                best = sub < del ? sub : del;
                if (ins < best) best = ins;
            }
            dist[i][j] = (uint8_t)best;
        }
    mt.v.wrong = 0U;
    mt.v.expected = mt.v.heard = 0U;
    for (uint32_t i = n, j = m; i || j;) {
        if (i && j && dist[i][j] == dist[i - 1U][j - 1U] + (uint32_t)(p[i - 1U] != a[j - 1U])) {
            --i, --j;
            if (p[i] == a[j]) continue;
            mt.v.wrong |= (uint16_t)(1U << i);
            mt.v.expected = p[i];
            mt.v.heard = a[j];
        } else if (i && dist[i][j] == dist[i - 1U][j] + 1U) {
            --i;
            mt.v.wrong |= (uint16_t)(1U << i);
            mt.v.expected = p[i];
            mt.v.heard = 0U;
        } else {
            --j;
            mt.v.expected = 0U;
            mt.v.heard = a[j];
        }
    }
}

static uint8_t lesson_place(uint8_t sym)
{
    for (uint8_t i = 0U; i < mt.v.lesson; ++i)
        if (mt.course[i] == sym) return i;
    return 0xFFU;
}

/* Morserino-32's adaptive weights: a right answer lowers each of its
 * characters by one (not below 1), a wrong one raises the first missed by
 * four and its different neighbours by two (not above the lesson's size). */
static void weigh(uint8_t right)
{
    const uint8_t n = mt.v.lesson;
    for (uint8_t i = 0U; i < mt.v.prompt_len; ++i) {
        const uint8_t at = lesson_place(mt.v.prompt[i]);
        if (at == 0xFFU) continue;
        if (right) {
            if (mt.weight[at] > 1U) --mt.weight[at];
            continue;
        }
        uint8_t add = 0U;
        const uint8_t first = (uint8_t)__builtin_ctz(mt.v.wrong | 0x8000U);
        if (i == first) add = 4U;
        else if ((i + 1U == first || i == first + 1U) && mt.v.prompt[i] != mt.v.prompt[first]) add = 2U;
        mt.weight[at] = (uint8_t)(mt.weight[at] + add > n ? n : mt.weight[at] + add);
    }
}

static void push(uint64_t *win, uint8_t *count, uint8_t ok, uint8_t cap)
{
    *win = (*win << 1) | ok;
    if (*count < cap) ++*count;
}

/* Of a window's last `count` outcomes, the right ones. */
static uint8_t rights(uint64_t win, uint8_t count)
{
    return popcount64(win & ((count >= 64U) ? ~0ULL : ((1ULL << count) - 1U)));
}

static uint8_t share_ok(uint64_t win, uint8_t count)
{
    return (uint8_t)(count && rights(win, count) * 10U >= count * 9U);
}

/* Koch's mark: 90 % of the lesson's last 50 first tries right, and with
 * `newest` 90 % of the newest character's last ten or more. */
static uint8_t passing(uint8_t newest)
{
    return (uint8_t)(prog.win_n >= MT_WIN && share_ok(prog.win, prog.win_n) &&
                     (!newest || (prog.newest_n >= 10U && share_ok(prog.newest_win, prog.newest_n))));
}

/* The weakest of the lesson's characters by first-try share, once tried
 * five times. */
static void weakest(void)
{
    uint32_t best = 101U;
    mt.v.weakest = 0U;
    for (uint8_t i = 0U; i < mt.v.lesson; ++i) {
        const uint8_t x = MORSE_Index(mt.course[i]);
        if (prog.tries[x] < 5U) continue;
        const uint32_t pct = 100U * prog.right[x] / prog.tries[x];
        if (pct < best) {
            best = pct;
            mt.v.weakest = mt.course[i];
        }
    }
}

/* A first attempt's characters into the record and the lesson's score. */
static void count_first(void)
{
    for (uint8_t i = 0U; i < mt.v.prompt_len; ++i) {
        const uint8_t sym = mt.v.prompt[i], x = MORSE_Index(sym);
        const uint8_t ok = (uint8_t)!((mt.v.wrong >> i) & 1U);
        if (x >= MORSE_SYMBOLS) continue;
        if (prog.tries[x] == 0xFFU) {
            prog.tries[x] = 0x80U;
            prog.right[x] = (uint8_t)(prog.right[x] / 2U);
        }
        ++prog.tries[x];
        prog.right[x] = (uint8_t)(prog.right[x] + ok);
        /* The lesson is judged by ear: the course's sending drill, read
         * off the screen, is not in its score. */
        if (!mt.sight) {
            push(&prog.win, &prog.win_n, ok, MT_WIN);
            if (sym == mt.v.newest) {
                uint64_t w = prog.newest_win;
                push(&w, &prog.newest_n, ok, MT_NEWEST_WIN);
                prog.newest_win = (uint32_t)w;
            }
        }
        ++mt.v.done;
        mt.v.right = (uint16_t)(mt.v.right + ok);
    }
    /* Ready for the next character: 50 first tries in this lesson, ten of
     * them the newest character, each at 90 % or better. */
    /* The course does not ask: it moves on by itself (MASTER). */
    mt.v.recommend = (uint8_t)(!mt.v.guided && mt.act != MTRAIN_LEARN && mt.v.lesson < mt.v.lesson_max && passing(1U));
    mt.v.window = rights(prog.win, prog.win_n);
    weakest();
    mt.dirty = 1U;
}

/* ---- The flow ------------------------------------------------------------------ */

static void answer_clear(void)
{
    mt.v.answer_len = 0U;
    mt.v.answer[0] = 0U;
    dec_reset();
}

/* Give the levers back for an answer once both are open: a lever still held
 * from before the prompt ended is not an answer. */
static void open_levers(void)
{
    mt.open = 1U;
    mt.t_state = mt.now;
    dec_reset();
    gate(0U);
}

static void to_think(void)
{
    answer_clear();
    enter(MTRAIN_THINK);
    mt.open = 0U;
    if (!mt.v.levers) open_levers();
}

static void start_prompt(void)
{
    answer_clear();
    mt.v.verdict = MTRAIN_V_NONE;
    const uint8_t show = mt.v.guided ? mt.show : prefs.show;
    mt.sight = (uint8_t)(mt.v.guided && show == MTRAIN_SHOW_TEXT);
    mt.v.reveal = (uint8_t)(mt.act == MTRAIN_LEARN || (mt.act == MTRAIN_ECHO && show != MTRAIN_SHOW_SOUND));
    gate(1U);
    if (mt.act == MTRAIN_ECHO && show == MTRAIN_SHOW_TEXT) {
        to_think();
        return;
    }
    mt.replay = 1U;
    enter(MTRAIN_PLAY);
}

static void step_done(void)
{
    gate(1U);
    enter(MTRAIN_STEP_DONE);
}

/* The verdict on an answer (or on none). */
static void verdict(uint8_t v)
{
    gate(1U);
    mt.v.verdict = v;
    mt.v.reveal = 1U;
    if (v == MTRAIN_V_RIGHT || v == MTRAIN_V_WRONG || v == MTRAIN_V_TIMEOUT) {
        if (v == MTRAIN_V_TIMEOUT) {
            mt.v.wrong = (uint16_t)((1U << mt.v.prompt_len) - 1U);
            mt.v.expected = mt.v.prompt[0];
            mt.v.heard = 0U;
            ++mt.v.timeouts;
        }
        /* The course counts a first try that ran out of time as missed:
         * waiting is no way past a character. */
        if (mt.v.attempt == 0U && (v != MTRAIN_V_TIMEOUT || mt.v.guided)) count_first();
        weigh((uint8_t)(v == MTRAIN_V_RIGHT));
        if (prefs.adapt && mt.act == MTRAIN_ECHO && !mt.v.guided) {
            const uint8_t lo = clamp8(prefs.listen_wpm > 15U ? prefs.listen_wpm - 10U : 5U, 5U, 60U);
            mt.wpm = clamp8(v == MTRAIN_V_RIGHT ? mt.wpm + 1U : mt.wpm - 1U, lo,
                            clamp8(prefs.listen_wpm + 10U, 5U, 60U));
            mt.v.listen_wpm = mt.wpm;
        }
        beep((uint8_t)(v == MTRAIN_V_RIGHT));
    }
    if (v == MTRAIN_V_LOST) ++mt.v.lost;
    enter(MTRAIN_FEEDBACK);
}

/* A right answer teaches the spacing: a gap a join closed was inside a
 * letter, any other between letters (word gaps say nothing about either). */
static void learn_answer(uint16_t joined)
{
    for (uint8_t j = 1U; j < mt.v.answer_len; ++j) {
        const uint32_t gap = (uint32_t)mt.gap8[j] * 8U;
        if (joined & (1U << j)) learn(&mt.space16, gap, 1U);
        else if (gap < gap_ms(1U)) learn(&mt.char16, gap, 0U);
    }
}

/* The answer read again with letters joined across gaps shorter than a
 * word's: right if that is the prompt, and the answer becomes it. Each
 * prompt letter takes the answer's letters until it has as many elements,
 * so there is only one way to join, and a join never changes an element. */
static uint8_t join_answer(void)
{
    uint16_t joined = 0U;
    uint8_t j = 0U;
    for (uint8_t i = 0U; i < mt.v.prompt_len; ++i) {
        const uint8_t want = MORSE_Code(mt.v.prompt[i]), n = MORSE_Length(want);
        uint32_t code = 1U;
        uint8_t len = 0U;
        for (;;) {
            if (j >= mt.v.answer_len) return 0U;
            const uint8_t c = MORSE_Code(mt.v.answer[j]), k = MORSE_Length(c);
            if (k == 0U) return 0U;
            code = (code << k) | (c & ((1U << k) - 1U));
            len = (uint8_t)(len + k);
            ++j;
            if (len >= n) break;
            if (j >= mt.v.answer_len || (uint32_t)mt.gap8[j] * 8U >= gap_ms(1U)) return 0U;
            joined |= (uint16_t)(1U << j);
        }
        if (code != want) return 0U;
    }
    if (j != mt.v.answer_len || joined == 0U) return 0U;
    learn_answer(joined);
    memcpy(mt.v.answer, mt.v.prompt, sizeof(mt.v.prompt));
    mt.v.answer_len = mt.v.prompt_len;
    mt.v.joined = 1U;
    mt.v.wrong = 0U;
    mt.v.expected = mt.v.heard = 0U;
    return 1U;
}

static void judge(void)
{
    mt.v.joined = 0U;
    if (mt.v.answer_len == 0U) {
        verdict(MTRAIN_V_TIMEOUT);
        return;
    }
    grade();
    uint8_t right = (uint8_t)(mt.v.wrong == 0U && mt.v.answer_len == mt.v.prompt_len);
    if (right) learn_answer(0U);
    else right = join_answer();
    verdict(right ? MTRAIN_V_RIGHT : MTRAIN_V_WRONG);
}

/* After the verdict has been shown: the same prompt again, or the next. */
static void after_feedback(void)
{
    const uint8_t v = mt.v.verdict;
    if (v == MTRAIN_V_LOST) {
        start_prompt(); /* not the learner's fault: the same try again */
        return;
    }
    if (mt.act == MTRAIN_LEARN) {
        mt.run = (v == MTRAIN_V_RIGHT) ? (uint8_t)(mt.run + 1U) : 0U;
        if (mt.run >= MT_LEARN_RUN) {
            step_done();
            return;
        }
        ++mt.v.attempt;
        start_prompt();
        return;
    }
    const uint8_t again = (v == MTRAIN_V_WRONG || v == MTRAIN_V_TIMEOUT) &&
                          (prefs.retries >= MTRAIN_RETRY_ALWAYS || mt.v.attempt < prefs.retries);
    if (again) {
        ++mt.v.attempt;
        start_prompt();
        return;
    }
    if (mt.v.guided && ((mt.trials && --mt.trials == 0U) ||
                        (mt.op == CWPACK_STEP_MASTER && passing((uint8_t)(mt.src == MTRAIN_CHARS))))) {
        step_done();
        return;
    }
    enter(MTRAIN_LOAD);
}

/* ---- The course ------------------------------------------------------------- */

static void begin_activity(uint8_t act)
{
    mt.act = act;
    mt.v.act = act;
    mt.v.attempt = 0U;
    mt.v.verdict = MTRAIN_V_NONE;
    mt.v.reveal = 0U;
    mt.run = 0U;
    mt.v.prompt_len = 0U;
    mt.v.prompt[0] = 0U;
    answer_clear();
    if (act == MTRAIN_LEARN) {
        mt.intro = 1U;
        if (!mt.v.guided) mt.learn = mt.v.newest;
    }
    if (act == MTRAIN_PADDLE) {
        mt.v.check = MTRAIN_CHECK_DIT;
        gate(0U);
        enter(MTRAIN_CHECK);
    } else if (act == MTRAIN_FREE) {
        mt.v.line_len = 0U;
        mt.v.line[0] = 0U;
        mt.explore = 0U;
        gate(0U);
        enter(MTRAIN_KEYING);
    } else {
        enter(MTRAIN_LOAD);
    }
}

/* Step `i` next: read from the card in LOAD. */
static void goto_step(uint8_t i)
{
    mt.step = i;
    mt.fetch = 1U;
    gate(1U);
    enter(MTRAIN_LOAD);
}

/* INTRO: the character at intro_at, heard, seen and keyed. */
static void teach(void)
{
    mt.learn = mt.course[mt.intro_at];
    begin_activity(MTRAIN_LEARN);
}

/* A step as the pack has it: op, a, b (source | group - 1 << 3 | prompt
 * << 6, or SPEED's WPM), text. What the record has done already skips
 * itself, so a session can always start at the top. */
static __attribute__((noinline)) void run_step(const uint8_t *s)
{
    const uint8_t op = s[0], a = s[1], b = s[2];
    /* The whole alphabet is in and its lesson passed. */
    const uint8_t all_in = (uint8_t)(mt.v.lesson >= mt.v.lesson_max && prog.stage);
    mt.op = op;
    mt.a = a;
    mt.src = (uint8_t)(b & 7U);
    mt.len = (uint8_t)(((b >> 3) & 7U) + 1U);
    mt.show = (uint8_t)(b >> 6);
    mt.trials = (op == CWPACK_STEP_LISTEN || op == CWPACK_STEP_ECHO) ? a : 0U;
    mt.v.text = s[3];
    mt.v.guided = (uint8_t)(mt.step + 1U);
    switch (op) {
        case CWPACK_STEP_PADDLE:
        case CWPACK_STEP_EXPLORE:
            if (prog.once & op) break; /* MTRAIN_ONCE_PADDLE, _EXPLORE: done for good */
            begin_activity(op == CWPACK_STEP_PADDLE ? MTRAIN_PADDLE : MTRAIN_FREE);
            return;
        case CWPACK_STEP_INTRO:
            if (all_in) break;
            mt.intro_at = stop_before();
            teach();
            return;
        case CWPACK_STEP_LISTEN:
            begin_activity(MTRAIN_LISTEN);
            return;
        case CWPACK_STEP_MASTER:
            if (all_in && mt.step < prog.stage) break; /* passed */
            /* fall through */
        case CWPACK_STEP_ECHO:
            begin_activity(MTRAIN_ECHO);
            return;
        case CWPACK_STEP_SPEED:
            /* Faster overall, the characters as they were: the Farnsworth
             * gaps shrink, and the stages from `a` come round again. */
            if (prefs.pace_wpm >= prefs.listen_wpm) break;
            prefs.pace_wpm = clamp8((uint32_t)prefs.pace_wpm + b, 2U, prefs.listen_wpm);
            MTRAIN_PrefsChanged();
            prog.stage = a;
            mt.dirty = 1U;
            goto_step(a);
            return;
        default: /* GOTO */
            goto_step(a);
            return;
    }
    goto_step((uint8_t)(mt.step + 1U));
}

/* The step is over: the next character of INTRO, the next lesson after a
 * MASTER passed below the last one, else the next step. */
static void step_end(void)
{
    const uint8_t op = mt.op;
    if (op <= CWPACK_STEP_EXPLORE) prog.once |= op;
    mt.dirty = 1U;
    if (op == CWPACK_STEP_INTRO && ++mt.intro_at < mt.v.lesson) {
        teach();
        return;
    }
    if (op == CWPACK_STEP_MASTER) {
        if (mt.a != 0xFFU && mt.v.lesson < mt.v.lesson_max) {
            lesson_set(stop_from((uint8_t)(mt.v.lesson + 1U))); /* a new lesson: a new score */
            goto_step(mt.a);
            return;
        }
        window_clear();
        prog.stage = (uint8_t)(mt.step + 1U);
    }
    goto_step((uint8_t)(mt.step + 1U));
}

/* ---- States ------------------------------------------------------------------- */

static void error(uint8_t why)
{
    mt.v.error = why;
    gate(1U);
    CWP_Stop();
    enter(MTRAIN_ERROR);
}

static __attribute__((noinline)) void state_load(void)
{
    if (!CWPACK_Mounted()) {
        error(MTRAIN_ERR_CARD);
        return;
    }
    if (!CWPACK_Ready()) return; /* the radio's realtime work first: later */
    if (!CWPACK_Open()) {
        if (CWPACK_Ready() || !CWPACK_Mounted()) error(CWPACK_Mounted() ? MTRAIN_ERR_PACK : MTRAIN_ERR_CARD);
        return;
    }
    if (!course_load()) {
        if (CWPACK_Ready()) error(prefs.course == MTRAIN_COURSE_CUSTOM ? MTRAIN_ERR_CUSTOM : MTRAIN_ERR_PACK);
        return;
    }
    if (mt.fetch) {
        uint8_t step[4];
        /* The pack's course never runs out (gen_cw_pack.py): a step that
         * cannot be read is the card's or the pack's. */
        if (!CWPACK_Step(mt.step, step)) {
            if (CWPACK_Ready()) error(MTRAIN_ERR_PACK);
            return;
        }
        mt.fetch = 0U;
        run_step(step);
        return;
    }
    if (mt.begin) {
        mt.begin = 0U;
        begin_activity(mt.act);
        return;
    }
    if (!mt.v.guided && prefs.minutes && mt.session_ms >= (uint32_t)prefs.minutes * 60000U) {
        gate(1U);
        enter(MTRAIN_SUMMARY);
        return;
    }
    const int8_t r = make_prompt();
    if (r < 0) {
        if (CWPACK_Ready()) error(MTRAIN_ERR_PACK);
        return;
    }
    mt.v.attempt = 0U;
    start_prompt();
}

/* The prompt has sounded: the levers back once both are open. */
static void state_play(void)
{
    if (CWP_Busy()) return;
    if (mt.replay) {
        mt.replay = 0U;
        play_prompt();
        return;
    }
    if (mt.act == MTRAIN_LISTEN) {
        mt.v.reveal = 1U;
        enter(MTRAIN_SHOW);
        return;
    }
    to_think();
}

static void state_think(void)
{
    if (!mt.open) {
        if (!mt.v.levers) open_levers();
        return;
    }
    if (!mt.hold && since() >= (uint32_t)(prefs.think_s ? prefs.think_s : 5U) * 1000U) verdict(MTRAIN_V_TIMEOUT);
}

/* Silence after the last key-up ends the answer: soon (about Morserino's
 * eight dits) once it is as long as the prompt, after the answer wait while
 * it is shorter. */
static void state_answer(void)
{
    if (mt.hold || mt.d.down || mt.d.n) return;
    const uint32_t silence = mt.now - mt.d.up_ms, done = 8U * dit_ms();
    uint32_t wait = (uint32_t)prefs.wait_ds * 100U;
    if (wait < done) wait = done;
    if ((mt.v.answer_len >= mt.v.prompt_len && silence >= done) || silence >= wait) judge();
}

static void state_feedback(void)
{
    const uint32_t show = (mt.v.verdict == MTRAIN_V_RIGHT) ? 700U : 1600U;
    if (CWP_Busy() || since() < show || mt.v.levers) return;
    after_feedback();
}

static void state_check(void)
{
    const uint8_t lev = mt.v.levers, rise = (uint8_t)(lev & (uint8_t)~mt.lev_prev);
    switch (mt.v.check) {
        case MTRAIN_CHECK_DIT:
        case MTRAIN_CHECK_SWAP:
            if (rise & 1U) mt.v.check = MTRAIN_CHECK_SQUEEZE;
            else if (rise & 2U) mt.v.check = MTRAIN_CHECK_SWAP;
            break;
        case MTRAIN_CHECK_SQUEEZE:
            if (lev == 3U) {
                mt.v.check = MTRAIN_CHECK_RELEASE;
                enter(MTRAIN_CHECK);
            }
            break;
        case MTRAIN_CHECK_RELEASE:
            if (lev) mt.t_state = mt.now;
            else if (since() >= 40U) {
                mt.v.check = MTRAIN_CHECK_OK;
                if (mt.v.guided) step_done();
            }
            break;
        default:
            break;
    }
}

static void state_keying(void)
{
    if (!mt.d.down && mt.d.any) gaps_until(mt.now);
    /* The course's first try of the keyer: held dits, held dahs and a
     * squeeze, each ended by a gap, and it goes on by itself. */
    if (mt.v.guided && mt.explore == 7U) step_done();
}

/* ---- Interface ---------------------------------------------------------------- */

void MTRAIN_DefaultPrefs(mtrain_prefs_t *p)
{
    memset(p, 0, sizeof(*p));
    p->act = MTRAIN_ECHO;
    p->src = MTRAIN_CHARS;
    p->course = MTRAIN_COURSE_M32;
    p->group = 1U;
    p->listen_wpm = 20U;
    p->pace_wpm = 10U;
    p->key_wpm = 15U;
    p->ics = 3U;
    p->iws = 7U;
    p->retries = 3U;     /* Morserino-32's echo repeats */
    p->wait_ds = 25U;
    p->think_s = 5U;
    p->show = MTRAIN_SHOW_SOUND;
    p->tone = 1U;
    p->minutes = 5U;
}

/* Each preference byte's range, in mtrain_prefs_t's order. */
_Static_assert(sizeof(mtrain_prefs_t) == 20U, "a range for every preference");
static const uint8_t pref_lo[20] = {0U, 0U, 0U, 0U, 1U, 5U, 2U, 5U, 0U, 3U, 7U, 0U, 10U, 2U, 0U, 0U, 0U, 0U, 0U, 0U};
static const uint8_t pref_hi[20] = {
    MTRAIN_ACT_COUNT - 1U, MTRAIN_SRC_COUNT - 1U, MTRAIN_COURSES - 1U, MTRAIN_CAROUSEL_MAX, MTRAIN_GROUP_MAX,
    60U, 60U, 60U, 1U, 15U, 30U, MTRAIN_RETRY_ALWAYS, 100U, 30U, MTRAIN_SHOW_COUNT - 1U, 1U, 1U, 1U, 60U, 1U};

static void prefs_clamp(void)
{
    uint8_t *b = (uint8_t *)&prefs;
    for (uint32_t i = 0U; i < sizeof(pref_lo); ++i) b[i] = clamp8(b[i], pref_lo[i], pref_hi[i]);
    if (prefs.pace_wpm > prefs.listen_wpm) prefs.pace_wpm = prefs.listen_wpm;
}

void MTRAIN_ResetProgress(void)
{
    memset(&prog, 0, sizeof(prog));
    prog.version = MTRAIN_PROGRESS_VERSION;
    mt.course_key = 0xFFU;
    mt.dirty = 1U;
}

void MTRAIN_Init(void)
{
    mt_capture = 0U;
    memset(&mt, 0, sizeof(mt));
    mt.course_key = 0xFFU;
    MTRAIN_DefaultPrefs(&prefs);
    MTRAIN_ResetProgress();
    mt.dirty = 0U;
}

uint8_t MTRAIN_Active(void) { return (uint8_t)(mt.v.state != MTRAIN_OFF); }

uint8_t MTRAIN_Start(uint8_t guided, uint32_t now)
{
    prefs_clamp();
    const mtrain_view_t keep = mt.v;
    memset(&mt.v, 0, sizeof(mt.v));
    mt.v.lesson = keep.lesson;
    mt.v.lesson_max = keep.lesson_max;
    mt.v.newest = keep.newest;
    mt.now = mt.t_last = now;
    mt.rng ^= now * 2654435761UL;
    mt.session_ms = 0U;
    mt.hold = mt.submit = 0U;
    mt.course_key = 0xFFU;
    mt.wpm = prefs.listen_wpm;
    mt.space16 = 16U;  /* a keyer's dit inside a letter, three between: */
    mt.char16 = 48U;   /* the standard 2.5-dit split to start with */
    mt.v.listen_wpm = prefs.listen_wpm;
    mt.v.pace_wpm = prefs.pace_wpm;
    mt.v.guided = guided ? 1U : 0U;  /* the steps come from the pack (LOAD) */
    mt.act = guided ? (uint8_t)MTRAIN_ECHO : prefs.act;
    mt.v.act = mt.act;
    mt.begin = (uint8_t)!guided;
    mt.fetch = (uint8_t)(guided != 0U);
    mt.step = 0U;
    mt.replay = 0U;
    mt_rd = mt_wr;
    mt_lost = 0U;
    mt_capture = 1U;
    MOD_CwSetTrainInhibit(1U);
    MOD_CwSetTrainingWpm((float)prefs.key_wpm);
    gate(1U);
    dec_reset();
    enter(MTRAIN_LOAD);
    return 1U;
}

void MTRAIN_Stop(void)
{
    if (!MTRAIN_Active()) return;
    mt_capture = 0U;
    CWP_Stop();
    gate(0U);
    MOD_CwSetTrainingWpm(0.0f);
    MOD_CwSetTrainInhibit(0U); /* the keyer gives RF back only once both levers are open */
    mt.v.state = MTRAIN_OFF;
}

/* The keyer's events since the last service, in order; 0 while a submit
 * waits for the key-up of an element still sounding. */
static __attribute__((noinline)) uint8_t service_input(uint32_t now)
{
    const uint8_t s = mt.v.state;
    if (mt_lost) {
        mt_lost = 0U;
        mt_rd = mt_wr;
        dec_reset();
        if (s == MTRAIN_THINK || s == MTRAIN_ANSWER) verdict(MTRAIN_V_LOST);
    }
    if (mt.submit) {
        /* What was keyed before the click, and the key-up of an element
         * sounding then; not what came after. */
        drain(mt.submit_wr);
        while (mt.d.down && mt_rd != mt_wr && (mt_q[mt_rd] >> 30) == MTRAIN_EV_UP) drain_one();
        if (mt.d.down && now - mt.d.down_ms < 12U * dit_ms()) return 0U; /* the element ends first */
        mt.submit = 0U;
        mt.d.down = 0U;
        sym_commit();
        if (mt.v.state == MTRAIN_THINK || mt.v.state == MTRAIN_ANSWER) judge();
    }
    drain(MT_Q);
    /* A key-up the keyer never sent (a reset mid-element): no element lasts twelve dits. */
    if (mt.d.down && now - mt.d.down_ms > 12U * dit_ms()) key_event(MTRAIN_EV_UP, now);
    if (mt.v.state == MTRAIN_THINK || mt.v.state == MTRAIN_ANSWER) gaps_until(now);
    return 1U;
}

static __attribute__((noinline)) void service_state(void)
{
    switch (mt.v.state) {
        case MTRAIN_ERROR:
            if (since() >= MT_RETRY_MS && CWPACK_Ready() && CWPACK_Open()) {
                mt.v.error = MTRAIN_ERR_NONE;
                enter(MTRAIN_LOAD);
            }
            break;
        case MTRAIN_LOAD:     state_load(); break;
        case MTRAIN_PLAY:     state_play(); break;
        case MTRAIN_THINK:    state_think(); break;
        case MTRAIN_ANSWER:   state_answer(); break;
        case MTRAIN_FEEDBACK: state_feedback(); break;
        case MTRAIN_SHOW:
            /* Listen: the group stays up a moment, never less than a second. */
            if (since() >= 1000U + mt.played_ms / 4U) {
                if (mt.v.guided && --mt.trials == 0U) step_done();
                else enter(MTRAIN_LOAD);
            }
            break;
        case MTRAIN_STEP_DONE:
            /* Well done, a moment, and on: no knob to press. */
            if (!mt.hold && since() >= 1200U) MTRAIN_Next();
            break;
        case MTRAIN_KEYING:   state_keying(); break;
        case MTRAIN_CHECK:    state_check(); break;
        default:              break;
    }
}

void MTRAIN_Service(uint32_t now)
{
    if (!MTRAIN_Active()) return;
    mt.now = now;
    const uint8_t s = mt.v.state;
    if (!mt.hold && s != MTRAIN_PAUSE && s != MTRAIN_SUMMARY && s != MTRAIN_STEP_DONE && s != MTRAIN_ERROR)
        mt.session_ms += now - mt.t_last;
    mt.t_last = now;
    mt.v.levers = MOD_CwLevers();
    if (!service_input(now)) return;
    service_state();
    /* Listen: a lever pauses and resumes it, as on the Morserino; not in the
     * course, where a newcomer keying along would be left PAUSED. */
    if (mt.act == MTRAIN_LISTEN && !mt.v.guided && (mt.v.levers & (uint8_t)~mt.lev_prev)) {
        if (mt.v.state == MTRAIN_PLAY || mt.v.state == MTRAIN_SHOW) MTRAIN_Pause(1U);
        else if (mt.v.state == MTRAIN_PAUSE) MTRAIN_Pause(0U);
    }
    mt.lev_prev = mt.v.levers;
    mt.v.left_ms = (prefs.minutes && !mt.v.guided && mt.session_ms < (uint32_t)prefs.minutes * 60000U)
                 ? (uint32_t)prefs.minutes * 60000U - mt.session_ms : 0U;
}

void MTRAIN_Submit(void)
{
    if (mt.v.state != MTRAIN_THINK && mt.v.state != MTRAIN_ANSWER) return;
    mt.submit_wr = mt_wr;
    mt.submit = 1U;
}

void MTRAIN_Repeat(void)
{
    const uint8_t s = mt.v.state;
    if (mt.v.prompt_len == 0U || (s != MTRAIN_THINK && s != MTRAIN_ANSWER && s != MTRAIN_FEEDBACK &&
                                  s != MTRAIN_SHOW && s != MTRAIN_PLAY)) return;
    CWP_Stop();
    mt.v.verdict = MTRAIN_V_NONE;
    mt.intro = 0U;
    answer_clear();
    gate(1U);
    mt.replay = 1U; /* once the stop has ramped down */
    enter(MTRAIN_PLAY);
}

void MTRAIN_Skip(void)
{
    const uint8_t s = mt.v.state;
    if (s != MTRAIN_PLAY && s != MTRAIN_THINK && s != MTRAIN_ANSWER && s != MTRAIN_FEEDBACK && s != MTRAIN_SHOW)
        return;
    CWP_Stop();
    gate(1U);
    mt.v.verdict = MTRAIN_V_SKIPPED;
    if (mt.act == MTRAIN_LEARN) step_done();
    else enter(MTRAIN_LOAD);
}

void MTRAIN_Pause(uint8_t on)
{
    const uint8_t s = mt.v.state;
    if (on) {
        if (s == MTRAIN_PAUSE || s == MTRAIN_OFF || s == MTRAIN_ERROR || s == MTRAIN_SUMMARY) return;
        mt.resume = s;
        CWP_Stop();
        gate(1U);
        enter(MTRAIN_PAUSE);
        return;
    }
    if (s != MTRAIN_PAUSE) return;
    switch (mt.resume) {
        case MTRAIN_PLAY: case MTRAIN_THINK: case MTRAIN_ANSWER: case MTRAIN_FEEDBACK:
            if (mt.act != MTRAIN_LISTEN && mt.v.prompt_len) {
                start_prompt();
                break;
            }
            enter(MTRAIN_LOAD);
            break;
        case MTRAIN_KEYING: case MTRAIN_CHECK:
            gate(0U);
            dec_reset();
            enter(mt.resume);
            break;
        default:
            enter(mt.resume == MTRAIN_STEP_DONE ? MTRAIN_STEP_DONE : MTRAIN_LOAD);
            break;
    }
}

void MTRAIN_Hold(uint8_t on)
{
    if (mt.hold && !on) {
        /* The deadlines start over from now. */
        if (mt.v.state == MTRAIN_THINK) mt.t_state = mt.now;
        if (mt.v.state == MTRAIN_ANSWER && !mt.d.down) mt.d.up_ms = mt.now;
    }
    mt.hold = (uint8_t)(on != 0U);
}

void MTRAIN_Next(void)
{
    const uint8_t s = mt.v.state;
    if (s == MTRAIN_SUMMARY) {
        mt.session_ms = 0U;
        mt.v.done = mt.v.right = mt.v.timeouts = mt.v.lost = 0U;
        begin_activity(prefs.act);
        return;
    }
    if (mt.v.guided && (s == MTRAIN_STEP_DONE || s == MTRAIN_KEYING || s == MTRAIN_CHECK)) {
        CWP_Stop();
        step_end();
        return;
    }
    if (s == MTRAIN_STEP_DONE || (s == MTRAIN_CHECK && mt.v.check == MTRAIN_CHECK_OK)) {
        /* LEARN done, or the paddle check: on to echo practice. */
        begin_activity(MTRAIN_ECHO);
    }
}

void MTRAIN_Swap(void)
{
    if (mt.v.state != MTRAIN_CHECK) return;
    MOD_SetCwSwap((uint8_t)!MOD_GetCwSwap());
    mt.v.check = MTRAIN_CHECK_DIT;
}

uint8_t MTRAIN_AddCharacter(void)
{
    if (mt.course_key == 0xFFU || mt.v.lesson >= mt.v.lesson_max) return 0U;
    lesson_set((uint8_t)(mt.v.lesson + 1U));
    return 1U;
}

void MTRAIN_SetLesson(uint8_t lesson)
{
    if (mt.course_key == 0xFFU) {
        *lesson_slot() = lesson;
        mt.dirty = 1U;
        return;
    }
    lesson_set(lesson);
}

const mtrain_view_t *MTRAIN_View(void) { return &mt.v; }
mtrain_prefs_t *MTRAIN_Prefs(void) { return &prefs; }
mtrain_progress_t *MTRAIN_Progress(void) { return &prog; }
uint8_t MTRAIN_ProgressDirty(void) { return mt.dirty; }

/* Version 1 (v8.10-v8.11) has the same layout: `once` was how many of the
 * guided session's steps were done, `stage` padding written 0. */
uint8_t MTRAIN_ProgressValid(const mtrain_progress_t *p)
{
    const uint8_t v1 = (uint8_t)(p->version == 1U);
    if ((!v1 && (p->version != MTRAIN_PROGRESS_VERSION || p->once > 3U)) || p->win_n > MT_WIN ||
        p->newest_n > MT_NEWEST_WIN) return 0U;
    for (uint32_t i = 0U; i < MTRAIN_COURSES; ++i)
        if (p->lesson[i] > MORSE_SYMBOLS) return 0U;
    for (uint32_t i = 0U; i < MORSE_SYMBOLS; ++i)
        if (p->right[i] > p->tries[i]) return 0U;
    return 1U;
}

void MTRAIN_ProgressSet(const mtrain_progress_t *p)
{
    prog = *p;
    if (prog.version == 1U) {
        /* The guided session began with the paddle check, then the keyer:
         * past those, the course skips them. */
        if (prog.once >= 2U) prog.once = MTRAIN_ONCE_PADDLE | MTRAIN_ONCE_EXPLORE;
        prog.version = MTRAIN_PROGRESS_VERSION;
    }
}
void MTRAIN_ProgressSaved(void) { mt.dirty = 0U; }

void MTRAIN_PrefsChanged(void)
{
    prefs_clamp();
    const uint8_t key = (uint8_t)(prefs.course * 16U + prefs.carousel);
    mt.v.listen_wpm = mt.wpm = prefs.listen_wpm;
    mt.v.pace_wpm = prefs.pace_wpm;
    if (key != mt.course_key) mt.course_key = 0xFFU;
    mt.n_words = mt.n_abbr = -1;
    if (MTRAIN_Active()) MOD_CwSetTrainingWpm((float)prefs.key_wpm);
}

uint8_t MTRAIN_LoadCourse(void)
{
    if (mt.course_key != 0xFFU) return 1U;
    if (MTRAIN_Active() || !CWPACK_Ready() || !CWPACK_Open()) return 0U;
    return course_load();
}

const uint8_t *MTRAIN_Course(void) { return (mt.course_key != 0xFFU) ? mt.course : NULL; }

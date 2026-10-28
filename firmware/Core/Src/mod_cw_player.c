/* The local Morse player (mod_cw_player.h). The main loop turns text into
 * codes and timing (CWP_SetText, CWP_Timing); the headphone renderer walks
 * the codes a sample at a time (CWP_Step: no table lookups, no division but
 * the ramp's). All of it built for size: the player runs only while a
 * prompt or a beep sounds, when the keyer it shares the renderer with is
 * held off. */
#pragma GCC optimize ("Os")
#include "mod_cw_player.h"
#include "mod_morse_tables.h"


enum { PH_LEAD = 0U, PH_MARK, PH_GAP, PH_END };

typedef struct {
    uint32_t left;       /* samples left in the phase */
    uint32_t rpos;       /* position in the current ramp */
    float    env;
    uint8_t  phase;
    uint8_t  pos;        /* code index sounding */
    uint8_t  el;         /* elements of it still to come, after this one */
    uint8_t  target;     /* the envelope's target: 1 during a mark */
} cwp_state_t;

#if defined(__GNUC__) && !defined(CWP_HOST)
#define CWP_CCM __attribute__((section(".ccmram_bss")))
#else
#define CWP_CCM
#endif

/* The program as played: written by CWP_Start only while nothing plays. */
static cwp_prog_t cwp_prog CWP_CCM;
static cwp_state_t cwp CWP_CCM;
static const uint16_t *cwp_lut;
static uint16_t cwp_lut_n;
static volatile uint8_t cwp_go, cwp_stop, cwp_running, cwp_done;

/* ---- Main loop ----------------------------------------------------------- */

void CWP_Timing(cwp_prog_t *p, float char_wpm, float eff_wpm, uint8_t ics, uint8_t iws, uint32_t ramp)
{
    if (!(char_wpm >= 5.0f)) char_wpm = 5.0f;
    if (char_wpm > 60.0f) char_wpm = 60.0f;
    const uint32_t dit = (uint32_t)(57600.0f / char_wpm + 0.5f);
    p->dit = dit;
    p->dah = 3U * dit;
    p->elem_gap = dit;
    /* Plain and manual spacing in whole dits, so PARIS keeps its cadence. */
    float cg = (float)(3U * dit), wg = (float)(7U * dit);
    if (ics || iws) {
        if (ics >= 3U) cg = (float)(ics * dit);
        if (iws >= 6U) wg = (float)(iws * dit);
    } else if (eff_wpm >= 2.0f && eff_wpm < char_wpm) {
        /* ARRL Farnsworth: the time a PARIS word gains, ta = (60c - 37.2s)
         * / (s c) s, spread 3/19 to each character gap and 7/19 to each
         * word gap. */
        const float ta = (60.0f * char_wpm - 37.2f * eff_wpm) / (eff_wpm * char_wpm);
        cg = 48000.0f * 3.0f * ta / 19.0f;
        wg = 48000.0f * 7.0f * ta / 19.0f;
    }
    p->char_gap = (uint32_t)(cg + 0.5f);
    p->word_gap = (uint32_t)(wg + 0.5f);
    if (p->word_gap < p->char_gap) p->word_gap = p->char_gap;
    p->ramp = ramp;
    if (p->lead == 0U) p->lead = 48U;
    if (p->level == 0U) p->level = 255U;
}

uint8_t CWP_SetText(cwp_prog_t *p, const uint8_t *text, uint8_t len)
{
    uint8_t n = 0U;
    for (uint8_t i = 0U; i < len && n < CWP_TEXT_MAX; ++i) {
        if (text[i] == ' ') {
            /* One word space, never two, never first. */
            if (n && p->text[n - 1U] != 0U) p->text[n++] = 0U;
            continue;
        }
        const uint8_t code = MORSE_Code(text[i]);
        if (code > 1U) p->text[n++] = code;
    }
    while (n && p->text[n - 1U] == 0U) --n;
    p->len = n;
    return n;
}

void CWP_Init(const uint16_t *ramp_lut, uint16_t lut_size)
{
    cwp_lut = ramp_lut;
    cwp_lut_n = lut_size;
}

uint8_t CWP_Busy(void) { return (uint8_t)(cwp_go || cwp_running); }
uint8_t CWP_Running(void) { return cwp_running; }
uint8_t CWP_Done(void) { return cwp_done; }

uint8_t CWP_Start(const cwp_prog_t *p)
{
    if (CWP_Busy() || p == 0 || p->len == 0U) return 0U;
    cwp_prog = *p;
    cwp_stop = 0U;
    cwp_done = 0U;
    cwp_go = 1U;
    return 1U;
}

void CWP_Stop(void)
{
    if (cwp_go) cwp_go = 0U;
    if (cwp_running) cwp_stop = 1U;
}

static uint8_t code_len(uint8_t code)
{
    uint8_t n = 0U;
    while (code > 1U) { code >>= 1; ++n; }
    return n;
}

uint32_t CWP_Duration(const cwp_prog_t *p)
{
    uint32_t t = p->lead;
    for (uint8_t i = 0U; i < p->len; ++i) {
        const uint8_t c = p->text[i];
        if (c == 0U) { t += p->word_gap - p->char_gap; continue; }
        const uint8_t n = code_len(c);
        for (uint8_t k = 0U; k < n; ++k) t += ((c >> k) & 1U) ? p->dah : p->dit;
        t += (uint32_t)(n - 1U) * p->elem_gap;
        if (i + 1U < p->len) t += p->char_gap;
    }
    return t + p->ramp;
}

/* ---- Headphone renderer ---------------------------------------------------- */

static inline float ramp_at(uint32_t pos, uint32_t total)
{
    if (total == 0U || pos >= total || cwp_lut == 0) return 1.0f;
    return (float)cwp_lut[(pos * cwp_lut_n) / total] * (1.0f / 65535.0f);
}

/* The mark of element `el` (counted down) of the code at `pos`. */
static inline void start_mark(void)
{
    const uint8_t c = cwp_prog.text[cwp.pos];
    cwp.phase = PH_MARK;
    cwp.left = ((c >> cwp.el) & 1U) ? cwp_prog.dah : cwp_prog.dit;
    cwp.target = 1U;
}

/* The first element of the code at `pos`, or the end. */
static inline void start_symbol(void)
{
    if (cwp.pos >= cwp_prog.len) {
        cwp.phase = PH_END;
        cwp.target = 0U;
        return;
    }
    cwp.el = (uint8_t)(code_len(cwp_prog.text[cwp.pos]) - 1U);
    start_mark();
}

/* A mark or a gap has run out. */
static __attribute__((noinline)) void next_phase(void)
{
    if (cwp.phase == PH_LEAD) { start_symbol(); return; }
    if (cwp.phase == PH_GAP) {
        if (cwp.el != 0xFFU) start_mark();
        else start_symbol();
        return;
    }
    /* End of a mark: the gap after it. */
    cwp.target = 0U;
    cwp.phase = PH_GAP;
    if (cwp.el) {
        --cwp.el;
        cwp.left = cwp_prog.elem_gap;
        return;
    }
    cwp_done = (uint8_t)(cwp_done + 1U);
    cwp.el = 0xFFU; /* the symbol is done */
    ++cwp.pos;
    if (cwp.pos < cwp_prog.len && cwp_prog.text[cwp.pos] == 0U) {
        ++cwp.pos;
        cwp.left = cwp_prog.word_gap;
    } else if (cwp.pos < cwp_prog.len) {
        cwp.left = cwp_prog.char_gap;
    } else {
        cwp.phase = PH_END;
    }
}

static inline void envelope(void)
{
    const uint32_t ramp = cwp_prog.ramp;
    if (cwp.target) {
        if (cwp.env < 1.0f) {
            if (cwp.rpos < ramp) ++cwp.rpos;
            cwp.env = ramp_at(cwp.rpos, ramp);
        }
    } else if (cwp.env > 0.0f) {
        if (cwp.rpos < ramp) ++cwp.rpos;
        cwp.env = 1.0f - ramp_at(cwp.rpos, ramp);
        if (cwp.env < 0.0001f) cwp.env = 0.0f;
    }
}

float CWP_Step(void)
{
    if (!cwp_running) {
        if (!cwp_go) return 0.0f;
        cwp = (cwp_state_t){0};
        cwp.rpos = cwp_prog.ramp; /* at rest: the last ramp is complete */
        cwp.phase = PH_LEAD;
        cwp.left = cwp_prog.lead;
        cwp_go = 0U;
        cwp_running = 1U;
    }
    const uint8_t was = cwp.target;
    if (cwp_stop) {
        cwp_stop = 0U;
        cwp.phase = PH_END;
        cwp.target = 0U;
    }
    if (cwp.phase != PH_END) {
        if (cwp.left) --cwp.left;
        if (cwp.left == 0U) next_phase();
    }
    if (cwp.target != was) {
        /* A ramp reversed part-way starts from where the other left it. */
        cwp.rpos = (cwp.rpos < cwp_prog.ramp) ? (cwp_prog.ramp - cwp.rpos) : 0U;
    }
    envelope();
    if (cwp.phase == PH_END && cwp.env <= 0.0f) {
        cwp_running = 0U;
        return 0.0f;
    }
    return cwp.env * (float)cwp_prog.level * (1.0f / 255.0f);
}

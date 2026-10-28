/* The Morse trainer (Core/Src/mod_morse_trainer.c) with its player, the
 * shared Morse table and the pack reader against the tree's sd/CWPACK.BIN.
 *
 * A scripted keyer sends what a hand on the paddle would -- each element's
 * start with its kind, its key-up, a straight key's press and release, all
 * timestamped in ms -- and drops it while the trainer holds the levers off,
 * as the radio's keyer does. The player is stepped 48 samples a ms, as the
 * headphone renderer steps it; the main loop services the trainer every ms
 * unless a test holds it up. */
#include "mod_morse_trainer.h"
#include "mod_cw_player.h"
#include "cw_pack.h"
#include "sd_cw_card.h"
#include "modulator.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++fails; } } while (0)

/* ---- The radio around the trainer ---- */
static uint8_t gate_on, inhibit_on, swap_on, levers;
static float train_wpm;
void MOD_CwSetTrainGate(uint8_t on) { gate_on = on; }
void MOD_CwSetTrainingWpm(float wpm) { train_wpm = wpm; }
void MOD_CwSetTrainInhibit(uint8_t on) { inhibit_on = on; }
uint8_t MOD_CwTrainInhibited(void) { return inhibit_on; }
uint8_t MOD_CwLevers(void) { return levers; }
void MOD_SetCwSwap(uint8_t en) { swap_on = en; }
uint8_t MOD_GetCwSwap(void) { return swap_on; }

static uint32_t now = 100000U;
static uint32_t hold_until;       /* the main loop is held up until then */
static uint32_t svc_lag;          /* ms the main loop's time is behind the keyer's stamps */

/* Scheduled key events. */
typedef struct { uint32_t t; uint8_t ev; } sched_t;
static sched_t sched[1024];
static unsigned n_sched, at_sched;
static uint32_t dropped;

static void tick(void)
{
    ++now;
    for (unsigned k = 0U; k < 48U; ++k) (void)CWP_Step();
    while (at_sched < n_sched && sched[at_sched].t <= now) {
        /* The keyer reads open levers while the trainer gates it. */
        if (!gate_on && MTRAIN_Capturing()) MTRAIN_KeyEvent(sched[at_sched].ev, sched[at_sched].t);
        else ++dropped;
        ++at_sched;
    }
    if ((int32_t)(now - hold_until) >= 0) MTRAIN_Service(now - svc_lag);
}

static void run(uint32_t ms) { while (ms--) tick(); }

static uint8_t run_until(uint8_t state, uint32_t limit)
{
    while (limit--) {
        tick();
        if (MTRAIN_View()->state == state) return 1U;
    }
    return 0U;
}

static void add(uint32_t t, uint8_t ev) { sched[n_sched].t = t; sched[n_sched].ev = ev; ++n_sched; }

/* Key `text` from now + 1 ms: elements exact (an iambic keyer), the gaps
 * in dits as given; a straight key presses and releases instead. Returns
 * the time of the last key-up. */
static uint32_t key(const char *text, float wpm, float elem_gap, float char_gap, float word_gap, uint8_t straight)
{
    const float dit = 1200.0f / wpm;
    float t = (float)now + 1.0f, last_up = t;
    n_sched = at_sched = 0U;
    char buf[8];
    for (const char *p = text; *p; ++p) {
        if (*p == ' ') { t += (word_gap - char_gap) * dit; continue; }
        const char *pat = MORSE_Pattern((uint8_t)*p, buf);
        if (p[0] == '#') pat = "........";   /* the error prosign */
        if (p[0] == '!') pat = "--------";   /* nothing at all */
        for (const char *e = pat; *e; ++e) {
            const float len = (*e == '-') ? 3.0f * dit : dit;
            add((uint32_t)lroundf(t), straight ? MTRAIN_EV_DOWN : (*e == '-' ? MTRAIN_EV_DAH : MTRAIN_EV_DIT));
            add((uint32_t)lroundf(t + len), MTRAIN_EV_UP);
            last_up = t + len;
            t += len + (e[1] ? elem_gap * dit : 0.0f);
        }
        t += char_gap * dit;
    }
    return (uint32_t)lroundf(last_up);
}

static uint32_t key_now(const char *text, float wpm) { return key(text, wpm, 1.0f, 3.0f, 7.0f, 0U); }

/* Run until `ms` after time `t`. */
static void run_past(uint32_t t, uint32_t ms) { while ((int32_t)(now - (t + ms)) < 0) tick(); }

static const mtrain_view_t *v(void) { return MTRAIN_View(); }

static void str_of(const uint8_t *s, uint8_t n, char *out)
{
    for (uint8_t i = 0U; i < n; ++i) out[i] = (char)s[i];
    out[n] = 0;
}

static void prompt_text(char *out) { str_of(v()->prompt, v()->prompt_len, out); }

static void fresh(void)
{
    MTRAIN_Stop();
    MTRAIN_Init();
    CWPACK_Close();
    sd_cw_card_reset();
    levers = 0U;
    swap_on = 0U;
    n_sched = at_sched = 0U;
    hold_until = 0U;
}

/* Start an activity and run to the first answer turn. */
static uint8_t start(uint8_t act, uint8_t src, uint8_t lesson)
{
    mtrain_prefs_t *p = MTRAIN_Prefs();
    p->act = act;
    p->src = src;
    MTRAIN_PrefsChanged();
    MTRAIN_SetLesson(lesson);
    MTRAIN_Start(0U, now);
    return run_until(act == MTRAIN_FREE ? MTRAIN_KEYING : MTRAIN_THINK, 20000U);
}

/* ---- Decoding ---- */

static void test_free_keying(void)
{
    fresh();
    CHECK(start(MTRAIN_FREE, MTRAIN_CHARS, 10U), "free keying starts");
    CHECK(inhibit_on && !gate_on && train_wpm == 15.0f, "RF inhibited, levers given, key at 15 WPM");
    run_past(key_now("PARIS PARIS", 15.0f), 1500U);
    CHECK(strcmp((char *)v()->line, "PARIS PARIS ") == 0, "free keying decodes '%s'", v()->line);

    /* E E with the main loop held up for a second: the gap is judged from
     * the timestamps, not from when the main loop gets to them. */
    fresh();
    start(MTRAIN_FREE, MTRAIN_CHARS, 10U);
    key_now("EE", 15.0f);
    hold_until = now + 1000U;
    run(1500U);
    CHECK(strcmp((char *)v()->line, "EE ") == 0, "delayed E E decodes '%s', not I", v()->line);
    /* And an I keyed as one stays one. */
    key_now("I", 15.0f);
    hold_until = now + 700U;
    run(1500U);
    CHECK(strcmp((char *)v()->line, "EE I ") == 0, "I stays I: '%s'", v()->line);

    /* Drained in pieces: the loop held up at odd moments. */
    fresh();
    start(MTRAIN_FREE, MTRAIN_CHARS, 10U);
    const uint32_t end = key_now("KMRSU", 15.0f);
    for (unsigned k = 0U; k < 30U; ++k) {
        hold_until = now + 37U + (k * 53U) % 140U;
        run(60U);
    }
    run_past(end, 1500U);
    CHECK(strcmp((char *)v()->line, "KMRSU ") == 0, "partial drains decode '%s'", v()->line);

    /* A learner slow between elements: 2 dits is inside the character, 3 is not. */
    fresh();
    start(MTRAIN_FREE, MTRAIN_CHARS, 10U);
    run_past(key("K", 15.0f, 2.0f, 3.0f, 7.0f, 0U), 1500U);
    run_past(key("K", 15.0f, 3.0f, 3.0f, 7.0f, 0U), 1500U);
    CHECK(strcmp((char *)v()->line, "K TET ") == 0, "element gaps: '%s'", v()->line);

    /* The error prosign takes the word back; eight dahs is no symbol. */
    fresh();
    start(MTRAIN_FREE, MTRAIN_CHARS, 10U);
    run_past(key_now("CQ DE#", 15.0f), 1500U);
    run_past(key_now("!", 15.0f), 1500U);
    CHECK(strcmp((char *)v()->line, "CQ * ") == 0, "erase and invalid: '%s'", v()->line);

    /* A straight key: elements by length, the averages following the hand. */
    fresh();
    start(MTRAIN_FREE, MTRAIN_CHARS, 10U);
    run_past(key("PARIS", 12.0f, 1.0f, 3.0f, 7.0f, 1U), 1500U);
    run_past(key("CQ", 12.0f, 1.0f, 3.0f, 7.0f, 1U), 1500U);
    CHECK(strcmp((char *)v()->line, "PARIS CQ ") == 0, "straight key: '%s'", v()->line);
    /* A 3 ms bounce inside a gap is not an element and does not split it. */
    n_sched = at_sched = 0U;
    add(now + 10U, MTRAIN_EV_DOWN); add(now + 13U, MTRAIN_EV_UP);
    run(2000U);
    CHECK(strcmp((char *)v()->line, "PARIS CQ ") == 0, "a bounce is ignored: '%s'", v()->line);
}

/* ---- Echo ---- */

/* Answer the prompt (or `text`) and run to the verdict. */
static uint8_t answer(const char *text, float wpm)
{
    char p[16];
    prompt_text(p);
    key_now(text ? text : p, wpm);
    return run_until(MTRAIN_FEEDBACK, 20000U);
}

static void test_echo(void)
{
    char p[16];
    fresh();
    MTRAIN_Prefs()->retries = 2U;
    CHECK(start(MTRAIN_ECHO, MTRAIN_CHARS, 2U), "echo reaches its first turn");
    prompt_text(p);
    CHECK(strlen(p) == 1U && strchr("MK", p[0]), "lesson 2 draws M or K: '%s'", p);
    CHECK(dropped == 0U, "nothing keyed yet");
    CHECK(answer(NULL, 15.0f) && v()->verdict == MTRAIN_V_RIGHT, "the right answer is right");
    CHECK(v()->done == 1U && v()->right == 1U, "one first try, right");

    /* Wrong, then right on the retry: one first attempt, counted wrong. */
    CHECK(run_until(MTRAIN_THINK, 20000U), "the next prompt");
    prompt_text(p);
    const char *wrong = (p[0] == 'M') ? "K" : "M";
    CHECK(answer(wrong, 15.0f) && v()->verdict == MTRAIN_V_WRONG, "wrong is wrong");
    CHECK(v()->expected == (uint8_t)p[0] && v()->heard == (uint8_t)wrong[0], "expected %c heard %c", v()->expected,
          v()->heard);
    CHECK(run_until(MTRAIN_THINK, 20000U) && v()->attempt == 1U, "the same prompt again");
    char again[16];
    prompt_text(again);
    CHECK(strcmp(again, p) == 0, "it is the same prompt");
    CHECK(answer(NULL, 15.0f) && v()->verdict == MTRAIN_V_RIGHT, "right on the retry");
    CHECK(v()->done == 2U && v()->right == 1U, "first tries %u right %u", v()->done, v()->right);
    const mtrain_progress_t *g = MTRAIN_Progress();
    const uint8_t x = MORSE_Index((uint8_t)p[0]);
    CHECK(g->tries[x] >= 1U && g->win_n == 2U, "the record has the first tries (%u)", g->win_n);

    /* Submit straight after the last element keeps that character. */
    CHECK(run_until(MTRAIN_THINK, 20000U), "next");
    prompt_text(p);
    n_sched = at_sched = 0U;
    const uint32_t last_up = key(p, 15.0f, 1.0f, 3.0f, 7.0f, 0U);
    while (now < last_up + 2U) tick();
    MTRAIN_Submit();
    run(2U);
    CHECK(v()->state == MTRAIN_FEEDBACK && v()->verdict == MTRAIN_V_RIGHT && v()->answer_len == 1U,
          "an immediate submit keeps the last character (verdict %u, %u symbols)", v()->verdict, v()->answer_len);

    /* Submit while the last element still sounds waits for its key-up. */
    CHECK(run_until(MTRAIN_THINK, 20000U), "next");
    prompt_text(p);
    n_sched = at_sched = 0U;
    const uint32_t up2 = key(p, 15.0f, 1.0f, 3.0f, 7.0f, 0U);
    while (now < up2 - 30U) tick();
    MTRAIN_Submit();
    run(40U);
    CHECK(v()->verdict == MTRAIN_V_RIGHT, "submit during the last element: verdict %u", v()->verdict);

    /* Keying after the click is not part of the answer. */
    CHECK(run_until(MTRAIN_THINK, 20000U), "next");
    prompt_text(p);
    n_sched = at_sched = 0U;
    const uint32_t up3 = key(p, 15.0f, 1.0f, 3.0f, 7.0f, 0U);
    add(up3 + 4U, MTRAIN_EV_DIT);
    add(up3 + 84U, MTRAIN_EV_UP);
    while (now < up3 + 2U) tick();
    MTRAIN_Submit();
    hold_until = now + 10U; /* the dit is queued before the main loop gets to the click */
    run(200U);
    CHECK(v()->verdict == MTRAIN_V_RIGHT && v()->answer_len == 1U, "a dit after the click is not in the answer");

    /* Nothing keyed: a timeout, not a first-try error. */
    CHECK(run_until(MTRAIN_THINK, 20000U), "next");
    const uint16_t done = v()->done;
    CHECK(run_until(MTRAIN_FEEDBACK, 8000U) && v()->verdict == MTRAIN_V_TIMEOUT, "think time runs out");
    CHECK(v()->done == done && v()->timeouts == 1U, "a timeout is counted apart");
    MTRAIN_Stop();
    CHECK(!inhibit_on && !gate_on && train_wpm == 0.0f, "stop gives the keyer back");
}

static void test_echo_details(void)
{
    char p[16];
    /* The error prosign starts the answer over. */
    fresh();
    MTRAIN_Prefs()->group = 3U;
    CHECK(start(MTRAIN_ECHO, MTRAIN_CHARS, 6U), "groups of three");
    prompt_text(p);
    CHECK(strlen(p) == 3U, "a group of three: '%s'", p);
    run_past(key_now("T#", 15.0f), 400U);
    CHECK(v()->state == MTRAIN_THINK && v()->answer_len == 0U, "eight dits clear the answer");
    CHECK(answer(NULL, 15.0f) && v()->verdict == MTRAIN_V_RIGHT, "then the answer is right");

    /* Lost input (the queue overflowed): the same try again, not counted. */
    CHECK(run_until(MTRAIN_THINK, 20000U), "next");
    const uint16_t done = v()->done;
    for (unsigned k = 0U; k < 40U; ++k) MTRAIN_KeyEvent(k & 1U ? MTRAIN_EV_UP : MTRAIN_EV_DIT, now);
    run(2U);
    CHECK(v()->verdict == MTRAIN_V_LOST && v()->lost == 1U && v()->done == done, "overflow: lost, not graded");
    CHECK(run_until(MTRAIN_THINK, 20000U) && v()->attempt == 0U, "the same first try again");

    /* A missed character is lined up, not everything after it wrong. */
    fresh();
    MTRAIN_Prefs()->group = 4U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 10U);
    prompt_text(p);
    char shorter[8] = {p[0], p[2], p[3], 0};
    CHECK(answer(shorter, 15.0f) && v()->verdict == MTRAIN_V_WRONG, "one missing");
    CHECK(v()->wrong == 0x2U || p[1] == p[0] || p[1] == p[2], "only the second marked: %x for %s/%s", v()->wrong, p,
          shorter);

    /* Prompt shown as text only: no sound, the levers given at once. */
    fresh();
    MTRAIN_Prefs()->show = MTRAIN_SHOW_TEXT;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 5U);
    CHECK(v()->reveal && !CWP_Busy(), "text prompt shown, nothing played");

    /* The levers held through the prompt: given back only once open. */
    fresh();
    MTRAIN_Prefs()->act = MTRAIN_ECHO;
    MTRAIN_PrefsChanged();
    MTRAIN_SetLesson(2U);
    levers = 1U;
    MTRAIN_Start(0U, now);
    run(3000U);
    CHECK(v()->state == MTRAIN_THINK && gate_on, "held lever: still gated");
    levers = 0U;
    run(2U);
    CHECK(!gate_on, "released: the levers are given back");
}

/* ---- A learner's spacing ---- */

/* Key `text` with exact keyer elements and standard 3-dit letter gaps,
 * except inside letter `slow` (its index), whose elements are `gap` dits
 * apart: a newcomer re-pressing the lever after a breath. */
static uint32_t key_slow(const char *text, float wpm, int slow, float gap)
{
    const float dit = 1200.0f / wpm;
    float t = (float)now + 1.0f, last_up = t;
    n_sched = at_sched = 0U;
    char buf[8];
    for (int i = 0; text[i]; ++i) {
        const char *pat = MORSE_Pattern((uint8_t)text[i], buf);
        for (const char *e = pat; *e; ++e) {
            const float len = (*e == '-') ? 3.0f * dit : dit;
            add((uint32_t)lroundf(t), *e == '-' ? MTRAIN_EV_DAH : MTRAIN_EV_DIT);
            add((uint32_t)lroundf(t + len), MTRAIN_EV_UP);
            last_up = t + len;
            t += len + (e[1] ? (i == slow ? gap : 1.0f) * dit : 0.0f);
        }
        t += 3.0f * dit;
    }
    return (uint32_t)lroundf(last_up);
}

static uint8_t answer_slow(int slow, float gap)
{
    char p[16];
    prompt_text(p);
    key_slow(p, 10.0f, slow, gap);
    return run_until(MTRAIN_FEEDBACK, 20000U);
}

static void test_spacing(void)
{
    char p[16], a[24];
    /* M as two dahs 3.5 dits apart (a breath at 10 WPM) was T T at any
     * speed. Joined, it is right, shown as the prompt, with the hint; and
     * the next try reads the learner's gap as inside the letter. */
    fresh();
    MTRAIN_Prefs()->retries = 2U;
    MTRAIN_Prefs()->key_wpm = 10U;
    CHECK(start(MTRAIN_ECHO, MTRAIN_CHARS, 2U), "echo, lesson 2");
    prompt_text(p);
    CHECK(answer_slow(0, 3.5f) && v()->verdict == MTRAIN_V_RIGHT && v()->joined, "%s keyed slowly is right, joined",
          p);
    str_of(v()->answer, v()->answer_len, a);
    CHECK(strcmp(a, p) == 0, "the answer shows as %s: '%s'", p, a);
    CHECK(v()->done == 1U && v()->right == 1U, "a first try, right");
    CHECK(run_until(MTRAIN_THINK, 20000U), "the next prompt");
    prompt_text(p);
    CHECK(answer_slow(0, 3.5f) && v()->verdict == MTRAIN_V_RIGHT && !v()->joined, "%s: the gap is now inside it", p);

    /* What a join may not do: make another letter, or close a word gap. */
    CHECK(run_until(MTRAIN_THINK, 20000U), "next");
    prompt_text(p);
    CHECK(answer(p[0] == 'M' ? "TE" : "TT", 10.0f) && v()->verdict == MTRAIN_V_WRONG, "another letter stays wrong");
    fresh();
    MTRAIN_Prefs()->key_wpm = 10U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 2U);
    prompt_text(p);
    CHECK(answer_slow(0, 6.0f) && v()->verdict == MTRAIN_V_WRONG && !v()->joined, "%s across a word gap is wrong",
          p);

    /* In a group only the slow letter is joined; the standard 3-dit letter
     * gaps stay letter gaps. */
    fresh();
    MTRAIN_Prefs()->key_wpm = 10U;
    MTRAIN_Prefs()->group = 3U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 10U);
    prompt_text(p);
    const int slow = (MORSE_Length(MORSE_Code((uint8_t)p[1])) > 1U) ? 1 : -1;
    CHECK(answer_slow(slow, 3.5f) && v()->verdict == MTRAIN_V_RIGHT && v()->joined == (slow == 1),
          "group %s, letter %d slow: verdict %u joined %u", p, slow, v()->verdict, v()->joined);

    /* A learner who closes up again is read as before: keyer gaps pull the
     * gap inside letters back, and E E at 3 dits is two letters again. */
    fresh();
    MTRAIN_Prefs()->key_wpm = 10U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 2U);
    CHECK(answer_slow(0, 3.5f) && v()->joined, "joined once");
    for (unsigned k = 0U; k < 6U; ++k) {
        CHECK(run_until(MTRAIN_THINK, 20000U), "next");
        CHECK(answer(NULL, 10.0f) && v()->verdict == MTRAIN_V_RIGHT && !v()->joined, "tight keying is right");
    }
    MTRAIN_Stop();
    MTRAIN_Prefs()->act = MTRAIN_FREE;
    MTRAIN_PrefsChanged();
    fresh();
    MTRAIN_Prefs()->key_wpm = 10U;
    start(MTRAIN_FREE, MTRAIN_CHARS, 10U);
    run_past(key("EE", 10.0f, 1.0f, 3.0f, 7.0f, 0U), 2000U);
    CHECK(strcmp((char *)v()->line, "EE ") == 0, "a new session starts at the standard split: '%s'", v()->line);
}

/* ---- Content ---- */

/* The main loop takes its time at the start of a pass; the keyer's interrupt
 * stamps an event when it happens, so an event can be a few ms younger than
 * the service's time. Read as 2^30 ms old, such an event ended the letter and
 * the word: M keyed as two dahs was T T at random (bench, 2026-10-07). */
static void test_late_service(void)
{
    char a[24];
    fresh();
    MTRAIN_Prefs()->key_wpm = 10U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 2U);
    for (uint32_t lag = 1U; lag <= 4U; ++lag) {
        svc_lag = lag;
        CHECK(answer(NULL, 10.0f) && v()->verdict == MTRAIN_V_RIGHT && !v()->joined,
              "%u ms behind: right, not joined (verdict %u)", lag, v()->verdict);
        CHECK(run_until(MTRAIN_THINK, 20000U), "next");
    }
    svc_lag = 3U;
    CHECK(answer_slow(0, 3.5f) && v()->verdict == MTRAIN_V_RIGHT, "slow keying, 3 ms behind: verdict %u",
          v()->verdict);
    MTRAIN_Stop();
    MTRAIN_Prefs()->act = MTRAIN_FREE;
    MTRAIN_PrefsChanged();
    fresh();
    MTRAIN_Prefs()->key_wpm = 10U;
    start(MTRAIN_FREE, MTRAIN_CHARS, 10U);
    run_past(key("PARIS PARIS", 10.0f, 1.0f, 3.0f, 7.0f, 0U), 2000U);
    str_of(v()->line, v()->line_len, a);
    CHECK(strcmp(a, "PARIS PARIS ") == 0, "free keying 3 ms behind: '%s'", a);
    svc_lag = 0U;
    MTRAIN_Stop();
}

static void test_content(void)
{
    char p[16];
    uint8_t c[MORSE_SYMBOLS], stops[7];
    CWPACK_Course(0U, c, stops);
    /* Characters only from the lesson; the newest third more often. */
    fresh();
    MTRAIN_Prefs()->group = 8U;
    MTRAIN_Prefs()->show = MTRAIN_SHOW_TEXT;
    MTRAIN_Prefs()->retries = 0U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 9U);
    unsigned count[256] = {0}, total = 0U;
    for (unsigned k = 0U; k < 150U; ++k) {
        prompt_text(p);
        for (const char *q = p; *q; ++q) {
            CHECK(memchr(c, *q, 9U) != NULL, "'%c' is not in lesson 9", *q);
            ++count[(uint8_t)*q];
            ++total;
        }
        MTRAIN_Skip();
        run_until(MTRAIN_THINK, 20000U);
    }
    /* Lesson 9: places 6..8 drawn 2/3 * 1/9 + 1/3 * 1/3 = 0.185 each, the rest 0.074. */
    const double newest = (double)count[c[8]] / total, oldest = (double)count[c[0]] / total;
    CHECK(newest > 0.13 && newest < 0.24 && oldest > 0.04 && oldest < 0.11, "draw: newest %.3f oldest %.3f", newest,
          oldest);

    /* Words made only of the lesson's characters. */
    fresh();
    MTRAIN_Prefs()->show = MTRAIN_SHOW_TEXT;
    start(MTRAIN_ECHO, MTRAIN_WORDS, 12U);
    for (unsigned k = 0U; k < 40U; ++k) {
        prompt_text(p);
        CHECK(strlen(p) >= 1U, "a word");
        for (const char *q = p; *q; ++q) CHECK(memchr(c, *q, 12U) != NULL, "word '%s' has '%c'", p, *q);
        MTRAIN_Skip();
        run_until(MTRAIN_THINK, 20000U);
    }
    /* Call signs: a prefix, a digit, a suffix. */
    fresh();
    MTRAIN_Prefs()->show = MTRAIN_SHOW_TEXT;
    start(MTRAIN_ECHO, MTRAIN_CALLS, 12U);
    for (unsigned k = 0U; k < 40U; ++k) {
        prompt_text(p);
        const size_t n = strlen(p);
        CHECK(n >= 3U && n <= 8U && p[n - 1U] >= 'A' && p[n - 1U] <= 'Z' && strpbrk(p, "0123456789"), "call '%s'", p);
        MTRAIN_Skip();
        run_until(MTRAIN_THINK, 20000U);
    }
    /* Weak characters come up more often (Morserino's adaptive weights). */
    fresh();
    MTRAIN_Prefs()->boost = 1U;
    MTRAIN_Prefs()->retries = 0U;
    MTRAIN_Prefs()->show = MTRAIN_SHOW_TEXT;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 10U);
    /* The first ten are each character once. */
    unsigned seen = 0U;
    for (unsigned k = 0U; k < 10U; ++k) {
        prompt_text(p);
        seen |= 1U << (unsigned)((const uint8_t *)memchr(c, p[0], 10U) - c);
        answer(NULL, 15.0f);
        run_until(MTRAIN_THINK, 20000U);
    }
    CHECK(seen == 0x3FFU, "adaptive: each character once first (%x)", seen);
    /* Then answer M wrong and everything else right: M climbs. */
    unsigned m = 0U, n = 0U;
    for (unsigned k = 0U; k < 200U; ++k) {
        prompt_text(p);
        if (p[0] == 'M') { ++m; answer("T", 15.0f); }
        else answer(NULL, 15.0f);
        ++n;
        run_until(MTRAIN_THINK, 20000U);
    }
    CHECK(m * 10U > n * 2U, "the weak M comes up %u of %u", m, n);
}

/* ---- Progress ---- */

static void test_progress(void)
{
    fresh();
    MTRAIN_Prefs()->retries = 0U;
    MTRAIN_Prefs()->minutes = 0U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 2U);
    CHECK(v()->lesson == 2U && v()->newest == 'K', "lesson 2's newest is K");
    unsigned tries = 0U;
    for (; tries < 60U && !v()->recommend; ++tries) {
        answer(NULL, 15.0f);
        run_until(MTRAIN_THINK, 20000U);
    }
    CHECK(v()->recommend && tries == 50U, "fifty right first tries recommend the next character (%u)", tries);
    CHECK(MTRAIN_AddCharacter() && v()->lesson == 3U && v()->newest == 'R' && !v()->recommend,
          "the learner adds R");
    CHECK(MTRAIN_Progress()->lesson[0] == 3U && MTRAIN_Progress()->win_n == 0U, "a fresh score for lesson 3");
    CHECK(MTRAIN_ProgressDirty(), "the record wants saving");
    /* Never by itself, and not before R has been tried ten times. */
    for (unsigned k = 0U; k < 30U; ++k) {
        answer(NULL, 15.0f);
        run_until(MTRAIN_THINK, 20000U);
    }
    CHECK(v()->lesson == 3U, "the lesson never moves by itself");

    /* The newest character's part of the rule, on a record set up for it:
     * 50 right overall, the newest tried fewer than ten times or not 90 %
     * right, then both. */
    fresh();
    MTRAIN_Prefs()->retries = 0U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 2U);
    mtrain_progress_t *g = MTRAIN_Progress();
    g->win = ~0ULL; g->win_n = 50U; g->newest_win = 0x1FU; g->newest_n = 5U;
    answer(NULL, 15.0f);
    CHECK(!v()->recommend, "the newest tried five or six times: not yet");
    run_until(MTRAIN_THINK, 20000U);
    g->newest_win = 0x3F0U; g->newest_n = 10U;
    answer(NULL, 15.0f);
    CHECK(!v()->recommend, "the newest right 6 of 10: not yet");
    run_until(MTRAIN_THINK, 20000U);
    g->newest_win = 0xFFFFFU; g->newest_n = 20U;
    answer(NULL, 15.0f);
    CHECK(v()->recommend, "both at 90 %%: recommended");

    /* The session ends after its minutes with a summary. */
    fresh();
    MTRAIN_Prefs()->minutes = 1U;
    start(MTRAIN_ECHO, MTRAIN_CHARS, 4U);
    uint8_t summary = 0U;
    for (unsigned k = 0U; k < 200U && !summary; ++k) {
        answer(NULL, 15.0f);
        summary = run_until(MTRAIN_THINK, 20000U) == 0U && v()->state == MTRAIN_SUMMARY;
    }
    CHECK(summary || v()->state == MTRAIN_SUMMARY, "a summary after a minute (state %u)", v()->state);
    CHECK(gate_on, "no keying in the summary");
    MTRAIN_Next();
    CHECK(run_until(MTRAIN_THINK, 20000U) && v()->done == 0U, "a new session");
}

/* ---- The course (START LEARNING), paddle check, listen, errors ---- */

/* The course's steps, as tools/cw_pack/steps.txt has them. */
enum { C_PADDLE = 1U, C_EXPLORE, C_INTRO, C_LISTEN, C_DRILL, C_ECHO, C_WORDS, C_MASTER, C_GROUPS, C_MWORDS,
       C_ABBR, C_CALLS, C_SPEED, C_MIXED };

/* A learner who gets every prompt right, on the paddle only: what the
 * course plays until `done` says stop (or `limit` ms). Never a knob. */
static unsigned seen_steps;   /* bit g: step g ran */
static unsigned seen_free;    /* free keying outside the course's EXPLORE */
static uint8_t learner(uint8_t (*done)(void), uint32_t limit)
{
    while (limit && !done()) {
        const uint8_t s = v()->state;
        /* A step ran when it asked something or played: skipped ones pass
         * through LOAD only. */
        if (s == MTRAIN_THINK || s == MTRAIN_SHOW) seen_steps |= 1U << v()->guided;
        seen_free += (s == MTRAIN_KEYING && v()->guided != C_EXPLORE);
        if (s == MTRAIN_THINK) {
            answer(NULL, 15.0f);
            limit = limit > 3000U ? limit - 3000U : 0U;
        } else {
            tick();
            --limit;
        }
        CHECK(s != MTRAIN_SUMMARY && s != MTRAIN_ERROR, "the course never stops (state %u)", s);
        if (s == MTRAIN_SUMMARY || s == MTRAIN_ERROR) return 0U;
    }
    return done();
}

static uint8_t lesson_3(void) { return v()->lesson == 3U; }
static uint8_t at_mixed(void) { return v()->guided == C_MIXED && v()->state == MTRAIN_THINK; }
static uint8_t at_intro(void) { return v()->guided == C_INTRO && v()->state == MTRAIN_THINK; }
static uint8_t at_listen(void) { return v()->guided == C_LISTEN; }
static uint8_t at_echo(void) { return v()->guided == C_ECHO && v()->state == MTRAIN_THINK; }
static uint8_t at_drill(void) { return v()->guided == C_DRILL && v()->state == MTRAIN_THINK; }
static uint8_t past_lesson_4(void) { return v()->lesson != 4U; }

static void test_course(void)
{
    char p[16];
    fresh();
    MTRAIN_Prefs()->retries = 0U;
    MTRAIN_Start(1U, now);
    CHECK(run_until(MTRAIN_CHECK, 5000U) && v()->guided == C_PADDLE, "the course starts with the paddle check");
    levers = 1U; run(5U); levers = 3U; run(5U); levers = 0U;
    CHECK(run_until(MTRAIN_STEP_DONE, 200U), "squeeze and release: the step is done");
    CHECK(run_until(MTRAIN_KEYING, 1500U) && v()->guided == C_EXPLORE, "and the keyer comes on by itself");
    /* Explore: held dits and held dahs are not enough... */
    run_past(key_now("S O", 15.0f), 1500U);
    CHECK(v()->state == MTRAIN_KEYING, "dits and dahs, no squeeze yet: still exploring");
    /* ... a squeeze too, and it goes on by itself. */
    run_past(key_now("C", 15.0f), 600U);
    CHECK(v()->state == MTRAIN_STEP_DONE, "held dits, held dahs, a squeeze: done (state %u)", v()->state);
    CHECK(learner(at_intro, 20000U), "then the first new character");
    prompt_text(p);
    CHECK(v()->act == MTRAIN_LEARN && v()->reveal && strcmp(p, "M") == 0, "M, heard and seen: '%s'", p);
    CHECK(MTRAIN_Progress()->once == (MTRAIN_ONCE_PADDLE | MTRAIN_ONCE_EXPLORE), "paddle and keyer done for good");
    seen_steps = seen_free = 0U;
    CHECK(learner(at_listen, 120000U), "M and K taught");
    CHECK(v()->act == MTRAIN_LISTEN && v()->lesson == 2U, "then listening to them");
    /* The whole first lesson on the paddle alone: lesson 3 starts itself. */
    CHECK(learner(lesson_3, 3000000U), "lesson 3 by itself (lesson %u)", v()->lesson);
    CHECK(seen_steps == ((1U << C_INTRO) | (1U << C_LISTEN) | (1U << C_DRILL) | (1U << C_ECHO) | (1U << C_WORDS) |
                         (1U << C_MASTER)), "every step of the lesson ran (0x%x)", seen_steps);
    CHECK(seen_free == 0U, "never free keying");
    CHECK(learner(at_intro, 10000U), "the new lesson begins with its character");
    prompt_text(p);
    CHECK(strcmp(p, "R") == 0 && v()->act == MTRAIN_LEARN, "R: '%s'", p);
    CHECK(MTRAIN_Progress()->lesson[0] == 3U && MTRAIN_ProgressDirty(), "the record has lesson 3");

    /* Another session: paddle and keyer skip themselves, R is reviewed. */
    MTRAIN_Stop();
    MTRAIN_Start(1U, now);
    CHECK(run_until(MTRAIN_THINK, 20000U) && v()->guided == C_INTRO, "resumed at the lesson (step %u)", v()->guided);
    prompt_text(p);
    CHECK(strcmp(p, "R") == 0, "R again: '%s'", p);
    /* In the course a first try that runs out of time counts as missed. */
    CHECK(learner(at_echo, 600000U), "on to echo");
    const uint8_t n0 = MTRAIN_Progress()->win_n;
    CHECK(run_until(MTRAIN_FEEDBACK, 20000U) && v()->verdict == MTRAIN_V_TIMEOUT, "time up");
    CHECK(MTRAIN_Progress()->win_n == n0 + 1U && !(MTRAIN_Progress()->win & 1U), "counted as missed");

    /* The DRILL step: by sight, each of the newest repeated. */
    fresh();
    MTRAIN_Progress()->once = 3U;
    MTRAIN_Start(1U, now);
    CHECK(learner(at_drill, 600000U) && v()->reveal, "the drill is given by text");
    prompt_text(p);
    CHECK((strcmp(p, "KKKKK") == 0 || strcmp(p, "MMMMM") == 0), "a character five times: '%s'", p);
    const uint8_t dx = MORSE_Index((uint8_t)p[0]), dt = MTRAIN_Progress()->tries[dx];
    const uint8_t dn = MTRAIN_Progress()->win_n, dw = MTRAIN_Progress()->newest_n;
    CHECK(answer(NULL, 15.0f) && v()->verdict == MTRAIN_V_RIGHT, "the drill keyed");
    CHECK(MTRAIN_Progress()->tries[dx] == dt + 5U, "the drill is in the character's tries");
    CHECK(MTRAIN_Progress()->win_n == dn && MTRAIN_Progress()->newest_n == dw,
          "but not in the lesson's score, which is by ear (%u %u)", MTRAIN_Progress()->win_n, dn);

    /* CW Academy: four characters in its first lesson, five in the next. */
    fresh();
    MTRAIN_Prefs()->course = MTRAIN_COURSE_CWA;
    MTRAIN_Prefs()->retries = 0U;
    MTRAIN_Progress()->once = 3U;
    MTRAIN_Start(1U, now);
    char taught[8] = "";
    for (unsigned k = 0U; k < 4U; ++k) {
        CHECK(learner(at_intro, 60000U), "CWA intro %u", k);
        prompt_text(p);
        strcat(taught, p);
        for (unsigned r = 0U; r < 3U; ++r) { answer(NULL, 15.0f); run_until(MTRAIN_THINK, 5000U); }
    }
    CHECK(strcmp(taught, "TEAN") == 0 && v()->lesson == 4U, "CWA starts with T E A N: '%s'", taught);
    CHECK(learner(past_lesson_4, 3000000U) && v()->lesson == 9U, "then five more: lesson 9 (%u)", v()->lesson);
    CHECK(learner(at_intro, 60000U), "its intro");
    prompt_text(p);
    CHECK(strcmp(p, "O") == 0, "O first: '%s'", p);

    /* LICW from basic course 2: the first 18 known, three more a lesson. */
    fresh();
    MTRAIN_Prefs()->course = MTRAIN_COURSE_LICW;
    MTRAIN_Prefs()->carousel = 6U;
    MTRAIN_Progress()->once = 3U;
    MTRAIN_Start(1U, now);
    CHECK(run_until(MTRAIN_THINK, 20000U) && v()->lesson == 21U, "LICW BC2 starts at lesson 21 (%u)", v()->lesson);
    prompt_text(p);
    CHECK(p[0] == MTRAIN_Course()[18], "teaching the 19th character first");

    /* The whole alphabet: the stages, faster overall, then mixed practice. */
    fresh();
    mtrain_prefs_t *pr = MTRAIN_Prefs();
    pr->retries = 0U;
    pr->listen_wpm = 20U;
    pr->pace_wpm = 19U;
    MTRAIN_PrefsChanged();
    mtrain_progress_t *g = MTRAIN_Progress();
    g->once = 3U;
    g->lesson[0] = MORSE_SYMBOLS;
    g->win = ~0ULL; g->win_n = 50U; g->newest_win = 0xFFFFFU; g->newest_n = 20U;
    MTRAIN_Start(1U, now);
    seen_steps = 0U;
    CHECK(learner(at_mixed, 50000000U), "through to mixed practice");
    CHECK(pr->pace_wpm == 20U, "the overall speed came up to the characters' (%u)", pr->pace_wpm);
    CHECK(g->stage == C_CALLS, "every stage passed (%u)", g->stage);
    CHECK((seen_steps & ((1U << C_GROUPS) | (1U << C_MWORDS) | (1U << C_ABBR) | (1U << C_CALLS))) ==
          ((1U << C_GROUPS) | (1U << C_MWORDS) | (1U << C_ABBR) | (1U << C_CALLS)), "every stage ran");
    /* Next time the passed ones skip themselves: warm-up, then practice. */
    MTRAIN_Stop();
    MTRAIN_Start(1U, now);
    seen_steps = 0U;
    CHECK(learner(at_mixed, 2000000U), "back to mixed practice");
    CHECK(!(seen_steps & ((1U << C_INTRO) | (1U << C_MASTER) | (1U << C_GROUPS) | (1U << C_CALLS))),
          "nothing passed runs again (0x%x)", seen_steps);
    CHECK(seen_steps & (1U << C_ECHO), "the warm-up does");

    /* SAVE PROGRESS's record: a v8.10 record is taken, past its paddle
     * check and keyer; a damaged one is not. */
    mtrain_progress_t r;
    memset(&r, 0, sizeof(r));
    r.version = 1U; r.once = MTRAIN_GUIDED_DONE; r.lesson[0] = 2U; r.tries[0] = 3U; r.right[0] = 2U;
    CHECK(MTRAIN_ProgressValid(&r), "a v1 record is valid");
    MTRAIN_ProgressSet(&r);
    CHECK(g->version == MTRAIN_PROGRESS_VERSION && g->once == 3U && g->lesson[0] == 2U && g->stage == 0U,
          "and read as v2");
    r.once = 1U;
    MTRAIN_ProgressSet(&r);
    CHECK(g->once == MTRAIN_ONCE_PADDLE, "a v1 record past the paddle check only");
    r.version = 2U; r.once = 4U;
    CHECK(!MTRAIN_ProgressValid(&r), "v2 with an unknown step done is refused");
    r.once = 3U; r.right[0] = 4U;
    CHECK(!MTRAIN_ProgressValid(&r), "more right than tried is refused");
    r.right[0] = 1U; r.version = 3U;
    CHECK(!MTRAIN_ProgressValid(&r), "a later version is refused");
}

static void test_listen_and_errors(void)
{
    fresh();
    MTRAIN_Prefs()->group = 5U;
    MTRAIN_Prefs()->act = MTRAIN_LISTEN;
    MTRAIN_PrefsChanged();
    MTRAIN_SetLesson(10U);
    MTRAIN_Start(0U, now);
    CHECK(run_until(MTRAIN_SHOW, 20000U) && v()->reveal && v()->prompt_len == 5U, "listen shows the group after it");
    const mtrain_progress_t *g = MTRAIN_Progress();
    unsigned heard = 0U;
    for (unsigned i = 0U; i < MORSE_SYMBOLS; ++i) heard += g->heard[i];
    CHECK(heard == 5U, "five characters heard (%u)", heard);
    CHECK(run_until(MTRAIN_PLAY, 5000U), "the next group");
    levers = 1U; run(3U); levers = 0U;
    CHECK(v()->state == MTRAIN_PAUSE && !CWP_Running(), "a lever pauses");
    run(10U);
    levers = 2U; run(3U); levers = 0U;
    CHECK(v()->state == MTRAIN_PLAY, "and resumes");

    /* No card, a damaged pack, I/O refused, the custom course without its file. */
    fresh();
    sd_cw_card.mounted = 0U;
    MTRAIN_Start(0U, now);
    run(10U);
    CHECK(v()->state == MTRAIN_ERROR && v()->error == MTRAIN_ERR_CARD, "no card");
    sd_cw_card.mounted = 1U;
    CHECK(run_until(MTRAIN_THINK, 20000U), "the card back: training starts");
    fresh();
    sd_cw_card.pack[1000] ^= 1U;
    MTRAIN_Start(0U, now);
    run(10U);
    CHECK(v()->state == MTRAIN_ERROR && v()->error == MTRAIN_ERR_PACK, "a damaged pack");
    fresh();
    sd_cw_card.io = 0U;
    MTRAIN_Start(0U, now);
    run(500U);
    CHECK(v()->state == MTRAIN_LOAD, "I/O refused: it waits");
    sd_cw_card.io = 1U;
    CHECK(run_until(MTRAIN_THINK, 20000U), "then goes on");
    fresh();
    MTRAIN_Prefs()->course = MTRAIN_COURSE_CUSTOM;
    MTRAIN_Start(0U, now);
    run(10U);
    CHECK(v()->state == MTRAIN_ERROR && v()->error == MTRAIN_ERR_CUSTOM, "custom course without its file");
    fresh();
    sd_cw_card.custom = "QZ<BK>";
    MTRAIN_Prefs()->course = MTRAIN_COURSE_CUSTOM;
    MTRAIN_Start(0U, now);
    CHECK(run_until(MTRAIN_THINK, 20000U) && v()->lesson_max == 3U && v()->lesson == 2U, "custom course of 3");
}

int main(void)
{
    static uint16_t lut[513];
    for (unsigned i = 0U; i <= 512U; ++i)
        lut[i] = (uint16_t)(65535.0 * (0.5 - 0.5 * cos(3.14159265358979323846 * i / 512.0)) + 0.5);
    CWP_Init(lut, 512U);
    sd_cw_card_reset();
    test_free_keying();
    test_echo();
    test_echo_details();
    test_spacing();
    test_late_service();
    test_content();
    test_progress();
    test_course();
    test_listen_and_errors();
    if (fails) {
        fprintf(stderr, "Morse trainer: %d failure(s)\n", fails);
        return 1;
    }
    puts("Morse trainer: decoding in time order, echo, retries, a learner's spacing, a late main loop, content, progress, the course, listen, "
         "errors: PASS");
    return 0;
}

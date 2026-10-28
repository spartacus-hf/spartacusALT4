/* The trainer's Morse player (Core/Src/mod_cw_player.c) and the shared
 * Morse table (Core/Src/mod_morse_tables.c): what a learner hears must be
 * exact. Marks and gaps are measured at the envelope's half-way crossings,
 * as an ear (or a decoder) times them; Farnsworth spacing follows the ARRL
 * formula; a stop ramps down instead of clicking; the table round-trips
 * every symbol, prosigns included. */
#include "mod_cw_player.h"
#include "mod_morse_tables.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LUT_N 512U
static uint16_t lut[LUT_N + 1U];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++fails; } } while (0)

typedef struct { uint32_t on, off; } edge_t;
static edge_t marks[256];
static unsigned nmarks;
static float peak;

/* Play to the end, timing marks between 0.5 crossings. */
static uint32_t play(float level)
{
    uint32_t t = 0U;
    float prev = 0.0f;
    const float half = 0.5f * level;
    nmarks = 0U;
    peak = 0.0f;
    while (CWP_Busy() && t < 48000U * 120U) {
        const float e = CWP_Step();
        if (e > peak) peak = e;
        if (prev < half && e >= half && nmarks < 256U) marks[nmarks++].on = t;
        if (prev >= half && e < half && nmarks) marks[nmarks - 1U].off = t;
        prev = e;
        ++t;
    }
    return t;
}

static void test_table(void)
{
    static const char *const syms = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:-/=?@+";
    static const char *const pat[] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
        "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
        "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
        ".-.-.-", "--..--", "---...", "-....-", "-..-.", "-...-", "..--..", ".--.-.", ".-.-."};
    char buf[8];
    for (unsigned i = 0U; syms[i]; ++i) {
        CHECK(strcmp(MORSE_Pattern((uint8_t)syms[i], buf), pat[i]) == 0, "%c is %s, want %s", syms[i], buf, pat[i]);
        CHECK(MORSE_Symbol(MORSE_Code((uint8_t)syms[i])) == (uint8_t)syms[i], "%c does not round-trip", syms[i]);
    }
    static const char *const pro[] = {".-...", "-.-.-", "-.--.", "...-.-", "...-.", "-...-.-"};
    static const char *const lab[] = {"<AS>", "<KA>", "<KN>", "<SK>", "<VE>", "<BK>"};
    char l[5];
    for (uint8_t i = 0U; i < 6U; ++i) {
        const uint8_t s = (uint8_t)(MORSE_AS + i);
        CHECK(strcmp(MORSE_Pattern(s, buf), pro[i]) == 0, "prosign %u is %s", i, buf);
        CHECK(strcmp(MORSE_Label(s, l), lab[i]) == 0, "prosign %u labelled %s", i, l);
        CHECK(MORSE_Symbol(MORSE_Code(s)) == s, "prosign %u does not round-trip", i);
    }
    CHECK(MORSE_Code('k') == MORSE_Code('K'), "lower case");
    CHECK(MORSE_Code('#') == 0U, "no code for #");
    CHECK(MTRAIN_DecodeSymbol(0x02U, 3U) == 'R', "decoder view R");
    CHECK(MTRAIN_DecodeSymbol(0x05U, 6U) == '?', "decoder view of SK is '?'");
    CHECK(MTRAIN_DecodeSymbol(0x38U, 6U) == ':', "decoder view of ':'");
}

static void test_timing(void)
{
    cwp_prog_t p;
    for (float wpm = 10.0f; wpm <= 40.0f; wpm += 5.0f) {
        memset(&p, 0, sizeof(p));
        CWP_Timing(&p, wpm, wpm, 0U, 0U, 240U);
        CWP_SetText(&p, (const uint8_t *)"PARIS PARIS", 11U);
        CHECK(p.len == 11U, "PARIS PARIS took %u codes", p.len);
        CHECK(CWP_Start(&p), "start");
        const uint32_t total = play(1.0f);
        /* P A R I S = 14 marks a word. */
        CHECK(nmarks == 28U, "%g WPM: %u marks", (double)wpm, nmarks);
        const uint32_t u = p.dit;
        for (unsigned i = 0U; i < nmarks; ++i) {
            const uint32_t len = marks[i].off - marks[i].on;
            /* One sample either way: where a ramp crosses half. */
            CHECK((len + 1U >= u && len <= u + 1U) || (len + 1U >= 3U * u && len <= 3U * u + 1U),
                  "%g WPM mark %u: %u samples (dit %u)", (double)wpm, i, len, u);
        }
        /* Gap after the S of the first word: 7 units; inside letters 1 unit. */
#define NEAR(a, b) ((a) + 1U >= (b) && (a) <= (b) + 1U)
        CHECK(NEAR(marks[14].on - marks[13].off, 7U * u), "%g WPM word gap %u, want %u", (double)wpm,
              marks[14].on - marks[13].off, 7U * u);
        CHECK(NEAR(marks[1].on - marks[0].off, u), "%g WPM element gap", (double)wpm);
        CHECK(NEAR(marks[4].on - marks[3].off, 3U * u), "%g WPM character gap %u", (double)wpm,
              marks[4].on - marks[3].off);
        CHECK(total + 2U >= CWP_Duration(&p) && total <= CWP_Duration(&p) + 2U,
              "%g WPM: played %u samples, duration says %u", (double)wpm, total, CWP_Duration(&p));
    }
    /* Farnsworth 20/10: characters at 20, gaps per ARRL. */
    memset(&p, 0, sizeof(p));
    CWP_Timing(&p, 20.0f, 10.0f, 0U, 0U, 240U);
    const double ta = (60.0 * 20.0 - 37.2 * 10.0) / (10.0 * 20.0);
    CHECK(fabs((double)p.char_gap - 48000.0 * 3.0 * ta / 19.0) < 1.0 &&
          fabs((double)p.word_gap - 48000.0 * 7.0 * ta / 19.0) < 1.0,
          "Farnsworth gaps %u %u", p.char_gap, p.word_gap);
    CHECK(p.dit == 2880U, "20 WPM dit %u", p.dit);
    /* Manual spacing in dits. */
    memset(&p, 0, sizeof(p));
    CWP_Timing(&p, 20.0f, 10.0f, 5U, 14U, 240U);
    CHECK(p.char_gap == 5U * 2880U && p.word_gap == 14U * 2880U, "manual spacing %u %u", p.char_gap, p.word_gap);
}

static void test_stop_and_level(void)
{
    cwp_prog_t p;
    memset(&p, 0, sizeof(p));
    p.level = 128U;
    CWP_Timing(&p, 20.0f, 20.0f, 0U, 0U, 240U);
    CWP_SetText(&p, (const uint8_t *)"  T T  ", 7U);
    CHECK(p.len == 3U, "spaces collapsed: %u", p.len);
    CHECK(CWP_Start(&p) && !CWP_Start(&p), "a second start while busy is refused");
    /* Stop in the middle of the first dah: the envelope ramps, no step. */
    float prev = 0.0f, maxstep = 0.0f, e;
    for (uint32_t t = 0U; t < 48U + 4000U; ++t) e = CWP_Step(), prev = e;
    CWP_Stop();
    uint32_t n = 0U;
    while (CWP_Busy() && n < 48000U) {
        e = CWP_Step();
        if (fabsf(e - prev) > maxstep) maxstep = fabsf(e - prev);
        prev = e;
        ++n;
    }
    CHECK(!CWP_Busy() && n <= 241U, "stop took %u samples", n);
    CHECK(maxstep < 0.03f, "stop stepped by %.3f", (double)maxstep);
    /* Level: half scale. */
    CWP_Start(&p);
    play(128.0f / 255.0f);
    CHECK(peak > 0.49f && peak < 0.51f, "level 128 peaks at %.3f", (double)peak);
    /* Nothing to play. */
    memset(&p, 0, sizeof(p));
    CHECK(!CWP_Start(&p), "an empty program does not start");
}

int main(void)
{
    for (unsigned i = 0U; i <= LUT_N; ++i) {
        const double x = (double)i / LUT_N;
        lut[i] = (uint16_t)(65535.0 * (0.5 - 0.5 * cos(3.14159265358979323846 * x)) + 0.5);
    }
    CWP_Init(lut, LUT_N);
    test_table();
    test_timing();
    test_stop_and_level();
    if (fails) {
        fprintf(stderr, "CW player: %d failure(s)\n", fails);
        return 1;
    }
    puts("CW player and Morse table: timing, Farnsworth, stop, prosigns: PASS");
    return 0;
}

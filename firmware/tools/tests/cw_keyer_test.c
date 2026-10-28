/* The production CW keyer (Core/Src/mod_cw_keyer.c) driven sample by
 * sample, with the 1 ms sampler between a scripted paddle and the keyer as
 * on the radio (SysTick samples, the renderer steps at 48 kHz).
 *
 * Pins what the keyer must do for a paddle newcomer and an experienced
 * operator alike: exact element timing at every speed and weight, Iambic A
 * and B squeeze releases on both sides of the Curtis point, taps made in a
 * gap or during auto space kept, a slow release adding nothing, Ultimatic's
 * closure memories and chatter guard, non-squeeze, and bounce on either
 * edge. */
#include "mod_cw_keyer.h"
#include <stdio.h>
#include <string.h>

static cwk_timing_t T;
static cwk_state_t S;
static cwk_sampler_t P;
static cwk_reader_t R;
static uint32_t sample, word;
static uint8_t raw, debounce = 3U;

typedef struct { uint8_t e; uint32_t start, len; } mark_t;
static mark_t marks[512];
static unsigned nmarks;
static uint32_t cur_start;
static uint8_t cur_e, key_was;
static unsigned char_ends;
static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++fails; } } while (0)

static void reset(float wpm, float weight, float cdit, float cdah, uint8_t lat, uint8_t acs, uint8_t mode)
{
    CWK_TimingInit(&T, wpm, weight, cdit, cdah, lat, acs, mode);
    memset(&P, 0, sizeof(P));
    memset(&R, 0, sizeof(R));
    CWK_Reset(&S);
    sample = 0U;
    raw = 0U;
    word = 0U;
    nmarks = 0U;
    key_was = 0U;
    char_ends = 0U;
}

/* A message the keyer sends by itself (radio memories), when q_on. */
static cwk_seq_t Q;
static uint8_t q_on;

static void step(void)
{
    if (sample % CWK_SAMPLES_PER_MS == 0U) word = CWK_SamplerTick(&P, raw, debounce);
    cwk_input_t in;
    CWK_ReadInput(&R, word, &in);
    if (q_on) in.level = CWK_SeqLevel(&Q);
    const uint8_t ev = CWK_Step(&S, &T, &in);
    if (q_on && !CWK_SeqAfter(&Q, &T, ev)) q_on = 0U;
    if (ev & CWK_EV_START) {
        cur_start = sample;
        cur_e = (ev & CWK_EV_DAH) ? CWK_DAH : CWK_DIT;
    }
    if ((ev & CWK_EV_KEY) && !key_was && !(ev & CWK_EV_START)) {
        fprintf(stderr, "key on without a start at %u\n", (unsigned)sample);
        ++fails;
    }
    if ((ev & CWK_EV_END) && nmarks < 512U) {
        marks[nmarks].e = cur_e;
        marks[nmarks].start = cur_start;
        marks[nmarks].len = sample - cur_start + 1U;
        ++nmarks;
    }
    if (ev & CWK_EV_CHAR_END) ++char_ends;
    key_was = (uint8_t)((ev & CWK_EV_KEY) && !(ev & CWK_EV_END));
    ++sample;
}

static void run(uint32_t n) { while (n--) step(); }
static void run_ms(float ms) { run((uint32_t)(ms * 48.0f + 0.5f)); }
/* Until the keyer has nothing in progress (or a limit). */
static void settle(void)
{
    for (uint32_t i = 0U; i < 48000U * 10U; ++i) {
        step();
        if (S.phase == CWK_IDLE && raw == 0U && !CWK_Pending(&R, word) && P.stable == 0U) return;
    }
}

static const char *seq(void)
{
    static char out[520];
    unsigned i;
    for (i = 0U; i < nmarks && i < 511U; ++i) out[i] = (marks[i].e == CWK_DAH) ? '-' : '.';
    out[i] = '\0';
    return out;
}

/* Samples into the current mark (or the gap after it) of mark n. */
static void run_until_mark(unsigned n)
{
    while (nmarks + (S.phase == CWK_MARK ? 1U : 0U) <= n) {
        step();
        if (sample > 48000U * 60U) break;
    }
}

static void wait_into_mark(unsigned n, float fraction)
{
    run_until_mark(n);
    const uint32_t len = T.mark[S.elem];
    while (S.phase == CWK_MARK && S.elapsed < (uint32_t)((float)len * fraction)) step();
}

static void wait_into_gap(unsigned n, float fraction)
{
    while (nmarks <= n) step();
    while (S.phase == CWK_GAP && S.elapsed < (uint32_t)((float)T.gap * fraction)) step();
}

/* ---- timing ---------------------------------------------------------- */

static void test_timing(void)
{
    static const float speeds[] = {5, 8, 10, 12, 15, 18, 20, 25, 30, 35, 40, 50, 60};
    for (unsigned si = 0U; si < sizeof(speeds) / sizeof(speeds[0]); ++si) {
        for (float w = 35.0f; w <= 65.0f; w += 5.0f) {
            for (uint8_t lever = CWK_DIT; lever <= CWK_DAH; ++lever) {
                reset(speeds[si], w, 75.0f, 45.0f, 0U, 0U, CWK_IAMBIC_B);
                CHECK(T.mark[CWK_DIT] + T.gap == 2U * T.unit && T.mark[CWK_DAH] + T.gap == 4U * T.unit,
                      "%g WPM weight %g: dit+gap %u dah+gap %u unit %u", (double)speeds[si], (double)w,
                      (unsigned)(T.mark[CWK_DIT] + T.gap), (unsigned)(T.mark[CWK_DAH] + T.gap), (unsigned)T.unit);
                raw = lever;
                run_until_mark(6U);
                raw = 0U;
                settle();
                CHECK(nmarks >= 6U, "%g WPM: %u marks held", (double)speeds[si], nmarks);
                for (unsigned i = 0U; i < nmarks; ++i) {
                    CHECK(marks[i].e == lever && marks[i].len == T.mark[lever],
                          "%g WPM weight %g lever %u mark %u: %u samples, want %u", (double)speeds[si], (double)w,
                          lever, i, (unsigned)marks[i].len, (unsigned)T.mark[lever]);
                    if (i) CHECK(marks[i].start - (marks[i - 1U].start + marks[i - 1U].len) == T.gap,
                                 "%g WPM weight %g gap %u: %u samples, want %u", (double)speeds[si], (double)w, i,
                                 (unsigned)(marks[i].start - marks[i - 1U].start - marks[i - 1U].len), (unsigned)T.gap);
                }
            }
        }
    }
    /* 60 % weight: the dah and its gap still make 4 units (was 4.4). */
    reset(20.0f, 60.0f, 75.0f, 45.0f, 0U, 0U, CWK_IAMBIC_B);
    CHECK(T.mark[CWK_DAH] == 3U * T.unit + T.unit / 5U && T.gap == T.unit - T.unit / 5U,
          "weight 60: dah %u gap %u unit %u", (unsigned)T.mark[CWK_DAH], (unsigned)T.gap, (unsigned)T.unit);
}

/* ---- squeezes ------------------------------------------------------------ */

/* Squeeze from rest (DIT first), let go of both `fraction` into mark n. */
static const char *squeeze_release(uint8_t mode, float cdit, float cdah, unsigned n, float fraction)
{
    reset(20.0f, 50.0f, cdit, cdah, 0U, 0U, mode);
    raw = 1U;
    run_ms(2.0f);
    raw = 3U;
    wait_into_mark(n, fraction);
    raw = 0U;
    settle();
    return seq();
}

static void test_squeeze(void)
{
    const char *s;
    s = squeeze_release(CWK_IAMBIC_A, 75, 45, 3U, 0.5f);
    CHECK(strcmp(s, ".-.-") == 0, "iambic A release in the 4th mark: %s", s);
    s = squeeze_release(CWK_IAMBIC_A, 75, 45, 1U, 0.95f);
    CHECK(strcmp(s, ".-") == 0, "iambic A release late in the dah: %s", s);
    /* B, 45 % dah: before the point stops like A, after it adds the dit. */
    s = squeeze_release(CWK_IAMBIC_B, 75, 45, 1U, 0.30f);
    CHECK(strcmp(s, ".-") == 0, "iambic B 45%% released at 30%% of a dah: %s", s);
    s = squeeze_release(CWK_IAMBIC_B, 75, 45, 1U, 0.60f);
    CHECK(strcmp(s, ".-.") == 0, "iambic B 45%% released at 60%% of a dah: %s", s);
    s = squeeze_release(CWK_IAMBIC_B, 75, 45, 2U, 0.50f);
    CHECK(strcmp(s, ".-.") == 0, "iambic B 75%% released at 50%% of a dit: %s", s);
    s = squeeze_release(CWK_IAMBIC_B, 75, 45, 2U, 0.90f);
    CHECK(strcmp(s, ".-.-") == 0, "iambic B 75%% released at 90%% of a dit: %s", s);
    /* Classic B (0/0): any release during a mark adds one. */
    s = squeeze_release(CWK_IAMBIC_B, 0, 0, 1U, 0.10f);
    CHECK(strcmp(s, ".-.") == 0, "classic B released at 10%% of a dah: %s", s);
    /* 100 %: as A. */
    s = squeeze_release(CWK_IAMBIC_B, 100, 100, 1U, 0.97f);
    CHECK(strcmp(s, ".-") == 0, "B at 100%% released at 97%% of a dah: %s", s);
    /* The squeeze-release path obeys the Curtis point too (the old keyer
     * queued the opposite element on any release). */
    s = squeeze_release(CWK_IAMBIC_B, 75, 45, 0U, 0.40f);
    CHECK(strcmp(s, ".") == 0, "B released at 40%% of the first dit: %s", s);

    /* Both closed in the same ms from rest: DIT first. */
    reset(20.0f, 50.0f, 75, 45, 0U, 0U, CWK_IAMBIC_A);
    raw = 3U;
    run_until_mark(4U);
    raw = 0U;
    settle();
    CHECK(strncmp(seq(), ".-.-", 4) == 0, "simultaneous squeeze: %s", seq());
    /* DAH first by a few ms. */
    reset(20.0f, 50.0f, 75, 45, 0U, 0U, CWK_IAMBIC_A);
    raw = 2U;
    run_ms(4.0f);
    raw = 3U;
    run_until_mark(3U);
    raw = 0U;
    settle();
    CHECK(strncmp(seq(), "-.-", 3) == 0, "dah-first squeeze: %s", seq());
}

/* ---- taps, releases, latency --------------------------------------------- */

static void test_taps(void)
{
    /* Holding DIT, a DAH tap inside a gap is sent (the old keyer looked only
     * at the levers at the end of the gap and lost it). */
    for (uint8_t mode = CWK_IAMBIC_A; mode <= CWK_IAMBIC_B; ++mode) {
        reset(15.0f, 50.0f, 75, 45, 0U, 0U, mode);
        raw = 1U;
        wait_into_gap(0U, 0.2f);
        raw = 3U;
        run_ms(8.0f);
        raw = 1U;
        run_until_mark(4U);
        raw = 0U;
        settle();
        CHECK(strncmp(seq(), ".-..", 4) == 0, "mode %u: dah tap in a gap: %s", mode, seq());
    }
    /* A tap in the gap after a single dit, released before the gap ends. */
    reset(15.0f, 50.0f, 75, 45, 0U, 0U, CWK_IAMBIC_A);
    raw = 1U;
    run_ms(20.0f);
    raw = 0U;
    wait_into_gap(0U, 0.3f);
    raw = 2U;
    run_ms(10.0f);
    raw = 0U;
    settle();
    CHECK(strcmp(seq(), ".-") == 0, "dah tapped in the gap after a dit: %s", seq());

    /* The same lever re-tapped in the gap: kept with latency 0... */
    reset(15.0f, 50.0f, 75, 45, 0U, 0U, CWK_IAMBIC_B);
    raw = 1U;
    run_ms(20.0f);
    raw = 0U;
    wait_into_gap(0U, 0.4f);
    raw = 1U;
    run_ms(10.0f);
    raw = 0U;
    settle();
    CHECK(strcmp(seq(), "..") == 0, "dit re-tapped in the gap, latency 0: %s", seq());
    /* ...ignored inside a full-dit latency window. */
    reset(15.0f, 50.0f, 75, 45, 8U, 0U, CWK_IAMBIC_B);
    raw = 1U;
    run_ms(20.0f);
    raw = 0U;
    wait_into_gap(0U, 0.4f);
    raw = 1U;
    run_ms(10.0f);
    raw = 0U;
    settle();
    CHECK(strcmp(seq(), ".") == 0, "dit re-tapped inside the latency window: %s", seq());

    /* A slow release -- the lever still down 60 % into the gap -- adds no
     * element, at any latency. */
    for (uint8_t lat = 0U; lat <= 8U; lat = (uint8_t)(lat + 4U)) {
        reset(15.0f, 50.0f, 75, 45, lat, 0U, CWK_IAMBIC_B);
        raw = 1U;
        wait_into_gap(0U, 0.6f);
        raw = 0U;
        settle();
        CHECK(strcmp(seq(), ".") == 0, "slow release, latency %u: %s", lat, seq());
    }
    /* A short tap while idle is never lost, however short (one debounce). */
    reset(25.0f, 50.0f, 75, 45, 0U, 0U, CWK_IAMBIC_B);
    raw = 2U;
    run_ms(4.0f);
    raw = 0U;
    settle();
    CHECK(strcmp(seq(), "-") == 0, "4 ms dah tap: %s", seq());
}

/* ---- Ultimatic, non-squeeze ------------------------------------------------- */

static void test_ultimatic(void)
{
    /* DIT, then DAH held as well: the later lever takes over. */
    reset(20.0f, 50.0f, 75, 45, 0U, 0U, CWK_ULTIMATIC);
    raw = 1U;
    run_ms(20.0f);
    raw = 3U;
    run_until_mark(4U);
    /* A flick of DIT (open 15 ms, closed again) hands control back to it. */
    raw = 2U;
    run_ms(15.0f);
    raw = 3U;
    run_until_mark(9U);
    raw = 0U;
    settle();
    const char *s = seq();
    CHECK(strncmp(s, ".---", 4) == 0, "ultimatic dit then dah held: %s", s);
    CHECK(strchr(s + 4, '.') != NULL && s[strlen(s) - 1U] == '.', "ultimatic flick back to dit: %s", s);
    /* Holding DAH, a DIT tap inside a dah mark is guaranteed its dit. */
    reset(20.0f, 50.0f, 75, 45, 0U, 0U, CWK_ULTIMATIC);
    raw = 2U;
    wait_into_mark(1U, 0.3f);
    raw = 3U;
    run_ms(6.0f);
    raw = 2U;
    run_until_mark(4U);
    raw = 0U;
    settle();
    CHECK(strncmp(seq(), "--.-", 4) == 0, "ultimatic dit tap inside a dah: %s", seq());
    /* Chatter on the sending lever's release is no closure. */
    debounce = 1U;
    reset(15.0f, 50.0f, 75, 45, 0U, 0U, CWK_ULTIMATIC);
    raw = 1U;
    run_ms(30.0f);
    raw = 0U;
    run_ms(2.0f);
    raw = 1U;
    run_ms(1.0f);
    raw = 0U;
    settle();
    CHECK(strcmp(seq(), ".") == 0, "ultimatic release chatter: %s", seq());
    debounce = 3U;
}

static void test_nonsqueeze(void)
{
    reset(20.0f, 50.0f, 75, 45, 0U, 0U, CWK_NONSQUEEZE);
    raw = 1U;
    run_ms(20.0f);
    raw = 3U;
    run_until_mark(5U);
    raw = 2U;
    run_until_mark(8U);
    raw = 0U;
    settle();
    const char *s = seq();
    CHECK(strncmp(s, ".....", 5) == 0, "non-squeeze keeps the first lever: %s", s);
    CHECK(s[strlen(s) - 1U] == '-', "non-squeeze hands over on release: %s", s);
    /* A DAH touched while DIT is held is no DAH, even after both let go. */
    reset(20.0f, 50.0f, 75, 45, 0U, 0U, CWK_NONSQUEEZE);
    raw = 1U;
    run_ms(10.0f);
    raw = 3U;
    run_ms(10.0f);
    raw = 1U;
    run_ms(10.0f);
    raw = 0U;
    settle();
    CHECK(strcmp(seq(), ".") == 0, "non-squeeze ignores a squeezed DAH: %s", seq());
}

/* ---- auto space, bounce -------------------------------------------------- */

static void test_acs(void)
{
    for (float w = 40.0f; w <= 60.0f; w += 10.0f) {
        reset(15.0f, w, 75, 45, 0U, 3U, CWK_IAMBIC_B);
        raw = 1U;
        run_ms(20.0f);
        raw = 0U;
        /* Press again 1.5 dits after key-up: inside the character space. */
        while (nmarks < 1U) step();
        run((uint32_t)(T.unit * 3U / 2U));
        raw = 1U;
        run_ms(5.0f);
        raw = 0U; /* released long before the space ends: still kept */
        settle();
        CHECK(nmarks == 2U, "ACS weight %g: %u marks", (double)w, nmarks);
        if (nmarks == 2U) {
            const uint32_t space = marks[1].start - (marks[0].start + marks[0].len);
            const int32_t delta = (int32_t)T.mark[CWK_DIT] - (int32_t)T.unit;
            CHECK((int32_t)space == 3 * (int32_t)T.unit - delta, "ACS weight %g: space %u, want %d",
                  (double)w, (unsigned)space, 3 * (int32_t)T.unit - delta);
        }
    }
}

static void test_bounce(void)
{
    /* Bounce on the press (raw 1,0,1 in 2 ms) and on the release. */
    reset(15.0f, 50.0f, 75, 45, 0U, 0U, CWK_IAMBIC_B);
    raw = 1U; run(30U); raw = 0U; run(30U); raw = 1U; run(20U); raw = 0U; run(10U); raw = 1U;
    run_ms(30.0f);
    raw = 0U; run(40U); raw = 1U; run(40U); raw = 0U; run(30U); raw = 1U; run(20U); raw = 0U;
    settle();
    CHECK(strcmp(seq(), ".") == 0, "press and release bounce: %s", seq());
    CHECK(P.closures[0] == 1U, "bounce counted %u closures", P.closures[0]);
}

/* A message: PARIS PARIS in every mode, at weights 35-65 and 5-60 WPM, with
 * and without ACS -- marks as the weight makes them, the element gap
 * inside a character, gap + 2 dits between characters (or ACS's longer
 * space), gap + 6 between words: 50 dits a PARIS. */
static void test_message(void)
{
    /* P .--.  A .-  R .-.  I ..  S ...  (mod_morse_tables.h codes) */
    static const uint8_t paris[] = {22U, 5U, 10U, 4U, 8U, 0U, 22U, 5U, 10U, 4U, 8U};
    static const char want[] = ".--..-.-.......--..-.-......";
    static const uint8_t elems[] = {4U, 2U, 3U, 2U, 3U};
    static const uint8_t modes[] = {CWK_STRAIGHT, CWK_IAMBIC_A, CWK_IAMBIC_B, CWK_ULTIMATIC, CWK_NONSQUEEZE};
    static const float speeds[] = {5.0f, 20.0f, 60.0f}, weights[] = {35.0f, 50.0f, 65.0f};
    static const uint8_t acs[] = {0U, 3U, 4U};
    unsigned runs = 0U;
    for (unsigned mi = 0U; mi < 5U; ++mi)
        for (unsigned si = 0U; si < 3U; ++si)
            for (unsigned wi = 0U; wi < 3U; ++wi)
                for (unsigned ai = 0U; ai < 3U; ++ai) {
                    reset(speeds[si], weights[wi], 75.0f, 45.0f, 0U, acs[ai], modes[mi]);
                    q_on = CWK_SeqStart(&Q, paris, (uint8_t)sizeof(paris));
                    for (uint32_t i = 0U; i < 48000U * 60U && (q_on || S.phase != CWK_IDLE); ++i) step();
                    ++runs;
                    CHECK(!q_on && strcmp(seq(), want) == 0, "mode %u %g WPM w%g acs %u: \"%s\"", modes[mi],
                          (double)speeds[si], (double)weights[wi], acs[ai], seq());
                    if (strcmp(seq(), want) != 0) continue;
                    const uint32_t cs = T.gap + (acs[ai] > 3U ? 3U : 2U) * T.unit, ws = T.gap + 6U * T.unit;
                    unsigned k = 0U;
                    for (unsigned w = 0U; w < 2U; ++w)
                        for (unsigned c = 0U; c < 5U; ++c)
                            for (unsigned e = 0U; e < elems[c]; ++e, ++k) {
                                CHECK(marks[k].len == T.mark[marks[k].e], "mark %u: %u samples", k, (unsigned)marks[k].len);
                                if (!k) continue;
                                const uint32_t space = marks[k].start - (marks[k - 1U].start + marks[k - 1U].len);
                                const uint32_t expect = e ? T.gap : (c ? cs : ws);
                                CHECK(space == expect, "mode %u %g WPM w%g acs %u: space before mark %u %u, not %u",
                                      modes[mi], (double)speeds[si], (double)weights[wi], acs[ai], k,
                                      (unsigned)space, (unsigned)expect);
                            }
                    if (acs[ai] <= 3U)
                        CHECK(marks[14].start - marks[0].start == 50U * T.unit, "PARIS took %u samples, not %u",
                              (unsigned)(marks[14].start - marks[0].start), (unsigned)(50U * T.unit));
                }
    CHECK(runs == 135U, "runs");
    /* Nothing to send: nothing starts. */
    static const uint8_t blank[] = {0U, 0U};
    CHECK(!CWK_SeqStart(&Q, blank, 2U), "a blank message started");
}

int main(void)
{
    test_timing();
    test_squeeze();
    test_taps();
    test_ultimatic();
    test_nonsqueeze();
    test_acs();
    test_bounce();
    test_message();
    if (fails) {
        fprintf(stderr, "CW keyer: %d failure(s)\n", fails);
        return 1;
    }
    puts("CW keyer: timing, squeezes, taps, latency, Ultimatic, non-squeeze, ACS, bounce, messages: PASS");
    return 0;
}

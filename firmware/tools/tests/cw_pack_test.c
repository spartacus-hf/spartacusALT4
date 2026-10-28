/* The CW trainer's pack reader (Core/Src/cw_pack.c) against the tree's
 * sd/CWPACK.BIN: the courses are Morserino-32's, every word's mask agrees
 * with its text, the scan finds exactly the words a lesson allows; and a
 * missing, short, damaged or foreign pack is refused, a card that only may
 * not be read just now is not mistaken for one, a record changed after the
 * check is still caught, and CW/CUSTOM.TXT is read as the operator wrote it. */
#include "cw_pack.h"
#include "mod_morse_tables.h"
#include "sd_cw_card.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++fails; } } while (0)

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Section `kind` of the file: where it starts and its records. */
static uint32_t section(uint8_t kind, uint32_t *count)
{
    const uint8_t *b = sd_cw_card.pack;
    for (uint32_t i = 0U; i < 7U; ++i) {
        const uint8_t *e = &b[12U + 8U * i];
        if (e[0] == kind) {
            *count = (uint32_t)(e[2] | (e[3] << 8));
            return le32(&e[4]);
        }
    }
    *count = 0U;
    return 0U;
}

static void reopen(void)
{
    CWPACK_Close();
    sd_cw_card_reset();
}

static void test_courses(void)
{
    static const char *const start[4] = {"MKRSUAPTLOWI", "KMURESNAPTLW", "TEANOIS14RHD", "REATINPGSLCD"};
    /* Where the lessons end: Koch two then one more, CW Academy its
     * Beginner sessions, LICW three at a time; every course ends whole. */
    static const uint64_t koch = ~1ULL & ((1ULL << MORSE_SYMBOLS) - 1U);
    static const uint8_t cwa[] = {4, 9, 15, 17, 22, 25, 31, 34, 39, 44, 45};
    uint8_t c[MORSE_SYMBOLS];
    uint64_t stops[4] = {0U};
    uint8_t st[7];
    CHECK(CWPACK_Open(), "the tree's pack opens");
    CHECK(CWPACK_CardState() == 2U, "card state ok");
    for (uint8_t k = 0U; k < 4U; ++k) {
        CHECK(CWPACK_Course(k, c, st) == MORSE_SYMBOLS, "course %u is the whole alphabet", k);
        memcpy(&stops[k], st, 7U);
        CHECK(memcmp(c, start[k], 12U) == 0, "course %u starts %.12s", k, start[k]);
        CHECK(stops[k] >> (MORSE_SYMBOLS - 1U) == 1U, "course %u: the last lesson is all of it", k);
    }
    CHECK(stops[0] == koch && stops[1] == koch, "Koch: two characters, then one at a time");
    uint64_t want = 0U;
    for (uint32_t i = 0U; i < sizeof(cwa); ++i) want |= 1ULL << (cwa[i] - 1U);
    CHECK((stops[2] & ((1ULL << 45) - 1U)) == want, "CW Academy: its sessions' groups");
    CHECK((stops[3] & 0x3FFFFU) == 0x24924U && ((stops[3] >> 40) & 0xFU) == 0xAU, "LICW: three a lesson to 42, then 44");
    /* M32 ends ... x - = <sk> + <as> <kn> <ka> <ve> <bk> @ : */
    CWPACK_Course(0U, c, st);
    static const uint8_t tail[] = {'X', '-', '=', MORSE_SK, '+', MORSE_AS, MORSE_KN, MORSE_KA, MORSE_VE, MORSE_BK, '@', ':'};
    CHECK(memcmp(&c[MORSE_SYMBOLS - sizeof(tail)], tail, sizeof(tail)) == 0, "M32's prosigns where Morserino has them");
    CHECK(CWPACK_Course(4U, c, st) == 0U, "no fifth course in the pack");
}

/* Every record's mask is its text's, and the scan agrees with a brute-force
 * filter for several lessons of every course. */
static void test_words(void)
{
    for (int l = 0; l < 2; ++l) {
        uint32_t n = 0U, nt = 0U;
        const uint32_t m_at = section(l ? 4U : 2U, &n), t_at = section(l ? 5U : 3U, &nt);
        CHECK(n == nt && n > 0U, "list %d: %u masks, %u texts", l, n, nt);
        for (uint32_t i = 0U; i < n; ++i) {
            const uint8_t *t = &sd_cw_card.pack[t_at + 12U * i];
            uint64_t m = 0U;
            uint32_t len = 0U;
            while (len < 12U && t[len]) m |= 1ULL << MORSE_Index(t[len++]);
            m |= (uint64_t)len << 56;
            const uint64_t stored = (uint64_t)le32(&sd_cw_card.pack[m_at + 8U * i]) |
                                    ((uint64_t)le32(&sd_cw_card.pack[m_at + 8U * i + 4U]) << 32);
            CHECK(stored == m, "list %d record %u: mask differs from its text", l, i);
        }
        uint8_t bitmap[CWPACK_WORDS_MAX / 8U], c[MORSE_SYMBOLS];
        for (uint8_t course = 0U; course < 4U; ++course) {
            uint8_t st[7];
            CWPACK_Course(course, c, st);
            for (uint8_t lesson = 4U; lesson <= MORSE_SYMBOLS; lesson += 9U) {
                uint64_t allowed = 0U;
                for (uint8_t k = 0U; k < lesson; ++k) allowed |= 1ULL << MORSE_Index(c[k]);
                const int32_t got = CWPACK_Scan(l ? CWPACK_ABBR : CWPACK_WORDS, allowed, 12U, bitmap);
                int32_t want = 0;
                for (uint32_t i = 0U; i < n; ++i) {
                    const uint8_t *t = &sd_cw_card.pack[t_at + 12U * i];
                    uint8_t ok = 1U;
                    for (uint32_t k = 0U; k < 12U && t[k]; ++k) ok &= (uint8_t)((allowed >> MORSE_Index(t[k])) & 1U);
                    want += ok;
                    CHECK(((bitmap[i >> 3] >> (i & 7U)) & 1U) == ok, "list %d course %u lesson %u: record %u", l, course,
                          lesson, i);
                }
                CHECK(got == want, "list %d course %u lesson %u: scan %d, filter %d", l, course, lesson, got, want);
            }
        }
    }
    /* The early lessons have words to practise: M32 lesson 10. */
    uint8_t bitmap[CWPACK_WORDS_MAX / 8U], c[MORSE_SYMBOLS];
    uint64_t allowed = 0U;
    uint8_t st[7];
    CWPACK_Course(0U, c, st);
    for (uint8_t k = 0U; k < 10U; ++k) allowed |= 1ULL << MORSE_Index(c[k]);
    CHECK(CWPACK_Scan(CWPACK_WORDS, allowed, 12U, bitmap) >= 40, "M32 lesson 10 has words");
    CHECK(CWPACK_Scan(CWPACK_WORDS, allowed, 2U, bitmap) >= 1, "length limit");
    /* The abbreviations' bitmap is theirs only: nothing after it is touched
     * (the trainer keeps 32 bytes for it, and its state behind them). */
    uint8_t abbr[CWPACK_ABBR_MAX / 8U + 8U];
    memset(abbr, 0xA5, sizeof(abbr));
    CHECK(CWPACK_Scan(CWPACK_ABBR, ~0ULL, 12U, abbr) > 200, "every abbreviation in the whole alphabet");
    CHECK(abbr[CWPACK_ABBR_MAX / 8U] == 0xA5U && abbr[sizeof(abbr) - 1U] == 0xA5U, "nothing past the abbreviations' bitmap");
    uint8_t w[CWPACK_TEXT_MAX + 1U];
    CHECK(CWPACK_Text(CWPACK_WORDS, 0U, w) == 3U && strcmp((char *)w, "THE") == 0, "the most frequent word first");
    CHECK(CWPACK_Text(CWPACK_WORDS, CWPACK_WORDS_MAX, w) == 0U, "no record past the end");
}

static void test_prefix_steps(void)
{
    uint8_t p[5];
    unsigned k = 0U, dl = 0U;
    for (uint32_t i = 0U; i < 4000U; ++i) {
        const uint8_t n = CWPACK_Prefix(i * 2654435761UL, p);
        CHECK(n >= 1U && n <= 4U, "prefix %u", i);
        k += (strcmp((char *)p, "K") == 0);
        dl += (strcmp((char *)p, "DL") == 0);
    }
    CHECK(k > 4U && dl > 4U, "common prefixes come up (K %u, DL %u)", k, dl);
    /* The course: the paddle first, a lesson that loops back on itself, and
     * an end that never runs out. */
    uint8_t s[4];
    unsigned n = 0U, loops = 0U;
    CHECK(CWPACK_Step(0U, s) && s[0] == CWPACK_STEP_PADDLE, "the course starts with the paddle");
    while (n < CWPACK_STEPS_MAX && CWPACK_Step((uint8_t)n, s)) {
        loops += (s[0] == CWPACK_STEP_MASTER && s[1] < n);
        ++n;
    }
    CHECK(n > 8U && n < CWPACK_STEPS_MAX && !CWPACK_Step((uint8_t)n, s), "the course's steps (%u)", n);
    CWPACK_Step((uint8_t)(n - 1U), s);
    CHECK(s[0] == CWPACK_STEP_ECHO && s[1] == 0U, "it ends in practice without an end");
    CHECK(loops == 1U, "one lesson loop");
}

static void test_refusals(void)
{
    /* Missing, then no card. */
    reopen();
    sd_cw_card.pack_len = 0U;
    CHECK(!CWPACK_Open() && CWPACK_CardState() == 0U, "a missing pack");
    reopen();
    sd_cw_card.mounted = 0U;
    CHECK(!CWPACK_Open() && CWPACK_CardState() == 0U, "no card");
    /* One byte damaged anywhere after the header. */
    reopen();
    sd_cw_card.pack[sd_cw_card.pack_len / 2U] ^= 0x01U;
    CHECK(!CWPACK_Open() && CWPACK_CardState() == 1U, "a damaged pack");
    reopen();
    sd_cw_card.pack[sd_cw_card.pack_len - 1U] ^= 0x80U;
    CHECK(!CWPACK_Open(), "the last byte damaged");
    /* Short. */
    reopen();
    sd_cw_card.pack_len -= 2U;
    CHECK(!CWPACK_Open() && CWPACK_CardState() == 1U, "a short pack");
    reopen();
    sd_cw_card.pack_len = 40U;
    CHECK(!CWPACK_Open() && CWPACK_CardState() == 1U, "only a header");
    /* Another build's (its id), or not a pack. */
    reopen();
    sd_cw_card.pack[4] ^= 0x01U;
    CHECK(!CWPACK_Open() && CWPACK_CardState() == 1U, "another build's pack");
    reopen();
    memcpy(sd_cw_card.pack, "RHLP", 4U);
    CHECK(!CWPACK_Open(), "HELP.BIN under the pack's name");
    /* A section table pointing out of the file. */
    reopen();
    sd_cw_card.pack[12U + 8U + 7U] = 0x7FU;
    CHECK(!CWPACK_Open(), "a section past the end");
}

static void test_io_refused(void)
{
    uint8_t w[CWPACK_TEXT_MAX + 1U], c[MORSE_SYMBOLS];
    reopen();
    CHECK(CWPACK_Open(), "open");
    const uint32_t hashes = sd_cw_card.hashes;
    sd_cw_card.io = 0U;
    CHECK(!CWPACK_Ready() && CWPACK_Text(CWPACK_WORDS, 1U, w) == 0U, "no read while I/O is refused");
    sd_cw_card.io = 1U;
    CHECK(CWPACK_Text(CWPACK_WORDS, 1U, w) > 0U && sd_cw_card.hashes == hashes,
          "a refusal is not a damaged pack: no second whole-file check");
    /* One read per word once the pack is checked. */
    const uint32_t opens = sd_cw_card.opens;
    CWPACK_Text(CWPACK_WORDS, 7U, w);
    CHECK(sd_cw_card.opens == opens + 1U, "a word costs one read (%u)", sd_cw_card.opens - opens);
    /* A record changed after the check (a card swapped unseen). */
    uint32_t n = 0U;
    const uint32_t t_at = section(3U, &n);
    sd_cw_card.pack[t_at + 12U * 5U] = 'a';
    CHECK(CWPACK_Text(CWPACK_WORDS, 5U, w) == 0U, "a lower-case record is refused");
    sd_cw_card.pack[t_at + 12U * 5U] = '#';
    CHECK(CWPACK_Text(CWPACK_WORDS, 5U, w) == 0U, "an unsendable character is refused");
    sd_cw_card.pack[t_at + 12U * 5U] = MORSE_SK;
    CHECK(CWPACK_Text(CWPACK_WORDS, 5U, w) == 0U, "a prosign id in a word is refused");
    uint32_t nc = 0U;
    const uint32_t c_at = section(1U, &nc);
    uint8_t st[7];
    sd_cw_card.pack[c_at + 2U] = sd_cw_card.pack[c_at + 1U];
    CHECK(CWPACK_Course(0U, c, st) == 0U, "a course with a symbol twice is refused");
    /* The card taken away and back: checked again. */
    reopen();
    CHECK(CWPACK_Open(), "open");
    sd_cw_card.mounted = 0U;
    CHECK(!CWPACK_Open(), "gone");
    sd_cw_card.mounted = 1U;
    const uint32_t h2 = sd_cw_card.hashes;
    CHECK(CWPACK_Open() && sd_cw_card.hashes == h2 + 1U, "a new mount is checked again");
}

static void test_custom(void)
{
    uint8_t c[MORSE_SYMBOLS];
    reopen();
    CHECK(CWPACK_Custom(c) == 0U, "no CW/CUSTOM.TXT");
    sd_cw_card.custom = "kmr <sk> a a # b\r\n<xx><Bt>?";
    const uint8_t n = CWPACK_Custom(c);
    /* <BT> is the = it sounds as, as in a memory's text. */
    static const uint8_t want[] = {'K', 'M', 'R', MORSE_SK, 'A', 'B', 'X', '=', '?'};
    CHECK(n == sizeof(want) && memcmp(c, want, sizeof(want)) == 0, "custom course: %u symbols", n);
    sd_cw_card.custom = "<";
    CHECK(CWPACK_Custom(c) == 0U, "a lone < is nothing");
}

int main(void)
{
    sd_cw_card_reset();
    test_courses();
    test_words();
    test_prefix_steps();
    test_refusals();
    test_io_refused();
    test_custom();
    if (fails) {
        fprintf(stderr, "CW pack: %d failure(s)\n", fails);
        return 1;
    }
    puts("CW pack: courses, word masks and scans, prefixes, steps, refusals, custom course: PASS");
    return 0;
}

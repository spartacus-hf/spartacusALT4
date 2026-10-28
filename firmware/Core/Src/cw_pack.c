#pragma GCC optimize ("Os") /* card reads in the main loop: built for size */

/* The trainer's resource pack (cw_pack.h). The header and section table are
 * checked against what this image expects, then every byte after the header
 * against the id (FNV-1a), once per mount. After that each record is still
 * checked as it is read: a symbol the radio cannot send never reaches the
 * player or the grader. */

#include "cw_pack.h"
#include "mod_morse_tables.h"
#include "mod_sdcard_service.h"
#include <string.h>

#include "cw_pack_id.inc"

#define PACK_FILE    "CWPACK.BIN"
#define CUSTOM_FILE  "CW/CUSTOM.TXT"
#define PACK_HEADER  12U
#define PACK_KINDS   7U
#define PACK_TABLE   (PACK_HEADER + 8U * PACK_KINDS)
#define SYM_BITS     ((1ULL << MORSE_SYMBOLS) - 1ULL)

enum { K_COURSE = 0, K_WMASK, K_WTEXT, K_AMASK, K_ATEXT, K_PREFIX, K_STEPS };

/* Per kind: the record size and the most records this image takes. */
static const uint8_t rec_size[PACK_KINDS] = {64U, 8U, CWPACK_TEXT_MAX, 8U, CWPACK_TEXT_MAX, 4U, 4U};
static const uint16_t rec_max[PACK_KINDS] = {
    CWPACK_COURSES, CWPACK_WORDS_MAX, CWPACK_WORDS_MAX, CWPACK_ABBR_MAX, CWPACK_ABBR_MAX, 4096U, CWPACK_STEPS_MAX};

static uint32_t sect_at[PACK_KINDS];
static uint16_t sect_n[PACK_KINDS];
static uint8_t pack_ok;

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t CWPACK_CardId(void) { return CWPACK_ID; }

void CWPACK_Close(void) { pack_ok = 0U; }

uint8_t CWPACK_Ready(void) { return SD_DiagIoAllowed(); }
uint8_t CWPACK_Mounted(void) { return SD_IsMounted(); }

/* A read failed: the pack is checked again unless the card was only not
 * to be read just now (the radio's realtime work comes first). */
static void read_failed(void)
{
    if (SD_DiagIoAllowed()) pack_ok = 0U;
}

/* Header and section table: this image's pack, every section once, sized
 * and placed inside the file. */
static uint8_t table_ok(const uint8_t *h, uint32_t size)
{
    if (memcmp(h, "CWPK", 4U) != 0 || le32(&h[4]) != CWPACK_ID || le16(&h[8]) != CWPACK_FORMAT ||
        le16(&h[10]) != PACK_KINDS) return 0U;
    for (uint32_t k = 0U; k < PACK_KINDS; ++k) {
        const uint8_t *e = &h[PACK_HEADER + 8U * k];
        const uint32_t at = le32(&e[4]), n = le16(&e[2]);
        if (e[0] != k + 1U || e[1] != rec_size[k] || n == 0U || n > rec_max[k] || at < PACK_TABLE ||
            at > size || n * rec_size[k] > size - at) return 0U;
        sect_at[k] = at;
        sect_n[k] = (uint16_t)n;
    }
    return (uint8_t)(sect_n[K_COURSE] == CWPACK_COURSES && sect_n[K_WMASK] == sect_n[K_WTEXT] &&
                     sect_n[K_AMASK] == sect_n[K_ATEXT]);
}

uint8_t CWPACK_Open(void)
{
    if (!SD_IsMounted()) pack_ok = 0U;
    if (pack_ok) return 1U;
    uint8_t h[PACK_TABLE];
    uint32_t got = 0U, size = 0U, hash = 0U;
    if (!SD_DiagStat(PACK_FILE, &size, NULL) || !SD_DiagReadFile(PACK_FILE, 0U, h, sizeof(h), &got) ||
        got != sizeof(h) || !table_ok(h, size)) return 0U;
    pack_ok = (uint8_t)(SD_DiagFileFnv1a(PACK_FILE, PACK_HEADER, &hash) && hash == CWPACK_ID);
    return pack_ok;
}

uint8_t CWPACK_CardState(void)
{
    uint8_t h[4];
    uint32_t got = 0U;
    if (!SD_IsMounted() || !SD_DiagReadFile(PACK_FILE, 0U, h, sizeof(h), &got) || got != sizeof(h)) return 0U;
    pack_ok = 0U; /* the console asks after a copy: check the file again */
    return CWPACK_Open() ? 2U : 1U;
}

/* Record `i` of section `k` into out. */
static uint8_t record(uint32_t k, uint32_t i, void *out)
{
    uint32_t got = 0U;
    if (!CWPACK_Open() || i >= sect_n[k]) return 0U;
    if (SD_DiagReadFile(PACK_FILE, sect_at[k] + i * rec_size[k], out, rec_size[k], &got) && got == rec_size[k])
        return 1U;
    read_failed();
    return 0U;
}

uint8_t CWPACK_Course(uint8_t course, uint8_t *out, uint8_t *stops)
{
    uint8_t r[64];
    uint64_t seen = 0U;
    if (!record(K_COURSE, course, r) || r[0] != MORSE_SYMBOLS) return 0U;
    memcpy(stops, &r[52], 7U); /* any mask is safe: the trainer ends the last lesson whole */
    /* The whole alphabet, each symbol once. */
    for (uint32_t i = 0U; i < MORSE_SYMBOLS; ++i) {
        const uint8_t x = MORSE_Index(r[1U + i]);
        if (x >= MORSE_SYMBOLS || (seen & (1ULL << x))) return 0U;
        seen |= 1ULL << x;
        out[i] = r[1U + i];
    }
    return MORSE_SYMBOLS;
}

/* Sendable characters as the table writes them (no prosign ids, no lower
 * case), then zeros to the record's end; returns how many. */
static uint8_t text_ok(const uint8_t *t, uint32_t size)
{
    uint32_t n = 0U;
    while (n < size && t[n] != 0U) {
        const uint8_t x = MORSE_Index(t[n]);
        if (t[n] < 0x20U || x >= MORSE_SYMBOLS || MORSE_SymbolAt(x) != t[n]) return 0U;
        ++n;
    }
    for (uint32_t i = n; i < size; ++i)
        if (t[i] != 0U) return 0U;
    return (uint8_t)n;
}

uint8_t CWPACK_Text(cwpack_list_t list, uint16_t index, uint8_t *out)
{
    out[CWPACK_TEXT_MAX] = 0U;
    if (!record(list == CWPACK_ABBR ? K_ATEXT : K_WTEXT, index, out)) return 0U;
    return text_ok(out, CWPACK_TEXT_MAX);
}

uint8_t CWPACK_Prefix(uint32_t pick, uint8_t *out)
{
    out[4] = 0U;
    if (!CWPACK_Open() || !record(K_PREFIX, pick % sect_n[K_PREFIX], out)) return 0U;
    return text_ok(out, 4U);
}

/* The pack is this image's to the byte (CWPACK_Open): gen_cw_pack.py
 * checked every step's op, arguments and jumps. */
uint8_t CWPACK_Step(uint8_t i, uint8_t *out) { return record(K_STEPS, i, out); }

/* The word scan: masks arrive a chunk at a time, a whole number of them
 * (64-byte chunks, 8-byte masks). */
typedef struct {
    uint64_t allowed;
    uint8_t *bitmap;
    uint32_t index;
    int32_t count;
    uint8_t max_len;
} scan_t;

static void scan_chunk(const uint8_t *data, uint32_t len, void *ctx)
{
    scan_t *s = (scan_t *)ctx;
    for (uint32_t at = 0U; at + 8U <= len; at += 8U, ++s->index) {
        const uint64_t m = (uint64_t)le32(&data[at]) | ((uint64_t)le32(&data[at + 4U]) << 32);
        const uint32_t n = (uint32_t)(m >> 56);
        if (n == 0U || n > s->max_len || (m & (0x1FULL << MORSE_SYMBOLS)) != 0U ||
            (m & SYM_BITS & ~s->allowed) != 0U) continue;
        s->bitmap[s->index >> 3] |= (uint8_t)(1U << (s->index & 7U));
        ++s->count;
    }
}

int32_t CWPACK_Scan(cwpack_list_t list, uint64_t allowed, uint8_t max_len, uint8_t *bitmap)
{
    const uint32_t k = (list == CWPACK_ABBR) ? K_AMASK : K_WMASK;
    scan_t s = {allowed, bitmap, 0U, 0, max_len};
    /* The list's own size: the trainer's abbreviation bitmap is 32 bytes,
     * and clearing 256 there wiped the trainer's state after it. */
    memset(bitmap, 0, ((list == CWPACK_ABBR) ? CWPACK_ABBR_MAX : CWPACK_WORDS_MAX) / 8U);
    if (!CWPACK_Open()) return -1;
    if (!SD_DiagReadEach(PACK_FILE, sect_at[k], 8U * sect_n[k], scan_chunk, &s)) {
        read_failed();
        return -1;
    }
    return s.count;
}

/* "<SK>" (either case) at t: the prosign, else 0. */
uint8_t CWPACK_Custom(uint8_t *out)
{
    uint8_t t[160];
    uint32_t got = 0U, n = 0U;
    uint64_t seen = 0U;
    if (!SD_DiagReadFile(CUSTOM_FILE, 0U, t, sizeof(t), &got)) return 0U;
    for (uint32_t i = 0U; i < got && n < MORSE_SYMBOLS; ++i) {
        uint8_t sym = t[i];
        if (sym == '<' && i + 4U <= got && (sym = MORSE_Prosign((const char *)&t[i + 1U])) != 0U) i += 3U;
        else if (sym <= ' ') continue;
        const uint8_t x = MORSE_Index(sym);
        if (x >= MORSE_SYMBOLS || (seen & (1ULL << x))) continue;
        seen |= 1ULL << x;
        out[n++] = MORSE_SymbolAt(x);
    }
    return (uint8_t)n;
}

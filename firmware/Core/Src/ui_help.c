#pragma GCC optimize ("Os") /* one text per help panel or console line */

/* UI_SettingHelp: the settings help, stored byte-pair coded (the text is in
 * Core/Inc/ui_help_text.def; tools/gen_help_text.py packs it at 61 % of its
 * 13 KB). Since v8.08 the packed help is HELP.BIN on the SD card, which the
 * radio requires; flash keeps only where each text starts and the file's
 * hash (ui_help_packed.inc). A text is read from the card when the panel
 * asks for another setting's and kept until then: the menu redraws every
 * frame.
 *
 * Since v8.09 HELP.BIN carries the card texts too (card_text.def: console
 * help, the menu's category descriptions, export headers), read the same
 * way; and the first read after a mount checks the whole file against the
 * hash its header and this image agree on, so a damaged card shows the
 * HELP.BIN MISSING OR OLD line rather than garbled text. */

#include "ui_priv.h"
#include "ui_app.h"
#include "mod_priv.h"
#include "mod_sdcard_service.h"
#include "card_text.h"
#include <stdint.h>
#include <string.h>

#include "ui_help_packed.inc"
#include "ui_names_packed.inc"

_Static_assert(HELP_STACK <= 16U, "help nests deeper than the unpacker's stack");

#define HELP_FILE        "HELP.BIN"
#define HELP_HEADER      12U
#define HELP_RETRY_MS    2000U

static char help_text[HELP_TEXT_MAX + 1U];
static uint16_t help_shown_at;     /* the text help_text holds; 0 none */
static uint32_t help_failed_ms;    /* last failed read, for the retry pace */
static uint8_t help_failed;
/* The card's HELP.BIN header was checked against this image; again after
 * the card is gone or a read fails. */
static uint8_t help_valid;
/* The text asked for last, and since when: the card is read only once the
 * cursor has rested HELP_SETTLE_MS on a setting. Every row scrolled past
 * read the card in the main loop -- three file opens, 4-55 ms each -- and
 * scrolling the settings menu lagged behind the knob (bench, 2026-10-01). */
static uint16_t help_want_at;
static uint32_t help_want_ms;
static uint8_t help_kicked;        /* the frame it settled for was asked */
#ifndef HELP_SETTLE_MS
#define HELP_SETTLE_MS   150U /* the host suites set 0 */
#endif

/* The right file for this build, whole? Its header against this image's
 * numbers, then everything after the header against the hash (FNV-1a, as
 * tools/gen_help_text.py computes it). */
static uint8_t help_file_ok(void)
{
    uint8_t h[HELP_HEADER];
    uint32_t got = 0U, body = 0U;
    if (!SD_DiagReadFile(HELP_FILE, 0U, h, sizeof(h), &got) || got != sizeof(h)) return 0U;
    uint32_t hash;
    memcpy(&hash, &h[4], 4U);
    if (!(memcmp(h, "RHLP", 4U) == 0 && hash == HELP_HASH &&
          (uint16_t)(h[8] | (h[9] << 8)) == HELP_PAIRS &&
          (uint16_t)(h[10] | (h[11] << 8)) == HELP_BLOB_LEN)) return 0U;
    return (uint8_t)(SD_DiagFileFnv1a(HELP_FILE, HELP_HEADER, &body) &&
                     body == HELP_HASH);
}

/* The cursor has rested HELP_SETTLE_MS on the setting asked for (written so
 * a settle time of 0 compiles without an always-true comparison). */
static uint8_t help_settled(uint32_t now)
{
    return (uint8_t)((uint32_t)(now - help_want_ms) + 1U > HELP_SETTLE_MS);
}

/* One frame per text asked for, once it has settled: the page asking reads
 * it in that frame, and its own refresh retries a busy card. A page left
 * before then (or the console's `set N help`) never asked again, and every
 * main loop pass rebuilt the whole screen from then on, ~60 frames a second:
 * the main loop starved and S-CTRL's screen took seconds a frame (bench,
 * 2026-10-06, after leaving the Morse trainer). */
uint8_t UI_HelpWantsFrame(uint32_t now)
{
    if (!help_want_at || help_want_at == help_shown_at || help_kicked || help_failed || !help_settled(now)) return 0U;
    help_kicked = 1U;
    return 1U;
}

/* For the boot check and the console's `card`: the card's HELP.BIN against
 * this image -- 0 missing, 1 another build's, 2 this one's -- and the id
 * the image wants in bytes 4..7 (after the 4-byte magic), which is how
 * tools/flash_rev1.py matches it to a file in the tree. */
uint8_t UI_HelpCardState(void)
{
    uint8_t h[4];
    uint32_t got = 0U;
    if (!SD_IsMounted() || !SD_DiagReadFile(HELP_FILE, 0U, h, sizeof(h), &got) || got != sizeof(h)) return 0U;
    return help_file_ok() ? 2U : 1U;
}

uint32_t UI_HelpCardId(void) { return HELP_HASH; }

#define HELP_PAIRS_AT (HELP_HEADER + 2U * HELP_KEYS)
#define UNPAIR_STACK  ((HELP_STACK > NAMES_STACK) ? HELP_STACK : NAMES_STACK)

/* Byte-pair decoding (tools/gen_help_text.py): a byte below 0x20 or from
 * 0x7F up stands for pair[] of it, two bytes that are each a character or
 * another pair. src up to its 0 (or len) into out (cap bytes, the
 * terminating 0 included). */
static uint8_t unpair(const uint8_t (*pair)[2], const uint8_t *src, uint32_t len, char *out, uint16_t cap)
{
    uint8_t stack[UNPAIR_STACK];
    unsigned i = 0U, depth = 0U, n = 0U;
    for (;;) {
        uint8_t c;
        if (depth) c = stack[--depth];
        else if (i < len) c = src[i++];
        else break;
        if (c == 0U) break; /* only ever from the blob: no pair holds a 0 */
        while ((c < 0x20U) || (c >= 0x7FU)) {
            const uint8_t *p = pair[(c >= 0x7FU) ? (unsigned)(c - 0x7FU) : (unsigned)c + 128U];
            if (depth >= sizeof(stack)) return 0U;
            stack[depth++] = p[1];
            c = p[0];
        }
        if (n + 1U < cap) out[n++] = (char)c;
    }
    out[n] = '\0';
    return 1U;
}

/* Two names at a time are enough for every caller (a row and its edit
 * title); the UI's, not realtime's: CCM. */
static char name_text[2][24] CCMRAM_BSS;
static uint8_t name_slot;

const char *UI_SettingName(setting_id_t id)
{
    char *out = name_text[name_slot];
    name_slot ^= 1U;
    out[0] = '\0';
    if ((unsigned)id >= (unsigned)SET_COUNT) return out;
    /* From the 16th name at or before it, past the names between. */
    uint32_t at = name_at16[(unsigned)id >> 4];
    for (unsigned k = (unsigned)id & 15U; k; --k)
        while (name_blob[at++] != 0U) {}
    if (!unpair(name_pair, &name_blob[at], sizeof(name_blob) - at, out, sizeof(name_text[0]))) out[0] = '\0';
    return out;
}

/* Text `key` of the card's index (HELP_KEY_*), unpacked into out (cap
 * bytes, the terminating 0 included): where it starts from the index, then
 * the pairs and the text in one open. */
static uint8_t pack_read(uint16_t key, char *out, uint16_t cap)
{
    uint8_t pair[HELP_PAIRS][2];
    uint8_t packed[HELP_TEXT_MAX + 1U];
    uint8_t le[2];
    uint32_t got = 0U;
    if (!help_valid) {
        if (!help_file_ok()) return 0U;
        help_valid = 1U;
    }
    if (key >= HELP_KEYS || !SD_DiagReadFile(HELP_FILE, HELP_HEADER + 2U * (uint32_t)key, le, 2U, &got) ||
        got != 2U) {
        help_valid = 0U;
        return 0U;
    }
    const uint32_t at = (uint32_t)(le[0] | (le[1] << 8));
    if (at == 0U || at > HELP_BLOB_LEN) return 0U;
    /* A packed text is never longer than itself unpacked, plus its 0. */
    uint32_t want = HELP_BLOB_LEN - (at - 1U);
    if (want > sizeof(packed)) want = sizeof(packed);
    if (!SD_DiagReadSpans(HELP_FILE, HELP_PAIRS_AT, pair, sizeof(pair),
                          HELP_PAIRS_AT + sizeof(pair) + (at - 1U), packed, want, &got) ||
        got != want) {
        help_valid = 0U;
        return 0U;
    }
    return unpair((const uint8_t (*)[2])pair, packed, got, out, cap);
}

/* The voice tools USB CLEAN bypasses for USB input say so instead. */
static uint8_t bypassed_by_usb_clean(setting_id_t id)
{
    return (uint8_t)(cfg_usb_clean_mode &&
                     (id == SET_COMP || id == SET_EQ || id == SET_MICAGC || id == SET_NG || id == SET_ALC));
}

uint8_t UI_CwMemoryText(uint8_t n, char *out, uint16_t cap)
{
    char buf[512];
    uint32_t got = 0U;
    if (cap == 0U) return 0U;
    out[0] = '\0';
    if (!SD_IsMounted() || !MOD_BlockingIoAllowed(HAL_GetTick()) ||
        !SD_DiagReadFile("CW/MEMORIES.TXT", 0U, buf, sizeof(buf) - 1U, &got))
        return 0U;
    buf[got] = '\0';
    const char *p = buf;
    for (; n && *p; ++p)
        if (*p == '\n') --n;
    uint16_t k = 0U;
    while (*p && *p != '\n' && *p != '\r' && k + 1U < cap) out[k++] = *p++;
    out[k] = '\0';
    return 1U;
}

/* A memory's row shows its line where a setting shows its help. */
static uint8_t memory_help(uint16_t n)
{
    if (!UI_CwMemoryText((uint8_t)n, help_text, (uint16_t)sizeof(help_text)))
        strcpy(help_text, "NO CW/MEMORIES.TXT ON THE CARD");
    else if (!help_text[0])
        strcpy(help_text, "EMPTY LINE");
    return 1U;
}

/* Text `key` (HELP_KEY_*; past HELP_KEYS a memory's line) for a panel drawn
 * every frame: read once the cursor has rested, then kept (help_text) until
 * another is asked for. help_shown_at and help_want_at hold key + 1, 0 for
 * none. */
static const char *pack_text(uint16_t key)
{
    const uint16_t at = (uint16_t)(key + 1U);
    if (at == help_shown_at) return help_text;
    const uint32_t now = HAL_GetTick();
    if (!SD_IsMounted()) help_valid = 0U;
    if (at != help_want_at) { help_want_at = at; help_want_ms = now; help_kicked = 0U; }
    if (!help_settled(now)) return "";
    /* The card is not read while transmitting or streaming. */
    if (SD_IsMounted() && !MOD_BlockingIoAllowed(now)) return "HELP: SD CARD BUSY";
    if (help_failed && (uint32_t)(now - help_failed_ms) < HELP_RETRY_MS) {
        return SD_IsMounted() ? "HELP.BIN MISSING OR OLD ON THE SD CARD" : "HELP IS ON THE SD CARD";
    }
    if (!(key >= HELP_KEYS ? memory_help((uint16_t)(key - HELP_KEYS))
                           : pack_read(key, help_text, (uint16_t)sizeof(help_text)))) {
        help_failed = 1U;
        help_failed_ms = now;
        help_shown_at = 0U;
        return SD_IsMounted() ? "HELP.BIN MISSING OR OLD ON THE SD CARD" : "HELP IS ON THE SD CARD";
    }
    help_failed = 0U;
    help_shown_at = at;
    return help_text;
}

/* A setting without help of its own has the general line in its place in
 * the index (gen_help_text.py). */
const char *UI_SettingHelp(setting_id_t id)
{
    if ((unsigned)id >= (unsigned)SET_COUNT) return pack_text(HELP_KEY_CARD); /* the general line */
    if (id >= SET_CW_MEM1 && id <= SET_CW_MEM8) return pack_text((uint16_t)(HELP_KEYS + (id - SET_CW_MEM1)));
    return pack_text(bypassed_by_usb_clean(id) ? HELP_KEY_BYPASSED : (uint16_t)id);
}

const char *UI_CardTextCached(card_text_t id)
{
    if (id == TXT_NONE || (unsigned)id >= (unsigned)TXT_COUNT) return "";
    return pack_text((uint16_t)(HELP_KEY_CARD + (unsigned)id));
}

uint16_t UI_CardText(card_text_t id, char *buf, uint16_t cap)
{
    if (buf == NULL || cap == 0U) return 0U;
    buf[0] = '\0';
    if (id == TXT_NONE || (unsigned)id >= (unsigned)TXT_COUNT) return 0U;
    if (!SD_IsMounted() || !MOD_BlockingIoAllowed(HAL_GetTick())) return 0U;
    if (!pack_read((uint16_t)(HELP_KEY_CARD + (unsigned)id), buf, cap)) {
        help_valid = 0U;
        buf[0] = '\0';
        return 0U;
    }
    return (uint16_t)strlen(buf);
}

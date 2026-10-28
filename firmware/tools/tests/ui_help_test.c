/* UI_SettingHelp unpacks every text of Core/Inc/ui_help_text.def exactly:
 * the packed store (ui_help_packed.inc) against the source it was made from,
 * the USB CLEAN bypass, and the fallback for a setting without help. So do
 * UI_CardText and UI_CardTextCached for every text of card_text.def, into a
 * buffer of any size. */
#include "ui_priv.h"
#include "ui_app.h"
#include "card_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint8_t cfg_usb_clean_mode;

enum { HELP_NONE = -1, HELP_BYPASSED = -2 };
static const struct { int id; const char *text; } expect[] = {
#define HELP(key, text) {key, text},
#include "ui_help_text.def"
#undef HELP
};
#define EXPECTED (sizeof(expect) / sizeof(expect[0]))

static const struct { card_text_t id; const char *text; } cards[] = {
#define TEXT(key, text) {key, text},
#include "card_text.def"
#undef TEXT
};

static const char *text_of(int id)
{
    for (unsigned k = 0U; k < EXPECTED; ++k)
        if (expect[k].id == id) return expect[k].text;
    return NULL;
}

static uint8_t bypassed(int id)
{
    return (uint8_t)(id == SET_COMP || id == SET_EQ || id == SET_MICAGC || id == SET_NG || id == SET_ALC);
}

int main(void)
{
    const char *none = text_of(HELP_NONE), *bypass = text_of(HELP_BYPASSED);
    assert(none && bypass);
    unsigned with_help = 0U, bytes = 0U;
    for (int clean = 0; clean <= 1; ++clean) {
        cfg_usb_clean_mode = (uint8_t)clean;
        for (int id = 0; id < (int)SET_COUNT; ++id) {
            const char *want = text_of(id);
            if (!want) want = none;
            else if (clean && bypassed(id)) want = bypass;
            /* A memory's row shows its line of CW/MEMORIES.TXT (the stub's). */
            if (id >= SET_CW_MEM1 && id <= SET_CW_MEM8)
                want = (id == SET_CW_MEM1) ? "CQ CQ DE TEST K" : (id == SET_CW_MEM3 ? "TEST <SK>" : "EMPTY LINE");
            const char *got = UI_SettingHelp((setting_id_t)id);
            if (strcmp(got, want) != 0) {
                printf("setting %d: got \"%s\"\n            want \"%s\"\n", id, got, want);
                return 1;
            }
            if (!clean && text_of(id) && !(id >= SET_CW_MEM1 && id <= SET_CW_MEM8)) {
                ++with_help;
                bytes += (unsigned)strlen(got) + 1U;
            }
        }
    }
    assert(strcmp(UI_SettingHelp((setting_id_t)SET_COUNT), none) == 0);
    {
        char line[8];
        assert(UI_CwMemoryText(0U, line, sizeof(line)) && strcmp(line, "CQ CQ D") == 0); /* cut to fit */
        assert(UI_CwMemoryText(1U, line, sizeof(line)) && line[0] == '\0');
        assert(UI_CwMemoryText(7U, line, sizeof(line)) && line[0] == '\0');
    }
    printf("settings help: %u settings, %u bytes of text, all unpacked exactly: PASS\n", with_help, bytes);

    unsigned card_bytes = 0U;
    for (unsigned k = 0U; k < sizeof(cards) / sizeof(cards[0]); ++k) {
        char buf[200];
        const size_t want = strlen(cards[k].text);
        assert(UI_CardText(cards[k].id, buf, sizeof(buf)) == want && strcmp(buf, cards[k].text) == 0);
        assert(strcmp(UI_CardTextCached(cards[k].id), cards[k].text) == 0);
        /* a short buffer gets the start of the text, terminated */
        char small[9];
        assert(UI_CardText(cards[k].id, small, sizeof(small)) == (want < 8U ? want : 8U));
        assert(strncmp(small, cards[k].text, 8U) == 0 || strcmp(small, cards[k].text) == 0);
        card_bytes += (unsigned)want + 1U;
    }
    char buf[8] = "x";
    assert(UI_CardText(TXT_NONE, buf, sizeof(buf)) == 0U && buf[0] == '\0');
    assert(UI_CardText(TXT_COUNT, buf, sizeof(buf)) == 0U);
    assert(UI_CardText(cards[0].id, buf, 0U) == 0U);
    assert(UI_CardTextCached(TXT_NONE)[0] == '\0');
    printf("card texts: %u texts, %u bytes, all unpacked exactly: PASS\n",
           (unsigned)(sizeof(cards) / sizeof(cards[0])), card_bytes);

    /* A text asked for and not read yet gets one frame, not a frame every
     * main loop pass after its page was left (bench, 2026-10-06). */
    extern uint8_t sd_help_card_busy;
    extern uint32_t HAL_GetTick(void);
    assert(!UI_HelpWantsFrame(HAL_GetTick()));
    sd_help_card_busy = 1U;
    assert(strcmp(UI_CardTextCached(cards[1].id), "HELP: SD CARD BUSY") == 0);
    sd_help_card_busy = 0U;
    assert(UI_HelpWantsFrame(HAL_GetTick()));
    assert(!UI_HelpWantsFrame(HAL_GetTick()) && !UI_HelpWantsFrame(HAL_GetTick()));
    assert(strcmp(UI_CardTextCached(cards[1].id), cards[1].text) == 0); /* the page came back */
    assert(!UI_HelpWantsFrame(HAL_GetTick()));
    sd_help_card_busy = 1U;
    (void)UI_CardTextCached(cards[2].id);                               /* another text: a frame again */
    sd_help_card_busy = 0U;
    assert(UI_HelpWantsFrame(HAL_GetTick()) && !UI_HelpWantsFrame(HAL_GetTick()));
    printf("help frames: one per text asked for: PASS\n");
    return 0;
}

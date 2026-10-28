#pragma GCC optimize ("Os")

#include "ui_state.h"
#include "card_text.h"
#include <stddef.h>

static const mod_config_t prod_modes_normal[] = {
    MOD_CFG_PROD_SSB_14MHZ,
    MOD_CFG_FM_NARROW_14MHZ,
    MOD_CFG_AM_14MHZ,
    MOD_CFG_CW_14MHZ
};

#if MOD_DEEP_DIAG_ENABLE
static const mod_config_t prod_modes_diagnostic[] = {
    MOD_CFG_BASELINE_27MHZ,
    MOD_CFG_PROD_SSB_14MHZ,
    MOD_CFG_RAW_DIRECT_14MHZ,
    MOD_CFG_ENHANCED_SSB_14MHZ,
    MOD_CFG_FILTERED_STD_14MHZ,
    MOD_CFG_FILTERED_NARROW_14MHZ,
    MOD_CFG_FM_NARROW_14MHZ,
    MOD_CFG_AM_14MHZ,
    MOD_CFG_CW_14MHZ
};
#endif

static const mod_config_t *prod_mode_list(uint8_t *count)
{
#if MOD_DEEP_DIAG_ENABLE
    if (ui_debug_visible != 0U) {
        *count = (uint8_t)(sizeof(prod_modes_diagnostic) / sizeof(prod_modes_diagnostic[0]));
        return prod_modes_diagnostic;
    }
#endif
    *count = (uint8_t)(sizeof(prod_modes_normal) / sizeof(prod_modes_normal[0]));
    return prod_modes_normal;
}

static const mod_config_t test_modes[] = {
    MOD_CFG_INT_1KHZ_TONE,
    MOD_CFG_INT_2TONE_IMD,
    MOD_CFG_INT_2TONE_BRANCH_A,
    MOD_CFG_INT_2TONE_BRANCH_C,
    MOD_CFG_INT_SWEEP,
    MOD_CFG_INT_SSB_1KHZ,
    MOD_CFG_INT_SSB_2TONE,
    MOD_CFG_INT_SSB_SWEEP,
    MOD_CFG_INT_THETA_SWEEP
};

/* ---- Menu categories ---------------------------------------------------
 *
 * The old menu was fifteen flat entries in bring-up order, with headphone
 * volume under BOARD / HARDWARE and the receive mode under RX TRIM. The
 * landscape menu is a short list of categories named for what an operator is
 * looking for; inside, existing setting groups appear either inline as a
 * titled section or, for the long commissioning pages, as a link to their own
 * list. A setting may appear in more than one category (volume is both an
 * operating and an audio control). */

enum { ENTRY_ITEMS = 0x10, ENTRY_SECTION = 0x11 };

#define E_SET(id)     {UI_ROW_SETTING, (uint16_t)(id)}
#define E_PAGE(id)    {UI_ROW_PAGE, (uint16_t)(id)}
#define E_ACT(id)     {UI_ROW_ACTION, (uint16_t)(id)}
#define E_LINK(g)     {UI_ROW_GROUP, (uint16_t)(g)}
#define E_ITEMS(g)    {ENTRY_ITEMS, (uint16_t)(g)}
#define E_SECTION(g)  {ENTRY_SECTION, (uint16_t)(g)}

static const ui_row_t cat_operate[] = {
    E_ITEMS(AUDIO_GROUP_OPERATE), E_PAGE(PAGE_FREQ), E_ACT(UI_ACT_AB_SWAP)
};
static const ui_row_t cat_audio[] = {
    E_SET(SET_REV1_VOLUME), E_SET(SET_REV1_MIC), E_SET(SET_REV1_MIC_GAIN), E_SET(SET_REV1_USB_IQ),
    E_SECTION(AUDIO_GROUP_USB_INPUT)
};
/* CW: the trainer, the memories, the everyday controls, then ADVANCED CW
 * and ADVANCED TRAINING -- each one list in sections (composites below). */
static const ui_row_t cat_cw[] = {
    E_LINK(AUDIO_GROUP_CW_TRAIN), E_LINK(AUDIO_GROUP_CW_MEMORIES), E_ITEMS(AUDIO_GROUP_CW),
    E_LINK(AUDIO_GROUP_CW_ADVANCED), E_LINK(AUDIO_GROUP_TRAIN_ADVANCED)
};
static const ui_row_t cat_voice[] = {
    E_SECTION(AUDIO_GROUP_SSB), E_SECTION(AUDIO_GROUP_VOICE)
};
static const ui_row_t cat_amfm[] = {
    E_SECTION(AUDIO_GROUP_FM), E_SECTION(AUDIO_GROUP_AM)
};
static const ui_row_t cat_tx[] = {
    E_ITEMS(AUDIO_GROUP_PA_RF), E_PAGE(PAGE_CAL), E_PAGE(PAGE_TX_TEST)
};
static const ui_row_t cat_display[] = {
    E_ITEMS(AUDIO_GROUP_UI), E_SET(SET_REV1_BACKLIGHT), E_SET(SET_UI_FKEY_PIN), E_SET(SET_REV1_ENC_REVERSE),
    E_SET(SET_REV1_ENC_DETENT)
};
static const ui_row_t cat_board[] = {
    E_LINK(AUDIO_GROUP_BOARD_ASSEMBLY), E_LINK(AUDIO_GROUP_BOARD_CONTROLS),
    E_LINK(AUDIO_GROUP_BOARD_FILTERS), E_LINK(AUDIO_GROUP_BOARD_BIAS),
    E_LINK(AUDIO_GROUP_BOARD_RXTRIM), E_LINK(AUDIO_GROUP_BOARD_POWER),
    E_LINK(AUDIO_GROUP_BOARD_CAL)
};
static const ui_row_t cat_system[] = {
    E_PAGE(PAGE_SD), E_ACT(UI_ACT_SAVE), E_ACT(UI_ACT_LOAD), E_ACT(UI_ACT_FACTORY),
#if MOD_DEEP_DIAG_ENABLE
    E_ACT(UI_ACT_DEBUG_UI), /* the debug UI exists only in the diagnostic image */
#endif
    E_PAGE(PAGE_DIAG), E_SET(SET_REV1_OFF)
};
static const ui_row_t cat_diag[] = {
    E_PAGE(PAGE_TESTS), E_PAGE(PAGE_RUNTIME), E_SECTION(AUDIO_GROUP_DSP_ENGINE),
    E_SECTION(AUDIO_GROUP_DEBUG)
};

/* Groups whose list is made of other groups' sections, as a category is. */
static const ui_row_t adv_cw[] = {
    E_SECTION(AUDIO_GROUP_CW_TIMING), E_SECTION(AUDIO_GROUP_CW_BREAKIN), E_SECTION(AUDIO_GROUP_CW_RECEIVE),
    E_SECTION(AUDIO_GROUP_CW_RF), E_SECTION(AUDIO_GROUP_CW_SHAPING)
};
/* The trainer: what to do, then its everyday settings. */
static const ui_row_t trainer[] = {
    E_ACT(UI_ACT_TRAIN_GUIDED), E_ACT(UI_ACT_TRAIN_ECHO), E_ACT(UI_ACT_TRAIN_LISTEN), E_ACT(UI_ACT_TRAIN_LEARN),
    E_ACT(UI_ACT_TRAIN_FREE), E_ACT(UI_ACT_TRAIN_PADDLE), E_ACT(UI_ACT_TRAIN_ADD), E_ACT(UI_ACT_TRAIN_DELETE),
    E_SECTION(AUDIO_GROUP_TRAIN_BASIC)
};
static const ui_row_t adv_train[] = {
    E_SECTION(AUDIO_GROUP_TRAIN_COURSE), E_SECTION(AUDIO_GROUP_TRAIN_SPACING), E_SECTION(AUDIO_GROUP_TRAIN_ANSWER),
    E_SECTION(AUDIO_GROUP_TRAIN_SESSION)
};

typedef struct {
    const char     *label;
    card_text_t     help;   /* on the card's text pack (card_text.def) */
    const ui_row_t *entries;
    uint8_t         count;
} ui_category_t;

#define CAT(label, help, list) {label, help, list, (uint8_t)(sizeof(list) / sizeof(list[0]))}
static const ui_category_t categories[UI_CAT_COUNT] = {
    [UI_CAT_OPERATE] = CAT("OPERATE", TXT_CAT_OPERATE, cat_operate),
    [UI_CAT_AUDIO]   = CAT("AUDIO", TXT_CAT_AUDIO, cat_audio),
    [UI_CAT_CW]      = CAT("CW", TXT_CAT_CW, cat_cw),
    [UI_CAT_VOICE]   = CAT("VOICE", TXT_CAT_VOICE, cat_voice),
    [UI_CAT_AMFM]    = CAT("AM / FM", TXT_CAT_AMFM, cat_amfm),
    [UI_CAT_TX]      = CAT("TX / RF", TXT_CAT_TX, cat_tx),
    [UI_CAT_DISPLAY] = CAT("DISPLAY", TXT_CAT_DISPLAY, cat_display),
    [UI_CAT_BOARD]   = CAT("BOARD", TXT_CAT_BOARD, cat_board),
    [UI_CAT_SYSTEM]  = CAT("SYSTEM", TXT_CAT_SYSTEM, cat_system),
    [UI_CAT_DIAG]    = CAT("DIAG", TXT_CAT_DIAG, cat_diag),
};

uint8_t UI_MenuCategoryCount(void)
{
    return ui_debug_visible ? (uint8_t)UI_CAT_COUNT : (uint8_t)UI_CAT_DIAG;
}

uint8_t UI_MenuCategoryAt(uint8_t visible)
{
    return (visible < UI_MenuCategoryCount()) ? visible : (uint8_t)UI_CAT_OPERATE;
}

const char *UI_MenuCategoryLabel(uint8_t cat)
{
    return (cat < (uint8_t)UI_CAT_COUNT) ? categories[cat].label : "";
}

const char *UI_MenuCategoryHelp(uint8_t cat)
{
    return (cat < (uint8_t)UI_CAT_COUNT) ? UI_CardTextCached(categories[cat].help) : "";
}

static uint8_t group_count(uint16_t group)
{
    const audio_group_info_t *g = UI_GetAudioGroupInfo((uint8_t)group);
    return g ? g->count : 0U;
}

static uint8_t entry_rows(const ui_row_t *e)
{
    if (e->kind == ENTRY_ITEMS) return group_count(e->id);
    if (e->kind == ENTRY_SECTION) return (uint8_t)(group_count(e->id) + 1U);
    return 1U;
}

/* The entries a list is made of: a category's, or a composite group's. */
static const ui_row_t *list_entries(uint8_t cat, uint8_t group, uint8_t *count)
{
    if (group == AUDIO_GROUP_CW_ADVANCED) { *count = (uint8_t)(sizeof(adv_cw) / sizeof(adv_cw[0])); return adv_cw; }
    if (group == AUDIO_GROUP_TRAIN_ADVANCED) { *count = (uint8_t)(sizeof(adv_train) / sizeof(adv_train[0])); return adv_train; }
    if (group == AUDIO_GROUP_CW_TRAIN) { *count = (uint8_t)(sizeof(trainer) / sizeof(trainer[0])); return trainer; }
    if (group != AUDIO_GROUP_NONE || cat >= (uint8_t)UI_CAT_COUNT) { *count = 0U; return NULL; }
    *count = categories[cat].count;
    return categories[cat].entries;
}

uint8_t UI_MenuRowCount(uint8_t cat, uint8_t group)
{
    uint8_t n;
    const ui_row_t *list = list_entries(cat, group, &n);
    if (list == NULL) return (group != AUDIO_GROUP_NONE) ? group_count(group) : 0U;
    uint16_t rows = 0U;
    for (uint8_t i = 0U; i < n; ++i) rows = (uint16_t)(rows + entry_rows(&list[i]));
    return (uint8_t)((rows > 255U) ? 255U : rows);
}

uint8_t UI_MenuRowAt(uint8_t cat, uint8_t group, uint8_t index, ui_row_t *out)
{
    const audio_group_info_t *g;
    uint8_t n;
    const ui_row_t *list = list_entries(cat, group, &n);
    if (list == NULL) {
        g = UI_GetAudioGroupInfo(group);
        if ((g == NULL) || (index >= g->count)) return 0U;
        out->kind = UI_ROW_SETTING;
        out->id = (uint16_t)g->items[index];
        return 1U;
    }
    for (uint8_t i = 0U; i < n; ++i) {
        const ui_row_t *e = &list[i];
        const uint8_t n = entry_rows(e);
        if (index >= n) { index = (uint8_t)(index - n); continue; }
        if ((e->kind != ENTRY_ITEMS) && (e->kind != ENTRY_SECTION)) { *out = *e; return 1U; }
        if (e->kind == ENTRY_SECTION) {
            if (index == 0U) { out->kind = UI_ROW_HEADER; out->id = e->id; return 1U; }
            index--;
        }
        g = UI_GetAudioGroupInfo((uint8_t)e->id);
        out->kind = UI_ROW_SETTING;
        out->id = (uint16_t)g->items[index];
        return 1U;
    }
    return 0U;
}

uint8_t UI_MenuSelectable(uint8_t cat, uint8_t group, uint8_t index, int8_t dir)
{
    const uint8_t total = UI_MenuRowCount(cat, group);
    ui_row_t row;
    for (int16_t i = (int16_t)index; (i >= 0) && (i < (int16_t)total); i = (int16_t)(i + ((dir < 0) ? -1 : 1))) {
        if (UI_MenuRowAt(cat, group, (uint8_t)i, &row) && (row.kind != UI_ROW_HEADER)) return (uint8_t)i;
    }
    return index;
}

const char *UI_PageTitle(ui_page_t page)
{
    switch (page) {
        case PAGE_HOME:         return "HOME";
        case PAGE_MENU:         return "MENU";
        case PAGE_FREQ:         return "FREQUENCY ENTRY";
        case PAGE_CAL:          return "SSB CALIBRATION";
        case PAGE_DIAG:         return "DIAGNOSTICS";
        case PAGE_RUNTIME:      return "RUNTIME DIAG";
        case PAGE_TESTS:        return "TEST SIGNALS";
        case PAGE_TX_TEST:      return "TX CARRIER TEST";
        case PAGE_CW_TRAINER:   return "MORSE TRAINER";
        case PAGE_SD:           return "SD CARD";
        case PAGE_PD:           return "POWER SUPPLY";
        case PAGE_BPF_SWEEP:    return "FILTER SWEEP";
        case PAGE_AF_SWEEP:     return "AUDIO SWEEP";
        case PAGE_IQ_SCOPE:     return "ADC I/Q SCOPE";
        case PAGE_SELFTEST:     return "SELF TEST";
        case PAGE_WATERFALL:    return "WATERFALL";
        case PAGE_COUNT:
        default:                return "";
    }
}

static card_text_t page_help(ui_page_t page)
{
    switch (page) {
        case PAGE_FREQ:     return TXT_PAGE_FREQ;
        case PAGE_CAL:      return TXT_PAGE_CAL;
        case PAGE_TX_TEST:  return TXT_PAGE_TX_TEST;
        case PAGE_SD:       return TXT_PAGE_SD;
        case PAGE_TESTS:    return TXT_PAGE_TESTS;
        case PAGE_DIAG:     return TXT_PAGE_DIAG;
        case PAGE_RUNTIME:  return TXT_PAGE_RUNTIME;
        default:            return TXT_PAGE_DEFAULT;
    }
}

const char *UI_ActionLabel(ui_action_t action)
{
    switch (action) {
        case UI_ACT_SAVE:     return "SAVE SETTINGS";
        case UI_ACT_LOAD:     return "LOAD SETTINGS";
        case UI_ACT_FACTORY:  return "FACTORY RESET";
        case UI_ACT_AB_SWAP:  return "A/B MODE SWAP";
        case UI_ACT_DEBUG_UI: return ui_debug_visible ? "DEBUG UI OFF" : "DEBUG UI ON";
        case UI_ACT_TRAIN_GUIDED: return "START LEARNING";
        case UI_ACT_TRAIN_ECHO:   return "ECHO PRACTICE";
        case UI_ACT_TRAIN_LISTEN: return "LISTEN";
        case UI_ACT_TRAIN_LEARN:  return "LEARN NEW CHARACTER";
        case UI_ACT_TRAIN_FREE:   return "FREE KEYING";
        case UI_ACT_TRAIN_PADDLE: return "PADDLE CHECK";
        case UI_ACT_TRAIN_ADD:    return "ADD NEXT CHARACTER";
        case UI_ACT_TRAIN_DELETE: return "DELETE PROGRESS";
        case UI_ACT_COUNT:
        default:              return "";
    }
}

static card_text_t action_help(ui_action_t action)
{
    switch (action) {
        case UI_ACT_SAVE:     return TXT_ACT_SAVE;
        case UI_ACT_LOAD:     return TXT_ACT_LOAD;
        case UI_ACT_FACTORY:  return TXT_ACT_FACTORY;
        case UI_ACT_AB_SWAP:  return TXT_ACT_AB_SWAP;
        case UI_ACT_DEBUG_UI: return TXT_ACT_DEBUG_UI;
        case UI_ACT_TRAIN_GUIDED: return TXT_ACT_TRAIN_GUIDED;
        case UI_ACT_TRAIN_ECHO:   return TXT_ACT_TRAIN_ECHO;
        case UI_ACT_TRAIN_LISTEN: return TXT_ACT_TRAIN_LISTEN;
        case UI_ACT_TRAIN_LEARN:  return TXT_ACT_TRAIN_LEARN;
        case UI_ACT_TRAIN_FREE:   return TXT_ACT_TRAIN_FREE;
        case UI_ACT_TRAIN_PADDLE: return TXT_ACT_TRAIN_PADDLE;
        case UI_ACT_TRAIN_ADD:    return TXT_ACT_TRAIN_ADD;
        case UI_ACT_TRAIN_DELETE: return TXT_ACT_TRAIN_DELETE;
        case UI_ACT_COUNT:
        default:              return TXT_NONE;
    }
}

#if !defined(UI_SETTINGS_POLICY_ONLY)
const char *UI_MenuRowLabel(const ui_row_t *row)
{
    switch (row->kind) {
        case UI_ROW_SETTING: {
            const setting_item_t *it = UI_GetSetting((setting_id_t)row->id);
            return it ? UI_SettingName((setting_id_t)row->id) : "?";
        }
        case UI_ROW_HEADER:
        case UI_ROW_GROUP: {
            const audio_group_info_t *g = UI_GetAudioGroupInfo((uint8_t)row->id);
            return g ? g->title : "?";
        }
        case UI_ROW_PAGE:   return UI_PageTitle((ui_page_t)row->id);
        case UI_ROW_ACTION: return UI_ActionLabel((ui_action_t)row->id);
        default:            return "";
    }
}
#endif

const char *UI_MenuRowHelp(const ui_row_t *row)
{
    switch (row->kind) {
        case UI_ROW_SETTING: return UI_SettingHelp((setting_id_t)row->id);
        case UI_ROW_HEADER:
        case UI_ROW_GROUP:   return UI_AudioGroupHelp((uint8_t)row->id);
        case UI_ROW_PAGE:    return UI_CardTextCached(page_help((ui_page_t)row->id));
        case UI_ROW_ACTION:  return UI_CardTextCached(action_help((ui_action_t)row->id));
        default:             return "";
    }
}

uint8_t UI_GetProdModeCount(void)
{
    uint8_t count = 0U;
    (void)prod_mode_list(&count);
    return count;
}

mod_config_t UI_GetProdModeAt(uint8_t idx)
{
    uint8_t count = 0U;
    const mod_config_t *list = prod_mode_list(&count);
    return list[idx < count ? idx : 0U];
}

uint8_t UI_GetTestModeCount(void)
{
    return (uint8_t)(sizeof(test_modes) / sizeof(test_modes[0]));
}

mod_config_t UI_GetTestModeAt(uint8_t idx)
{
    return test_modes[idx < UI_GetTestModeCount() ? idx : 0U];
}

uint8_t UI_FindProdMode(mod_config_t cfg)
{
    uint8_t count = 0U;
    const mod_config_t *list = prod_mode_list(&count);
    for (uint8_t i = 0U; i < count; i++) {
        if (list[i] == cfg) return i;
    }
    return 0U;
}

static card_text_t mode_help(mod_config_t cfg)
{
    switch (cfg) {
        case MOD_CFG_PROD_SSB_14MHZ:
            return TXT_MODE_PROD_SSB_14MHZ;
        case MOD_CFG_FM_NARROW_14MHZ:
            return TXT_MODE_FM_NARROW_14MHZ;
        case MOD_CFG_AM_14MHZ:
            return TXT_MODE_AM_14MHZ;
        case MOD_CFG_CW_14MHZ:
            return TXT_MODE_CW_14MHZ;
#if MOD_DEEP_DIAG_ENABLE /* the debug UI's mode list */
        case MOD_CFG_ENHANCED_SSB_14MHZ:
            return TXT_MODE_ENHANCED_SSB_14MHZ;
        case MOD_CFG_RAW_DIRECT_14MHZ:
            return TXT_MODE_RAW_DIRECT_14MHZ;
        case MOD_CFG_FILTERED_STD_14MHZ:
            return TXT_MODE_FILTERED_STD_14MHZ;
        case MOD_CFG_FILTERED_NARROW_14MHZ:
            return TXT_MODE_FILTERED_NARROW_14MHZ;
        case MOD_CFG_BASELINE_27MHZ:
            return TXT_MODE_BASELINE_27MHZ;
#endif
        default:
            return TXT_MODE_DEFAULT;
    }
}

/* On the card (card_text.def), like every menu description. */
const char *UI_ProdModeHelp(mod_config_t cfg) { return UI_CardTextCached(mode_help(cfg)); }

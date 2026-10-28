#pragma GCC optimize ("Os")

/* The settings menu. Top level: categories on the left, a preview of the
 * highlighted category on the right. Inside a category the list takes the
 * whole width so setting names fit, section headers group related rows, and
 * the bottom panel explains the selected row or, while editing, shows the
 * range or the neighbouring choices. */

#include "ui_state.h"
#include "ui_gfx.h"
#include "ui_format.h"
#include "main.h"
#include <string.h>

#define MENU_VISIBLE   10U
#define MENU_HELP_Y    176U
#define MENU_LEFT_W    96U
#define MENU_RIGHT_X   104U
#define MENU_RIGHT_W   208U
#define MENU_FULL_W    312U

static uint8_t cur_cat(void) { return UI_MenuCategoryAt(ui.menu_cat); }
static uint8_t cur_group(void) { return (ui.menu_level == 2U) ? ui.menu_group : (uint8_t)AUDIO_GROUP_NONE; }

static const char *band_name(void)
{
    const mod_band_t *b = MOD_GetBand(MOD_GetActiveBandIndex());
    return (b != NULL) ? b->name : "?";
}

/* Percent, dB and wpm for the settings the home row shows that way; the
 * settings map's own text for everything else. */
static void value_text(setting_id_t id, const setting_item_t *it, float v, char *dst)
{
    const ui_slide_fmt_t fmt = UI_SlideFormat((uint8_t)id);
    if ((fmt == UI_SLIDE_FMT_SETTING) || (fmt == UI_SLIDE_FMT_ACTION)) UI_FormatSettingValue(it, v, dst);
    else UI_SlideValue((uint8_t)id, v, dst);
}

/* Label and value text of one row; returns the setting (or NULL). */
static const setting_item_t *row_text(const ui_row_t *row, uint8_t editing, char *label, char *value)
{
    const setting_item_t *it = NULL;
    label[0] = 0;
    value[0] = 0;
    switch (row->kind) {
        case UI_ROW_SETTING:
            it = UI_GetSetting((setting_id_t)row->id);
            if (it == NULL) break;
            strncat(label, UI_SettingName((setting_id_t)row->id), 31U);
            if (UI_SettingIsBandScoped((setting_id_t)row->id)) {
                strcat(label, " [");
                strncat(label, band_name(), 8U);
                strcat(label, "]");
            }
            value_text((setting_id_t)row->id, it, editing ? ui.edit_value : (it->get ? it->get() : 0.0f), value);
            break;
        case UI_ROW_GROUP:
        case UI_ROW_PAGE:
            strncat(label, UI_MenuRowLabel(row), 31U);
            strcpy(value, GFX_SYM_RIGHT);
            break;
        case UI_ROW_ACTION:
        case UI_ROW_HEADER:
        default:
            strncat(label, UI_MenuRowLabel(row), 31U);
            break;
    }
    return it;
}

static void draw_rows(uint8_t cat, uint8_t group, uint16_t x, uint16_t w, uint8_t cursor,
                      uint8_t first, uint8_t preview)
{
    char label[48], value[32];
    ui_row_t row;
    const uint8_t total = UI_MenuRowCount(cat, group);
    for (uint8_t i = 0U; (i < MENU_VISIBLE) && ((uint8_t)(first + i) < total); ++i) {
        const uint8_t index = (uint8_t)(first + i);
        const uint16_t y = (uint16_t)(UI_LIST_Y + i * UI_ROW_H);
        if (!UI_MenuRowAt(cat, group, index, &row)) continue;
        if (row.kind == UI_ROW_HEADER) {
            UI_DrawSection(x, w, y, UI_MenuRowLabel(&row));
            continue;
        }
        const uint8_t selected = (uint8_t)(!preview && (index == cursor));
        (void)row_text(&row, (uint8_t)(selected && ui.editing), label, value);
        ui_row_state_t state = UI_ROW_NORMAL;
        if (preview) state = UI_ROW_DIM;
        else if (selected) state = ui.editing ? UI_ROW_EDIT : UI_ROW_FOCUS;
        UI_DrawRow(x, w, y, state, label, value);
    }
    UI_DrawScrollbar((uint16_t)(x + w + 1U), UI_LIST_Y, (uint16_t)(MENU_VISIBLE * UI_ROW_H), total, first,
                     MENU_VISIBLE);
}

static void draw_categories(void)
{
    const uint8_t count = UI_MenuCategoryCount();
    const uint8_t first = UI_ListFirst(&ui.cat_first, ui.menu_cat, count, MENU_VISIBLE);
    for (uint8_t i = 0U; (i < MENU_VISIBLE) && ((uint8_t)(first + i) < count); ++i) {
        const uint8_t index = (uint8_t)(first + i);
        UI_DrawRow(0U, MENU_LEFT_W, (uint16_t)(UI_LIST_Y + i * UI_ROW_H),
                   (index == ui.menu_cat) ? UI_ROW_FOCUS : UI_ROW_NORMAL,
                   UI_MenuCategoryLabel(UI_MenuCategoryAt(index)), NULL);
    }
    GFX_VLine(99U, UI_LIST_Y, (uint16_t)(MENU_VISIBLE * UI_ROW_H), GFX_LINE);
}

/* ---- Bottom panel ---------------------------------------------------------- */

static void help_panel(const char *text)
{
    GFX_Fill(0U, MENU_HELP_Y, GFX_W, (uint16_t)(UI_FOOTER_Y - MENU_HELP_Y), GFX_PANEL);
    (void)UI_DrawWrapped(GFX_MARGIN, (uint16_t)(MENU_HELP_Y + 6U), (uint16_t)(GFX_W - 2U * GFX_MARGIN), 3U,
                         GFX_DIM, text);
}

static void edit_numeric(setting_id_t id, const setting_item_t *it)
{
    char lo_text[16], hi_text[16], step[40];
    float lo = 0.0f, hi = 0.0f;
    const uint16_t y = (uint16_t)(MENU_HELP_Y + 20U);
    if (UI_SettingRange(id, &lo, &hi) && (hi > lo)) {
        lo_text[0] = 0;
        hi_text[0] = 0;
        value_text(id, it, lo, lo_text);
        value_text(id, it, hi, hi_text);
        (void)GFX_Text(GFX_MARGIN, y, GFX_FONT_SMALL, GFX_MUTED, lo_text, 48U);
        (void)GFX_TextRight((uint16_t)(GFX_W - GFX_MARGIN), y, GFX_FONT_SMALL, GFX_MUTED, hi_text);
        float pos = (ui.edit_value - lo) / (hi - lo);
        if (pos < 0.0f) pos = 0.0f;
        if (pos > 1.0f) pos = 1.0f;
        UI_DrawBar(56U, (uint16_t)(y + 1U), 200U, 10U, (uint32_t)(pos * 1000.0f + 0.5f), 1000U, GFX_ACCENT);
    }
    strcpy(step, "STEP ");
    if (id == SET_REV1_MIC_GAIN) strcat(step, "1 dB");
    else value_text(id, it, UI_SettingStep(it), step + strlen(step));
    strcat(step, "    HOLD+" GFX_SYM_TURN " x10");
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(y + 16U), GFX_FONT_SMALL, GFX_DIM, step, 240U);
}

static void edit_choices(const setting_item_t *it)
{
    char text[5][24];
    uint16_t width[5];
    const int32_t count = UI_SettingDiscreteCount(it->style);
    const int32_t current = (int32_t)(ui.edit_value + 0.5f);
    const uint16_t y = (uint16_t)(MENU_HELP_Y + 22U);
    if (count <= 0) return;
    for (int32_t k = -2; k <= 2; ++k) {
        int32_t i = (current + k) % count;
        if (i < 0) i += count;
        text[k + 2][0] = 0;
        UI_FormatSettingValue(it, (float)i, text[k + 2]);
        width[k + 2] = (uint16_t)((GFX_TextWidth(GFX_FONT_SMALL, text[k + 2]) + 17U) & ~7U);
    }
    /* Current choice as a badge in the middle, neighbours outward while they fit. */
    const uint16_t cx = (uint16_t)((160U - width[2] / 2U) & ~7U);
    (void)GFX_Badge(cx, (uint16_t)(y - 1U), GFX_FONT_SMALL, GFX_ACCENT, text[2]);
    uint16_t left = cx, right = (uint16_t)(cx + width[2] + 8U);
    for (uint8_t k = 1U; (k <= 2U) && (k < (uint8_t)count); ++k) {
        if (left >= GFX_MARGIN + width[2U - k] + 8U) {
            left = (uint16_t)(left - width[2U - k] - 8U);
            (void)GFX_Text(left, y, GFX_FONT_SMALL, GFX_MUTED, text[2U - k], (uint16_t)(left + width[2U - k]));
        }
        if ((k < (uint8_t)(count - 1)) && (right + width[2U + k] <= GFX_W - GFX_MARGIN)) {
            (void)GFX_Text(right, y, GFX_FONT_SMALL, GFX_MUTED, text[2U + k], (uint16_t)(right + width[2U + k]));
            right = (uint16_t)(right + width[2U + k] + 8U);
        }
    }
}

static uint8_t edit_is_live(setting_id_t id)
{
    return (uint8_t)((id == SET_REV1_VOLUME) || (id == SET_REV1_BACKLIGHT) || (id == SET_REV1_MIC_GAIN) ||
                     (id == SET_USB_GAIN) || (id == SET_FM_GAIN) || (id == SET_DRIVE) ||
                     (id == SET_CW_DRIVE) || (id == SET_CW_WPM) || (id == SET_UI_STEP));
}

static void edit_panel(const ui_row_t *row)
{
    const setting_id_t id = (setting_id_t)row->id;
    const setting_item_t *it = UI_GetSetting(id);
    GFX_Fill(0U, MENU_HELP_Y, GFX_W, (uint16_t)(UI_FOOTER_Y - MENU_HELP_Y), GFX_PANEL);
    if (it == NULL) return;
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(MENU_HELP_Y + 4U), GFX_FONT_SMALL, GFX_ACCENT, UI_SettingName(id), 232U);
    (void)GFX_Badge(248U, (uint16_t)(MENU_HELP_Y + 3U), GFX_FONT_SMALL, edit_is_live(id) ? GFX_OK : GFX_MUTED,
                    edit_is_live(id) ? "LIVE" : "ON SET");
    if (UI_SettingIsDiscrete(it->style)) edit_choices(it);
    else edit_numeric(id, it);
}

/* ---- Screen ------------------------------------------------------------------ */

static uint8_t has_sections(uint8_t cat, uint8_t group)
{
    ui_row_t row;
    const uint8_t total = UI_MenuRowCount(cat, group);
    for (uint8_t i = 0U; i < total; ++i)
        if (UI_MenuRowAt(cat, group, i, &row) && (row.kind == UI_ROW_HEADER)) return 1U;
    return 0U;
}

static void menu_title(uint8_t cat, uint8_t group)
{
    char title[40], right[24];
    right[0] = 0;
    if (ui.menu_level == 0U) {
        strcpy(title, "MENU");
        strcpy(right, "SPARTACUS ");
        strcat(right, FW_VERSION_STRING); /* kept whole: version_tool finds it in the image */
        UI_DrawTitle(title, right);
        return;
    }
    strcpy(title, UI_MenuCategoryLabel(cat));
    if (group != (uint8_t)AUDIO_GROUP_NONE) {
        const audio_group_info_t *g = UI_GetAudioGroupInfo(group);
        strcat(title, " " GFX_SYM_RIGHT " ");
        strncat(title, g ? g->title : "?", 20U);
    }
    (void)UI_AppendU(right, (uint32_t)ui.menu_row + 1U, 1U);
    strcat(right, "/");
    (void)UI_AppendU(right + strlen(right), UI_MenuRowCount(cat, group), 1U);
    UI_DrawTitle(title, right);
}

void UI_RenderMenu(uint32_t now)
{
    const uint8_t cat = cur_cat(), group = cur_group();
    ui_row_t row;
    menu_title(cat, group);
    if (ui.menu_level == 0U) {
        draw_categories();
        draw_rows(cat, AUDIO_GROUP_NONE, MENU_RIGHT_X, MENU_RIGHT_W, 0xFFU, 0U, 1U);
        help_panel(UI_MenuCategoryHelp(cat));
        UI_DrawFooter(GFX_SYM_TURN " CATEGORY   " GFX_SYM_PRESS " OPEN   HOLD HOME", NULL, now);
        return;
    }
    const uint8_t total = UI_MenuRowCount(cat, group);
    const uint8_t first = UI_ListFirst(&ui.menu_first, ui.menu_row, total, MENU_VISIBLE);
    draw_rows(cat, group, 0U, MENU_FULL_W, ui.menu_row, first, 0U);
    if (!UI_MenuRowAt(cat, group, ui.menu_row, &row)) {
        help_panel("");
    } else if (ui.editing && (row.kind == UI_ROW_SETTING)) {
        edit_panel(&row);
    } else {
        help_panel(UI_MenuRowHelp(&row));
    }
    if (ui.editing) {
        UI_DrawFooter(GFX_SYM_TURN " ADJUST   " GFX_SYM_PRESS " SET   HOLD CANCEL", NULL, now);
    } else {
        UI_DrawFooter(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " SELECT   HOLD BACK",
                      has_sections(cat, group) ? "HOLD+" GFX_SYM_TURN " SECTION" : NULL, now);
    }
}

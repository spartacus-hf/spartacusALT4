/* Host suite for the landscape menu model (Core/Src/ui_menu.c over the
 * settings map): every setting is reachable by an operator through the
 * categories, not merely listed in some group array; categories fit their
 * pane; the board commissioning pages keep their bring-up order; header rows
 * are never selectable; the debug groups appear only with the debug UI of
 * the diagnostic image. */
#include "ui_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t ui_debug_visible = 0U;
uint8_t cfg_usb_clean_mode = 1U;
ui_state_t ui;

static void fail(const char *message)
{
    fprintf(stderr, "UI menu model: %s\n", message);
    exit(1);
}

static uint8_t reached[SET_COUNT];

/* A linked group's rows as the menu lists them: its settings, or the
 * sections of a composite group (ADVANCED CW). */
static void mark_group(uint8_t group)
{
    if (UI_GetAudioGroupInfo(group) == NULL) fail("a linked group has no info");
    const uint8_t total = UI_MenuRowCount(0U, group);
    if (total == 0U) fail("a linked group is empty");
    for (uint8_t i = 0U; i < total; ++i) {
        ui_row_t row;
        if (!UI_MenuRowAt(0U, group, i, &row)) fail("group row index inside the count is missing");
        if (row.kind == UI_ROW_SETTING) reached[row.id] = 1U;
        if (row.kind != UI_ROW_HEADER && UI_MenuRowHelp(&row)[0] == 0) fail("a group row has no help");
    }
}

static void walk_categories(void)
{
    memset(reached, 0, sizeof(reached));
    for (uint8_t v = 0U; v < UI_MenuCategoryCount(); ++v) {
        const uint8_t cat = UI_MenuCategoryAt(v);
        const uint8_t total = UI_MenuRowCount(cat, AUDIO_GROUP_NONE);
        uint8_t selectable = 0U;
        if (strlen(UI_MenuCategoryLabel(cat)) > 10U) fail("a category label does not fit its pane");
        if (UI_MenuCategoryHelp(cat)[0] == 0) fail("a category has no help");
        for (uint8_t i = 0U; i < total; ++i) {
            ui_row_t row;
            if (!UI_MenuRowAt(cat, AUDIO_GROUP_NONE, i, &row)) fail("row index inside the count is missing");
            if (row.kind == UI_ROW_SETTING) reached[row.id] = 1U;
            if (row.kind == UI_ROW_GROUP) mark_group((uint8_t)row.id);
            if (row.kind != UI_ROW_HEADER) selectable++;
            /* The cursor never rests on a header. */
            ui_row_t landed;
            (void)UI_MenuRowAt(cat, AUDIO_GROUP_NONE, UI_MenuSelectable(cat, AUDIO_GROUP_NONE, i, 1), &landed);
            if ((landed.kind == UI_ROW_HEADER) && (i + 1U < total)) fail("selectable search stopped on a header");
            if (row.kind != UI_ROW_HEADER && UI_MenuRowHelp(&row)[0] == 0) fail("a row has no help");
        }
        if (selectable == 0U) fail("a category has nothing to select");
        ui_row_t first;
        (void)UI_MenuRowAt(cat, AUDIO_GROUP_NONE, UI_MenuSelectable(cat, AUDIO_GROUP_NONE, 0U, 1), &first);
        if (first.kind == UI_ROW_HEADER) fail("a category opens on a header");
    }
    if (UI_MenuRowCount(0U, AUDIO_GROUP_NONE) == 0U) fail("OPERATE is empty");
    /* The Morse trainer's hub opens these two (ui_trainer.c). */
    mark_group(AUDIO_GROUP_CW_TRAIN);
    mark_group(AUDIO_GROUP_TRAIN_ADVANCED);
}

/* Settings the operator reaches another way, or that this image omits. */
static uint8_t exempt(setting_id_t id)
{
    switch (id) {
        case SET_FREQ:  /* FREQUENCY ENTRY page and the knob */
        case SET_TRAIN_OPEN: /* the CW category's TRAINER page row; the console's id */
            return 1U;
        /* The v7 engineering line, retired: the DIAGNOSTICS pages show these
         * readings in words. The console still reaches the settings. */
        case SET_HOME_LAYOUT: case SET_HOME_GAINS: case SET_HOME_HEALTH: case SET_HOME_RFEN: case SET_HOME_VREF:
            return 1U;
#if !MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
        case SET_SSB_BIAS: case SET_SSB_CE: case SET_CE_MIX: case SET_CE_FLOOR: case SET_CE_KNEE:
            return 1U;
#endif
#if !MOD_CESSB_EXPERIMENT_ENABLE
        case SET_CESSB: case SET_CESSB_CLIP: case SET_CESSB_ITER:
            return 1U;
#endif
        default:
            break;
    }
    if (ui_debug_visible) return 0U;
    /* Without the debug UI, the DSP-engine and debug groups are console-only. */
    const audio_group_info_t *dbg = UI_GetAudioGroupInfo(AUDIO_GROUP_DEBUG);
    const audio_group_info_t *dsp = UI_GetAudioGroupInfo(AUDIO_GROUP_DSP_ENGINE);
    for (uint8_t i = 0U; i < dbg->count; ++i) if (dbg->items[i] == id) return 1U;
    for (uint8_t i = 0U; i < dsp->count; ++i) if (dsp->items[i] == id) return 1U;
    return 0U;
}

static void verify_reachable(void)
{
    unsigned orphans = 0U;
    walk_categories();
    for (uint32_t id = 0U; id < (uint32_t)SET_COUNT; ++id) {
        if (reached[id] || exempt((setting_id_t)id)) continue;
        fprintf(stderr, "setting_id_t %u is not reachable from the menu\n", (unsigned)id);
        orphans++;
    }
    if (orphans) fail("settings exist that the menu cannot reach");
}

static void verify_board(void)
{
    static const uint8_t order[7] = {
        AUDIO_GROUP_BOARD_ASSEMBLY, AUDIO_GROUP_BOARD_CONTROLS, AUDIO_GROUP_BOARD_FILTERS,
        AUDIO_GROUP_BOARD_BIAS, AUDIO_GROUP_BOARD_RXTRIM, AUDIO_GROUP_BOARD_POWER, AUDIO_GROUP_BOARD_CAL
    };
    if (UI_MenuRowCount(UI_CAT_BOARD, AUDIO_GROUP_NONE) != 7U) fail("BOARD must list the seven commissioning pages");
    for (uint8_t i = 0U; i < 7U; ++i) {
        ui_row_t row;
        (void)UI_MenuRowAt(UI_CAT_BOARD, AUDIO_GROUP_NONE, i, &row);
        if ((row.kind != UI_ROW_GROUP) || (row.id != order[i])) fail("BOARD pages out of bring-up order");
        if (UI_MenuRowCount(UI_CAT_BOARD, order[i]) == 0U) fail("an empty board page");
    }
}

static void verify_operate(void)
{
    /* The everyday controls are the first things in the first category. */
    static const setting_id_t head[] = {SET_UI_MODE, SET_UI_BAND, SET_UI_STEP, SET_REV1_VOLUME, SET_MUTE};
    for (uint8_t i = 0U; i < sizeof(head) / sizeof(head[0]); ++i) {
        ui_row_t row;
        (void)UI_MenuRowAt(UI_CAT_OPERATE, AUDIO_GROUP_NONE, i, &row);
        if ((row.kind != UI_ROW_SETTING) || (row.id != head[i])) fail("OPERATE does not start with the everyday controls");
    }
    /* DIAGNOSTICS is in SYSTEM, where ABOUT used to be. */
    uint8_t found = 0U;
    for (uint8_t i = 0U; i < UI_MenuRowCount(UI_CAT_SYSTEM, AUDIO_GROUP_NONE); ++i) {
        ui_row_t r;
        (void)UI_MenuRowAt(UI_CAT_SYSTEM, AUDIO_GROUP_NONE, i, &r);
        if ((r.kind == UI_ROW_PAGE) && (r.id == PAGE_DIAG)) found = 1U;
    }
    if (!found) fail("SYSTEM does not open DIAGNOSTICS");
    /* Rotation is the first DISPLAY row. */
    ui_row_t row;
    (void)UI_MenuRowAt(UI_CAT_DISPLAY, AUDIO_GROUP_NONE, 0U, &row);
    if ((row.kind != UI_ROW_SETTING) || (row.id != SET_UI_ROTATION)) fail("SCREEN ROTATION is not first in DISPLAY");
}

int main(void)
{
    ui_debug_visible = 0U;
    if (UI_MenuCategoryCount() != (uint8_t)UI_CAT_DIAG) fail("DIAG category visible without the debug UI");
    verify_reachable();
    verify_board();
    verify_operate();
    ui_debug_visible = 1U;
    if (UI_MenuCategoryCount() != (uint8_t)UI_CAT_COUNT) fail("DIAG category missing with the debug UI");
    verify_reachable();
    printf("UI menu model: %u categories (+DIAG), every setting reachable, board order kept: PASS\n",
           (unsigned)UI_CAT_DIAG);
    return 0;
}

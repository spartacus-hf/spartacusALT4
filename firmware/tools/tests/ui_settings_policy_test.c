#include "ui_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t ui_debug_visible = 0U;
uint8_t cfg_usb_clean_mode = 1U;

static void fail(const char *message)
{
    fprintf(stderr, "UI settings policy: %s\n", message);
    exit(1);
}

static uint8_t group_has(uint8_t group, setting_id_t wanted)
{
    const audio_group_info_t *info = UI_GetAudioGroupInfo(group);
    if (info == NULL) return 0U;
    for (uint8_t index = 0U; index < info->count; index++) {
        if (info->items[index] == wanted) return 1U;
    }
    return 0U;
}

static void verify_analytic_choices(void)
{
    ui_debug_visible = 0U;
    if (UI_SsbAnalyticChoiceAt(0U) != SSB_ANALYTIC_WEAVER) {
        fail("Weaver is not the first normal analytic choice");
    }
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    if (UI_SsbAnalyticChoiceCount() != 2U ||
        UI_SsbAnalyticChoiceAt(1U) != SSB_ANALYTIC_H12P) {
        fail("normal H12P build must show only Weaver and H12P");
    }
#else
    if (UI_SsbAnalyticChoiceCount() != 1U) {
        fail("H12P-disabled build must show only Weaver");
    }
#endif
    if (UI_SsbAnalyticChoiceAt(255U) != SSB_ANALYTIC_WEAVER) {
        fail("out-of-range analytic choice must fall back to Weaver");
    }

    ui_debug_visible = 1U;
#if MOD_DEEP_DIAG_ENABLE
    if (UI_SsbAnalyticChoiceCount() !=
        (uint8_t)(MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE ? 5U : 4U)) {
        fail("diagnostic analytic choice count is inconsistent");
    }
    if (UI_SsbAnalyticChoiceAt(1U) != SSB_ANALYTIC_HILBERT ||
        UI_SsbAnalyticChoiceAt(2U) != SSB_ANALYTIC_HILBERT_PAIR_24 ||
        UI_SsbAnalyticChoiceAt(3U) != SSB_ANALYTIC_HILBERT_PAIR_12) {
        fail("legacy Hilbert choices are not diagnostic-only in the expected order");
    }
#else
    if (UI_SsbAnalyticChoiceAt(1U) !=
        (MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE ? SSB_ANALYTIC_H12P
                                            : SSB_ANALYTIC_WEAVER)) {
        fail("normal build exposed a deep-diagnostic Hilbert backend");
    }
#endif
}

static void verify_group_topology(void)
{
    static const uint8_t expected_modulation[] = {
        AUDIO_GROUP_SSB,
        AUDIO_GROUP_FM,
        AUDIO_GROUP_AM,
        AUDIO_GROUP_USB_INPUT
    };
    ui_debug_visible = 0U;
    if (UI_GetModulationGroupVisibleCount() !=
        (uint8_t)(sizeof(expected_modulation) / sizeof(expected_modulation[0]))) {
        fail("normal modulation group count changed");
    }
    for (uint8_t index = 0U; index < sizeof(expected_modulation); index++) {
        if (UI_GetModulationGroupAt(index) != expected_modulation[index]) {
            fail("normal modulation groups are not ordered SSB/FM/AM/USB");
        }
    }
    if (UI_GetModulationGroupAt(255U) != AUDIO_GROUP_SSB) {
        fail("out-of-range modulation group must fall back to SSB");
    }
    if (!group_has(AUDIO_GROUP_SSB, SET_SIDEBAND) ||
        !group_has(AUDIO_GROUP_SSB, SET_SSB_ANA) ||
        !group_has(AUDIO_GROUP_SSB, SET_TX_BW) ||
        !group_has(AUDIO_GROUP_SSB, SET_GATE) ||
        !group_has(AUDIO_GROUP_SSB, SET_OUTPH_SSB_CAP)) {
        fail("SSB group is missing an operator-critical control");
    }
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
    if (!group_has(AUDIO_GROUP_SSB, SET_SSB_BIAS) ||
        !group_has(AUDIO_GROUP_SSB, SET_SSB_CE)) {
        fail("diagnostic nonlinear-envelope controls are missing");
    }
#else
    if (group_has(AUDIO_GROUP_SSB, SET_SSB_BIAS) ||
        group_has(AUDIO_GROUP_SSB, SET_SSB_CE)) {
        fail("normal build exposed nonlinear-envelope experiments");
    }
#endif
    if (strstr(UI_SettingHelp(SET_SSB_ANA), "H12P EXP") == NULL ||
        strstr(UI_SettingHelp(SET_SSB_ANA), "stable default") == NULL) {
        fail("analytic help does not distinguish Weaver from H12P EXP");
    }
}


/* Settings are only reachable through a group array. A setting that is in the
 * table but in no group is invisible to the operator, which is exactly how the
 * old "SEL PULLUP" entry rotted, and how the whole REV1 board group became
 * unreachable once it lost its menu entry. */
static void verify_every_setting_is_reachable(void)
{
    static const setting_id_t exempt[] = {
        SET_FREQ, /* edited on the BAND page */
        SET_MUTE, /* toggled on the SYSTEM page */
        SET_TRAIN_OPEN, /* the CW category's TRAINER page row; kept for the console */
        /* v7 engineering line, retired in v8.01 for the DIAGNOSTICS pages;
         * still read and written by the console and settings storage. */
        SET_HOME_LAYOUT, SET_HOME_GAINS, SET_HOME_HEALTH, SET_HOME_RFEN, SET_HOME_VREF,
#if !MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
        /* Compiled out of group_ssb in this build; verify_group_topology
         * separately asserts they are absent. */
        SET_SSB_BIAS, SET_SSB_CE, SET_CE_MIX, SET_CE_FLOOR, SET_CE_KNEE,
#endif
#if !MOD_CESSB_EXPERIMENT_ENABLE
        SET_CESSB, SET_CESSB_CLIP, SET_CESSB_ITER,
#endif
    };
    unsigned orphans = 0U;
    for (uint32_t id = 0U; id < (uint32_t)SET_COUNT; id++) {
        uint8_t reachable = 0U;
        for (uint8_t group = 0U; group < (uint8_t)AUDIO_GROUP_COUNT; group++) {
            if (group_has(group, (setting_id_t)id)) { reachable = 1U; break; }
        }
        for (uint8_t n = 0U; !reachable && n < sizeof(exempt) / sizeof(exempt[0]); n++) {
            if (exempt[n] == (setting_id_t)id) reachable = 1U;
        }
        if (!reachable) {
            /* UI_GetSetting is compiled out of the policy-only build, so the
             * id is the handle: look it up in setting_id_t. */
            fprintf(stderr, "unreachable setting_id_t index %u\n", (unsigned)id);
            orphans++;
        }
    }
    if (orphans) fail("settings exist that no group or page can reach");
}

/* The REV1 board block is reached from its own menu entry, as seven pages. */
static void verify_board_groups(void)
{
    static const uint8_t expected[] = {
        AUDIO_GROUP_BOARD_ASSEMBLY, AUDIO_GROUP_BOARD_CONTROLS, AUDIO_GROUP_BOARD_FILTERS,
        AUDIO_GROUP_BOARD_BIAS, AUDIO_GROUP_BOARD_RXTRIM, AUDIO_GROUP_BOARD_POWER,
        AUDIO_GROUP_BOARD_CAL
    };
    const uint8_t count = (uint8_t)(sizeof(expected) / sizeof(expected[0]));
    if (UI_GetBoardGroupVisibleCount() != count) fail("board group count changed");
    if (UI_GroupListCount(UI_AUDIO_SCOPE_BOARD) != count) fail("board scope count disagrees");
    for (uint8_t i = 0U; i < count; i++) {
        if (UI_GetBoardGroupAt(i) != expected[i]) fail("board groups are out of bring-up order");
        if (UI_GroupListAt(UI_AUDIO_SCOPE_BOARD, i) != expected[i]) fail("board scope list disagrees");
        const audio_group_info_t *info = UI_GetAudioGroupInfo(expected[i]);
        if (info == NULL || info->count == 0U) fail("a board group is empty");
        if (strlen(info->title) > 16U) fail("a board group title exceeds the 16-char column");
    }
    if (UI_GetBoardGroupAt(255U) != AUDIO_GROUP_BOARD_ASSEMBLY) {
        fail("out-of-range board group must fall back to ASSEMBLY");
    }

    /* The operator-critical commissioning controls must stay where the bench
     * procedure says they are. */
    if (!group_has(AUDIO_GROUP_BOARD_ASSEMBLY, SET_REV1_DRIVER_A) ||
        !group_has(AUDIO_GROUP_BOARD_ASSEMBLY, SET_REV1_DRAIN_A) ||
        !group_has(AUDIO_GROUP_BOARD_ASSEMBLY, SET_REV1_DRAIN_B) ||
        !group_has(AUDIO_GROUP_BOARD_ASSEMBLY, SET_REV1_QUAL) ||
        !group_has(AUDIO_GROUP_BOARD_ASSEMBLY, SET_REV1_CONFIRM)) {
        fail("ASSEMBLY is missing a control TX depends on");
    }
    if (!group_has(AUDIO_GROUP_BOARD_CONTROLS, SET_REV1_ENC_A) ||
        !group_has(AUDIO_GROUP_BOARD_CONTROLS, SET_REV1_ENC_B) ||
        !group_has(AUDIO_GROUP_BOARD_CONTROLS, SET_REV1_ENC_SW) ||
        !group_has(AUDIO_GROUP_BOARD_CONTROLS, SET_REV1_SW4) ||
        !group_has(AUDIO_GROUP_BOARD_CONTROLS, SET_REV1_SW5)) {
        fail("CONTROLS is missing a panel input");
    }

    /* The old flat group is retired: it must not appear in a list any more. */
    ui_debug_visible = 0U;
    for (uint8_t i = 0U; i < UI_GetAudioGroupVisibleCount(); i++) {
        if (UI_GetAudioGroupAt(i) == AUDIO_GROUP_BOARD) fail("retired board group is still listed");
    }
    ui_debug_visible = 1U;
    for (uint8_t i = 0U; i < UI_GetAudioGroupVisibleCount(); i++) {
        if (UI_GetAudioGroupAt(i) == AUDIO_GROUP_BOARD) fail("retired board group is still listed");
    }
    ui_debug_visible = 0U;
}

int main(void)
{
    verify_analytic_choices();
    verify_group_topology();
    verify_every_setting_is_reachable();
    verify_board_groups();
    printf("UI settings policy: h12p=%u deep=%u analytic choices=%u\n",
           (unsigned)MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE,
           (unsigned)MOD_DEEP_DIAG_ENABLE,
           (unsigned)UI_SsbAnalyticChoiceCount());
    return 0;
}

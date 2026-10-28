#include "ui_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t ui_debug_visible = 0U;
uint8_t cfg_usb_clean_mode; /* ui_help.c: the mode help comes from the card's HELP.BIN */

static void require_equal_u8(uint8_t actual, uint8_t expected, const char *what)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %u, expected %u\n",
                what, (unsigned)actual, (unsigned)expected);
        exit(1);
    }
}

static void require_mode(uint8_t index, mod_config_t expected, const char *what)
{
    mod_config_t actual = UI_GetProdModeAt(index);
    if (actual != expected) {
        fprintf(stderr, "%s: got %d, expected %d\n",
                what, (int)actual, (int)expected);
        exit(1);
    }
}

static void verify_normal_operator_list(void)
{
    static const mod_config_t expected[] = {
        MOD_CFG_PROD_SSB_14MHZ,
        MOD_CFG_FM_NARROW_14MHZ,
        MOD_CFG_AM_14MHZ,
        MOD_CFG_CW_14MHZ
    };

    ui_debug_visible = 0U;
    require_equal_u8(UI_GetProdModeCount(),
                     (uint8_t)(sizeof(expected) / sizeof(expected[0])),
                     "normal operating-mode count");
    for (uint8_t index = 0U;
         index < (uint8_t)(sizeof(expected) / sizeof(expected[0]));
         index++) {
        require_mode(index, expected[index], "normal operating-mode order");
        require_equal_u8(UI_FindProdMode(expected[index]), index,
                         "normal operating-mode lookup");
    }
    require_mode(255U, expected[0], "out-of-range mode fallback");
    if (strstr(UI_ProdModeHelp(expected[0]), "H12P EXP") == NULL) {
        fputs("SSB help must identify H12P as experimental\n", stderr);
        exit(1);
    }
}

static void verify_diagnostic_visibility(void)
{
    ui_debug_visible = 1U;
#if MOD_DEEP_DIAG_ENABLE
    require_equal_u8(UI_GetProdModeCount(), 9U, "diagnostic operating-mode count");
    require_mode(0U, MOD_CFG_BASELINE_27MHZ, "diagnostic baseline mode");
    require_mode(1U, MOD_CFG_PROD_SSB_14MHZ, "diagnostic production SSB mode");
    require_mode(8U, MOD_CFG_CW_14MHZ, "diagnostic final mode");
#else
    require_equal_u8(UI_GetProdModeCount(), 4U,
                     "normal build cannot expose diagnostic transmitter modes");
    require_mode(0U, MOD_CFG_PROD_SSB_14MHZ,
                 "normal build debug UI retains production mode list");
#endif
}

int main(void)
{
    verify_normal_operator_list();
    verify_diagnostic_visibility();
    printf("UI operating-mode policy: deep=%u normal=4 diagnostic=%u\n",
           (unsigned)MOD_DEEP_DIAG_ENABLE,
           (unsigned)(MOD_DEEP_DIAG_ENABLE ? 9U : 4U));
    return 0;
}

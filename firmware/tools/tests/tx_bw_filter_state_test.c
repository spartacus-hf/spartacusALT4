#include "mod_dsp_tables_priv.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *message)
{
    fprintf(stderr, "TX bandwidth filter state: %s\n", message);
    exit(1);
}

int main(void)
{
    /*
     * The runtime preset selector can return the SPEECH_* cascades.  Every
     * per-path state array therefore needs capacity for both the dedicated
     * TX_BW_* tables and the longer speech tables.
     */
    if (TX_BW_HPF_STATE_SECTIONS < TX_BW_HPF_SECTIONS ||
        TX_BW_HPF_STATE_SECTIONS < SPEECH_HPF_SECTIONS) {
        fail("HPF state capacity is smaller than a selectable cascade");
    }
    if (TX_BW_LPF_STATE_SECTIONS < TX_BW_LPF_SECTIONS ||
        TX_BW_LPF_STATE_SECTIONS < SPEECH_LPF_SECTIONS) {
        fail("LPF state capacity is smaller than a selectable cascade");
    }

    for (uint32_t raw = 0U; raw < (uint32_t)TX_BW_COUNT; raw++) {
        tx_bw_preset_t preset = (tx_bw_preset_t)raw;
        if (mod_tx_bw_hpf_section_count(preset) > TX_BW_HPF_STATE_SECTIONS ||
            mod_tx_bw_lpf_section_count(preset) > TX_BW_LPF_STATE_SECTIONS) {
            fail("runtime preset exceeds its state capacity");
        }
    }
    if (mod_tx_bw_hpf_section_count((tx_bw_preset_t)UINT32_MAX) >
            TX_BW_HPF_STATE_SECTIONS ||
        mod_tx_bw_lpf_section_count((tx_bw_preset_t)UINT32_MAX) >
            TX_BW_LPF_STATE_SECTIONS) {
        fail("invalid preset fallback exceeds its state capacity");
    }

    if (mod_tx_bw_lpf_section_count(TX_BW_100_3000) != TX_BW_LPF_SECTIONS ||
        mod_tx_bw_lpf_section_count(TX_BW_200_2900) != TX_BW_LPF_SECTIONS ||
        mod_tx_bw_lpf_section_count(TX_BW_300_2400) != SPEECH_LPF_SECTIONS ||
        mod_tx_bw_lpf_section_count(TX_BW_300_2700) != SPEECH_LPF_SECTIONS) {
        fail("runtime LPF cascade selection changed");
    }

    printf("TX bandwidth filter state: HPF=%u LPF=%u sections\n",
           (unsigned)TX_BW_HPF_STATE_SECTIONS,
           (unsigned)TX_BW_LPF_STATE_SECTIONS);
    return 0;
}

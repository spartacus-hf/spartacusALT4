#ifndef MOD_DSP_TABLES_PRIV_H
#define MOD_DSP_TABLES_PRIV_H

#include "mod_priv.h"

/* Filter/table sizes shared by DSP state and immutable coefficient tables. */
#define SPEECH_HPF_SECTIONS     1U
#define SPEECH_LPF_SECTIONS     2U
#define WEAVER_LPF_SECTIONS     3U
#define ENH_HPF_SECTIONS        1U
#define ENH_LPF_SECTIONS        1U
#define WEAVER_ENH_LPF_SECTIONS 3U
#define TX_BW_HPF_SECTIONS      1U
#define TX_BW_LPF_SECTIONS      1U
#define QMX12_AA_LPF_SECTIONS   3U
#define FM_LPF_SECTIONS         2U

/*
 * TX bandwidth selection reuses either the dedicated TX_BW_* tables or the
 * longer SPEECH_* cascades.  State belongs to a signal path, not a coefficient
 * table, so it must be sized for the longest selectable cascade.
 */
#define TX_BW_HPF_STATE_SECTIONS \
    ((TX_BW_HPF_SECTIONS > SPEECH_HPF_SECTIONS) ? \
     TX_BW_HPF_SECTIONS : SPEECH_HPF_SECTIONS)
#define TX_BW_LPF_STATE_SECTIONS \
    ((TX_BW_LPF_SECTIONS > SPEECH_LPF_SECTIONS) ? \
     TX_BW_LPF_SECTIONS : SPEECH_LPF_SECTIONS)

_Static_assert(TX_BW_HPF_STATE_SECTIONS >= TX_BW_HPF_SECTIONS,
               "TX HPF state must fit dedicated coefficients");
_Static_assert(TX_BW_HPF_STATE_SECTIONS >= SPEECH_HPF_SECTIONS,
               "TX HPF state must fit speech coefficients");
_Static_assert(TX_BW_LPF_STATE_SECTIONS >= TX_BW_LPF_SECTIONS,
               "TX LPF state must fit dedicated coefficients");
_Static_assert(TX_BW_LPF_STATE_SECTIONS >= SPEECH_LPF_SECTIONS,
               "TX LPF state must fit speech coefficients");

static inline uint32_t mod_tx_bw_hpf_section_count(tx_bw_preset_t preset)
{
    switch (preset) {
        case TX_BW_100_3000:
        case TX_BW_200_2900:
            return TX_BW_HPF_SECTIONS;
        case TX_BW_300_2400:
        case TX_BW_300_2700:
        default:
            return SPEECH_HPF_SECTIONS;
    }
}

static inline uint32_t mod_tx_bw_lpf_section_count(tx_bw_preset_t preset)
{
    switch (preset) {
        case TX_BW_100_3000:
        case TX_BW_200_2900:
            return TX_BW_LPF_SECTIONS;
        case TX_BW_300_2400:
        case TX_BW_300_2700:
        default:
            return SPEECH_LPF_SECTIONS;
    }
}

#define HILB_NUM_TAPS 255U
#define HILB_GROUP_DELAY ((HILB_NUM_TAPS - 1U) / 2U)
#define HILB_PAIR24_NUM_TAPS 97U
#define HILB_PAIR24_GROUP_DELAY ((HILB_PAIR24_NUM_TAPS - 1U) / 2U)
#define HILB_PAIR12_NUM_TAPS 49U
#define HILB_PAIR12_GROUP_DELAY ((HILB_PAIR12_NUM_TAPS - 1U) / 2U)
#define HILB_PAIR24_DECIM 2U
#define HILB_PAIR12_DECIM 4U
#define INV_SQRT2_F 0.70710678118f

#define DC_BLOCK_SECTIONS 1U

extern const BiquadCoeffs speech_hpf_std[SPEECH_HPF_SECTIONS];
extern const BiquadCoeffs speech_lpf_std[SPEECH_LPF_SECTIONS];
extern const BiquadCoeffs speech_lpf_narrow[SPEECH_LPF_SECTIONS];
extern const BiquadCoeffs weaver_lpf_prod[WEAVER_LPF_SECTIONS];
extern const BiquadCoeffs weaver_lpf_raw_legacy[WEAVER_LPF_SECTIONS];
extern const BiquadCoeffs weaver_lpf_enh[WEAVER_ENH_LPF_SECTIONS];
extern const BiquadCoeffs weaver_lpf_enh_12k[WEAVER_ENH_LPF_SECTIONS];
extern const BiquadCoeffs enh_hpf_100hz[ENH_HPF_SECTIONS];
extern const BiquadCoeffs enh_lpf_3000hz[ENH_LPF_SECTIONS];
extern const BiquadCoeffs speech_hpf_200hz[TX_BW_HPF_SECTIONS];
extern const BiquadCoeffs speech_lpf_2900hz[TX_BW_LPF_SECTIONS];
extern const BiquadCoeffs qmx12_aa_lpf[QMX12_AA_LPF_SECTIONS];
extern const BiquadCoeffs fm_lpf_3000hz[FM_LPF_SECTIONS];
extern const float hilb_coeffs[HILB_NUM_TAPS];
extern const float hilb_pair24_coeffs[HILB_PAIR24_NUM_TAPS];
extern const float hilb_pair12_coeffs[HILB_PAIR12_NUM_TAPS];
extern const BiquadCoeffs dc_block[DC_BLOCK_SECTIONS];

#endif /* MOD_DSP_TABLES_PRIV_H */

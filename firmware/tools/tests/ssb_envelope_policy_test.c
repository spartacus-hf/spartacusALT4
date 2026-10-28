#include "mod_ssb_envelope_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
#error "The production host-test flavor must use the direct SSB envelope law"
#endif

static float aggressive_qrp_request(float envelope, float low_cut)
{
    return mod_ssb_theta_envelope_map(envelope, low_cut,
                                      0.16f, 1U, 0.55f, 0.090f, 0.62f);
}

int main(void)
{
    static const float envelopes[] = {
        0.005f, 0.010f, 0.020f, 0.050f, 0.100f, 0.200f, 0.500f, 0.900f
    };

    if (mod_ssb_phase_bias_for_build(0.35f) != 0.0f) return 1;
    if (mod_ssb_ce_enable_for_build(UINT8_MAX) != 0U) return 1;
    for (uint32_t i = 0U; i < sizeof(envelopes) / sizeof(envelopes[0]); i++) {
        if (aggressive_qrp_request(envelopes[i], 0.0f) != envelopes[i]) return 1;
    }
    if (aggressive_qrp_request(0.004f, 0.0045f) != 0.0f) return 1;
    if (aggressive_qrp_request(0.010f, 0.0045f) != 0.010f) return 1;
    if (aggressive_qrp_request(2.0f, 0.0f) != 0.999f) return 1;
    if (aggressive_qrp_request(NAN, 0.0f) != 0.0f) return 1;
    if (aggressive_qrp_request(INFINITY, 0.0f) != 0.999f) return 1;
    if (aggressive_qrp_request(-INFINITY, 0.0f) != 0.0f) return 1;
    puts("SSB envelope policy: production magnitude law remains direct: PASS");
    return 0;
}

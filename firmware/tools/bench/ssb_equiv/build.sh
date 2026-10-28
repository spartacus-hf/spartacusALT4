#!/usr/bin/env bash
# build.sh <Core dir: Src/ and Inc/> <out-binary>
# Host build of the TX pipeline from one source tree plus the equivalence
# harness (equiv_main.c). harness_state() is appended to a copy of the
# pipeline so the dump can see the renderers' file-static state. The CORDIC
# is off on the host (its software fallback is the same law).
set -eu
H=$(cd "$(dirname "$0")" && pwd); T=$1; OUT=$2
A=$(cd "$H/../../.." && pwd)              # alt2/: HAL and CMSIS headers
P="$(dirname "$OUT")/pipe_$(basename "$OUT").c"
sed -e 's/__DMB();/__sync_synchronize();/' -e 's/^#define USE_CORDIC  *1$/#define USE_CORDIC 0/' "$T/Src/mod_tx_pipeline.c" > "$P"
cat >> "$P" <<'EOF'

/* ---- harness accessor (appended by build_pair.sh) ---- */
void harness_state(float *o, uint32_t *u);
void harness_state(float *o, uint32_t *u)
{
    o[0] = live_comp_gain_db; o[1] = live_alc_gain_db; o[2] = live_mic_agc_gain_db;
    o[3] = mute_gain; o[4] = osc_re; o[5] = osc_im; o[6] = rf_en_follow_metric;
    o[7] = comp_gain; o[8] = mic_agc_gain; o[9] = NG_GAIN; o[10] = alc_state.gain;
    o[11] = gate_prod.last_phi; o[12] = prod_phase_state.gate_fade;
    o[13] = mic_agc_env; o[14] = NG_ENV;
    o[15] = weaver_i_state[2].z1; o[16] = weaver_q_state[2].z2;
    o[17] = txbw_lpf_state[0].z1; o[18] = dc_block_prod_state.z1; o[19] = eq_treble_state.z2;
    u[0] = live_gate_active; u[1] = gate_prod.open; u[2] = gate_prod.hang;
    u[3] = NG_OPEN; u[4] = NG_HANG; u[5] = alc_state.index;
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    const mod_analytic12_state_t *a12 = &hilbert_workspace.production12;
    o[20] = gate_ext.last_phi; o[21] = ext_phase_state.gate_fade; o[22] = dc_block_ext_state.z1;
    o[23] = a12->interp_i_hist[3]; o[24] = a12->interp_q_hist[7]; o[25] = a12->hilbert_hist[100];
    u[6] = gate_ext.open; u[7] = gate_ext.hang; u[8] = a12->decim_wr; u[9] = a12->hilbert_wr;
    u[10] = a12->interp_wr; u[11] = a12->decim_phase;
#endif
}
EOF
if grep -q "static noise_gate_state_t ng" "$P"; then
  sed -i -e 's/NG_GAIN/ng.gain/; s/NG_ENV/ng.env/; s/NG_OPEN/ng.open/; s/NG_HANG/ng.hang/' "$P"
else
  sed -i -e 's/NG_GAIN/ng_gain/; s/NG_ENV/ng_env/; s/NG_OPEN/ng_open/; s/NG_HANG/ng_hang/' "$P"
fi
cc -std=gnu11 -O2 -DSTM32G474xx -DUSE_HAL_DRIVER -DMOD_HOST_TEST_CRITICAL_STUBS=1 \
   -Wno-int-to-pointer-cast -ffunction-sections -fdata-sections -Wl,--gc-sections \
   -I "$T/Inc" -I "$A/Drivers/STM32G4xx_HAL_Driver/Inc" \
   -I "$A/Drivers/CMSIS/Device/ST/STM32G4xx/Include" -I "$A/Drivers/CMSIS/Include" \
   "$P" "$H/equiv_main.c" "$H/stubs.c" "$T/Src/mod_state.c" "$T/Src/mod_outphase.c" \
   "$T/Src/mod_transport_logic.c" "$T/Src/mod_dsp_tables.c" -lm -o "$OUT"

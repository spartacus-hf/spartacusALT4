#include "mod_analytic_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE_HZ       48000U
#define TEST_PI              3.14159265358979323846
#define WARMUP_SAMPLES        4096U
#define MEASURE_SAMPLES      48000U
#define IMAGE_REJECTION_DB      60.0
#define PASSBAND_RIPPLE_DB       0.10
#define SIDEBAND_MATCH_DB        0.001
#define ALIAS_REJECTION_DB      80.0
#define MULTITONE_REJECTION_DB  80.0
#define CONTINUITY_SAMPLES      4096U

typedef struct {
    double desired;
    double image;
    double image_rejection_db;
} tone_result_t;

static double frame_boundary_error;

static void fail(const char *message, int32_t frequency_hz, double actual, double limit)
{
    fprintf(stderr, "FAIL analytic reference: %s at %ld Hz: %.6f (limit %.6f)\n",
            message, (long)frequency_hz, actual, limit);
    exit(1);
}

static tone_result_t measure_tone(int32_t frequency_hz, float sideband_sign)
{
    mod_analytic12_state_t st;
    mod_analytic12_reset(&st);
    double desired_re = 0.0;
    double desired_im = 0.0;
    double image_re = 0.0;
    double image_im = 0.0;
    uint32_t total = WARMUP_SAMPLES + MEASURE_SAMPLES;

    for (uint32_t n = 0U; n < total; n++) {
        double phase = 2.0 * TEST_PI * (double)frequency_hz * (double)n / (double)SAMPLE_RATE_HZ;
        float i_sig;
        float q_sig;
        mod_analytic12_process(&st, (float)cos(phase), sideband_sign, &i_sig, &q_sig);
        if (n < WARMUP_SAMPLES) continue;

        double desired_phase = (double)sideband_sign * phase;
        double c = cos(desired_phase);
        double s = sin(desired_phase);
        desired_re += (double)i_sig * c + (double)q_sig * s;
        desired_im += (double)q_sig * c - (double)i_sig * s;
        image_re += (double)i_sig * c - (double)q_sig * s;
        image_im += (double)q_sig * c + (double)i_sig * s;
    }

    tone_result_t result;
    result.desired = hypot(desired_re, desired_im) / (double)MEASURE_SAMPLES;
    result.image = hypot(image_re, image_im) / (double)MEASURE_SAMPLES;
    result.image_rejection_db = 20.0 * log10(result.desired / result.image);
    return result;
}

static double measure_output_at(int32_t input_hz, int32_t output_hz)
{
    mod_analytic12_state_t st;
    mod_analytic12_reset(&st);
    double acc_re = 0.0;
    double acc_im = 0.0;
    uint32_t total = WARMUP_SAMPLES + MEASURE_SAMPLES;

    for (uint32_t n = 0U; n < total; n++) {
        double in_phase = 2.0 * TEST_PI * (double)input_hz * (double)n / (double)SAMPLE_RATE_HZ;
        float i_sig;
        float q_sig;
        mod_analytic12_process(&st, (float)cos(in_phase), 1.0f, &i_sig, &q_sig);
        if (n < WARMUP_SAMPLES) continue;
        double out_phase = 2.0 * TEST_PI * (double)output_hz * (double)n / (double)SAMPLE_RATE_HZ;
        double c = cos(out_phase);
        double s = sin(out_phase);
        acc_re += (double)i_sig * c + (double)q_sig * s;
        acc_im += (double)q_sig * c - (double)i_sig * s;
    }
    return hypot(acc_re, acc_im) / (double)MEASURE_SAMPLES;
}

static double measure_multitone_at(int32_t output_hz)
{
    mod_analytic12_state_t st;
    mod_analytic12_reset(&st);
    double acc_re = 0.0;
    double acc_im = 0.0;
    uint32_t total = WARMUP_SAMPLES + MEASURE_SAMPLES;

    for (uint32_t n = 0U; n < total; n++) {
        double phase_700 = 2.0 * TEST_PI * 700.0 * (double)n / (double)SAMPLE_RATE_HZ;
        double phase_1900 = 2.0 * TEST_PI * 1900.0 * (double)n / (double)SAMPLE_RATE_HZ;
        float input = 0.35f * (float)(cos(phase_700) + cos(phase_1900));
        float i_sig;
        float q_sig;
        mod_analytic12_process(&st, input, 1.0f, &i_sig, &q_sig);
        if (n < WARMUP_SAMPLES) continue;

        double output_phase = 2.0 * TEST_PI * (double)output_hz
                            * (double)n / (double)SAMPLE_RATE_HZ;
        double c = cos(output_phase);
        double s = sin(output_phase);
        acc_re += (double)i_sig * c + (double)q_sig * s;
        acc_im += (double)q_sig * c - (double)i_sig * s;
    }
    return hypot(acc_re, acc_im) / (double)MEASURE_SAMPLES;
}

static float continuity_input(uint32_t n)
{
    double phase_700 = 2.0 * TEST_PI * 700.0 * (double)n / (double)SAMPLE_RATE_HZ;
    double phase_1900 = 2.0 * TEST_PI * 1900.0 * (double)n / (double)SAMPLE_RATE_HZ;
    return 0.35f * (float)(cos(phase_700) + cos(phase_1900));
}

static void verify_frame_continuity(void)
{
    static float continuous_i[CONTINUITY_SAMPLES];
    static float continuous_q[CONTINUITY_SAMPLES];
    static const uint32_t frame_sizes[] = { 1U, 7U, 48U, 53U, 192U };
    mod_analytic12_state_t continuous;
    mod_analytic12_state_t framed;
    mod_analytic12_reset(&continuous);
    mod_analytic12_reset(&framed);

    for (uint32_t n = 0U; n < CONTINUITY_SAMPLES; n++) {
        mod_analytic12_process(&continuous, continuity_input(n), 1.0f,
                               &continuous_i[n], &continuous_q[n]);
    }

    uint32_t n = 0U;
    uint32_t frame = 0U;
    while (n < CONTINUITY_SAMPLES) {
        uint32_t end = n + frame_sizes[frame % (sizeof(frame_sizes) / sizeof(frame_sizes[0]))];
        if (end > CONTINUITY_SAMPLES) end = CONTINUITY_SAMPLES;
        for (; n < end; n++) {
            float framed_i;
            float framed_q;
            mod_analytic12_process(&framed, continuity_input(n), 1.0f,
                                   &framed_i, &framed_q);
            double error = hypot((double)framed_i - (double)continuous_i[n],
                                 (double)framed_q - (double)continuous_q[n]);
            if (error > frame_boundary_error) frame_boundary_error = error;
        }
        frame++;
    }
    if (frame_boundary_error != 0.0) {
        fail("frame-boundary discontinuity", 0, frame_boundary_error, 0.0);
    }
}

static void verify_input_domain_and_reset(void)
{
    mod_analytic12_state_t tested;
    mod_analytic12_state_t reference;
    mod_analytic12_reset(&tested);
    mod_analytic12_reset(&reference);

    for (uint32_t n = 0U; n < 2048U; n++) {
        float input = continuity_input(n);
        float reference_input = input;
        if (n == 31U) input = NAN;
        if (n == 32U) input = INFINITY;
        if (n == 33U) input = -INFINITY;
        if (n >= 31U && n <= 33U) reference_input = 0.0f;

        float tested_i;
        float tested_q;
        float reference_i;
        float reference_q;
        mod_analytic12_process(&tested, input, 1.0f, &tested_i, &tested_q);
        mod_analytic12_process(&reference, reference_input, 1.0f,
                               &reference_i, &reference_q);
        if (!isfinite(tested_i) || !isfinite(tested_q)) {
            fail("non-finite input contaminated output", (int32_t)n,
                 (double)tested_i, 0.0);
        }
        double error = hypot((double)tested_i - (double)reference_i,
                             (double)tested_q - (double)reference_q);
        if (error > 1.0e-7) {
            fail("non-finite input was not replaced by silence", (int32_t)n,
                 error, 1.0e-7);
        }
    }

    mod_analytic12_reset(&tested);
    for (uint32_t n = 0U; n < 2048U; n++) {
        float tested_i;
        float tested_q;
        float endpoint = ((n & 1U) != 0U) ? 1.0f : -1.0f;
        mod_analytic12_process(&tested, endpoint, -1.0f, &tested_i, &tested_q);
        if (!isfinite(tested_i) || !isfinite(tested_q)) {
            fail("finite endpoint produced non-finite output", (int32_t)n,
                 (double)tested_i, 0.0);
        }
    }

    mod_analytic12_reset(&tested);
    mod_analytic12_reset(&reference);
    for (uint32_t n = 0U; n < 2048U; n++) {
        float tested_i;
        float tested_q;
        float reference_i;
        float reference_q;
        float input = continuity_input(n);
        mod_analytic12_process(&tested, input, NAN, &tested_i, &tested_q);
        mod_analytic12_process(&reference, input, 1.0f, &reference_i, &reference_q);
        double error = hypot((double)tested_i - (double)reference_i,
                             (double)tested_q - (double)reference_q);
        if (error > 1.0e-7) {
            fail("non-finite sideband selection did not use USB fallback",
                 (int32_t)n, error, 1.0e-7);
        }
    }
}

/* The renderer runs mod_analytic12_block over chunks; it must give what the
 * per-sample reference gives, bit for bit, whatever the chunking, the
 * sideband and the input -- non-finite samples included -- and leave the
 * same state behind. */
static void verify_block_form(void)
{
    static const uint32_t block_sizes[] = { 1U, 3U, 4U, 7U, 16U, 48U, 53U, 192U, 30U, 2U };
    static const float sidebands[] = { 1.0f, -1.0f, NAN };
    static float input[CONTINUITY_SAMPLES];
    static float ref_i[CONTINUITY_SAMPLES];
    static float ref_q[CONTINUITY_SAMPLES];
    static float blk_i[CONTINUITY_SAMPLES];
    static float blk_q[CONTINUITY_SAMPLES];
    static mod_analytic12_coeffs_t coeffs;
    uint32_t lcg = 12345U;
    mod_analytic12_coeffs_load(&coeffs);

    for (uint32_t n = 0U; n < CONTINUITY_SAMPLES; n++) {
        lcg = lcg * 1664525U + 1013904223U;
        float noise = (float)(int32_t)lcg / 2147483648.0f;
        input[n] = 0.8f * continuity_input(n) + 0.2f * noise;
        if (n % 997U == 5U) input[n] = NAN;
        if (n % 1291U == 9U) input[n] = INFINITY;
        if (n % 1777U == 11U) input[n] = (n & 1U) ? 1.0f : -1.0f;
    }

    for (uint32_t sb = 0U; sb < sizeof(sidebands) / sizeof(sidebands[0]); sb++) {
        for (uint32_t start = 0U; start < sizeof(block_sizes) / sizeof(block_sizes[0]); start++) {
            mod_analytic12_state_t ref;
            mod_analytic12_state_t blk;
            mod_analytic12_reset(&ref);
            mod_analytic12_reset(&blk);
            for (uint32_t n = 0U; n < CONTINUITY_SAMPLES; n++) {
                mod_analytic12_process(&ref, input[n], sidebands[sb], &ref_i[n], &ref_q[n]);
            }
            uint32_t n = 0U;
            uint32_t b = start;
            while (n < CONTINUITY_SAMPLES) {
                uint32_t len = block_sizes[b % (sizeof(block_sizes) / sizeof(block_sizes[0]))];
                if (len > CONTINUITY_SAMPLES - n) len = CONTINUITY_SAMPLES - n;
                if ((b & 1U) != 0U) {
                    /* in place, as the renderer runs it */
                    for (uint32_t k = 0U; k < len; k++) blk_i[n + k] = input[n + k];
                    mod_analytic12_block(&blk, &coeffs, &blk_i[n], &blk_i[n], &blk_q[n], len,
                                         sidebands[sb]);
                } else {
                    mod_analytic12_block(&blk, &coeffs, &input[n], &blk_i[n], &blk_q[n], len,
                                         sidebands[sb]);
                }
                n += len;
                b++;
            }
            if ((memcmp(ref_i, blk_i, sizeof(ref_i)) != 0) ||
                (memcmp(ref_q, blk_q, sizeof(ref_q)) != 0)) {
                for (uint32_t k = 0U; k < CONTINUITY_SAMPLES; k++) {
                    if ((memcmp(&ref_i[k], &blk_i[k], sizeof(float)) != 0) ||
                        (memcmp(&ref_q[k], &blk_q[k], sizeof(float)) != 0)) {
                        fail("block form differs from the per-sample form", (int32_t)k,
                             (double)blk_i[k] - (double)ref_i[k], 0.0);
                    }
                }
            }
            /* the block form's phase outputs are its own */
            memset(blk.out_i, 0, sizeof(blk.out_i));
            memset(blk.out_q, 0, sizeof(blk.out_q));
            if (memcmp(&ref, &blk, sizeof(ref)) != 0) {
                fail("block form left a different state", (int32_t)start, 1.0, 0.0);
            }
        }
    }
}

int main(void)
{
    static const int32_t tones_hz[] = { 100, 200, 300, 500, 700, 1000, 1900, 2700, 2900, 3000 };
    double minimum_rejection = 1000.0;
    double minimum_amplitude = 1.0e9;
    double maximum_amplitude = 0.0;

    verify_frame_continuity();
    verify_input_domain_and_reset();
    verify_block_form();

    for (uint32_t t = 0U; t < sizeof(tones_hz) / sizeof(tones_hz[0]); t++) {
        tone_result_t usb = measure_tone(tones_hz[t], 1.0f);
        tone_result_t lsb = measure_tone(tones_hz[t], -1.0f);
        if (usb.image_rejection_db < IMAGE_REJECTION_DB) {
            fail("USB image rejection", tones_hz[t], usb.image_rejection_db, IMAGE_REJECTION_DB);
        }
        if (lsb.image_rejection_db < IMAGE_REJECTION_DB) {
            fail("LSB image rejection", tones_hz[t], lsb.image_rejection_db, IMAGE_REJECTION_DB);
        }
        double match_db = fabs(20.0 * log10(usb.desired / lsb.desired));
        if (match_db > SIDEBAND_MATCH_DB) {
            fail("USB/LSB amplitude mismatch", tones_hz[t], match_db, SIDEBAND_MATCH_DB);
        }
        if (usb.image_rejection_db < minimum_rejection) minimum_rejection = usb.image_rejection_db;
        if (lsb.image_rejection_db < minimum_rejection) minimum_rejection = lsb.image_rejection_db;
        if (usb.desired < minimum_amplitude) minimum_amplitude = usb.desired;
        if (usb.desired > maximum_amplitude) maximum_amplitude = usb.desired;
    }

    double ripple_db = 20.0 * log10(maximum_amplitude / minimum_amplitude);
    if (ripple_db > PASSBAND_RIPPLE_DB) {
        fail("100-3000 Hz amplitude spread", 0, ripple_db, PASSBAND_RIPPLE_DB);
    }

    double reference_3k = measure_output_at(3000, 3000);
    double alias_9k = measure_output_at(9000, 3000);
    double alias_rejection_db = 20.0 * log10(reference_3k / alias_9k);
    if (alias_rejection_db < ALIAS_REJECTION_DB) {
        fail("9 kHz decimation alias rejection", 9000, alias_rejection_db, ALIAS_REJECTION_DB);
    }

    double desired_700 = measure_multitone_at(700);
    double desired_1900 = measure_multitone_at(1900);
    double desired_min = (desired_700 < desired_1900) ? desired_700 : desired_1900;
    double unwanted = measure_multitone_at(-700);
    double candidate = measure_multitone_at(-1900);
    if (candidate > unwanted) unwanted = candidate;
    candidate = measure_multitone_at(-500);  /* 2*700 - 1900 */
    if (candidate > unwanted) unwanted = candidate;
    candidate = measure_multitone_at(3100);  /* 2*1900 - 700 */
    if (candidate > unwanted) unwanted = candidate;
    candidate = measure_multitone_at(0);     /* carrier/DC leakage */
    if (candidate > unwanted) unwanted = candidate;
    double multitone_rejection_db = 20.0 * log10(desired_min / unwanted);
    if (multitone_rejection_db < MULTITONE_REJECTION_DB) {
        fail("700+1900 Hz image/IMD/carrier rejection", 0,
             multitone_rejection_db, MULTITONE_REJECTION_DB);
    }

    printf("analytic production core (float32, block form bit-exact): min image %.2f dB, ripple %.4f dB, alias %.2f dB, two-tone %.2f dB, frame error %.3e",
           minimum_rejection, ripple_db, alias_rejection_db, multitone_rejection_db,
           frame_boundary_error);
    return 0;
}

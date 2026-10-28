#include "sd_audio_math.h"

#include <limits.h>

static uint16_t read_le16(const uint8_t *sample)
{
    return (uint16_t)((uint16_t)sample[0] | ((uint16_t)sample[1] << 8));
}

int32_t SD_AudioPcmSampleToS32(const uint8_t *sample, uint8_t bits_per_sample)
{
    if (sample == 0) return 0;
    if (bits_per_sample == 8U) {
        return (int32_t)(((int64_t)sample[0] - 128LL) * 16777216LL);
    }
    if (bits_per_sample == 16U) {
        return (int32_t)((int64_t)(int16_t)read_le16(sample) * 65536LL);
    }
    if (bits_per_sample == 24U) {
        int32_t value = (int32_t)((uint32_t)sample[0] |
                                  ((uint32_t)sample[1] << 8) |
                                  ((uint32_t)sample[2] << 16));
        if ((value & 0x00800000L) != 0) value |= (int32_t)0xFF000000L;
        return (int32_t)((int64_t)value * 256LL);
    }
    return 0;
}

int32_t SD_AudioDownmixS32(const int32_t *frame, uint8_t channels, uint8_t mode)
{
    if ((frame == 0) || (channels == 0U)) return 0;
    if (channels == 1U) return frame[0];
    if (mode == SD_AUDIO_DOWNMIX_LEFT) return frame[0];
    if (mode == SD_AUDIO_DOWNMIX_RIGHT) return frame[1];
    if (mode == SD_AUDIO_DOWNMIX_STRONGER) {
        int64_t left_magnitude = (frame[0] < 0) ? -(int64_t)frame[0] : frame[0];
        int64_t right_magnitude = (frame[1] < 0) ? -(int64_t)frame[1] : frame[1];
        return (left_magnitude >= right_magnitude) ? frame[0] : frame[1];
    }
    return (int32_t)(((int64_t)frame[0] + (int64_t)frame[1]) / 2LL);
}

uint32_t SD_AudioResampleStepQ16(uint32_t source_rate_hz, uint32_t output_rate_hz)
{
    if ((source_rate_hz == 0U) || (output_rate_hz == 0U)) return 0U;
    uint64_t step = ((uint64_t)source_rate_hz << 16) / output_rate_hz;
    if (step == 0ULL) step = 1ULL;
    return (step > UINT32_MAX) ? UINT32_MAX : (uint32_t)step;
}

uint32_t SD_AudioSamplesToMs(uint64_t samples, uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U) return 0U;
    uint64_t saturating_samples =
        ((uint64_t)UINT32_MAX * sample_rate_hz + 999ULL) / 1000ULL;
    if (samples >= saturating_samples) return UINT32_MAX;
    uint64_t milliseconds = (samples * 1000ULL) / sample_rate_hz;
    return (uint32_t)milliseconds;
}

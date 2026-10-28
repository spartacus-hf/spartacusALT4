#ifndef SD_AUDIO_MATH_H
#define SD_AUDIO_MATH_H

#include <stdint.h>

enum {
    SD_AUDIO_DOWNMIX_AVERAGE = 0U,
    SD_AUDIO_DOWNMIX_LEFT = 1U,
    SD_AUDIO_DOWNMIX_RIGHT = 2U,
    SD_AUDIO_DOWNMIX_STRONGER = 3U
};

int32_t SD_AudioPcmSampleToS32(const uint8_t *sample, uint8_t bits_per_sample);
int32_t SD_AudioDownmixS32(const int32_t *frame, uint8_t channels, uint8_t mode);
uint32_t SD_AudioResampleStepQ16(uint32_t source_rate_hz, uint32_t output_rate_hz);
uint32_t SD_AudioSamplesToMs(uint64_t samples, uint32_t sample_rate_hz);

#endif /* SD_AUDIO_MATH_H */

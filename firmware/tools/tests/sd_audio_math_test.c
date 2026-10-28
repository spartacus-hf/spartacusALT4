#include "sd_audio_math.h"

#include <stdint.h>
#include <stdio.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

int main(void)
{
    const uint8_t u8[] = {0U, 128U, 255U};
    const uint8_t s16_min[] = {0x00U, 0x80U};
    const uint8_t s16_max[] = {0xFFU, 0x7FU};
    const uint8_t s24_min[] = {0x00U, 0x00U, 0x80U};
    const uint8_t s24_max[] = {0xFFU, 0xFFU, 0x7FU};
    CHECK(SD_AudioPcmSampleToS32(&u8[0], 8U) == INT32_MIN);
    CHECK(SD_AudioPcmSampleToS32(&u8[1], 8U) == 0);
    CHECK(SD_AudioPcmSampleToS32(&u8[2], 8U) == 2130706432L);
    CHECK(SD_AudioPcmSampleToS32(s16_min, 16U) == INT32_MIN);
    CHECK(SD_AudioPcmSampleToS32(s16_max, 16U) == 2147418112L);
    CHECK(SD_AudioPcmSampleToS32(s24_min, 24U) == INT32_MIN);
    CHECK(SD_AudioPcmSampleToS32(s24_max, 24U) == 2147483392L);
    CHECK(SD_AudioPcmSampleToS32(s24_max, 20U) == 0);

    const int32_t stereo[] = {-2000000000L, 1000000000L};
    CHECK(SD_AudioDownmixS32(stereo, 2U, SD_AUDIO_DOWNMIX_AVERAGE) == -500000000L);
    CHECK(SD_AudioDownmixS32(stereo, 2U, SD_AUDIO_DOWNMIX_LEFT) == stereo[0]);
    CHECK(SD_AudioDownmixS32(stereo, 2U, SD_AUDIO_DOWNMIX_RIGHT) == stereo[1]);
    CHECK(SD_AudioDownmixS32(stereo, 2U, SD_AUDIO_DOWNMIX_STRONGER) == stereo[0]);
    CHECK(SD_AudioDownmixS32(stereo, 1U, SD_AUDIO_DOWNMIX_RIGHT) == stereo[0]);

    CHECK(SD_AudioResampleStepQ16(48000U, 48000U) == 65536U);
    CHECK(SD_AudioResampleStepQ16(44100U, 48000U) == 60211U);
    CHECK(SD_AudioResampleStepQ16(0U, 48000U) == 0U);
    CHECK(SD_AudioSamplesToMs(48000ULL * 60ULL * 60ULL, 48000U) == 3600000U);
    CHECK(SD_AudioSamplesToMs(UINT64_MAX, 1U) == UINT32_MAX);
    CHECK(SD_AudioSamplesToMs(1U, 0U) == 0U);

    if (failures != 0U) return 1;
    puts("SD audio production math: PASS");
    return 0;
}

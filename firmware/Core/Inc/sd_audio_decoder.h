/**
 * @file    sd_audio_decoder.h
 * @brief   Streaming WAV/MP3 decoder over a FatFs file; native FLAC only
 *          when SD_AUDIO_FLAC_ENABLE is set (see sd_file_browser.h).
 */
#ifndef SD_AUDIO_DECODER_H
#define SD_AUDIO_DECODER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "sd_file_browser.h"

typedef struct {
    sd_file_type_t type;
    uint32_t sample_rate;
    uint32_t total_frames;
    uint32_t duration_ms;
    uint8_t channels;
    uint8_t bits_per_sample;
    char title[64];
    char artist[64];
} sd_audio_stream_info_t;

uint8_t SD_AudioDecoderOpen(const char *path, sd_file_type_t type,
                            sd_audio_stream_info_t *info);
void SD_AudioDecoderClose(void);
uint8_t SD_AudioDecoderIsOpen(void);
uint32_t SD_AudioDecoderReadS32(int32_t *interleaved, uint32_t frames);
uint8_t SD_AudioDecoderSeekFrame(uint32_t frame);
uint32_t SD_AudioDecoderTellFrame(void);
const char *SD_AudioDecoderError(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_AUDIO_DECODER_H */

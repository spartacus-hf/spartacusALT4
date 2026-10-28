/**
 * @file    sd_audio_player.h
 * @brief   Foreground-only SD audio player feeding the RF PCM transport.
 */
#ifndef SD_AUDIO_PLAYER_H
#define SD_AUDIO_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "sd_audio_decoder.h"

typedef enum {
    SD_PLAYER_CLOSED = 0,
    SD_PLAYER_CONFIRM,
    SD_PLAYER_PREPARING,
    SD_PLAYER_PREBUFFER,
    SD_PLAYER_PLAYING,
    SD_PLAYER_PAUSED,
    SD_PLAYER_REBUFFER,
    SD_PLAYER_DRAINING,
    SD_PLAYER_FINISHED,
    SD_PLAYER_ERROR
} sd_player_state_t;

typedef enum {
    SD_PLAYER_RF_SSB = 0,
    SD_PLAYER_RF_AM,
    SD_PLAYER_RF_FM
} sd_player_rf_mode_t;

typedef enum {
    SD_PLAYER_DOWNMIX_AVERAGE = 0,
    SD_PLAYER_DOWNMIX_LEFT,
    SD_PLAYER_DOWNMIX_RIGHT,
    SD_PLAYER_DOWNMIX_STRONGER
} sd_player_downmix_t;

typedef struct {
    uint8_t ask_each_track;
    uint8_t advance;
    uint8_t repeat;
    uint8_t shuffle;
    uint8_t clean;
    uint8_t gain_percent;
    uint8_t seek_seconds;
    sd_player_downmix_t downmix;
} sd_audio_player_config_t;

typedef struct {
    sd_player_state_t state;
    sd_player_rf_mode_t rf_mode;
    sd_audio_stream_info_t stream;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t buffer_ms;
    uint32_t rebuffer_count;
    const char *path;
    const char *error;
} sd_audio_player_status_t;

void SD_AudioPlayerInit(void);
uint8_t SD_AudioPlayerOpen(const char *path, sd_file_type_t type);
uint8_t SD_AudioPlayerContinue(const char *path, sd_file_type_t type);
void SD_AudioPlayerConfirm(void);
void SD_AudioPlayerReject(void);
void SD_AudioPlayerClose(void);
void SD_AudioPlayerService(void);
void SD_AudioPlayerTogglePause(void);
uint8_t SD_AudioPlayerSeekRelative(int32_t seconds);
uint8_t SD_AudioPlayerIsOpen(void);
uint8_t SD_AudioPlayerRealtimeActive(void);
void SD_AudioPlayerGetStatus(sd_audio_player_status_t *status);
void SD_AudioPlayerSetRfMode(sd_player_rf_mode_t mode);
sd_player_rf_mode_t SD_AudioPlayerGetRfMode(void);
void SD_AudioPlayerSetConfig(const sd_audio_player_config_t *config);
void SD_AudioPlayerGetConfig(sd_audio_player_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* SD_AUDIO_PLAYER_H */

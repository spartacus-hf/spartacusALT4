/**
 * @file    sd_audio_player.c
 * @brief   Buffered SD audio playback and 8-48 kHz to 48 kHz conversion.
 */

#include "sd_audio_player.h"
#include "sd_audio_math.h"
#include "audio_resample.h"
#include "modulator.h"
#include <limits.h>
#include <string.h>

#define PLAYER_DECODE_FRAMES       64U
#define PLAYER_FIFO_FRAMES        128U
#define PLAYER_OUTPUT_SAMPLES     128U
#define PLAYER_PREBUFFER_SAMPLES (48U * 96U)
#define PLAYER_REFILL_SAMPLES    (48U * 80U)
#define PLAYER_REBUFFER_SAMPLES  (48U * 24U)
#define PLAYER_MIN_START_SAMPLES (48U * 32U)
#define PLAYER_FIR_TAPS            16U
#define PLAYER_FIR_PHASES          32U
/* These playback scratch buffers are CPU-only foreground state.  Keep them in
 * ordinary SRAM so CCMRAM remains available for the RF real-time path and its
 * diagnostic build headroom. */
#define PLAYER_BSS __attribute__((aligned(4)))

_Static_assert((uint32_t)SD_PLAYER_DOWNMIX_AVERAGE == (uint32_t)SD_AUDIO_DOWNMIX_AVERAGE,
               "downmix enum mismatch");
_Static_assert((uint32_t)SD_PLAYER_DOWNMIX_STRONGER == (uint32_t)SD_AUDIO_DOWNMIX_STRONGER,
               "downmix enum mismatch");



static PLAYER_BSS int32_t resample_fifo[PLAYER_FIFO_FRAMES];
static PLAYER_BSS int16_t output_s16[PLAYER_OUTPUT_SAMPLES];

static sd_player_state_t player_state;
static sd_player_rf_mode_t player_rf_mode = SD_PLAYER_RF_SSB;
static sd_audio_player_config_t player_config = {
    1U, 0U, 0U, 0U, 1U, 100U, 10U, SD_PLAYER_DOWNMIX_AVERAGE
};
static sd_audio_stream_info_t player_stream;
static mod_config_t prior_mode;
static char player_path[SD_BROWSER_PATH_MAX];
static char player_error[40];
static sd_file_type_t player_file_type;
static uint32_t fifo_count;
static uint32_t source_pos_q16;
static uint32_t source_step_q16;
static uint32_t output_pending;
static uint32_t output_frames_produced;
static uint32_t position_base_ms;
static uint32_t rebuffer_count;
static uint32_t dither_state = 0x62C4A71DUL;
static uint32_t pending_seek_frame;
static uint8_t seek_pending;
static uint8_t decoder_eof;
static uint8_t eof_padding_added;

static void copy_text(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0U;
    if (cap == 0U) return;
    while ((i + 1U) < cap && src != NULL && src[i] != 0) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static uint32_t random16(void)
{
    uint32_t x = dither_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    dither_state = x;
    return x & 0xFFFFU;
}

static int32_t downmix_frame(const int32_t *frame)
{
    return SD_AudioDownmixS32(frame, player_stream.channels,
                              (uint8_t)player_config.downmix);
}

static int16_t quantize_sample(int64_t sample)
{
    int32_t gain_q15 = ((int32_t)player_config.gain_percent * 32768) / 100;
    sample = (sample * gain_q15) >> 15;
    if (player_stream.bits_per_sample > 16U) {
        sample += (int32_t)random16() - (int32_t)random16();
    }
    if (sample > INT32_MAX) sample = INT32_MAX;
    if (sample < INT32_MIN) sample = INT32_MIN;
    return (int16_t)(sample >> 16);
}

static void resampler_reset(void)
{
    memset(resample_fifo, 0, 8U * sizeof(resample_fifo[0]));
    fifo_count = 8U;
    source_pos_q16 = 7U << 16;
    source_step_q16 = SD_AudioResampleStepQ16(player_stream.sample_rate, 48000U);
    output_pending = 0U;
    decoder_eof = 0U;
    eof_padding_added = 0U;
}

static void resampler_compact(void)
{
    uint32_t integer_pos = source_pos_q16 >> 16;
    if (integer_pos > 7U) {
        uint32_t drop = integer_pos - 7U;
        if (drop > fifo_count) drop = fifo_count;
        memmove(resample_fifo, &resample_fifo[drop],
                (fifo_count - drop) * sizeof(resample_fifo[0]));
        fifo_count -= drop;
        source_pos_q16 -= drop << 16;
    }
}

static void resampler_fill_output(void)
{
    while (output_pending < PLAYER_OUTPUT_SAMPLES) {
        uint32_t index = source_pos_q16 >> 16;
        uint32_t phase = ((source_pos_q16 & 0xFFFFU) * PLAYER_FIR_PHASES) >> 16;
        int64_t sum = 0;
        if (index < 7U || index + 8U >= fifo_count) break;
        for (uint32_t tap = 0U; tap < PLAYER_FIR_TAPS; tap++) {
            sum += (int64_t)resample_fifo[index - 7U + tap] * audio_sinc_q15[phase][tap];
        }
        output_s16[output_pending++] = quantize_sample(sum >> 15);
        source_pos_q16 += source_step_q16;
    }
    resampler_compact();
}

static uint32_t output_samples_ms(uint32_t samples)
{
    return SD_AudioSamplesToMs(samples, 48000U);
}

static uint32_t current_play_position_ms(void)
{
    uint32_t queued = MOD_LocalAudioQueuedSamples();
    uint32_t played = (output_frames_produced > queued) ?
                      (output_frames_produced - queued) : 0U;
    uint64_t position = (uint64_t)position_base_ms + output_samples_ms(played);
    return (position > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)position;
}

static uint8_t submit_pending(uint8_t flush)
{
    uint32_t submit = (output_pending / AUDIO_FRAME_SIZE) * AUDIO_FRAME_SIZE;
    if (flush != 0U && output_pending != 0U) {
        while (output_pending < AUDIO_FRAME_SIZE) output_s16[output_pending++] = 0;
        submit = AUDIO_FRAME_SIZE;
    }
    if (submit == 0U || MOD_LocalAudioPcmFreeSamples() < submit) return 0U;
    if (!MOD_LocalAudioSubmit(output_s16, submit)) return 0U;
    output_frames_produced += submit;
    output_pending -= submit;
    if (output_pending != 0U) {
        memmove(output_s16, &output_s16[submit], output_pending * sizeof(output_s16[0]));
    }
    return 1U;
}

static mod_config_t selected_mod_config(void)
{
    if (player_rf_mode == SD_PLAYER_RF_AM) return MOD_CFG_AM_14MHZ;
    if (player_rf_mode == SD_PLAYER_RF_FM) return MOD_CFG_FM_NARROW_14MHZ;
    return MOD_CFG_PROD_SSB_14MHZ;
}

static void player_fail(const char *error)
{
    copy_text(player_error, error, sizeof(player_error));
    if (MOD_GetAudioSource() == MOD_AUDIO_SOURCE_SD) MOD_LocalAudioRelease();
    SD_AudioDecoderClose();
    player_state = SD_PLAYER_ERROR;
}

static uint8_t prepare_decoder(void)
{
    MOD_SetTestConfig(selected_mod_config());
    MOD_ApplyRequestedConfigNow();
    if (!MOD_LocalAudioAcquire()) return 0U;
    MOD_LocalAudioSetClean(player_config.clean);
    if (!MOD_LocalAudioIoBegin()) return 0U;
    uint8_t opened = SD_AudioDecoderOpen(player_path, player_file_type, &player_stream);
    MOD_LocalAudioIoEnd();
    if (!opened) {
        copy_text(player_error, SD_AudioDecoderError(), sizeof(player_error));
        MOD_LocalAudioRelease();
        return 2U;
    }
    resampler_reset();
    output_frames_produced = 0U;
    position_base_ms = 0U;
    player_state = SD_PLAYER_PREBUFFER;
    return 1U;
}

static uint8_t append_decoded_frames(void)
{
    int32_t decoded_s32[PLAYER_DECODE_FRAMES * 2U];
    uint32_t frames;
    if (fifo_count + PLAYER_DECODE_FRAMES > PLAYER_FIFO_FRAMES) return 0U;
    if (!MOD_LocalAudioIoBegin()) return 0U;
    frames = SD_AudioDecoderReadS32(decoded_s32, PLAYER_DECODE_FRAMES);
    MOD_LocalAudioIoEnd();
    if (frames == 0U) {
        decoder_eof = 1U;
        return 1U;
    }
    for (uint32_t i = 0U; i < frames; i++) {
        resample_fifo[fifo_count++] = downmix_frame(&decoded_s32[i * player_stream.channels]);
    }
    return 1U;
}

static void service_rebuffer(void)
{
    if (MOD_GetAudioSource() != MOD_AUDIO_SOURCE_SD) {
        if (!MOD_LocalAudioAcquire()) return;
        MOD_LocalAudioSetClean(player_config.clean);
    }
    if (seek_pending != 0U) {
        if (!MOD_LocalAudioIoBegin()) return;
        uint8_t ok = SD_AudioDecoderSeekFrame(pending_seek_frame);
        MOD_LocalAudioIoEnd();
        if (!ok) { player_fail("SEEK FAILED"); return; }
        position_base_ms = (uint32_t)(((uint64_t)pending_seek_frame * 1000ULL) /
                                     player_stream.sample_rate);
        output_frames_produced = 0U;
        seek_pending = 0U;
    }
    resampler_reset();
    player_state = SD_PLAYER_PREBUFFER;
}

void SD_AudioPlayerInit(void)
{
    memset(&player_stream, 0, sizeof(player_stream));
    player_state = SD_PLAYER_CLOSED;
    player_path[0] = 0;
    player_error[0] = 0;
}

uint8_t SD_AudioPlayerOpen(const char *path, sd_file_type_t type)
{
    uint32_t len;
    if (path == NULL || !SD_FileTypeIsPlayable(type) ||
        player_state != SD_PLAYER_CLOSED) return 0U;
    len = (uint32_t)strlen(path);
    if (len == 0U || len >= sizeof(player_path)) return 0U;
    memcpy(player_path, path, len + 1U);
    player_file_type = type;
    prior_mode = MOD_GetTestConfig();
    player_error[0] = 0;
    rebuffer_count = 0U;
    memset(&player_stream, 0, sizeof(player_stream));
    player_stream.type = type;
    player_state = player_config.ask_each_track ? SD_PLAYER_CONFIRM : SD_PLAYER_PREPARING;
    return 1U;
}

uint8_t SD_AudioPlayerContinue(const char *path, sd_file_type_t type)
{
    uint32_t len;
    if (path == NULL || player_state != SD_PLAYER_FINISHED ||
        !SD_FileTypeIsPlayable(type)) return 0U;
    len = (uint32_t)strlen(path);
    if (len == 0U || len >= sizeof(player_path)) return 0U;
    memcpy(player_path, path, len + 1U);
    player_file_type = type;
    player_error[0] = 0;
    rebuffer_count = 0U;
    memset(&player_stream, 0, sizeof(player_stream));
    player_stream.type = type;
    player_state = player_config.ask_each_track ? SD_PLAYER_CONFIRM : SD_PLAYER_PREPARING;
    return 1U;
}

void SD_AudioPlayerConfirm(void)
{
    if (player_state == SD_PLAYER_CONFIRM) player_state = SD_PLAYER_PREPARING;
}

void SD_AudioPlayerReject(void)
{
    if (player_state == SD_PLAYER_CONFIRM) SD_AudioPlayerClose();
}

void SD_AudioPlayerClose(void)
{
    if (player_state == SD_PLAYER_CLOSED) return;
    if (MOD_GetAudioSource() == MOD_AUDIO_SOURCE_SD) MOD_LocalAudioRelease();
    SD_AudioDecoderClose();
    player_state = SD_PLAYER_CLOSED;
    player_path[0] = 0;
    MOD_SetTestConfig(prior_mode);
    MOD_ApplyRequestedConfigNow();
}

void SD_AudioPlayerService(void)
{
    if (player_state == SD_PLAYER_PREPARING) {
        uint8_t result = prepare_decoder();
        if (result == 2U) player_state = SD_PLAYER_ERROR;
        return;
    }
    if (player_state == SD_PLAYER_REBUFFER) {
        service_rebuffer();
        return;
    }
    if (player_state == SD_PLAYER_DRAINING) {
        if (MOD_LocalAudioDrainComplete()) {
            MOD_LocalAudioRelease();
            if (player_config.repeat != 0U) {
                player_state = player_config.ask_each_track ? SD_PLAYER_CONFIRM : SD_PLAYER_PREPARING;
            } else {
                player_state = SD_PLAYER_FINISHED;
            }
        }
        return;
    }
    if (player_state != SD_PLAYER_PREBUFFER && player_state != SD_PLAYER_PLAYING) return;

    uint32_t queued = MOD_LocalAudioQueuedSamples();
    if (player_state == SD_PLAYER_PLAYING && queued < PLAYER_REBUFFER_SAMPLES && decoder_eof == 0U) {
        position_base_ms = (uint32_t)(((uint64_t)SD_AudioDecoderTellFrame() * 1000ULL) /
                                     player_stream.sample_rate);
        output_frames_produced = 0U;
        MOD_LocalAudioRelease();
        rebuffer_count++;
        player_state = SD_PLAYER_REBUFFER;
        return;
    }
    if (player_state == SD_PLAYER_PLAYING && queued >= PLAYER_REFILL_SAMPLES) return;

    resampler_fill_output();
    if (submit_pending(0U)) {
        queued = MOD_LocalAudioQueuedSamples();
        if (player_state == SD_PLAYER_PREBUFFER && queued >= PLAYER_PREBUFFER_SAMPLES) {
            if (MOD_LocalAudioArm()) player_state = SD_PLAYER_PLAYING;
        }
        return;
    }

    if (decoder_eof == 0U) {
        (void)append_decoded_frames();
        return;
    }
    if (eof_padding_added == 0U && fifo_count + PLAYER_FIR_TAPS <= PLAYER_FIFO_FRAMES) {
        memset(&resample_fifo[fifo_count], 0, PLAYER_FIR_TAPS * sizeof(resample_fifo[0]));
        fifo_count += PLAYER_FIR_TAPS;
        eof_padding_added = 1U;
        return;
    }

    resampler_fill_output();
    if (submit_pending(1U)) return;
    if (output_pending == 0U) {
        if (player_state == SD_PLAYER_PREBUFFER && MOD_LocalAudioQueuedSamples() >= AUDIO_FRAME_SIZE) {
            while (MOD_LocalAudioQueuedSamples() < PLAYER_MIN_START_SAMPLES &&
                   MOD_LocalAudioPcmFreeSamples() >= AUDIO_FRAME_SIZE) {
                memset(output_s16, 0, AUDIO_FRAME_SIZE * sizeof(output_s16[0]));
                if (!MOD_LocalAudioSubmit(output_s16, AUDIO_FRAME_SIZE)) break;
            }
            (void)MOD_LocalAudioArm();
            player_state = SD_PLAYER_PLAYING;
        }
        MOD_LocalAudioEndOfStream();
        SD_AudioDecoderClose();
        player_state = SD_PLAYER_DRAINING;
    }
}

void SD_AudioPlayerTogglePause(void)
{
    if (player_state == SD_PLAYER_PLAYING || player_state == SD_PLAYER_PREBUFFER) {
        position_base_ms = (uint32_t)(((uint64_t)SD_AudioDecoderTellFrame() * 1000ULL) /
                                     player_stream.sample_rate);
        output_frames_produced = 0U;
        MOD_LocalAudioRelease();
        player_state = SD_PLAYER_PAUSED;
    } else if (player_state == SD_PLAYER_PAUSED) {
        player_state = SD_PLAYER_REBUFFER;
    }
}

uint8_t SD_AudioPlayerSeekRelative(int32_t seconds)
{
    uint32_t current_ms;
    int64_t target_ms;
    if (player_stream.sample_rate == 0U ||
        (player_state != SD_PLAYER_PLAYING && player_state != SD_PLAYER_PAUSED &&
         player_state != SD_PLAYER_PREBUFFER)) return 0U;
    current_ms = current_play_position_ms();
    target_ms = (int64_t)current_ms + (int64_t)seconds * 1000LL;
    if (target_ms < 0) target_ms = 0;
    if (player_stream.duration_ms != 0U && (uint64_t)target_ms > player_stream.duration_ms) {
        target_ms = player_stream.duration_ms;
    }
    pending_seek_frame = (uint32_t)(((uint64_t)target_ms * player_stream.sample_rate) / 1000ULL);
    seek_pending = 1U;
    if (MOD_GetAudioSource() == MOD_AUDIO_SOURCE_SD) MOD_LocalAudioRelease();
    player_state = SD_PLAYER_REBUFFER;
    return 1U;
}

uint8_t SD_AudioPlayerIsOpen(void) { return (player_state != SD_PLAYER_CLOSED) ? 1U : 0U; }

uint8_t SD_AudioPlayerRealtimeActive(void)
{
    return (uint8_t)(player_state == SD_PLAYER_PREBUFFER || player_state == SD_PLAYER_PLAYING ||
                     player_state == SD_PLAYER_REBUFFER || player_state == SD_PLAYER_DRAINING);
}

void SD_AudioPlayerGetStatus(sd_audio_player_status_t *status)
{
    if (status == NULL) return;
    uint32_t queued = MOD_LocalAudioQueuedSamples();
    status->state = player_state;
    status->rf_mode = player_rf_mode;
    status->stream = player_stream;
    status->position_ms = current_play_position_ms();
    status->duration_ms = player_stream.duration_ms;
    status->buffer_ms = queued / 48U;
    status->rebuffer_count = rebuffer_count;
    status->path = player_path;
    status->error = player_error;
}

void SD_AudioPlayerSetRfMode(sd_player_rf_mode_t mode)
{
    if (mode <= SD_PLAYER_RF_FM &&
        (player_state == SD_PLAYER_CLOSED || player_state == SD_PLAYER_CONFIRM ||
         player_state == SD_PLAYER_FINISHED || player_state == SD_PLAYER_ERROR)) player_rf_mode = mode;
}

sd_player_rf_mode_t SD_AudioPlayerGetRfMode(void) { return player_rf_mode; }

void SD_AudioPlayerSetConfig(const sd_audio_player_config_t *config)
{
    if (config == NULL) return;
    player_config = *config;
    player_config.ask_each_track = player_config.ask_each_track ? 1U : 0U;
    player_config.advance = player_config.advance ? 1U : 0U;
    player_config.repeat = player_config.repeat ? 1U : 0U;
    player_config.shuffle = player_config.shuffle ? 1U : 0U;
    player_config.clean = player_config.clean ? 1U : 0U;
    if (player_config.gain_percent > 200U) player_config.gain_percent = 200U;
    if (player_config.seek_seconds == 0U) player_config.seek_seconds = 10U;
    if (player_config.downmix > SD_PLAYER_DOWNMIX_STRONGER) {
        player_config.downmix = SD_PLAYER_DOWNMIX_AVERAGE;
    }
}

void SD_AudioPlayerGetConfig(sd_audio_player_config_t *config)
{
    if (config != NULL) *config = player_config;
}

/**
 * @file    sd_audio_decoder.c
 * @brief   Fixed-arena streaming audio decoders backed by FatFs.
 *          WAV and MP3 always; FLAC only with SD_AUDIO_FLAC_ENABLE.
 */

#include "sd_audio_decoder.h"
#include "sd_audio_math.h"
#include "modulator.h"
#include "main.h"
#include "../../Middlewares/Third_Party/FatFs/src/ff.h"
#include <stddef.h>
#include <string.h>

/* dr_mp3 0.7 spells its switch DR_MP3_NO_STDIO; the DRMP3_NO_STDIO this file
 * used to define was ignored, so drmp3_uninit kept fclose(), and with it
 * newlib's whole FILE layer, in the image. Allocation always goes through
 * the arena callbacks below, so the heap defaults are never reached either. */
#define DR_MP3_NO_STDIO
#define DRMP3_MALLOC(sz) ((void)(sz), NULL)
#define DRMP3_REALLOC(p, sz) ((void)(p), (void)(sz), NULL)
#define DRMP3_FREE(p) ((void)(p))
/* 4 KiB is comfortably above the largest legal Layer III frame while fitting
 * beside dr_mp3's fixed decoder state in the shared MCU arena. */
#define DRMP3_DATA_CHUNK_SIZE 4096
/* dr_mp3's checks stay, with assert()'s outcome (syscalls.c: emergency stop),
 * but without the file, function and expression strings assert() hands to
 * __assert_func -- which ignores them. 792 B of flash. */
#define DRMP3_ASSERT(expression) ((expression) ? (void)0 : __assert_func(NULL, 0, NULL, NULL))
void __assert_func(const char *file, int line, const char *func, const char *expr) __attribute__((noreturn));
/* MPEG Layer III only: the browser offers .mp3 files, which are Layer III;
 * the Layer I/II decoder and its tables (2 KB) went unused. A Layer I/II
 * stream renamed .mp3 is refused like any other undecodable file. */
#define DR_MP3_ONLY_MP3
#define DR_MP3_IMPLEMENTATION
#include "../../Middlewares/Third_Party/dr_libs/dr_mp3.h"

#if SD_AUDIO_FLAC_ENABLE
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SEEKTABLE
#define DR_FLAC_BUFFER_SIZE 512
#define DR_FLAC_IMPLEMENTATION
#include "../../Middlewares/Third_Party/dr_libs/dr_flac.h"
#endif

typedef enum {
    DECODER_NONE = 0,
    DECODER_WAV,
    DECODER_MP3,
    DECODER_FLAC
} decoder_kind_t;

typedef struct {
    uint32_t size;
    uint32_t marker;
} arena_header_t;

#define ARENA_MARKER 0x41554430UL

static FIL audio_file;
static uint8_t file_open;
static uint8_t *arena_base;
static uint32_t arena_offset;
static decoder_kind_t decoder_kind;
static drmp3 *mp3_decoder;
#if SD_AUDIO_FLAC_ENABLE
static drflac *flac_decoder;
#endif
static char decoder_error[32];
static sd_audio_stream_info_t stream_info;
static uint32_t wav_data_offset;
static uint32_t wav_data_size;
static uint32_t wav_frame_cursor;
static uint16_t wav_block_align;

static void set_error(const char *error)
{
    uint32_t i = 0U;
    if (error != NULL) {
        while ((i + 1U) < sizeof(decoder_error) && error[i] != 0) {
            decoder_error[i] = error[i];
            i++;
        }
    }
    decoder_error[i] = 0;
}

static uint32_t align8(uint32_t value)
{
    return (value + 7U) & ~7U;
}

static void arena_reset(uint32_t reserved)
{
    arena_offset = align8(reserved);
}

static void *arena_malloc(size_t size, void *user_data)
{
    uint32_t start;
    arena_header_t *header;
    (void)user_data;
    if (arena_base == NULL || size == 0U || size > 0xFFFFFFFFUL) return NULL;
    start = align8(arena_offset + (uint32_t)sizeof(arena_header_t));
    if (start + (uint32_t)size > SD_AUDIO_WORKSPACE_SIZE) return NULL;
    header = (arena_header_t *)&arena_base[start - sizeof(arena_header_t)];
    header->size = (uint32_t)size;
    header->marker = ARENA_MARKER;
    arena_offset = align8(start + (uint32_t)size);
    return &arena_base[start];
}

static void *arena_realloc(void *memory, size_t size, void *user_data)
{
    void *replacement;
    uint32_t old_size = 0U;
    if (memory == NULL) return arena_malloc(size, user_data);
    arena_header_t *header = (arena_header_t *)((uint8_t *)memory - sizeof(arena_header_t));
    if (header->marker == ARENA_MARKER) old_size = header->size;
    replacement = arena_malloc(size, user_data);
    if (replacement != NULL && old_size != 0U) {
        uint32_t copy_size = (old_size < size) ? old_size : (uint32_t)size;
        memcpy(replacement, memory, copy_size);
    }
    return replacement;
}

static void arena_free(void *memory, void *user_data)
{
    (void)memory;
    (void)user_data;
}

static size_t file_read(void *user_data, void *buffer, size_t bytes)
{
    UINT got = 0U;
    (void)user_data;
    if (!file_open || MOD_LocalAudioIoScopeActive() == 0U || bytes > 0xFFFFFFFFUL) return 0U;
    if (f_read(&audio_file, buffer, (UINT)bytes, &got) != FR_OK) return 0U;
    if (got != 0U) APP_WatchdogProgress();
    return got;
}

static uint8_t file_seek_common(int offset, int origin)
{
    int64_t base;
    int64_t target;
    if (!file_open || MOD_LocalAudioIoScopeActive() == 0U) return 0U;
    if (origin == 0) base = 0;
    else if (origin == 1) base = (int64_t)f_tell(&audio_file);
    else base = (int64_t)f_size(&audio_file);
    target = base + (int64_t)offset;
    if (target < 0 || (uint64_t)target > (uint64_t)f_size(&audio_file)) return 0U;
    return (f_lseek(&audio_file, (FSIZE_t)target) == FR_OK) ? 1U : 0U;
}

static drmp3_bool32 mp3_seek(void *user_data, int offset, drmp3_seek_origin origin)
{
    (void)user_data;
    return file_seek_common(offset, (int)origin) ? DRMP3_TRUE : DRMP3_FALSE;
}

static drmp3_bool32 mp3_tell(void *user_data, drmp3_int64 *cursor)
{
    (void)user_data;
    if (cursor == NULL || !file_open) return DRMP3_FALSE;
    *cursor = (drmp3_int64)f_tell(&audio_file);
    return DRMP3_TRUE;
}

#if SD_AUDIO_FLAC_ENABLE
static drflac_bool32 flac_seek(void *user_data, int offset, drflac_seek_origin origin)
{
    (void)user_data;
    return file_seek_common(offset, (int)origin) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 flac_tell(void *user_data, drflac_int64 *cursor)
{
    (void)user_data;
    if (cursor == NULL || !file_open) return DRFLAC_FALSE;
    *cursor = (drflac_int64)f_tell(&audio_file);
    return DRFLAC_TRUE;
}
#endif

static const char *base_name(const char *path)
{
    const char *base = path;
    for (uint32_t i = 0U; path[i] != 0; i++) {
        if (path[i] == '/') base = &path[i + 1U];
    }
    return base;
}

static void text_copy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0U;
    if (cap == 0U) return;
    while ((i + 1U) < cap && src[i] != 0) {
        uint8_t c = (uint8_t)src[i];
        if (c < 0x20U && c != '\t') break;
        dst[i] = (char)c;
        i++;
    }
    while (i > 0U && dst[i - 1U] == ' ') i--;
    dst[i] = 0;
}

static uint32_t synchsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7FU) << 21) |
           ((uint32_t)(p[1] & 0x7FU) << 14) |
           ((uint32_t)(p[2] & 0x7FU) << 7) |
           (uint32_t)(p[3] & 0x7FU);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* PCM WAV is parsed locally instead of through a conversion helper. This
 * keeps every SD read bounded to the caller's 64-frame decode block and avoids
 * multi-kilobyte automatic conversion buffers on the MCU stack. */
static uint8_t open_pcm_wav(uint64_t *total_frames)
{
    uint8_t riff[12];
    uint8_t chunk[8];
    uint8_t fmt[40];
    UINT got = 0U;
    uint8_t have_fmt = 0U;
    uint8_t have_data = 0U;
    uint16_t format_tag = 0U;
    uint32_t file_size = (uint32_t)f_size(&audio_file);

    if (f_lseek(&audio_file, 0U) != FR_OK ||
        f_read(&audio_file, riff, sizeof(riff), &got) != FR_OK || got != sizeof(riff) ||
        memcmp(riff, "RIFF", 4U) != 0 || memcmp(&riff[8], "WAVE", 4U) != 0) return 0U;

    while ((uint32_t)f_tell(&audio_file) + sizeof(chunk) <= file_size) {
        uint32_t payload;
        uint32_t size;
        got = 0U;
        if (f_read(&audio_file, chunk, sizeof(chunk), &got) != FR_OK || got != sizeof(chunk)) break;
        size = le32(&chunk[4]);
        payload = (uint32_t)f_tell(&audio_file);
        if (size > file_size - payload) return 0U;

        if (memcmp(chunk, "fmt ", 4U) == 0 && size >= 16U) {
            uint32_t want = (size < sizeof(fmt)) ? size : sizeof(fmt);
            got = 0U;
            if (f_read(&audio_file, fmt, (UINT)want, &got) != FR_OK || got != want) return 0U;
            format_tag = le16(&fmt[0]);
            stream_info.channels = (uint8_t)le16(&fmt[2]);
            stream_info.sample_rate = le32(&fmt[4]);
            wav_block_align = le16(&fmt[12]);
            stream_info.bits_per_sample = (uint8_t)le16(&fmt[14]);
            /* WAVE_FORMAT_EXTENSIBLE: accept only the standard PCM subtype. */
            if (format_tag == 0xFFFEU && size >= 40U && got >= 40U &&
                le16(&fmt[16]) >= 22U && le16(&fmt[24]) == 1U) format_tag = 1U;
            have_fmt = 1U;
        } else if (memcmp(chunk, "data", 4U) == 0) {
            wav_data_offset = payload;
            wav_data_size = size;
            have_data = 1U;
        }
        if (have_fmt != 0U && have_data != 0U) break;
        if (f_lseek(&audio_file, payload + size + (size & 1U)) != FR_OK) return 0U;
    }

    if (have_fmt == 0U || have_data == 0U || format_tag != 1U ||
        wav_block_align == 0U || wav_block_align !=
        (uint16_t)(stream_info.channels * ((stream_info.bits_per_sample + 7U) / 8U))) return 0U;
    wav_frame_cursor = 0U;
    *total_frames = wav_data_size / wav_block_align;
    return (f_lseek(&audio_file, wav_data_offset) == FR_OK) ? 1U : 0U;
}

static void read_mp3_metadata(void)
{
    uint8_t header[10];
    UINT got = 0U;
    if (f_lseek(&audio_file, 0U) != FR_OK ||
        f_read(&audio_file, header, sizeof(header), &got) != FR_OK || got != sizeof(header)) return;
    if (memcmp(header, "ID3", 3U) == 0 && (header[3] == 3U || header[3] == 4U)) {
        uint32_t remaining = synchsafe32(&header[6]);
        uint8_t version = header[3];
        for (uint8_t frame = 0U; frame < 32U && remaining >= 10U; frame++) {
            uint8_t frame_header[10];
            uint32_t frame_size;
            uint32_t payload_start;
            got = 0U;
            if (f_read(&audio_file, frame_header, sizeof(frame_header), &got) != FR_OK ||
                got != sizeof(frame_header)) break;
            remaining -= 10U;
            if (frame_header[0] == 0U) break;
            frame_size = (version == 4U) ? synchsafe32(&frame_header[4]) : be32(&frame_header[4]);
            if (frame_size == 0U || frame_size > remaining) break;
            payload_start = (uint32_t)f_tell(&audio_file);
            if ((memcmp(frame_header, "TIT2", 4U) == 0 ||
                 memcmp(frame_header, "TPE1", 4U) == 0) && frame_size > 1U) {
                uint8_t text[64];
                UINT text_got = 0U;
                uint32_t want = (frame_size < sizeof(text)) ? frame_size : sizeof(text);
                if (f_read(&audio_file, text, (UINT)want, &text_got) == FR_OK && text_got > 1U &&
                    (text[0] == 0U || text[0] == 3U)) {
                    uint32_t copy = text_got - 1U;
                    if (copy >= sizeof(text)) copy = sizeof(text) - 1U;
                    memmove(text, &text[1], copy);
                    text[copy] = 0U;
                    text_copy((memcmp(frame_header, "TIT2", 4U) == 0)
                              ? stream_info.title : stream_info.artist,
                              (const char *)text, 64U);
                }
            }
            if (f_lseek(&audio_file, payload_start + frame_size) != FR_OK) break;
            remaining -= frame_size;
        }
    }
    if (stream_info.title[0] == 0 && f_size(&audio_file) >= 128U &&
        f_lseek(&audio_file, f_size(&audio_file) - 128U) == FR_OK &&
        f_read(&audio_file, header, 3U, &got) == FR_OK && got == 3U &&
        memcmp(header, "TAG", 3U) == 0) {
        uint8_t id3v1[60];
        got = 0U;
        if (f_read(&audio_file, id3v1, sizeof(id3v1), &got) == FR_OK && got == sizeof(id3v1)) {
            char field[31];
            memcpy(field, id3v1, 30U); field[30] = 0;
            text_copy(stream_info.title, field, sizeof(stream_info.title));
            memcpy(field, &id3v1[30], 30U); field[30] = 0;
            text_copy(stream_info.artist, field, sizeof(stream_info.artist));
        }
    }
    (void)f_lseek(&audio_file, 0U);
}

static uint8_t validate_info(void)
{
    if ((stream_info.channels != 1U && stream_info.channels != 2U) ||
        stream_info.sample_rate < 8000U || stream_info.sample_rate > 48000U ||
        stream_info.bits_per_sample < 8U || stream_info.bits_per_sample > 24U) {
        set_error("UNSUPPORTED FORMAT");
        return 0U;
    }
    if (stream_info.total_frames != 0U) {
        uint64_t duration = ((uint64_t)stream_info.total_frames * 1000ULL) /
                            stream_info.sample_rate;
        stream_info.duration_ms = (duration > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)duration;
    }
    return 1U;
}

__attribute__((cold)) uint8_t SD_AudioDecoderOpen(const char *path, sd_file_type_t type,
                            sd_audio_stream_info_t *info)
{
    drmp3_allocation_callbacks mp3_alloc;
#if SD_AUDIO_FLAC_ENABLE
    drflac_allocation_callbacks flac_alloc;
#endif
    uint64_t total;

    if (path == NULL || MOD_LocalAudioIoScopeActive() == 0U || !SD_FileTypeIsPlayable(type)) {
        set_error(type == SD_FILE_TYPE_FLAC ? "FLAC NOT BUILT IN" : "BAD OPEN REQUEST");
        return 0U;
    }
    SD_AudioDecoderClose();
    arena_base = (uint8_t *)SD_SharedAudioWorkspaceAcquire(SD_AUDIO_WORKSPACE_SIZE);
    if (arena_base == NULL) { set_error("AUDIO WORKSPACE"); return 0U; }
    memset(&stream_info, 0, sizeof(stream_info));
    stream_info.type = type;
    if (f_open(&audio_file, path, FA_READ) != FR_OK) {
        set_error("OPEN FILE");
        SD_SharedAudioWorkspaceRelease();
        arena_base = NULL;
        return 0U;
    }
    file_open = 1U;

    mp3_alloc.pUserData = NULL;
    mp3_alloc.onMalloc = arena_malloc;
    mp3_alloc.onRealloc = arena_realloc;
    mp3_alloc.onFree = arena_free;
#if SD_AUDIO_FLAC_ENABLE
    flac_alloc.pUserData = NULL;
    flac_alloc.onMalloc = arena_malloc;
    flac_alloc.onRealloc = arena_realloc;
    flac_alloc.onFree = arena_free;
#endif

    if (type == SD_FILE_TYPE_WAV) {
        arena_reset(0U);
        if (!open_pcm_wav(&total)) {
            set_error("WAV PCM ONLY");
            SD_AudioDecoderClose();
            return 0U;
        }
        decoder_kind = DECODER_WAV;
    } else if (type == SD_FILE_TYPE_MP3) {
        read_mp3_metadata();
        arena_reset(sizeof(drmp3));
        mp3_decoder = (drmp3 *)arena_base;
        memset(mp3_decoder, 0, sizeof(*mp3_decoder));
        if (!drmp3_init(mp3_decoder, file_read, mp3_seek, mp3_tell, NULL, NULL, &mp3_alloc)) {
            set_error("MP3 DECODE");
            SD_AudioDecoderClose();
            return 0U;
        }
        decoder_kind = DECODER_MP3;
        stream_info.channels = (uint8_t)mp3_decoder->channels;
        stream_info.sample_rate = mp3_decoder->sampleRate;
        stream_info.bits_per_sample = 16U;
        total = mp3_decoder->totalPCMFrameCount;
        if (total == DRMP3_UINT64_MAX) total = 0U;
#if SD_AUDIO_FLAC_ENABLE
    } else {
        arena_reset(0U);
        flac_decoder = drflac_open(file_read, flac_seek, flac_tell, NULL, &flac_alloc);
        if (flac_decoder == NULL || flac_decoder->container != drflac_container_native) {
            set_error("FLAC DECODE/RAM");
            SD_AudioDecoderClose();
            return 0U;
        }
        decoder_kind = DECODER_FLAC;
        stream_info.channels = flac_decoder->channels;
        stream_info.sample_rate = flac_decoder->sampleRate;
        stream_info.bits_per_sample = flac_decoder->bitsPerSample;
        total = flac_decoder->totalPCMFrameCount;
    }
#else
    } else {
        /* SD_FileTypeIsPlayable already rejected it above; this keeps `total`
         * definitely assigned without a decoder that is not linked in. */
        set_error("FLAC NOT BUILT IN");
        SD_AudioDecoderClose();
        return 0U;
    }
#endif

    stream_info.total_frames = (total > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)total;
    if (!validate_info()) {
        SD_AudioDecoderClose();
        return 0U;
    }
    if (stream_info.title[0] == 0) {
        text_copy(stream_info.title, base_name(path), sizeof(stream_info.title));
    }
    if (info != NULL) *info = stream_info;
    set_error("");
    return 1U;
}

void SD_AudioDecoderClose(void)
{
    if (decoder_kind == DECODER_MP3 && mp3_decoder != NULL) drmp3_uninit(mp3_decoder);
#if SD_AUDIO_FLAC_ENABLE
    else if (decoder_kind == DECODER_FLAC && flac_decoder != NULL) drflac_close(flac_decoder);
#endif
    decoder_kind = DECODER_NONE;
    mp3_decoder = NULL;
#if SD_AUDIO_FLAC_ENABLE
    flac_decoder = NULL;
#endif
    if (file_open != 0U) (void)f_close(&audio_file);
    file_open = 0U;
    if (arena_base != NULL) SD_SharedAudioWorkspaceRelease();
    arena_base = NULL;
}

uint8_t SD_AudioDecoderIsOpen(void) { return (decoder_kind != DECODER_NONE) ? 1U : 0U; }

uint32_t SD_AudioDecoderReadS32(int32_t *interleaved, uint32_t frames)
{
    uint64_t got = 0U;
    if (interleaved == NULL || frames == 0U || MOD_LocalAudioIoScopeActive() == 0U) return 0U;
    if (decoder_kind == DECODER_WAV) {
        uint32_t remaining = wav_data_size - (wav_frame_cursor * wav_block_align);
        uint32_t bytes;
        uint32_t samples;
        uint32_t bytes_per_sample = (stream_info.bits_per_sample + 7U) / 8U;
        UINT read_len = 0U;
        if (frames > remaining / wav_block_align) frames = remaining / wav_block_align;
        bytes = frames * wav_block_align;
        if (bytes != 0U && f_read(&audio_file, interleaved, (UINT)bytes, &read_len) == FR_OK) {
            got = read_len / wav_block_align;
        }
        samples = (uint32_t)got * stream_info.channels;
        while (samples > 0U) {
            uint8_t *raw = (uint8_t *)interleaved;
            samples--;
            interleaved[samples] = SD_AudioPcmSampleToS32(
                &raw[samples * bytes_per_sample], stream_info.bits_per_sample);
        }
        wav_frame_cursor += (uint32_t)got;
#if SD_AUDIO_FLAC_ENABLE
    } else if (decoder_kind == DECODER_FLAC) {
        got = drflac_read_pcm_frames_s32(flac_decoder, frames, (drflac_int32 *)interleaved);
#endif
    } else if (decoder_kind == DECODER_MP3) {
        int16_t *temporary = (int16_t *)interleaved;
        got = drmp3_read_pcm_frames_s16(mp3_decoder, frames, (drmp3_int16 *)temporary);
        uint32_t samples = (uint32_t)got * stream_info.channels;
        while (samples > 0U) {
            samples--;
            interleaved[samples] = (int32_t)temporary[samples] << 16;
        }
    }
    return (got > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)got;
}

uint8_t SD_AudioDecoderSeekFrame(uint32_t frame)
{
    if (MOD_LocalAudioIoScopeActive() == 0U) return 0U;
    if (stream_info.total_frames != 0U && frame > stream_info.total_frames) frame = stream_info.total_frames;
    if (decoder_kind == DECODER_WAV) {
        if (f_lseek(&audio_file, wav_data_offset + frame * wav_block_align) != FR_OK) return 0U;
        wav_frame_cursor = frame;
        return 1U;
    }
    if (decoder_kind == DECODER_MP3) return drmp3_seek_to_pcm_frame(mp3_decoder, frame) ? 1U : 0U;
#if SD_AUDIO_FLAC_ENABLE
    if (decoder_kind == DECODER_FLAC) return drflac_seek_to_pcm_frame(flac_decoder, frame) ? 1U : 0U;
#endif
    return 0U;
}

uint32_t SD_AudioDecoderTellFrame(void)
{
    uint64_t frame = 0U;
    if (decoder_kind == DECODER_WAV) frame = wav_frame_cursor;
    else if (decoder_kind == DECODER_MP3) frame = mp3_decoder->currentPCMFrame;
#if SD_AUDIO_FLAC_ENABLE
    else if (decoder_kind == DECODER_FLAC) frame = flac_decoder->currentPCMFrame;
#endif
    return (frame > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)frame;
}

const char *SD_AudioDecoderError(void) { return decoder_error; }

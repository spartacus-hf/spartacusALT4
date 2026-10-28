/**
 * @file    sd_file_browser.h
 * @brief   Bounded UTF-8 SD browser, viewers, and background file jobs.
 */
#ifndef SD_FILE_BROWSER_H
#define SD_FILE_BROWSER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SD_BROWSER_PATH_MAX       512U
#define SD_BROWSER_NAME_MAX       256U
#define SD_BROWSER_ENTRY_MAX      192U
#define SD_BROWSER_NAME_POOL_SIZE 12288U
#define SD_BROWSER_DEPTH_MAX       32U
#define SD_AUDIO_WORKSPACE_SIZE  38400U

#define SD_FILE_ATTR_READONLY 0x01U
#define SD_FILE_ATTR_HIDDEN   0x02U
#define SD_FILE_ATTR_SYSTEM   0x04U
#define SD_FILE_ATTR_DIR      0x10U
#define SD_FILE_ATTR_ARCHIVE  0x20U

typedef enum {
    SD_FILE_TYPE_DIRECTORY = 0,
    SD_FILE_TYPE_WAV,
    SD_FILE_TYPE_MP3,
    SD_FILE_TYPE_FLAC,
    SD_FILE_TYPE_TEXT,
    SD_FILE_TYPE_BINARY
} sd_file_type_t;

/* Decoding FLAC costs 34 KB of flash -- drflac's Rice residual decoder alone is
 * 15 KB, the largest single function in the image -- which REV1 spends on the
 * receive bench instead. WAV and MP3 playback are unaffected. A .flac file
 * keeps its type and label in the browser, so it stays visibly present rather
 * than looking like a missing file, but it is not offered for playback. */
#ifndef SD_AUDIO_FLAC_ENABLE
#define SD_AUDIO_FLAC_ENABLE 0
#endif

/* The one place that decides which file types the audio player accepts: the
 * browser filter, the LCD list, the player and the decoder all ask here. */
static inline uint8_t SD_FileTypeIsPlayable(sd_file_type_t type)
{
    return (uint8_t)(type == SD_FILE_TYPE_WAV || type == SD_FILE_TYPE_MP3 ||
                     (SD_AUDIO_FLAC_ENABLE && type == SD_FILE_TYPE_FLAC));
}

typedef enum {
    SD_BROWSER_VIEW_DETAILS = 0,
    SD_BROWSER_VIEW_LIST,
    SD_BROWSER_VIEW_COMPACT
} sd_browser_view_t;

typedef enum {
    SD_BROWSER_SORT_NAME = 0,
    SD_BROWSER_SORT_TYPE,
    SD_BROWSER_SORT_SIZE,
    SD_BROWSER_SORT_DATE,
    SD_BROWSER_SORT_CARD
} sd_browser_sort_t;

typedef enum {
    SD_BROWSER_FILTER_ALL = 0,
    SD_BROWSER_FILTER_AUDIO,
    SD_BROWSER_FILTER_TEXT,
    SD_BROWSER_FILTER_OTHER
} sd_browser_filter_t;

typedef struct {
    const char     *name;
    uint32_t        size;
    uint16_t        date;
    uint16_t        time;
    uint8_t         attr;
    sd_file_type_t  type;
} sd_file_entry_t;

typedef struct {
    sd_browser_view_t   view;
    sd_browser_sort_t   sort;
    sd_browser_filter_t filter;
    uint8_t descending;
    uint8_t folders_first;
    uint8_t show_hidden;
} sd_browser_config_t;

typedef enum {
    SD_FILE_JOB_IDLE = 0,
    SD_FILE_JOB_COPY,
    SD_FILE_JOB_DELETE,
    SD_FILE_JOB_FAILED,
    SD_FILE_JOB_DONE,
    SD_FILE_JOB_CANCELLED
} sd_file_job_kind_t;

typedef struct {
    sd_file_job_kind_t kind;
    uint32_t files_done;
    uint32_t dirs_done;
    uint32_t bytes_done;
    uint32_t current_size;
    uint8_t depth;
    uint8_t cancellable;
    const char *current_name;
    const char *error;
} sd_file_job_status_t;

void SD_BrowserInit(void);
void SD_BrowserReset(void);
uint8_t SD_BrowserOpen(const char *path);
uint8_t SD_BrowserRefresh(void);
uint8_t SD_BrowserEnter(uint8_t index);
uint8_t SD_BrowserUp(void);
uint8_t SD_BrowserNextPage(void);
uint8_t SD_BrowserPreviousPage(void);
uint8_t SD_BrowserHasNextPage(void);
uint8_t SD_BrowserHasPreviousPage(void);
const char *SD_BrowserPath(void);
uint8_t SD_BrowserCount(void);
uint8_t SD_BrowserOverflow(void);
const sd_file_entry_t *SD_BrowserEntry(uint8_t index);

void SD_BrowserSetConfig(const sd_browser_config_t *config);
void SD_BrowserGetConfig(sd_browser_config_t *config);

sd_file_type_t SD_FileTypeFromName(const char *name, uint8_t attr);
const char *SD_FileTypeLabel(sd_file_type_t type);
uint8_t SD_BrowserSelectedPath(uint8_t index, char *path, uint32_t cap);
uint8_t SD_BrowserMkdir(const char *name);
uint8_t SD_BrowserRename(uint8_t index, const char *new_name);
uint8_t SD_BrowserDeleteStart(uint8_t index);
uint8_t SD_BrowserClipboardSet(uint8_t index, uint8_t move);
uint8_t SD_BrowserClipboardReady(void);
uint8_t SD_BrowserClipboardIsMove(void);
const char *SD_BrowserClipboardPath(void);
uint8_t SD_BrowserPasteStart(void);

void SD_FileJobService(void);
void SD_FileJobCancel(void);
void SD_FileJobClearResult(void);
void SD_FileJobGetStatus(sd_file_job_status_t *status);
uint8_t SD_FileJobActive(void);

uint8_t SD_ViewerOpen(const char *path);
void SD_ViewerClose(void);
uint8_t SD_ViewerIsOpen(void);
sd_file_type_t SD_ViewerType(void);
uint32_t SD_ViewerSize(void);
const char *SD_ViewerPath(void);
uint8_t SD_ViewerRead(uint32_t offset, void *data, uint32_t len, uint32_t *read_len);
uint8_t SD_ViewerFindStart(const char *needle, uint32_t start, uint8_t match_case);
void SD_ViewerFindService(void);
uint8_t SD_ViewerFindActive(void);
uint8_t SD_ViewerFindTakeResult(uint32_t *found_offset, uint8_t *found,
                                uint8_t *read_error);
void SD_ViewerFindCancel(void);

uint8_t SD_PathIsValidName(const char *name);
uint8_t SD_PathJoin(const char *directory, const char *name, char *out, uint32_t cap);
void *SD_SharedReceiverWorkspaceAcquire(uint32_t bytes);
void SD_SharedReceiverWorkspaceRelease(void);
void *SD_SharedAudioWorkspaceAcquire(uint32_t bytes);
void SD_SharedAudioWorkspaceRelease(void);
uint8_t SD_SharedAudioWorkspaceOwned(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_FILE_BROWSER_H */

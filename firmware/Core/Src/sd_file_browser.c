/* Size over speed: nothing here runs in the RF or audio path. */
#pragma GCC optimize ("Os")

/**
 * @file    sd_file_browser.c
 * @brief   UTF-8 SD browser, streaming viewers, and incremental file jobs.
 *
 * All FatFs calls in this module run from the foreground main loop.  The
 * module deliberately refuses work while the RF/audio path owns the CPU.
 */

#include "sd_file_browser.h"
#include "mod_sdcard_service.h"
#include "modulator.h"
#include "main.h"
#include "../../Middlewares/Third_Party/FatFs/src/ff.h"
#include <string.h>

#define BROWSER_PAGE_HISTORY 128U

typedef struct {
    uint16_t name_offset;
    uint16_t date;
    uint16_t time;
    uint16_t card_order;
    uint32_t size;
    uint8_t attr;
    uint8_t type;
} browser_entry_t;

typedef enum {
    JOB_PHASE_NONE = 0,
    JOB_PHASE_COPY_SCAN,
    JOB_PHASE_COPY_OPEN,
    JOB_PHASE_COPY_DATA,
    JOB_PHASE_DELETE_SCAN,
    JOB_PHASE_DELETE_FILE
} job_phase_t;

typedef struct {
    sd_file_job_kind_t public_kind;
    sd_file_job_kind_t operation;
    job_phase_t phase;
    uint8_t root_is_dir;
    uint8_t depth;
    uint8_t file_open;
    uint8_t cancel_requested;
    uint32_t files_done;
    uint32_t dirs_done;
    uint32_t bytes_done;
    uint32_t current_size;
    uint16_t next_index[SD_BROWSER_DEPTH_MAX];
    uint16_t src_parent_len[SD_BROWSER_DEPTH_MAX];
    uint16_t dst_parent_len[SD_BROWSER_DEPTH_MAX];
    char src_root[SD_BROWSER_PATH_MAX];
    char dst_root[SD_BROWSER_PATH_MAX];
    char src_dir[SD_BROWSER_PATH_MAX];
    char dst_dir[SD_BROWSER_PATH_MAX];
    char src_item[SD_BROWSER_PATH_MAX];
    char dst_item[SD_BROWSER_PATH_MAX];
    char error[32];
    FIL src_file;
    FIL dst_file;
    uint8_t copy_buffer[256];
} file_job_t;

typedef struct {
    char name_pool[SD_BROWSER_NAME_POOL_SIZE];
    browser_entry_t entries[SD_BROWSER_ENTRY_MAX];
    file_job_t file_job;
    char clipboard_path[SD_BROWSER_PATH_MAX];
    char viewer_path[SD_BROWSER_PATH_MAX];
} browser_workspace_t;

typedef union {
    browser_workspace_t browser;
    uint64_t align;
    uint8_t audio[SD_AUDIO_WORKSPACE_SIZE];
} sd_shared_workspace_t;

_Static_assert(sizeof(browser_workspace_t) <= SD_AUDIO_WORKSPACE_SIZE,
               "browser workspace must fit the shared audio arena");

static sd_shared_workspace_t shared_workspace;
#define name_pool (shared_workspace.browser.name_pool)
#define entries (shared_workspace.browser.entries)
#define file_job  (shared_workspace.browser.file_job)
#define copy_buffer (file_job.copy_buffer)
#define clipboard_path (shared_workspace.browser.clipboard_path)
#define viewer_path (shared_workspace.browser.viewer_path)

static sd_file_entry_t public_entry;
static uint16_t name_pool_used;
static uint8_t entry_count;
static uint8_t entry_overflow;
static uint8_t browser_has_more;
static uint8_t browser_page_index;
static uint32_t browser_page_skip;
static uint32_t browser_page_starts[BROWSER_PAGE_HISTORY];
/* Zeroed, not "/": an initializer would put all 512 bytes in flash as .data.
 * SD_BrowserInit sets the root before anything reads it. */
static char browser_path[SD_BROWSER_PATH_MAX];
static sd_browser_config_t browser_config = {
    SD_BROWSER_VIEW_DETAILS,
    SD_BROWSER_SORT_NAME,
    SD_BROWSER_FILTER_ALL,
    0U,
    1U,
    0U
};

static uint8_t clipboard_ready;
static uint8_t clipboard_move;
static uint8_t clipboard_attr;
static uint8_t audio_workspace_owned;

static FIL viewer_file;
static uint8_t viewer_open;
static uint32_t viewer_size;
static sd_file_type_t viewer_type;
static uint8_t viewer_find_pattern[64];
static uint8_t viewer_find_prefix[64];
static uint8_t viewer_find_pattern_len;
static uint8_t viewer_find_match_case;
static uint8_t viewer_find_active;
static uint8_t viewer_find_done;
static uint8_t viewer_find_found;
static uint8_t viewer_find_error;
static uint8_t viewer_find_matched;
static uint32_t viewer_find_offset;
static uint32_t viewer_find_result;

static uint8_t io_allowed(void)
{
    return (audio_workspace_owned == 0U) && (SD_IsMounted() != 0U) &&
           MOD_BlockingIoAllowed(HAL_GetTick());
}

static void str_copy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0U;
    if (dst == NULL || cap == 0U) return;
    if (src != NULL) {
        while ((i + 1U) < cap && src[i] != 0) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void browser_page_reset(void)
{
    browser_page_skip = 0U;
    browser_page_index = 0U;
    browser_page_starts[0] = 0U;
    browser_has_more = 0U;
}

static uint8_t ascii_lower(uint8_t c)
{
    return (c >= (uint8_t)'A' && c <= (uint8_t)'Z') ? (uint8_t)(c + 32U) : c;
}

static int name_compare(const char *a, const char *b)
{
    uint32_t i = 0U;
    while (a[i] != 0 && b[i] != 0) {
        uint8_t ac = ascii_lower((uint8_t)a[i]);
        uint8_t bc = ascii_lower((uint8_t)b[i]);
        if (ac != bc) return (ac < bc) ? -1 : 1;
        i++;
    }
    if (a[i] == b[i]) return 0;
    return (a[i] == 0) ? -1 : 1;
}

static const char *entry_name(const browser_entry_t *entry)
{
    return &name_pool[entry->name_offset];
}

static const char *path_basename(const char *path)
{
    const char *base = path;
    if (path == NULL) return "";
    for (uint32_t i = 0U; path[i] != 0; i++) {
        if (path[i] == '/') base = &path[i + 1U];
    }
    return base;
}

static const char *extension(const char *name)
{
    const char *ext = NULL;
    if (name == NULL) return NULL;
    for (uint32_t i = 0U; name[i] != 0; i++) {
        if (name[i] == '.') ext = &name[i + 1U];
    }
    return ext;
}

static uint8_t ext_is(const char *ext, const char *wanted)
{
    uint32_t i = 0U;
    if (ext == NULL) return 0U;
    while (ext[i] != 0 && wanted[i] != 0) {
        if (ascii_lower((uint8_t)ext[i]) != (uint8_t)wanted[i]) return 0U;
        i++;
    }
    return (ext[i] == 0 && wanted[i] == 0) ? 1U : 0U;
}

sd_file_type_t SD_FileTypeFromName(const char *name, uint8_t attr)
{
    const char *ext;
    if ((attr & SD_FILE_ATTR_DIR) != 0U) return SD_FILE_TYPE_DIRECTORY;
    ext = extension(name);
    if (ext_is(ext, "wav")) return SD_FILE_TYPE_WAV;
    if (ext_is(ext, "mp3")) return SD_FILE_TYPE_MP3;
    if (ext_is(ext, "flac")) return SD_FILE_TYPE_FLAC;
    if (ext_is(ext, "txt") || ext_is(ext, "log") || ext_is(ext, "md") ||
        ext_is(ext, "ini") || ext_is(ext, "cfg") || ext_is(ext, "csv") ||
        ext_is(ext, "json") || ext_is(ext, "xml") || ext_is(ext, "c") ||
        ext_is(ext, "h")) return SD_FILE_TYPE_TEXT;
    return SD_FILE_TYPE_BINARY;
}

const char *SD_FileTypeLabel(sd_file_type_t type)
{
    switch (type) {
        case SD_FILE_TYPE_DIRECTORY: return "DIR";
        case SD_FILE_TYPE_WAV:       return "WAV";
        case SD_FILE_TYPE_MP3:       return "MP3";
        case SD_FILE_TYPE_FLAC:      return "FLAC";
        case SD_FILE_TYPE_TEXT:      return "TEXT";
        case SD_FILE_TYPE_BINARY:
        default:                     return "FILE";
    }
}

uint8_t SD_PathIsValidName(const char *name)
{
    uint32_t len = 0U;
    if (name == NULL || name[0] == 0) return 0U;
    if ((name[0] == '.' && name[1] == 0) ||
        (name[0] == '.' && name[1] == '.' && name[2] == 0)) return 0U;
    while (name[len] != 0) {
        uint8_t c = (uint8_t)name[len];
        if (c < 0x20U || c == 0x7FU || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0U;
        len++;
        if (len >= SD_BROWSER_NAME_MAX) return 0U;
    }
    return (name[len - 1U] != ' ' && name[len - 1U] != '.') ? 1U : 0U;
}

uint8_t SD_PathJoin(const char *directory, const char *name, char *out, uint32_t cap)
{
    uint32_t dlen;
    uint32_t nlen;
    uint8_t slash;
    if (directory == NULL || name == NULL || out == NULL || cap == 0U) return 0U;
    dlen = (uint32_t)strlen(directory);
    nlen = (uint32_t)strlen(name);
    slash = (dlen > 0U && directory[dlen - 1U] != '/') ? 1U : 0U;
    if (dlen + (uint32_t)slash + nlen + 1U > cap) return 0U;
    memcpy(out, directory, dlen);
    if (slash != 0U) out[dlen++] = '/';
    memcpy(&out[dlen], name, nlen + 1U);
    return 1U;
}

static uint8_t filter_accept(const FILINFO *info, sd_file_type_t type)
{
    if ((info->fattrib & AM_DIR) != 0U) return 1U;
    switch (browser_config.filter) {
        case SD_BROWSER_FILTER_AUDIO: return SD_FileTypeIsPlayable(type);
        case SD_BROWSER_FILTER_TEXT:  return (type == SD_FILE_TYPE_TEXT) ? 1U : 0U;
        case SD_BROWSER_FILTER_OTHER: return (type == SD_FILE_TYPE_BINARY) ? 1U : 0U;
        case SD_BROWSER_FILTER_ALL:
        default:                      return 1U;
    }
}

static int entry_compare(const browser_entry_t *a, const browser_entry_t *b)
{
    int cmp = 0;
    uint8_t a_dir = ((a->attr & AM_DIR) != 0U) ? 1U : 0U;
    uint8_t b_dir = ((b->attr & AM_DIR) != 0U) ? 1U : 0U;

    if (browser_config.folders_first != 0U && a_dir != b_dir) return a_dir ? -1 : 1;
    switch (browser_config.sort) {
        case SD_BROWSER_SORT_TYPE:
            if (a->type != b->type) cmp = (a->type < b->type) ? -1 : 1;
            break;
        case SD_BROWSER_SORT_SIZE:
            if (a->size != b->size) cmp = (a->size < b->size) ? -1 : 1;
            break;
        case SD_BROWSER_SORT_DATE:
            if (a->date != b->date) cmp = (a->date < b->date) ? -1 : 1;
            else if (a->time != b->time) cmp = (a->time < b->time) ? -1 : 1;
            break;
        case SD_BROWSER_SORT_CARD:
            if (a->card_order != b->card_order) cmp = (a->card_order < b->card_order) ? -1 : 1;
            break;
        case SD_BROWSER_SORT_NAME:
        default:
            break;
    }
    if (cmp == 0) cmp = name_compare(entry_name(a), entry_name(b));
    if (browser_config.descending != 0U) cmp = -cmp;
    return cmp;
}

static void sort_entries(void)
{
    for (uint8_t i = 1U; i < entry_count; i++) {
        browser_entry_t value = entries[i];
        uint8_t j = i;
        while (j > 0U && entry_compare(&value, &entries[j - 1U]) < 0) {
            entries[j] = entries[j - 1U];
            j--;
        }
        entries[j] = value;
    }
}

void SD_BrowserInit(void)
{
    browser_path[0] = '/';
    browser_path[1] = 0;
    SD_BrowserReset();
}

void SD_BrowserReset(void)
{
    if (audio_workspace_owned != 0U) return;
    entry_count = 0U;
    entry_overflow = 0U;
    name_pool_used = 0U;
    clipboard_ready = 0U;
    clipboard_move = 0U;
    browser_path[0] = '/';
    browser_path[1] = 0;
    browser_page_reset();
    SD_ViewerClose();
    memset(&file_job, 0, sizeof(file_job));
}

uint8_t SD_BrowserOpen(const char *path)
{
    DIR dir;
    FRESULT result;
    uint32_t len;
    if (!io_allowed() || path == NULL) return 0U;
    len = (uint32_t)strlen(path);
    if (len == 0U || len >= sizeof(browser_path) || path[0] != '/') return 0U;
    result = f_opendir(&dir, path);
    if (result != FR_OK) return 0U;
    (void)f_closedir(&dir);
    str_copy(browser_path, path, sizeof(browser_path));
    while (len > 1U && browser_path[len - 1U] == '/') browser_path[--len] = 0;
    browser_page_reset();
    SD_ViewerClose();
    return SD_BrowserRefresh();
}

uint8_t SD_BrowserRefresh(void)
{
    DIR dir;
    FILINFO info;
    FRESULT result;
    uint16_t card_order = 0U;
    uint32_t accepted = 0U;

    entry_count = 0U;
    entry_overflow = 0U;
    browser_has_more = 0U;
    name_pool_used = 0U;
    if (!io_allowed()) return 0U;
    result = f_opendir(&dir, browser_path);
    if (result != FR_OK) return 0U;

    for (;;) {
        uint32_t len;
        sd_file_type_t type;
        memset(&info, 0, sizeof(info));
        result = f_readdir(&dir, &info);
        if (result != FR_OK) {
            (void)f_closedir(&dir);
            return 0U;
        }
        if (info.fname[0] == 0) break;
        if ((info.fname[0] == '.' && info.fname[1] == 0) ||
            (info.fname[0] == '.' && info.fname[1] == '.' && info.fname[2] == 0)) continue;
        if (browser_config.show_hidden == 0U &&
            (info.fattrib & (AM_HID | AM_SYS)) != 0U) continue;
        type = SD_FileTypeFromName(info.fname, info.fattrib);
        if (!filter_accept(&info, type)) continue;
        if (accepted++ < browser_page_skip) continue;
        len = (uint32_t)strlen(info.fname) + 1U;
        if (entry_count >= SD_BROWSER_ENTRY_MAX ||
            (uint32_t)name_pool_used + len > SD_BROWSER_NAME_POOL_SIZE) {
            entry_overflow = 1U;
            browser_has_more = 1U;
            break;
        }
        memcpy(&name_pool[name_pool_used], info.fname, len);
        entries[entry_count].name_offset = name_pool_used;
        entries[entry_count].date = info.fdate;
        entries[entry_count].time = info.ftime;
        entries[entry_count].card_order = card_order++;
        entries[entry_count].size = (uint32_t)info.fsize;
        entries[entry_count].attr = info.fattrib;
        entries[entry_count].type = (uint8_t)type;
        name_pool_used = (uint16_t)(name_pool_used + len);
        entry_count++;
    }
    (void)f_closedir(&dir);
    sort_entries();
    return 1U;
}

uint8_t SD_BrowserEnter(uint8_t index)
{
    char path[SD_BROWSER_PATH_MAX];
    const sd_file_entry_t *entry = SD_BrowserEntry(index);
    if (entry == NULL || (entry->attr & AM_DIR) == 0U) return 0U;
    if (!SD_PathJoin(browser_path, entry->name, path, sizeof(path))) return 0U;
    return SD_BrowserOpen(path);
}

uint8_t SD_BrowserUp(void)
{
    uint32_t len = (uint32_t)strlen(browser_path);
    if (len <= 1U) return 1U;
    while (len > 1U && browser_path[len - 1U] != '/') len--;
    if (len > 1U) len--;
    browser_path[len] = 0;
    if (browser_path[0] == 0) str_copy(browser_path, "/", sizeof(browser_path));
    browser_page_reset();
    return SD_BrowserRefresh();
}

uint8_t SD_BrowserNextPage(void)
{
    uint32_t previous_skip;
    uint8_t previous_index;
    if (browser_has_more == 0U || entry_count == 0U || !io_allowed()) return 0U;
    previous_skip = browser_page_skip;
    previous_index = browser_page_index;
    browser_page_skip += entry_count;
    if (browser_page_index + 1U < BROWSER_PAGE_HISTORY) {
        browser_page_index++;
    } else {
        memmove(&browser_page_starts[0], &browser_page_starts[1],
                (BROWSER_PAGE_HISTORY - 1U) * sizeof(browser_page_starts[0]));
    }
    browser_page_starts[browser_page_index] = browser_page_skip;
    if (SD_BrowserRefresh()) return 1U;
    browser_page_skip = previous_skip;
    browser_page_index = previous_index;
    (void)SD_BrowserRefresh();
    return 0U;
}

uint8_t SD_BrowserPreviousPage(void)
{
    uint32_t previous_skip;
    uint8_t previous_index;
    if (browser_page_skip == 0U || browser_page_index == 0U || !io_allowed()) return 0U;
    previous_skip = browser_page_skip;
    previous_index = browser_page_index;
    browser_page_index--;
    browser_page_skip = browser_page_starts[browser_page_index];
    if (SD_BrowserRefresh()) return 1U;
    browser_page_skip = previous_skip;
    browser_page_index = previous_index;
    (void)SD_BrowserRefresh();
    return 0U;
}

uint8_t SD_BrowserHasNextPage(void) { return browser_has_more; }
uint8_t SD_BrowserHasPreviousPage(void) { return (browser_page_skip != 0U) ? 1U : 0U; }

const char *SD_BrowserPath(void) { return browser_path; }
uint8_t SD_BrowserCount(void) { return entry_count; }
uint8_t SD_BrowserOverflow(void) { return entry_overflow; }

const sd_file_entry_t *SD_BrowserEntry(uint8_t index)
{
    const browser_entry_t *entry;
    if (index >= entry_count) return NULL;
    entry = &entries[index];
    public_entry.name = entry_name(entry);
    public_entry.size = entry->size;
    public_entry.date = entry->date;
    public_entry.time = entry->time;
    public_entry.attr = entry->attr;
    public_entry.type = (sd_file_type_t)entry->type;
    return &public_entry;
}

void SD_BrowserSetConfig(const sd_browser_config_t *config)
{
    if (config == NULL) return;
    browser_config = *config;
    if (browser_config.view > SD_BROWSER_VIEW_COMPACT) browser_config.view = SD_BROWSER_VIEW_DETAILS;
    if (browser_config.sort > SD_BROWSER_SORT_CARD) browser_config.sort = SD_BROWSER_SORT_NAME;
    if (browser_config.filter > SD_BROWSER_FILTER_OTHER) browser_config.filter = SD_BROWSER_FILTER_ALL;
    browser_config.descending = browser_config.descending ? 1U : 0U;
    browser_config.folders_first = browser_config.folders_first ? 1U : 0U;
    browser_config.show_hidden = browser_config.show_hidden ? 1U : 0U;
    browser_page_reset();
    if (io_allowed()) (void)SD_BrowserRefresh();
}

void SD_BrowserGetConfig(sd_browser_config_t *config)
{
    if (config != NULL) *config = browser_config;
}

uint8_t SD_BrowserSelectedPath(uint8_t index, char *path, uint32_t cap)
{
    const sd_file_entry_t *entry = SD_BrowserEntry(index);
    return (entry != NULL) ? SD_PathJoin(browser_path, entry->name, path, cap) : 0U;
}

uint8_t SD_BrowserMkdir(const char *name)
{
    char path[SD_BROWSER_PATH_MAX];
    FRESULT result;
    if (!io_allowed() || !SD_PathIsValidName(name)) return 0U;
    if (!SD_PathJoin(browser_path, name, path, sizeof(path))) return 0U;
    result = f_mkdir(path);
    if (result != FR_OK) return 0U;
    return SD_BrowserRefresh();
}

uint8_t SD_BrowserRename(uint8_t index, const char *new_name)
{
    char old_path[SD_BROWSER_PATH_MAX];
    char new_path[SD_BROWSER_PATH_MAX];
    FRESULT result;
    if (!io_allowed() || !SD_PathIsValidName(new_name)) return 0U;
    if (!SD_BrowserSelectedPath(index, old_path, sizeof(old_path)) ||
        !SD_PathJoin(browser_path, new_name, new_path, sizeof(new_path))) return 0U;
    result = f_rename(old_path, new_path);
    if (result != FR_OK) return 0U;
    return SD_BrowserRefresh();
}

static void job_close_files(void)
{
    if (file_job.file_open != 0U) {
        (void)f_close(&file_job.src_file);
        (void)f_close(&file_job.dst_file);
        file_job.file_open = 0U;
    }
}

static void job_finish(sd_file_job_kind_t result, const char *error)
{
    job_close_files();
    file_job.public_kind = result;
    file_job.phase = JOB_PHASE_NONE;
    file_job.cancel_requested = 0U;
    str_copy(file_job.error, error, sizeof(file_job.error));
    if (io_allowed()) (void)SD_BrowserRefresh();
}

static void job_fail(const char *error)
{
    job_finish(SD_FILE_JOB_FAILED, error);
}

static uint8_t dir_item_at(const char *path, uint16_t wanted, FILINFO *out)
{
    DIR dir;
    FRESULT result;
    uint16_t index = 0U;
    result = f_opendir(&dir, path);
    if (result != FR_OK) return 0U;
    for (;;) {
        memset(out, 0, sizeof(*out));
        result = f_readdir(&dir, out);
        if (result != FR_OK) {
            (void)f_closedir(&dir);
            return 0U;
        }
        if (out->fname[0] == 0) {
            (void)f_closedir(&dir);
            return 2U;
        }
        if ((out->fname[0] == '.' && out->fname[1] == 0) ||
            (out->fname[0] == '.' && out->fname[1] == '.' && out->fname[2] == 0)) continue;
        if (index++ == wanted) {
            (void)f_closedir(&dir);
            return 1U;
        }
    }
}

static uint8_t path_is_inside(const char *parent, const char *candidate)
{
    uint32_t len = (uint32_t)strlen(parent);
    if (strncmp(parent, candidate, len) != 0) return 0U;
    return (candidate[len] == 0 || candidate[len] == '/') ? 1U : 0U;
}

uint8_t SD_BrowserDeleteStart(uint8_t index)
{
    FILINFO info;
    char path[SD_BROWSER_PATH_MAX];
    if (!io_allowed() || SD_FileJobActive()) return 0U;
    if (!SD_BrowserSelectedPath(index, path, sizeof(path))) return 0U;
    if (f_stat(path, &info) != FR_OK || (info.fattrib & AM_RDO) != 0U) return 0U;
    SD_ViewerClose();
    memset(&file_job, 0, sizeof(file_job));
    file_job.public_kind = SD_FILE_JOB_DELETE;
    file_job.operation = SD_FILE_JOB_DELETE;
    file_job.root_is_dir = ((info.fattrib & AM_DIR) != 0U) ? 1U : 0U;
    file_job.current_size = (uint32_t)info.fsize;
    str_copy(file_job.src_root, path, sizeof(file_job.src_root));
    str_copy(file_job.src_dir, path, sizeof(file_job.src_dir));
    file_job.phase = file_job.root_is_dir ? JOB_PHASE_DELETE_SCAN : JOB_PHASE_DELETE_FILE;
    return 1U;
}

uint8_t SD_BrowserClipboardSet(uint8_t index, uint8_t move)
{
    const sd_file_entry_t *entry = SD_BrowserEntry(index);
    if (entry == NULL || !SD_BrowserSelectedPath(index, clipboard_path, sizeof(clipboard_path))) return 0U;
    clipboard_ready = 1U;
    clipboard_move = move ? 1U : 0U;
    clipboard_attr = entry->attr;
    return 1U;
}

uint8_t SD_BrowserClipboardReady(void) { return clipboard_ready; }
uint8_t SD_BrowserClipboardIsMove(void) { return clipboard_move; }
const char *SD_BrowserClipboardPath(void) { return clipboard_path; }

uint8_t SD_BrowserPasteStart(void)
{
    FILINFO info;
    const char *base;
    char destination[SD_BROWSER_PATH_MAX];
    FRESULT result;
    if (!io_allowed() || !clipboard_ready || SD_FileJobActive()) return 0U;
    base = path_basename(clipboard_path);
    if (base[0] == 0 || !SD_PathJoin(browser_path, base, destination, sizeof(destination))) return 0U;
    if (strcmp(clipboard_path, destination) == 0) return 0U;
    if ((clipboard_attr & AM_DIR) != 0U && path_is_inside(clipboard_path, browser_path)) return 0U;
    if (f_stat(destination, &info) == FR_OK) return 0U;

    SD_ViewerClose();
    if (clipboard_move != 0U) {
        result = f_rename(clipboard_path, destination);
        if (result != FR_OK) return 0U;
        clipboard_ready = 0U;
        return SD_BrowserRefresh();
    }

    if (f_stat(clipboard_path, &info) != FR_OK) return 0U;
    memset(&file_job, 0, sizeof(file_job));
    file_job.public_kind = SD_FILE_JOB_COPY;
    file_job.operation = SD_FILE_JOB_COPY;
    file_job.root_is_dir = ((info.fattrib & AM_DIR) != 0U) ? 1U : 0U;
    file_job.current_size = (uint32_t)info.fsize;
    str_copy(file_job.src_root, clipboard_path, sizeof(file_job.src_root));
    str_copy(file_job.dst_root, destination, sizeof(file_job.dst_root));
    str_copy(file_job.src_dir, clipboard_path, sizeof(file_job.src_dir));
    str_copy(file_job.dst_dir, destination, sizeof(file_job.dst_dir));
    if (file_job.root_is_dir != 0U) {
        if (f_mkdir(destination) != FR_OK) {
            memset(&file_job, 0, sizeof(file_job));
            return 0U;
        }
        file_job.phase = JOB_PHASE_COPY_SCAN;
    } else {
        str_copy(file_job.src_item, clipboard_path, sizeof(file_job.src_item));
        str_copy(file_job.dst_item, destination, sizeof(file_job.dst_item));
        file_job.phase = JOB_PHASE_COPY_OPEN;
    }
    return 1U;
}

static void service_copy_open(void)
{
    FRESULT result = f_open(&file_job.src_file, file_job.src_item, FA_READ);
    if (result != FR_OK) { job_fail("OPEN SOURCE"); return; }
    result = f_open(&file_job.dst_file, file_job.dst_item, FA_WRITE | FA_CREATE_NEW);
    if (result != FR_OK) {
        (void)f_close(&file_job.src_file);
        job_fail("OPEN TARGET");
        return;
    }
    file_job.file_open = 1U;
    file_job.current_size = (uint32_t)f_size(&file_job.src_file);
    file_job.phase = JOB_PHASE_COPY_DATA;
}

static void service_copy_data(void)
{
    UINT read_len = 0U;
    UINT write_len = 0U;
    FRESULT result = f_read(&file_job.src_file, copy_buffer, sizeof(copy_buffer), &read_len);
    if (result != FR_OK) { job_fail("READ SOURCE"); return; }
    if (read_len == 0U) {
        result = f_sync(&file_job.dst_file);
        job_close_files();
        if (result != FR_OK) { job_fail("SYNC TARGET"); return; }
        file_job.files_done++;
        if (file_job.root_is_dir != 0U) file_job.phase = JOB_PHASE_COPY_SCAN;
        else job_finish(SD_FILE_JOB_DONE, "");
        return;
    }
    result = f_write(&file_job.dst_file, copy_buffer, read_len, &write_len);
    if (result != FR_OK || write_len != read_len) { job_fail("WRITE TARGET"); return; }
    file_job.bytes_done += write_len;
}

static void service_copy_scan(void)
{
    FILINFO info;
    uint8_t found = dir_item_at(file_job.src_dir, file_job.next_index[file_job.depth], &info);
    if (found == 0U) { job_fail("SCAN SOURCE"); return; }
    if (found == 2U) {
        file_job.dirs_done++;
        if (file_job.depth == 0U) {
            job_finish(SD_FILE_JOB_DONE, "");
            return;
        }
        file_job.src_dir[file_job.src_parent_len[file_job.depth]] = 0;
        file_job.dst_dir[file_job.dst_parent_len[file_job.depth]] = 0;
        file_job.depth--;
        return;
    }

    file_job.next_index[file_job.depth]++;
    if (!SD_PathJoin(file_job.src_dir, info.fname, file_job.src_item, sizeof(file_job.src_item)) ||
        !SD_PathJoin(file_job.dst_dir, info.fname, file_job.dst_item, sizeof(file_job.dst_item))) {
        job_fail("PATH TOO LONG");
        return;
    }
    if ((info.fattrib & AM_DIR) != 0U) {
        uint8_t child_depth = (uint8_t)(file_job.depth + 1U);
        if (child_depth >= SD_BROWSER_DEPTH_MAX) { job_fail("DEPTH LIMIT"); return; }
        if (f_mkdir(file_job.dst_item) != FR_OK) { job_fail("CREATE FOLDER"); return; }
        file_job.src_parent_len[child_depth] = (uint16_t)strlen(file_job.src_dir);
        file_job.dst_parent_len[child_depth] = (uint16_t)strlen(file_job.dst_dir);
        str_copy(file_job.src_dir, file_job.src_item, sizeof(file_job.src_dir));
        str_copy(file_job.dst_dir, file_job.dst_item, sizeof(file_job.dst_dir));
        file_job.depth = child_depth;
        file_job.next_index[child_depth] = 0U;
    } else {
        file_job.current_size = (uint32_t)info.fsize;
        file_job.phase = JOB_PHASE_COPY_OPEN;
    }
}

static void service_delete_scan(void)
{
    FILINFO info;
    uint8_t found = dir_item_at(file_job.src_dir, 0U, &info);
    if (found == 0U) { job_fail("SCAN FOLDER"); return; }
    if (found == 2U) {
        if (f_unlink(file_job.src_dir) != FR_OK) { job_fail("DELETE FOLDER"); return; }
        file_job.dirs_done++;
        if (file_job.depth == 0U) {
            job_finish(SD_FILE_JOB_DONE, "");
            return;
        }
        file_job.src_dir[file_job.src_parent_len[file_job.depth]] = 0;
        file_job.depth--;
        return;
    }

    if (!SD_PathJoin(file_job.src_dir, info.fname, file_job.src_item, sizeof(file_job.src_item))) {
        job_fail("PATH TOO LONG");
        return;
    }
    if ((info.fattrib & AM_RDO) != 0U) { job_fail("READ ONLY"); return; }
    if ((info.fattrib & AM_DIR) != 0U) {
        uint8_t child_depth = (uint8_t)(file_job.depth + 1U);
        if (child_depth >= SD_BROWSER_DEPTH_MAX) { job_fail("DEPTH LIMIT"); return; }
        file_job.src_parent_len[child_depth] = (uint16_t)strlen(file_job.src_dir);
        str_copy(file_job.src_dir, file_job.src_item, sizeof(file_job.src_dir));
        file_job.depth = child_depth;
    } else {
        file_job.current_size = (uint32_t)info.fsize;
        if (f_unlink(file_job.src_item) != FR_OK) { job_fail("DELETE FILE"); return; }
        file_job.files_done++;
        file_job.bytes_done += (uint32_t)info.fsize;
    }
}

void SD_FileJobService(void)
{
    if (!SD_FileJobActive() || !io_allowed()) return;
    if (file_job.cancel_requested != 0U) {
        if (file_job.operation == SD_FILE_JOB_COPY && file_job.file_open != 0U) {
            job_close_files();
            (void)f_unlink(file_job.dst_item);
        }
        job_finish(SD_FILE_JOB_CANCELLED, "CANCELLED");
        return;
    }
    switch (file_job.phase) {
        case JOB_PHASE_COPY_SCAN:   service_copy_scan(); break;
        case JOB_PHASE_COPY_OPEN:   service_copy_open(); break;
        case JOB_PHASE_COPY_DATA:   service_copy_data(); break;
        case JOB_PHASE_DELETE_SCAN: service_delete_scan(); break;
        case JOB_PHASE_DELETE_FILE:
            if (f_unlink(file_job.src_root) != FR_OK) job_fail("DELETE FILE");
            else {
                file_job.files_done++;
                file_job.bytes_done += file_job.current_size;
                job_finish(SD_FILE_JOB_DONE, "");
            }
            break;
        default:
            job_fail("BAD JOB STATE");
            break;
    }
}

void SD_FileJobCancel(void)
{
    if (SD_FileJobActive()) file_job.cancel_requested = 1U;
}

void SD_FileJobClearResult(void)
{
    if (!SD_FileJobActive()) memset(&file_job, 0, sizeof(file_job));
}

uint8_t SD_FileJobActive(void)
{
    return (file_job.public_kind == SD_FILE_JOB_COPY ||
            file_job.public_kind == SD_FILE_JOB_DELETE) ? 1U : 0U;
}

void SD_FileJobGetStatus(sd_file_job_status_t *status)
{
    if (status == NULL) return;
    status->kind = file_job.public_kind;
    status->files_done = file_job.files_done;
    status->dirs_done = file_job.dirs_done;
    status->bytes_done = file_job.bytes_done;
    status->current_size = file_job.current_size;
    status->depth = file_job.depth;
    status->cancellable = SD_FileJobActive();
    status->current_name = path_basename((file_job.phase == JOB_PHASE_COPY_DATA ||
                                          file_job.phase == JOB_PHASE_COPY_OPEN)
                                         ? file_job.src_item : file_job.src_dir);
    status->error = file_job.error;
}

uint8_t SD_ViewerOpen(const char *path)
{
    FILINFO info;
    FRESULT result;
    if (!io_allowed() || path == NULL || SD_FileJobActive()) return 0U;
    SD_ViewerClose();
    result = f_stat(path, &info);
    if (result != FR_OK || (info.fattrib & AM_DIR) != 0U) return 0U;
    result = f_open(&viewer_file, path, FA_READ);
    if (result != FR_OK) return 0U;
    viewer_open = 1U;
    viewer_size = (uint32_t)f_size(&viewer_file);
    viewer_type = SD_FileTypeFromName(path, info.fattrib);
    str_copy(viewer_path, path, sizeof(viewer_path));
    return 1U;
}

void SD_ViewerClose(void)
{
    SD_ViewerFindCancel();
    if (viewer_open != 0U) (void)f_close(&viewer_file);
    viewer_open = 0U;
    viewer_size = 0U;
    viewer_path[0] = 0;
}

uint8_t SD_ViewerIsOpen(void) { return viewer_open; }
sd_file_type_t SD_ViewerType(void) { return viewer_type; }
uint32_t SD_ViewerSize(void) { return viewer_size; }
const char *SD_ViewerPath(void) { return viewer_path; }

uint8_t SD_ViewerRead(uint32_t offset, void *data, uint32_t len, uint32_t *read_len)
{
    UINT got = 0U;
    FRESULT result;
    if (read_len != NULL) *read_len = 0U;
    if (!viewer_open || viewer_find_active != 0U || !io_allowed() || data == NULL) return 0U;
    if (offset > viewer_size) offset = viewer_size;
    result = f_lseek(&viewer_file, offset);
    if (result == FR_OK) result = f_read(&viewer_file, data, (UINT)len, &got);
    if (read_len != NULL) *read_len = got;
    return (result == FR_OK) ? 1U : 0U;
}

uint8_t SD_ViewerFindStart(const char *needle, uint32_t start, uint8_t match_case)
{
    uint32_t pattern_len;
    if (!viewer_open || viewer_find_active != 0U || !io_allowed() || needle == NULL) return 0U;
    pattern_len = (uint32_t)strlen(needle);
    if (pattern_len == 0U || pattern_len >= sizeof(viewer_find_pattern)) return 0U;
    if (start > viewer_size) start = viewer_size;
    for (uint32_t i = 0U; i < pattern_len; i++) {
        viewer_find_pattern[i] = match_case ? (uint8_t)needle[i] : ascii_lower((uint8_t)needle[i]);
    }
    viewer_find_prefix[0] = 0U;
    for (uint8_t i = 1U, matched = 0U; i < pattern_len; i++) {
        while (matched != 0U && viewer_find_pattern[i] != viewer_find_pattern[matched]) {
            matched = viewer_find_prefix[matched - 1U];
        }
        if (viewer_find_pattern[i] == viewer_find_pattern[matched]) matched++;
        viewer_find_prefix[i] = matched;
    }
    if (f_lseek(&viewer_file, start) != FR_OK) return 0U;
    viewer_find_pattern_len = (uint8_t)pattern_len;
    viewer_find_match_case = match_case ? 1U : 0U;
    viewer_find_active = 1U;
    viewer_find_done = 0U;
    viewer_find_found = 0U;
    viewer_find_error = 0U;
    viewer_find_matched = 0U;
    viewer_find_offset = start;
    viewer_find_result = 0U;
    return 1U;
}

void SD_ViewerFindService(void)
{
    uint8_t buffer[256];
    UINT got = 0U;
    if (viewer_find_active == 0U || !io_allowed()) return;
    if (viewer_find_offset >= viewer_size) {
        viewer_find_active = 0U;
        viewer_find_done = 1U;
        return;
    }
    if (f_read(&viewer_file, buffer, sizeof(buffer), &got) != FR_OK) {
        viewer_find_active = 0U;
        viewer_find_done = 1U;
        viewer_find_error = 1U;
        return;
    }
    if (got == 0U) {
        viewer_find_active = 0U;
        viewer_find_done = 1U;
        return;
    }
    for (UINT i = 0U; i < got; i++, viewer_find_offset++) {
        uint8_t value = viewer_find_match_case ? buffer[i] : ascii_lower(buffer[i]);
        while (viewer_find_matched != 0U &&
               value != viewer_find_pattern[viewer_find_matched]) {
            viewer_find_matched = viewer_find_prefix[viewer_find_matched - 1U];
        }
        if (value == viewer_find_pattern[viewer_find_matched]) viewer_find_matched++;
        if (viewer_find_matched == viewer_find_pattern_len) {
            viewer_find_result = viewer_find_offset + 1U - viewer_find_pattern_len;
            viewer_find_found = 1U;
            viewer_find_active = 0U;
            viewer_find_done = 1U;
            return;
        }
    }
}

uint8_t SD_ViewerFindActive(void)
{
    return viewer_find_active;
}

uint8_t SD_ViewerFindTakeResult(uint32_t *found_offset, uint8_t *found,
                                uint8_t *read_error)
{
    if (viewer_find_done == 0U) return 0U;
    if (found_offset != NULL) *found_offset = viewer_find_result;
    if (found != NULL) *found = viewer_find_found;
    if (read_error != NULL) *read_error = viewer_find_error;
    viewer_find_done = 0U;
    return 1U;
}

void SD_ViewerFindCancel(void)
{
    viewer_find_active = 0U;
    viewer_find_done = 0U;
    viewer_find_found = 0U;
    viewer_find_error = 0U;
    viewer_find_matched = 0U;
}

void *SD_SharedAudioWorkspaceAcquire(uint32_t bytes)
{
    if (audio_workspace_owned != 0U || bytes > SD_AUDIO_WORKSPACE_SIZE ||
        SD_FileJobActive() != 0U) return NULL;
    SD_ViewerClose();
    clipboard_ready = 0U;
    clipboard_move = 0U;
    entry_count = 0U;
    entry_overflow = 0U;
    name_pool_used = 0U;
    audio_workspace_owned = 1U;
    return shared_workspace.audio;
}

void SD_SharedAudioWorkspaceRelease(void)
{
    if (audio_workspace_owned != 1U) return;
    audio_workspace_owned = 0U;
    memset(&file_job, 0, sizeof(file_job));
    if (SD_IsMounted() != 0U && MOD_BlockingIoAllowed(HAL_GetTick()) != 0U) {
        (void)SD_BrowserRefresh();
    }
}

uint8_t SD_SharedAudioWorkspaceOwned(void)
{
    return audio_workspace_owned;
}

void *SD_SharedReceiverWorkspaceAcquire(uint32_t bytes)
{
    if (audio_workspace_owned || bytes > SD_AUDIO_WORKSPACE_SIZE || SD_FileJobActive()) return NULL;
    SD_ViewerClose();
    clipboard_ready = 0U; entry_count = 0U; name_pool_used = 0U;
    audio_workspace_owned = 2U;
    return shared_workspace.audio;
}

void SD_SharedReceiverWorkspaceRelease(void)
{
    if (audio_workspace_owned != 2U) return;
    audio_workspace_owned = 0U;
    memset(&shared_workspace, 0, sizeof(shared_workspace));
}

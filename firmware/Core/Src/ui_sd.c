#pragma GCC optimize ("Os")

#include "radio_audio.h"
/**
 * @file    ui_sd.c
 * @brief   Three-button SD file browser and foreground RF audio player UI.
 *
 * FatFs work is done only from event/service handlers. UI_SDRender() consumes
 * cached data exclusively and is therefore safe to call from the LCD builder.
 */

#include "ui_sd.h"
#include "ui_state.h"
#include "ui_gfx.h"
#include "ui_format.h"
#include "sd_audio_player.h"
#include "sd_file_browser.h"
#include "mod_sdcard_service.h"
#include "mod_sd_store.h"
#include "fw_update.h"
#include "usb_disk_mode.h"
#include "main.h"
#include <string.h>

#define SDUI_HUB_ROWS          9U
#define SDUI_BROWSER_ACTIONS   5U
#define SDUI_FILE_ROWS         8U
#define SDUI_VIEW_MENU_ROWS    5U
#define SDUI_BROWSER_SET_ROWS  7U
#define SDUI_PLAYER_SET_ROWS  11U
#define SDUI_VIEW_BYTES      288U
#define SDUI_TEXT_COLUMNS     50U

typedef enum {
    SDUI_HUB = 0,
    SDUI_BROWSER,
    SDUI_FILE_MENU,
    SDUI_PROPERTIES,
    SDUI_VIEWER,
    SDUI_VIEWER_MENU,
    SDUI_PLAYER,
    SDUI_BROWSER_SETTINGS,
    SDUI_PLAYER_SETTINGS,
    SDUI_EDITOR,
    SDUI_DELETE_CONFIRM,
    SDUI_FORMAT_CONFIRM,
    SDUI_FW_CONFIRM,
    SDUI_JOB
} sdui_screen_t;

typedef enum {
    EDIT_NEW_FOLDER = 0,
    EDIT_RENAME,
    EDIT_FIND,
    EDIT_GOTO
} editor_purpose_t;

typedef union {
    uint8_t view[SDUI_VIEW_BYTES];
    char edit[SD_BROWSER_NAME_MAX];
} sdui_scratch_t;

static sdui_scratch_t scratch;
static sdui_screen_t screen;
static sdui_screen_t editor_return;
static editor_purpose_t editor_purpose;
static uint8_t cursor;
static uint8_t first_row;
static uint8_t selected_index;
static uint8_t viewer_hex;
static uint16_t viewer_len;
static uint32_t viewer_offset;
static uint16_t editor_codepoint;
static char find_query[64];
static const char *notice;
static uint32_t notice_until;
static uint8_t advance_checked;
static uint8_t job_is_format;
static sd_player_state_t last_player_state;
static sd_service_op_t last_service_operation;

static const char editor_chars[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.()[]";

static void mark_dirty(void)
{
    ui.dirty = 1U;
}

static void flash(const char *text)
{
    notice = text;
    notice_until = HAL_GetTick() + 1500U;
    mark_dirty();
}

static void set_screen(sdui_screen_t next)
{
    screen = next;
    cursor = 0U;
    first_row = 0U;
    mark_dirty();
}

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

static const char *base_name(const char *path)
{
    const char *base = path;
    if (path == NULL) return "";
    for (uint32_t i = 0U; path[i] != 0; i++) {
        if (path[i] == '/') base = &path[i + 1U];
    }
    return base;
}

static uint8_t audio_type(sd_file_type_t type)
{
    return SD_FileTypeIsPlayable(type);
}

static void move_cursor(uint8_t total, int8_t direction)
{
    if (total == 0U) return;
    if (direction < 0) cursor = cursor ? (uint8_t)(cursor - 1U) : (uint8_t)(total - 1U);
    else cursor = (uint8_t)((cursor + 1U) % total);
    mark_dirty();
}

static uint8_t build_selected_path(char *path, uint32_t cap)
{
    return SD_BrowserSelectedPath(selected_index, path, cap);
}

static void viewer_load(uint32_t offset)
{
    uint32_t got = 0U;
    uint32_t size = SD_ViewerSize();
    if (offset > size) offset = size;
    if (SD_ViewerRead(offset, scratch.view, sizeof(scratch.view), &got) != 0U) {
        viewer_offset = offset;
        viewer_len = (uint16_t)got;
        mark_dirty();
    } else {
        flash("READ FAILED");
    }
}

static void open_viewer(uint8_t force_hex)
{
    char path[SD_BROWSER_PATH_MAX];
    const sd_file_entry_t *entry = SD_BrowserEntry(selected_index);
    if (entry == NULL || !build_selected_path(path, sizeof(path)) || !SD_ViewerOpen(path)) {
        flash("OPEN FAILED");
        return;
    }
    viewer_hex = (force_hex != 0U || entry->type != SD_FILE_TYPE_TEXT) ? 1U : 0U;
    set_screen(SDUI_VIEWER);
    viewer_load(0U);
}

static void open_player(void)
{
    char path[SD_BROWSER_PATH_MAX];
    const sd_file_entry_t *entry = SD_BrowserEntry(selected_index);
    if (entry == NULL || !audio_type(entry->type) ||
        !build_selected_path(path, sizeof(path)) || !SD_AudioPlayerOpen(path, entry->type)) {
        flash("PLAYER OPEN FAIL");
        return;
    }
    advance_checked = 0U;
    set_screen(SDUI_PLAYER);
}

static uint16_t utf8_byte_offset(const char *text, uint16_t codepoint)
{
    uint16_t byte = 0U;
    uint16_t index = 0U;
    while (text[byte] != 0 && index < codepoint) {
        uint8_t c = (uint8_t)text[byte++];
        if ((c & 0x80U) == 0U) {
        } else if ((c & 0xE0U) == 0xC0U && text[byte] != 0) {
            byte += 1U;
        } else if ((c & 0xF0U) == 0xE0U && text[byte] != 0 && text[byte + 1U] != 0) {
            byte += 2U;
        } else if ((c & 0xF8U) == 0xF0U && text[byte] != 0 &&
                   text[byte + 1U] != 0 && text[byte + 2U] != 0) {
            byte += 3U;
        }
        index++;
    }
    return byte;
}

static uint16_t utf8_codepoints(const char *text)
{
    uint16_t count = 0U;
    uint16_t byte = 0U;
    while (text[byte] != 0) {
        byte = utf8_byte_offset(text, (uint16_t)(count + 1U));
        count++;
    }
    return count;
}

static uint8_t editor_char_index(void)
{
    uint16_t byte = utf8_byte_offset(scratch.edit, editor_codepoint);
    if ((uint8_t)scratch.edit[byte] >= 0x80U) return 1U;
    const char *found = strchr(editor_chars, scratch.edit[byte]);
    return (found != NULL) ? (uint8_t)(found - editor_chars) : 1U;
}

static void editor_replace(char replacement)
{
    uint16_t start = utf8_byte_offset(scratch.edit, editor_codepoint);
    uint16_t end = utf8_byte_offset(scratch.edit, (uint16_t)(editor_codepoint + 1U));
    uint16_t len = (uint16_t)strlen(scratch.edit);
    if (start == len) {
        if ((uint32_t)len + 2U > sizeof(scratch.edit)) return;
        scratch.edit[len] = replacement;
        scratch.edit[len + 1U] = 0;
        return;
    }
    if (end > start + 1U) {
        memmove(&scratch.edit[start + 1U], &scratch.edit[end], len - end + 1U);
    }
    scratch.edit[start] = replacement;
}

static void editor_backspace(void)
{
    uint16_t start;
    uint16_t end;
    uint16_t len;
    if (editor_codepoint == 0U) return;
    editor_codepoint--;
    start = utf8_byte_offset(scratch.edit, editor_codepoint);
    end = utf8_byte_offset(scratch.edit, (uint16_t)(editor_codepoint + 1U));
    len = (uint16_t)strlen(scratch.edit);
    memmove(&scratch.edit[start], &scratch.edit[end], len - end + 1U);
    mark_dirty();
}

static void editor_begin(editor_purpose_t purpose, const char *initial, sdui_screen_t back)
{
    editor_purpose = purpose;
    editor_return = back;
    copy_text(scratch.edit, initial, sizeof(scratch.edit));
    editor_codepoint = 0U;
    set_screen(SDUI_EDITOR);
}

static uint32_t parse_u32(const char *text, uint8_t *ok)
{
    uint64_t value = 0U;
    uint8_t any = 0U;
    while (*text == ' ') text++;
    while (*text >= '0' && *text <= '9') {
        any = 1U;
        value = value * 10U + (uint32_t)(*text++ - '0');
        if (value > 0xFFFFFFFFULL) value = 0xFFFFFFFFULL;
    }
    *ok = (any != 0U && *text == 0) ? 1U : 0U;
    return (uint32_t)value;
}

static void editor_commit(void)
{
    uint8_t ok;
    if (editor_purpose == EDIT_NEW_FOLDER) {
        ok = SD_BrowserMkdir(scratch.edit);
        set_screen(SDUI_BROWSER);
        flash(ok ? "FOLDER CREATED" : "CREATE FAILED");
    } else if (editor_purpose == EDIT_RENAME) {
        ok = SD_BrowserRename(selected_index, scratch.edit);
        set_screen(SDUI_BROWSER);
        flash(ok ? "RENAMED" : "RENAME FAILED");
    } else if (editor_purpose == EDIT_FIND) {
        copy_text(find_query, scratch.edit, sizeof(find_query));
        set_screen(SDUI_VIEWER);
        if (SD_ViewerFindStart(find_query, viewer_offset + 1U, 0U)) flash("SEARCHING - SEL CANCEL");
        else flash("SEARCH START FAILED");
    } else {
        uint32_t offset = parse_u32(scratch.edit, &ok);
        set_screen(SDUI_VIEWER);
        if (ok != 0U) viewer_load(offset);
        else flash("BAD OFFSET");
    }
}

/* SETTINGS STORE: what selecting it does depends on the card. Nothing that
 * would discard a store happens from one click: another radio's is left to
 * the console (`storage adopt` / `storage new`) or the boot hold. */
static const char *store_action(void)
{
    if (MOD_StorageRunsWithoutCard()) return "NO CARD MODE UNTIL REBOOT";
    switch (SD_StoreState()) {
        case SD_STORE_READY:
            /* An import or restore cut short: run it again or keep the store. */
            if (MOD_StorageIncomplete()) return MOD_StorageResume() ? "STORE COMPLETE" : "STORE STILL INCOMPLETE";
            return MOD_GetStorageSdMissing() ? "SAVE FAILED - CHECK CARD" : "STORE READY";
        case SD_STORE_BLANK:
            if (MOD_StorageImportAvailable()) return MOD_StorageImport(0U) ? "V8.08 SETTINGS IMPORTED" : "IMPORT FAILED";
            return MOD_StorageCreate() ? "STORE CREATED" : "STORE CREATE FAILED";
        case SD_STORE_DAMAGED: return MOD_StorageAdopt() ? "STORE REPAIRED" : "REPAIR FAILED";
        case SD_STORE_FOREIGN: return "OTHER RADIO - USE CONSOLE";
        case SD_STORE_LAYOUT:  return "FORMAT THE CARD FIRST";
        case SD_STORE_IO:
        case SD_STORE_NO_CARD:
        default: {
            /* The card lost while the radio runs: mounted again, and if it is
             * still not there a second click within 5 s runs without it, as
             * the boot hold offers -- the operator's way past CAL NOT LOADED's
             * TX lock, knowingly, uncalibrated and nothing saved. */
            static uint32_t asked_at;
            const uint32_t now = HAL_GetTick();
            if (asked_at && (uint32_t)(now - asked_at) < 5000U) {
                asked_at = 0U;
                return MOD_StorageRunWithoutCard() ? "NO CARD MODE UNTIL REBOOT" : "NOT POSSIBLE NOW";
            }
            SD_ServiceRefresh();
            if (SD_StoreState() == SD_STORE_READY) return "STORE READY";
            asked_at = now;
            return "CLICK AGAIN: RUN WITHOUT CARD";
        }
    }
}

static void handle_hub(uint8_t select)
{
    if (select == 0U) return;
    if (USB_DiskModeIsActive() != 0U && cursor != 6U) {
        flash("EXIT USB DISK FIRST");
        return;
    }
    switch (cursor) {
        case 0U:
            if (!SD_IsMounted() || !SD_BrowserRefresh()) flash("BROWSER UNAVAILABLE");
            else set_screen(SDUI_BROWSER);
            break;
        case 1U: set_screen(SDUI_BROWSER_SETTINGS); break;
        case 2U: set_screen(SDUI_PLAYER_SETTINGS); break;
        case 3U:
            SD_ServiceRefresh();
            (void)SD_BrowserOpen("/");
            flash("CARD REFRESHED");
            break;
        case 4U: flash(store_action()); break;
        case 5U: flash(MOD_StorageBackup() ? "STOREBAK.BIN WRITTEN" : "BACKUP FAILED"); break;
        case 6U:
            if (USB_DiskModeIsActive()) {
                USB_DiskModeExit();
                (void)SD_BrowserOpen("/");
                flash("USB AUDIO MODE");
            } else {
                flash(USB_DiskModeEnter() ? "USB DISK MODE" : "USB DISK FAILED");
            }
            break;
        case 7U: {
            fw_update_result_t result = FW_UpdateVerify();
            if (result == FW_UPDATE_OK) set_screen(SDUI_FW_CONFIRM);
            else flash(FW_UpdateResultText(result));
            break;
        }
        case 8U: set_screen(SDUI_FORMAT_CONFIRM); break;
        default: break;
    }
}

static void handle_browser(uint8_t select)
{
    if (select == 0U) return;
    if (cursor == 0U) {
        if (strcmp(SD_BrowserPath(), "/") == 0) set_screen(SDUI_HUB);
        else if (!SD_BrowserUp()) flash("FOLDER UP FAILED");
        cursor = 0U;
    } else if (cursor == 1U) {
        if (!SD_BrowserPreviousPage()) flash("ALREADY FIRST PAGE");
        cursor = 0U;
    } else if (cursor == 2U) {
        if (!SD_BrowserNextPage()) flash("ALREADY LAST PAGE");
        cursor = 0U;
    } else if (cursor == 3U) {
        editor_begin(EDIT_NEW_FOLDER, "NEW_FOLDER", SDUI_BROWSER);
    } else if (cursor == 4U) {
        if (!SD_BrowserClipboardReady()) flash("CLIPBOARD EMPTY");
        else if (SD_BrowserPasteStart()) { job_is_format = 0U; set_screen(SDUI_JOB); }
        else flash("PASTE FAILED/EXISTS");
    } else {
        const sd_file_entry_t *entry;
        selected_index = (uint8_t)(cursor - SDUI_BROWSER_ACTIONS);
        entry = SD_BrowserEntry(selected_index);
        if (entry == NULL) return;
        if ((entry->attr & SD_FILE_ATTR_DIR) != 0U) {
            if (!SD_BrowserEnter(selected_index)) flash("OPEN FOLDER FAILED");
            cursor = 0U;
        } else {
            set_screen(SDUI_FILE_MENU);
        }
    }
}

static void handle_file_menu(uint8_t select)
{
    char path[SD_BROWSER_PATH_MAX];
    const sd_file_entry_t *entry = SD_BrowserEntry(selected_index);
    if (select == 0U || entry == NULL) return;
    if ((entry->attr & SD_FILE_ATTR_DIR) != 0U) {
        switch (cursor) {
            case 0U:
                if (SD_BrowserEnter(selected_index)) set_screen(SDUI_BROWSER);
                else flash("OPEN FOLDER FAILED");
                break;
            case 1U: set_screen(SDUI_PROPERTIES); break;
            case 2U: editor_begin(EDIT_RENAME, entry->name, SDUI_FILE_MENU); break;
            case 3U:
            case 4U:
                if (SD_BrowserClipboardSet(selected_index, (cursor == 4U) ? 1U : 0U)) {
                    set_screen(SDUI_BROWSER);
                    flash((cursor == 4U) ? "MOVE READY - PASTE" : "COPY READY - PASTE");
                } else flash("CLIPBOARD FAILED");
                break;
            case 5U: set_screen(SDUI_DELETE_CONFIRM); break;
            case 6U:
                if (!SD_BrowserClipboardReady()) flash("CLIPBOARD EMPTY");
                else if (!SD_BrowserEnter(selected_index)) flash("OPEN FOLDER FAILED");
                else if (SD_BrowserPasteStart()) { job_is_format = 0U; set_screen(SDUI_JOB); }
                else flash("PASTE FAILED/EXISTS");
                break;
            case 7U: set_screen(SDUI_BROWSER); break;
            default: break;
        }
        return;
    }
    switch (cursor) {
        case 0U:
            if (audio_type(entry->type)) open_player();
            else open_viewer(entry->type == SD_FILE_TYPE_TEXT ? 0U : 1U);
            break;
        case 1U: open_viewer(1U); break;
        case 2U: set_screen(SDUI_PROPERTIES); break;
        case 3U: editor_begin(EDIT_RENAME, entry->name, SDUI_FILE_MENU); break;
        case 4U:
        case 5U:
            if (SD_BrowserClipboardSet(selected_index, (cursor == 5U) ? 1U : 0U)) {
                set_screen(SDUI_BROWSER);
                flash((cursor == 5U) ? "MOVE READY - PASTE" : "COPY READY - PASTE");
            } else flash("CLIPBOARD FAILED");
            break;
        case 6U:
            if (build_selected_path(path, sizeof(path))) set_screen(SDUI_DELETE_CONFIRM);
            break;
        case 7U: set_screen(SDUI_BROWSER); break;
        default: break;
    }
}

static void handle_viewer(uint8_t up, uint8_t down, uint8_t select)
{
    uint32_t step = viewer_hex ? 128U : (uint32_t)sizeof(scratch.view);
    if (up != 0U) viewer_load((viewer_offset > step) ? (viewer_offset - step) : 0U);
    else if (down != 0U) viewer_load(viewer_offset + step);
    else if (select != 0U) set_screen(SDUI_VIEWER_MENU);
}

static void handle_viewer_menu(uint8_t select)
{
    char number[16];
    if (select == 0U) return;
    if (cursor == 0U) editor_begin(EDIT_FIND, find_query[0] ? find_query : "TEXT", SDUI_VIEWER_MENU);
    else if (cursor == 1U) {
        number[0] = 0;
        UI_AppendU(number, viewer_offset, 1U);
        editor_begin(EDIT_GOTO, number, SDUI_VIEWER_MENU);
    } else if (cursor == 2U) {
        viewer_hex = viewer_hex ? 0U : 1U;
        set_screen(SDUI_VIEWER);
    } else if (cursor == 3U) set_screen(SDUI_PROPERTIES);
    else {
        SD_ViewerClose();
        set_screen(SDUI_FILE_MENU);
    }
}

static void handle_player(uint8_t up, uint8_t down, uint8_t select)
{
    sd_audio_player_status_t status;
    sd_audio_player_config_t config;
    SD_AudioPlayerGetStatus(&status);
    SD_AudioPlayerGetConfig(&config);
    if (status.state == SD_PLAYER_CONFIRM) {
        if (up || down) {
            int32_t mode = (int32_t)SD_AudioPlayerGetRfMode() + (up ? -1 : 1);
            if (mode < 0) mode = SD_PLAYER_RF_FM;
            if (mode > SD_PLAYER_RF_FM) mode = SD_PLAYER_RF_SSB;
            SD_AudioPlayerSetRfMode((sd_player_rf_mode_t)mode);
            mark_dirty();
        } else if (select) SD_AudioPlayerConfirm();
    } else if (status.state == SD_PLAYER_FINISHED || status.state == SD_PLAYER_ERROR) {
        if (select) {
            SD_AudioPlayerClose();
            (void)SD_BrowserRefresh();
            set_screen(SDUI_FILE_MENU);
        }
    } else {
        if (up) (void)SD_AudioPlayerSeekRelative(-(int32_t)config.seek_seconds);
        else if (down) (void)SD_AudioPlayerSeekRelative((int32_t)config.seek_seconds);
        else if (select) SD_AudioPlayerTogglePause();
        mark_dirty();
    }
}

static void cycle_browser_setting(void)
{
    sd_browser_config_t config;
    SD_BrowserGetConfig(&config);
    switch (cursor) {
        case 0U: config.view = (sd_browser_view_t)(((uint32_t)config.view + 1U) % 3U); break;
        case 1U: config.sort = (sd_browser_sort_t)(((uint32_t)config.sort + 1U) % 5U); break;
        case 2U: config.descending ^= 1U; break;
        case 3U: config.folders_first ^= 1U; break;
        case 4U: config.filter = (sd_browser_filter_t)(((uint32_t)config.filter + 1U) % 4U); break;
        case 5U: config.show_hidden ^= 1U; break;
        case 6U: set_screen(SDUI_HUB); return;
        default: return;
    }
    SD_BrowserSetConfig(&config);
    mark_dirty();
}

static void cycle_player_setting(void)
{
    sd_audio_player_config_t config;
    SD_AudioPlayerGetConfig(&config);
    switch (cursor) {
        case 0U: config.ask_each_track ^= 1U; break;
        case 1U: config.advance ^= 1U; break;
        case 2U: config.repeat ^= 1U; break;
        case 3U: config.shuffle ^= 1U; break;
        case 4U:
            if (config.seek_seconds < 10U) config.seek_seconds = 10U;
            else if (config.seek_seconds < 30U) config.seek_seconds = 30U;
            else if (config.seek_seconds < 60U) config.seek_seconds = 60U;
            else config.seek_seconds = 5U;
            break;
        case 5U: config.downmix = (sd_player_downmix_t)(((uint32_t)config.downmix + 1U) % 4U); break;
        case 6U: config.gain_percent = (config.gain_percent >= 200U) ? 50U : (uint8_t)(config.gain_percent + 25U); break;
        case 7U: config.clean ^= 1U; break;
        case 8U: SD_AudioPlayerSetRfMode((sd_player_rf_mode_t)(((uint32_t)SD_AudioPlayerGetRfMode() + 1U) % 3U)); break;
        case 9U: flash(MOD_SaveSettings() ? "SETTINGS SAVED" : "SAVE FAILED"); break;
        case 10U: set_screen(SDUI_HUB); return;
        default: return;
    }
    SD_AudioPlayerSetConfig(&config);
    mark_dirty();
}

void UI_SDInit(void)
{
    screen = SDUI_HUB;
    notice = NULL;
    find_query[0] = 0;
    last_player_state = SD_PLAYER_CLOSED;
    job_is_format = 0U;
    last_service_operation = SD_ServiceOperation();
}

void UI_SDEnter(void)
{
    RADIO_AudioStorageMode(1U);
    set_screen(SDUI_HUB);
    if (USB_DiskModeIsActive() == 0U && SD_IsMounted() != 0U) (void)SD_BrowserRefresh();
}

void UI_SDExit(void)
{
    RADIO_AudioStorageMode(0U);
    SD_AudioPlayerClose();
    SD_ViewerClose();
    screen = SDUI_HUB;
}

static void service_advance(void)
{
    sd_audio_player_status_t status;
    sd_audio_player_config_t config;
    char current[SD_BROWSER_NAME_MAX];
    char next_path[SD_BROWSER_PATH_MAX];
    uint8_t candidates[SD_BROWSER_ENTRY_MAX];
    uint8_t candidate_count = 0U;
    uint8_t chosen = 0xFFU;

    SD_AudioPlayerGetStatus(&status);
    SD_AudioPlayerGetConfig(&config);
    if (status.state != SD_PLAYER_FINISHED) {
        advance_checked = 0U;
        last_player_state = status.state;
        return;
    }
    if (last_player_state != SD_PLAYER_FINISHED) mark_dirty();
    last_player_state = status.state;
    if (advance_checked != 0U || config.advance == 0U) return;
    advance_checked = 1U;
    copy_text(current, base_name(status.path), sizeof(current));
    if (!SD_BrowserRefresh()) return;
    for (uint8_t i = 0U; i < SD_BrowserCount(); i++) {
        const sd_file_entry_t *entry = SD_BrowserEntry(i);
        if (entry != NULL && audio_type(entry->type)) candidates[candidate_count++] = i;
    }
    if (candidate_count == 0U) return;
    if (config.shuffle != 0U) {
        chosen = candidates[HAL_GetTick() % candidate_count];
    } else {
        for (uint8_t i = 0U; i < candidate_count; i++) {
            const sd_file_entry_t *entry = SD_BrowserEntry(candidates[i]);
            if (entry != NULL && strcmp(entry->name, current) == 0 && i + 1U < candidate_count) {
                chosen = candidates[i + 1U];
                break;
            }
        }
    }
    if (chosen != 0xFFU) {
        const sd_file_entry_t *entry = SD_BrowserEntry(chosen);
        if (entry != NULL && SD_BrowserSelectedPath(chosen, next_path, sizeof(next_path)) &&
            SD_AudioPlayerContinue(next_path, entry->type)) {
            selected_index = chosen;
            advance_checked = 0U;
            mark_dirty();
        }
    }
}

void UI_SDService(uint32_t now)
{
    sd_service_op_t operation = SD_ServiceOperation();
    if (notice != NULL && (int32_t)(now - notice_until) >= 0) {
        notice = NULL;
        mark_dirty();
    }
    if (screen == SDUI_VIEWER && SD_ViewerFindActive() != 0U) {
        SD_ViewerFindService();
    }
    if (screen == SDUI_VIEWER) {
        uint32_t found_offset;
        uint8_t found;
        uint8_t read_error;
        if (SD_ViewerFindTakeResult(&found_offset, &found, &read_error) != 0U) {
            if (found != 0U) {
                viewer_load(found_offset);
                flash("TEXT FOUND");
            } else flash(read_error ? "SEARCH READ FAILED" : "TEXT NOT FOUND");
        }
    }
    if (screen == SDUI_PLAYER) service_advance();
    if (screen == SDUI_JOB && operation != last_service_operation) mark_dirty();
    last_service_operation = operation;
}

void UI_SDHandle(uint8_t up, uint8_t down, uint8_t select,
                 uint8_t up_long, uint8_t down_long, uint8_t select_long)
{
    if (screen == SDUI_EDITOR) {
        if (select_long) editor_commit();
        else if (up_long) editor_backspace();
        else if (down_long) set_screen(editor_return);
        else if (up || down) {
            int32_t index = editor_char_index() + (up ? 1 : -1);
            int32_t count = (int32_t)strlen(editor_chars);
            if (index < 0) index = count - 1;
            if (index >= count) index = 0;
            editor_replace(editor_chars[index]);
            mark_dirty();
        } else if (select) {
            if (editor_codepoint < utf8_codepoints(scratch.edit)) editor_codepoint++;
            else editor_replace('A');
            mark_dirty();
        }
        return;
    }

    if (select_long) {
        if (screen == SDUI_DELETE_CONFIRM) {
            if (SD_BrowserDeleteStart(selected_index)) { job_is_format = 0U; set_screen(SDUI_JOB); }
            else { set_screen(SDUI_FILE_MENU); flash("DELETE FAILED"); }
        } else if (screen == SDUI_FORMAT_CONFIRM) {
            if (SD_FormatFactoryStart()) { job_is_format = 1U; set_screen(SDUI_JOB); }
            else { set_screen(SDUI_HUB); flash("FORMAT START FAIL"); }
        } else if (screen == SDUI_FW_CONFIRM) {
            fw_update_result_t result = FW_UpdateInstall();
            set_screen(SDUI_HUB);
            flash(FW_UpdateResultText(result));
        } else if (screen == SDUI_PLAYER) {
            SD_AudioPlayerClose();
            (void)SD_BrowserRefresh();
            set_screen(SDUI_FILE_MENU);
        } else if (screen == SDUI_VIEWER || screen == SDUI_VIEWER_MENU ||
                   screen == SDUI_PROPERTIES) {
            SD_ViewerClose();
            set_screen(SDUI_FILE_MENU);
        } else if (screen == SDUI_BROWSER) {
            if (strcmp(SD_BrowserPath(), "/") != 0) (void)SD_BrowserUp();
            else set_screen(SDUI_HUB);
            cursor = 0U;
        } else if (screen != SDUI_HUB) set_screen(SDUI_HUB);
        return;
    }
    if (screen == SDUI_BROWSER && down_long != 0U && cursor >= SDUI_BROWSER_ACTIONS) {
        selected_index = (uint8_t)(cursor - SDUI_BROWSER_ACTIONS);
        set_screen(SDUI_FILE_MENU);
        return;
    }
    if (screen == SDUI_BROWSER && up_long != 0U) {
        flash(SD_BrowserRefresh() ? "FOLDER REFRESHED" : "REFRESH FAILED");
        return;
    }
    if (screen == SDUI_PLAYER) { handle_player(up, down, select); return; }
    if (screen == SDUI_VIEWER) {
        if (SD_ViewerFindActive() != 0U) {
            if (select != 0U) { SD_ViewerFindCancel(); flash("SEARCH CANCELLED"); }
            return;
        }
        handle_viewer(up, down, select);
        return;
    }
    if (screen == SDUI_PROPERTIES) {
        if (select || up || down) set_screen(SD_ViewerIsOpen() ? SDUI_VIEWER_MENU : SDUI_FILE_MENU);
        return;
    }
    if (screen == SDUI_DELETE_CONFIRM || screen == SDUI_FORMAT_CONFIRM || screen == SDUI_FW_CONFIRM) {
        if (select || up || down) set_screen((screen == SDUI_DELETE_CONFIRM) ? SDUI_FILE_MENU : SDUI_HUB);
        return;
    }
    if (screen == SDUI_JOB) {
        if (SD_FileJobActive()) {
            if (select) { SD_FileJobCancel(); flash("CANCEL REQUESTED"); }
        } else if (SD_ServiceOperation() == SD_SERVICE_OP_IDLE && select) {
            SD_FileJobClearResult();
            if (job_is_format) {
                (void)SD_BrowserOpen("/");
                set_screen(SDUI_HUB);
            } else {
                (void)SD_BrowserRefresh();
                set_screen(SDUI_BROWSER);
            }
        }
        return;
    }

    uint8_t total = 1U;
    if (screen == SDUI_HUB) total = SDUI_HUB_ROWS;
    else if (screen == SDUI_BROWSER) total = (uint8_t)(SDUI_BROWSER_ACTIONS + SD_BrowserCount());
    else if (screen == SDUI_FILE_MENU) total = SDUI_FILE_ROWS;
    else if (screen == SDUI_VIEWER_MENU) total = SDUI_VIEW_MENU_ROWS;
    else if (screen == SDUI_BROWSER_SETTINGS) total = SDUI_BROWSER_SET_ROWS;
    else if (screen == SDUI_PLAYER_SETTINGS) total = SDUI_PLAYER_SET_ROWS;
    if (up) move_cursor(total, -1);
    else if (down) move_cursor(total, 1);
    else if (screen == SDUI_HUB) handle_hub(select);
    else if (screen == SDUI_BROWSER) handle_browser(select);
    else if (screen == SDUI_FILE_MENU) handle_file_menu(select);
    else if (screen == SDUI_VIEWER_MENU) handle_viewer_menu(select);
    else if (screen == SDUI_BROWSER_SETTINGS && select) cycle_browser_setting();
    else if (screen == SDUI_PLAYER_SETTINGS && select) cycle_player_setting();
}

static void append(char *dst, const char *src, uint32_t cap)
{
    uint32_t len = (uint32_t)strlen(dst);
    if (len + 1U >= cap) return;
    copy_text(&dst[len], src, cap - len);
}

/* ---- Landscape rendering ----------------------------------------------------
 * The SD pages keep their own button grammar (see UI_SDHandle); ui_nav.c maps
 * the knob onto it: turn = up/down, click = select, hold = long select,
 * hold-and-turn right/left = long down/up. */

#define SDUI_VISIBLE 11U

static void footer(const char *hint, const char *right)
{
    char text[48];
    if (notice != NULL) { copy_text(text, notice, sizeof(text)); hint = text; right = NULL; }
    GFX_Fill(0U, UI_FOOTER_Y, GFX_W, (uint16_t)(GFX_H - UI_FOOTER_Y), GFX_PANEL);
    uint16_t limit = (uint16_t)(GFX_W - GFX_MARGIN);
    if (right != NULL) {
        const uint16_t w = GFX_TextWidth(GFX_FONT_SMALL, right);
        (void)GFX_TextRight(limit, (uint16_t)(UI_FOOTER_Y + 2U), GFX_FONT_SMALL, GFX_MUTED, right);
        limit = (uint16_t)(((limit - w) & ~7U) - 8U);
    }
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(UI_FOOTER_Y + 2U), GFX_FONT_SMALL,
                   (notice != NULL) ? GFX_ACCENT : GFX_DIM, hint, limit);
}

static void render_list(const char *const *labels, const char *const *values, uint8_t total)
{
    const uint8_t first = UI_ListFirst(&first_row, cursor, total, SDUI_VISIBLE);
    for (uint8_t i = 0U; (i < SDUI_VISIBLE) && ((uint8_t)(first + i) < total); i++) {
        const uint8_t index = (uint8_t)(first + i);
        UI_DrawRow(0U, (uint16_t)(GFX_W - 8U), (uint16_t)(UI_LIST_Y + i * UI_ROW_H),
                   (index == cursor) ? UI_ROW_FOCUS : UI_ROW_NORMAL, labels[index],
                   (values != NULL) ? values[index] : NULL);
    }
    UI_DrawScrollbar((uint16_t)(GFX_W - 6U), UI_LIST_Y, (uint16_t)(SDUI_VISIBLE * UI_ROW_H), total, first,
                     SDUI_VISIBLE);
}

static void render_hub(void)
{
    char status[40] = "";
    static const char *labels[SDUI_HUB_ROWS];
    static const char *values[SDUI_HUB_ROWS];
    UI_AppendU(status, SD_ServiceCapacityMiB(), 1U);
    append(status, " MiB  ", sizeof(status));
    append(status, SD_ServiceStatusString(), sizeof(status));
    UI_DrawTitle("SD CARD", status);
    labels[0] = "BROWSE FILES";       values[0] = GFX_SYM_RIGHT;
    labels[1] = "BROWSER SETTINGS";   values[1] = GFX_SYM_RIGHT;
    labels[2] = "PLAYER SETTINGS";    values[2] = GFX_SYM_RIGHT;
    labels[3] = "REFRESH / REMOUNT";  values[3] = NULL;
    labels[4] = "SETTINGS STORE";
    values[4] = MOD_StorageRunsWithoutCard() ? "OFF" : SD_StoreStateName(SD_StoreState());
    labels[5] = "BACK UP STORE";
    values[5] = NULL;
    labels[6] = "USB DISK MODE";      values[6] = USB_DiskModeIsActive() ? "ON" : "OFF";
    labels[7] = "FIRMWARE UPDATE";    values[7] = GFX_SYM_RIGHT;
    labels[8] = "FORMAT SD CARD";     values[8] = GFX_SYM_RIGHT;
    render_list(labels, values, SDUI_HUB_ROWS);
    footer(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " OPEN   HOLD BACK", NULL);
}

static void format_size(char *line, uint32_t size, uint32_t cap)
{
    if (size >= 1048576UL) {
        UI_AppendU(line + strlen(line), size / 1048576UL, 1U);
        append(line, "M", cap);
    } else if (size >= 1024UL) {
        UI_AppendU(line + strlen(line), size / 1024UL, 1U);
        append(line, "K", cap);
    } else {
        UI_AppendU(line + strlen(line), size, 1U);
        append(line, "B", cap);
    }
}

static const char *browser_action(uint8_t index)
{
    switch (index) {
        case 0U: return (strcmp(SD_BrowserPath(), "/") == 0) ? "SD MENU" : "PARENT FOLDER";
        case 1U: return SD_BrowserHasPreviousPage() ? "PREVIOUS FILE PAGE" : "FIRST FILE PAGE";
        case 2U: return SD_BrowserHasNextPage() ? "NEXT FILE PAGE" : "LAST FILE PAGE";
        case 3U: return "NEW FOLDER";
        default: return "PASTE";
    }
}

static void browser_row(uint8_t index, uint16_t y, uint8_t details)
{
    static const char *const tags[5] = {"..", GFX_SYM_UP, GFX_SYM_DOWN, "+", "P"};
    char value[24] = "";
    const char *tag, *name;
    uint8_t color = GFX_MUTED;
    if (index < SDUI_BROWSER_ACTIONS) {
        tag = tags[index];
        name = browser_action(index);
        if (index == 4U) copy_text(value, SD_BrowserClipboardReady() ? base_name(SD_BrowserClipboardPath()) : "EMPTY",
                                   sizeof(value));
    } else {
        const sd_file_entry_t *entry = SD_BrowserEntry((uint8_t)(index - SDUI_BROWSER_ACTIONS));
        if (entry == NULL) return;
        name = entry->name;
        if ((entry->attr & SD_FILE_ATTR_DIR) != 0U) { tag = "DIR"; color = GFX_ACCENT; }
        else if (audio_type(entry->type)) { tag = "AUD"; color = GFX_OK; }
        else if (entry->type == SD_FILE_TYPE_TEXT) { tag = "TXT"; color = GFX_DIM; }
        else tag = "BIN";
        if (details && !(entry->attr & SD_FILE_ATTR_DIR)) format_size(value, entry->size, sizeof(value));
    }
    UI_DrawRow(0U, (uint16_t)(GFX_W - 8U), y, (index == cursor) ? UI_ROW_FOCUS : UI_ROW_NORMAL, NULL, value);
    (void)GFX_Badge(GFX_MARGIN, (uint16_t)(y + 1U), GFX_FONT_SMALL, color, tag);
    const uint16_t vx = (uint16_t)((GFX_W - 16U - GFX_TextWidth(GFX_FONT_BODY, value)) & ~7U);
    (void)GFX_Text(48U, y, GFX_FONT_BODY, (index == cursor) ? GFX_WHITE : GFX_TEXT, name,
                   (uint16_t)(value[0] ? vx - 8U : GFX_W - 16U));
}

static void render_browser(void)
{
    sd_browser_config_t config;
    const uint8_t total = (uint8_t)(SDUI_BROWSER_ACTIONS + SD_BrowserCount());
    SD_BrowserGetConfig(&config);
    UI_DrawTitle(SD_BrowserPath(), SD_BrowserOverflow() ? "MORE ON NEXT PAGE" : NULL);
    const uint8_t first = UI_ListFirst(&first_row, cursor, total, SDUI_VISIBLE);
    for (uint8_t i = 0U; (i < SDUI_VISIBLE) && ((uint8_t)(first + i) < total); i++) {
        browser_row((uint8_t)(first + i), (uint16_t)(UI_LIST_Y + i * UI_ROW_H),
                    (uint8_t)(config.view == SD_BROWSER_VIEW_DETAILS));
    }
    UI_DrawScrollbar((uint16_t)(GFX_W - 6U), UI_LIST_Y, (uint16_t)(SDUI_VISIBLE * UI_ROW_H), total, first,
                     SDUI_VISIBLE);
    footer(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " OPEN   HOLD UP", "HOLD+" GFX_SYM_TURN " FILE MENU");
}

static const char *file_action(uint8_t index, const sd_file_entry_t *entry)
{
    if ((entry->attr & SD_FILE_ATTR_DIR) != 0U) {
        switch (index) {
            case 0U: return "ENTER FOLDER";
            case 1U: return "PROPERTIES";
            case 2U: return "RENAME";
            case 3U: return "COPY (WITH CONTENTS)";
            case 4U: return "MOVE";
            case 5U: return "DELETE (WITH CONTENTS)";
            case 6U: return "PASTE INTO FOLDER";
            case 7U: return "BACK";
            default: return "";
        }
    }
    switch (index) {
        case 0U: return audio_type(entry->type) ? "PLAY OVER RF" : (entry->type == SD_FILE_TYPE_TEXT ? "READ TEXT" : "VIEW HEX");
        case 1U: return "VIEW HEX";
        case 2U: return "PROPERTIES";
        case 3U: return "RENAME";
        case 4U: return "COPY";
        case 5U: return "MOVE";
        case 6U: return "DELETE";
        case 7U: return "BACK";
        default: return "";
    }
}

static void render_file_menu(void)
{
    const sd_file_entry_t *entry = SD_BrowserEntry(selected_index);
    const char *labels[SDUI_FILE_ROWS];
    if (entry == NULL) { render_hub(); return; }
    UI_DrawTitle(entry->name, SD_FileTypeLabel(entry->type));
    for (uint8_t i = 0U; i < SDUI_FILE_ROWS; i++) labels[i] = file_action(i, entry);
    render_list(labels, NULL, SDUI_FILE_ROWS);
    footer(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " DO   HOLD BACK", NULL);
}

static void render_properties(void)
{
    const sd_file_entry_t *entry = SD_BrowserEntry(selected_index);
    char line[40];
    if (entry == NULL) { render_hub(); return; }
    UI_DrawTitle("PROPERTIES", NULL);
    (void)GFX_Text(GFX_MARGIN, 32U, GFX_FONT_BODY, GFX_WHITE, entry->name, (uint16_t)(GFX_W - GFX_MARGIN));
    UI_DrawPair(GFX_MARGIN, 60U, "TYPE", SD_FileTypeLabel(entry->type), GFX_TEXT);
    line[0] = 0;
    UI_AppendU(line, entry->size, 1U);
    append(line, " BYTES", sizeof(line));
    UI_DrawPair(GFX_MARGIN, 76U, "SIZE", line, GFX_TEXT);
    line[0] = 0;
    UI_AppendU(line, 1980U + (entry->date >> 9), 4U); append(line, "-", sizeof(line));
    UI_AppendU(line + strlen(line), (entry->date >> 5) & 15U, 2U); append(line, "-", sizeof(line));
    UI_AppendU(line + strlen(line), entry->date & 31U, 2U); append(line, "  ", sizeof(line));
    UI_AppendU(line + strlen(line), entry->time >> 11, 2U); append(line, ":", sizeof(line));
    UI_AppendU(line + strlen(line), (entry->time >> 5) & 63U, 2U);
    UI_DrawPair(GFX_MARGIN, 92U, "DATE", line, GFX_TEXT);
    line[0] = 0;
    if (entry->attr & SD_FILE_ATTR_READONLY) append(line, "READ-ONLY ", sizeof(line));
    if (entry->attr & SD_FILE_ATTR_HIDDEN) append(line, "HIDDEN ", sizeof(line));
    if (entry->attr & SD_FILE_ATTR_SYSTEM) append(line, "SYSTEM ", sizeof(line));
    if (entry->attr & SD_FILE_ATTR_ARCHIVE) append(line, "ARCHIVE", sizeof(line));
    UI_DrawPair(GFX_MARGIN, 108U, "ATTRIBUTES", line[0] ? line : "-", GFX_TEXT);
    UI_DrawPair(GFX_MARGIN, 124U, "FOLDER", SD_BrowserPath(), GFX_TEXT);
    footer(GFX_SYM_PRESS " BACK   HOLD CLOSE", NULL);
}

static void render_text(void)
{
    char line[SDUI_TEXT_COLUMNS * 4U + 1U];
    uint16_t input = 0U;
    uint8_t row_index = 0U;
    while (input < viewer_len && row_index < 15U) {
        uint16_t used = 0U;
        uint8_t columns = 0U;
        while (input < viewer_len && columns < SDUI_TEXT_COLUMNS) {
            uint8_t c = scratch.view[input++];
            if (c == '\r') continue;
            if (c == '\n') break;
            if (c == '\t') c = ' ';
            if (c < 0x20U) c = '.';
            if (c < 0x80U) {
                line[used++] = (char)c;
            } else {
                uint8_t span = ((c & 0xE0U) == 0xC0U) ? 2U :
                               (((c & 0xF0U) == 0xE0U) ? 3U :
                               (((c & 0xF8U) == 0xF0U) ? 4U : 1U));
                uint8_t valid = (span > 1U && input + span - 1U <= viewer_len) ? 1U : 0U;
                for (uint8_t i = 1U; valid != 0U && i < span; i++) {
                    if ((scratch.view[input + i - 1U] & 0xC0U) != 0x80U) valid = 0U;
                }
                if (valid == 0U) {
                    line[used++] = '?';
                } else {
                    line[used++] = (char)c;
                    for (uint8_t i = 1U; i < span; i++) line[used++] = (char)scratch.view[input++];
                }
            }
            columns++;
        }
        line[used] = 0;
        (void)GFX_Text(GFX_MARGIN, (uint16_t)(28U + row_index * 13U), GFX_FONT_SMALL, GFX_TEXT, line,
                       (uint16_t)(GFX_W - GFX_MARGIN));
        row_index++;
    }
}

static void render_hex(void)
{
    char line[64];
    for (uint8_t r = 0U; r < 16U; r++) {
        const uint16_t start = (uint16_t)(r * 8U);
        if (start >= viewer_len) break;
        line[0] = 0;
        UI_AppendHex(line, viewer_offset + start, 8U);
        append(line, "  ", sizeof(line));
        for (uint8_t i = 0U; i < 8U; i++) {
            if (start + i < viewer_len) UI_AppendHex(line + strlen(line), scratch.view[start + i], 2U);
            else append(line, "  ", sizeof(line));
            append(line, " ", sizeof(line));
        }
        append(line, " ", sizeof(line));
        for (uint8_t i = 0U; i < 8U && start + i < viewer_len; i++) {
            const uint8_t c = scratch.view[start + i];
            const char ch[2] = {(char)(((c >= 0x20U) && (c < 0x7FU)) ? c : '.'), 0};
            append(line, ch, sizeof(line));
        }
        (void)GFX_Text(GFX_MARGIN, (uint16_t)(27U + r * 12U), GFX_FONT_SMALL, GFX_TEXT, line,
                       (uint16_t)(GFX_W - GFX_MARGIN));
    }
}

static void render_viewer(void)
{
    char heading[40] = "";
    UI_AppendU(heading, viewer_offset, 1U);
    append(heading, " / ", sizeof(heading));
    UI_AppendU(heading + strlen(heading), SD_ViewerSize(), 1U);
    UI_DrawTitle(viewer_hex ? "HEX VIEW" : "TEXT VIEW", heading);
    if (viewer_hex) render_hex(); else render_text();
    footer(SD_ViewerFindActive() ? "SEARCHING...   " GFX_SYM_PRESS " CANCEL"
                                 : GFX_SYM_TURN " PAGE   " GFX_SYM_PRESS " TOOLS   HOLD CLOSE", NULL);
}

static void render_viewer_menu(void)
{
    const char *labels[SDUI_VIEW_MENU_ROWS] = {
        "FIND TEXT", "GO TO BYTE OFFSET", viewer_hex ? "SWITCH TO TEXT" : "SWITCH TO HEX",
        "PROPERTIES", "CLOSE VIEWER"
    };
    UI_DrawTitle("VIEWER TOOLS", NULL);
    render_list(labels, NULL, SDUI_VIEW_MENU_ROWS);
    footer(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " DO   HOLD CLOSE", NULL);
}

static const char *player_state_label(sd_player_state_t state)
{
    switch (state) {
        case SD_PLAYER_CONFIRM: return "CONFIRM RF TRANSMIT";
        case SD_PLAYER_PREPARING: return "OPENING";
        case SD_PLAYER_PREBUFFER: return "PREBUFFER";
        case SD_PLAYER_PLAYING: return "PLAYING - RF TX";
        case SD_PLAYER_PAUSED: return "PAUSED - RF PARKED";
        case SD_PLAYER_REBUFFER: return "REBUFFERING";
        case SD_PLAYER_DRAINING: return "FINISHING";
        case SD_PLAYER_FINISHED: return "FINISHED - RF PARKED";
        case SD_PLAYER_ERROR: return "ERROR - RF PARKED";
        default: return "CLOSED";
    }
}

static const char *rf_label(sd_player_rf_mode_t mode)
{
    return (mode == SD_PLAYER_RF_AM) ? "AM" : ((mode == SD_PLAYER_RF_FM) ? "FM" : "SSB");
}

static void render_player(void)
{
    sd_audio_player_status_t status;
    sd_audio_player_config_t config;
    char line[64];
    SD_AudioPlayerGetStatus(&status);
    SD_AudioPlayerGetConfig(&config);
    UI_DrawTitle("SD PLAYER", rf_label(status.rf_mode));
    const uint8_t tx = (uint8_t)((status.state == SD_PLAYER_PLAYING) || (status.state == SD_PLAYER_REBUFFER) ||
                                 (status.state == SD_PLAYER_DRAINING));
    (void)GFX_Badge(GFX_MARGIN, 30U, GFX_FONT_BODY,
                    tx ? GFX_ALERT : ((status.state == SD_PLAYER_ERROR) ? GFX_WARN : GFX_ACCENT),
                    player_state_label(status.state));
    (void)GFX_Text(GFX_MARGIN, 56U, GFX_FONT_BODY, GFX_WHITE,
                   status.stream.title[0] ? status.stream.title : base_name(status.path), (uint16_t)(GFX_W - GFX_MARGIN));
    if (status.stream.artist[0]) (void)GFX_Text(GFX_MARGIN, 74U, GFX_FONT_SMALL, GFX_DIM, status.stream.artist, GFX_W);
    strcpy(line, SD_FileTypeLabel(status.stream.type)); append(line, "  ", sizeof(line));
    UI_AppendU(line + strlen(line), status.stream.sample_rate, 1U); append(line, " Hz  ", sizeof(line));
    UI_AppendU(line + strlen(line), status.stream.bits_per_sample, 1U); append(line, " BIT  ", sizeof(line));
    UI_AppendU(line + strlen(line), status.stream.channels, 1U); append(line, " CH", sizeof(line));
    UI_DrawPair(GFX_MARGIN, 96U, "SOURCE", line, GFX_TEXT);
    line[0] = 0;
    UI_AppendU(line, status.position_ms / 1000U, 1U); append(line, " / ", sizeof(line));
    UI_AppendU(line + strlen(line), status.duration_ms / 1000U, 1U); append(line, " s", sizeof(line));
    UI_DrawPair(GFX_MARGIN, 112U, "TIME", line, GFX_TEXT);
    UI_DrawBar(GFX_MARGIN, 132U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 10U, status.position_ms,
               status.duration_ms ? status.duration_ms : 1U, GFX_ACCENT);
    line[0] = 0;
    UI_AppendU(line, status.buffer_ms, 1U); append(line, " ms   REBUFFER ", sizeof(line));
    UI_AppendU(line + strlen(line), status.rebuffer_count, 1U);
    UI_DrawPair(GFX_MARGIN, 152U, "BUFFER", line, GFX_TEXT);
    line[0] = 0;
    UI_AppendU(line, config.gain_percent, 1U); append(line, "%   ", sizeof(line));
    append(line, config.clean ? "CLEAN" : "DSP", sizeof(line));
    UI_DrawPair(GFX_MARGIN, 168U, "GAIN", line, GFX_TEXT);
    if (status.state == SD_PLAYER_ERROR) {
        (void)GFX_Text(GFX_MARGIN, 192U, GFX_FONT_BODY, GFX_WARN, status.error, GFX_W);
    } else if (status.state == SD_PLAYER_CONFIRM) {
        (void)GFX_Text(GFX_MARGIN, 192U, GFX_FONT_BODY, GFX_WARN, "CLICK STARTS RF TRANSMISSION", GFX_W);
        (void)GFX_Text(GFX_MARGIN, 208U, GFX_FONT_SMALL, GFX_DIM, "TURN TO CHOOSE SSB / AM / FM FIRST", GFX_W);
    }
    footer((status.state == SD_PLAYER_CONFIRM) ? GFX_SYM_TURN " RF MODE   " GFX_SYM_PRESS " TRANSMIT   HOLD STOP"
                                               : GFX_SYM_TURN " SEEK   " GFX_SYM_PRESS " PAUSE   HOLD STOP", NULL);
}

static const char *view_label(sd_browser_view_t value)
{
    return (value == SD_BROWSER_VIEW_LIST) ? "LIST" : ((value == SD_BROWSER_VIEW_COMPACT) ? "COMPACT" : "DETAILS");
}

static const char *sort_label(sd_browser_sort_t value)
{
    switch (value) {
        case SD_BROWSER_SORT_TYPE: return "TYPE";
        case SD_BROWSER_SORT_SIZE: return "SIZE";
        case SD_BROWSER_SORT_DATE: return "DATE";
        case SD_BROWSER_SORT_CARD: return "CARD ORDER";
        default: return "NAME";
    }
}

static const char *filter_label(sd_browser_filter_t value)
{
    return (value == SD_BROWSER_FILTER_AUDIO) ? "AUDIO" :
           ((value == SD_BROWSER_FILTER_TEXT) ? "TEXT" :
           ((value == SD_BROWSER_FILTER_OTHER) ? "OTHER" : "ALL"));
}

static void render_browser_settings(void)
{
    sd_browser_config_t c;
    static const char *const labels[SDUI_BROWSER_SET_ROWS] = {
        "VIEW", "SORT BY", "ORDER", "FOLDERS FIRST", "SHOW", "HIDDEN FILES", "BACK"
    };
    const char *values[SDUI_BROWSER_SET_ROWS];
    SD_BrowserGetConfig(&c);
    values[0] = view_label(c.view);
    values[1] = sort_label(c.sort);
    values[2] = c.descending ? "DESCENDING" : "ASCENDING";
    values[3] = c.folders_first ? "YES" : "NO";
    values[4] = filter_label(c.filter);
    values[5] = c.show_hidden ? "SHOW" : "HIDE";
    values[6] = NULL;
    UI_DrawTitle("BROWSER SETTINGS", NULL);
    render_list(labels, values, SDUI_BROWSER_SET_ROWS);
    footer(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " CHANGE   HOLD BACK", NULL);
}

static const char *downmix_label(sd_player_downmix_t value)
{
    return (value == SD_PLAYER_DOWNMIX_LEFT) ? "LEFT" :
           ((value == SD_PLAYER_DOWNMIX_RIGHT) ? "RIGHT" :
           ((value == SD_PLAYER_DOWNMIX_STRONGER) ? "STRONGER" : "AVERAGE"));
}

static void render_player_settings(void)
{
    sd_audio_player_config_t c;
    static const char *const labels[SDUI_PLAYER_SET_ROWS] = {
        "ASK EACH TRACK", "AUTO ADVANCE", "REPEAT TRACK", "SHUFFLE", "SEEK STEP", "DOWNMIX",
        "GAIN", "AUDIO DSP", "RF MODE", "SAVE SETTINGS", "BACK"
    };
    const char *values[SDUI_PLAYER_SET_ROWS];
    char seek[12] = "", gain[12] = "";
    SD_AudioPlayerGetConfig(&c);
    UI_AppendU(seek, c.seek_seconds, 1U); append(seek, " s", sizeof(seek));
    UI_AppendU(gain, c.gain_percent, 1U); append(gain, "%", sizeof(gain));
    values[0] = c.ask_each_track ? "YES" : "NO";
    values[1] = c.advance ? "ON" : "OFF";
    values[2] = c.repeat ? "ON" : "OFF";
    values[3] = c.shuffle ? "ON" : "OFF";
    values[4] = seek;
    values[5] = downmix_label(c.downmix);
    values[6] = gain;
    values[7] = c.clean ? "CLEAN" : "NORMAL";
    values[8] = rf_label(SD_AudioPlayerGetRfMode());
    values[9] = NULL;
    values[10] = NULL;
    UI_DrawTitle("PLAYER SETTINGS", NULL);
    render_list(labels, values, SDUI_PLAYER_SET_ROWS);
    footer(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " CHANGE   HOLD BACK", NULL);
}

static void render_editor(void)
{
    UI_DrawTitle((editor_purpose == EDIT_NEW_FOLDER) ? "NEW FOLDER NAME" :
                 ((editor_purpose == EDIT_RENAME) ? "RENAME" :
                 ((editor_purpose == EDIT_FIND) ? "FIND TEXT" : "GO TO BYTE")), NULL);
    GFX_Fill(GFX_MARGIN, 40U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 32U, GFX_PANEL);
    /* Characters one per 8 px cell, so the cursor sits in its own tiles. */
    uint16_t cols = (uint16_t)((GFX_W - 32U) / 8U);
    uint16_t first = (editor_codepoint >= cols) ? (uint16_t)(editor_codepoint - cols + 1U) : 0U;
    const uint16_t start = utf8_byte_offset(scratch.edit, first);
    (void)GFX_Text(16U, 48U, GFX_FONT_BODY, GFX_WHITE, &scratch.edit[start], (uint16_t)(GFX_W - 16U));
    GFX_Rect((uint16_t)(16U + (editor_codepoint - first) * 8U), 66U, 8U, 2U, GFX_ACCENT);
    static const char *const help[5][2] = {
        {GFX_SYM_TURN " OR UP/DN", "CHANGE CHARACTER"},
        {GFX_SYM_PRESS " OR SEL", "NEXT CHARACTER"},
        {"HOLD+" GFX_SYM_TURN " LEFT", "BACKSPACE  (HOLD UP)"},
        {"HOLD+" GFX_SYM_TURN " RIGHT", "CANCEL  (HOLD DN)"},
        {"HOLD " GFX_SYM_PRESS, "SAVE  (HOLD SEL)"},
    };
    for (uint8_t i = 0U; i < 5U; i++) {
        (void)GFX_Text(GFX_MARGIN, (uint16_t)(92U + i * 20U), GFX_FONT_BODY, GFX_ACCENT, help[i][0], 136U);
        (void)GFX_Text(144U, (uint16_t)(92U + i * 20U), GFX_FONT_BODY, GFX_TEXT, help[i][1], (uint16_t)(GFX_W - GFX_MARGIN));
    }
    footer("HOLD " GFX_SYM_PRESS " SAVE", NULL);
}

static void render_warning(const char *title, const char *l1, const char *l2, const char *action)
{
    UI_DrawTitle(title, NULL);
    GFX_Fill(GFX_MARGIN, 40U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 128U, GFX_ALERT_BG);
    (void)GFX_TextCenter(0U, GFX_W, 56U, GFX_FONT_BODY, GFX_WHITE, l1);
    (void)GFX_TextCenter(0U, GFX_W, 80U, GFX_FONT_BODY, GFX_WHITE, l2);
    (void)GFX_TextCenter(0U, GFX_W, 120U, GFX_FONT_BODY, GFX_WARN, action);
    (void)GFX_TextCenter(0U, GFX_W, 144U, GFX_FONT_SMALL, GFX_TEXT, "A SHORT PRESS OR A TURN CANCELS");
    footer("HOLD " GFX_SYM_PRESS " CONFIRM", NULL);
}

static void render_confirm(uint8_t format)
{
    const sd_file_entry_t *entry = SD_BrowserEntry(selected_index);
    if (format) render_warning("FORMAT SD CARD?", "ALL FILES WILL BE ERASED", "A NEW FAT32 VOLUME IS CREATED",
                               "HOLD THE KNOB TO FORMAT");
    else render_warning("DELETE?", (entry != NULL) ? entry->name : "SELECTED ITEM", "FOLDERS ARE DELETED WITH CONTENTS",
                        "HOLD THE KNOB TO DELETE");
}

static void render_fw_confirm(void)
{
    render_warning("INSTALL FIRMWARE?", "REV1FW.UPD VERIFIED ON THE CARD", "KEEP POWER AND CARD CONNECTED",
                   "HOLD THE KNOB TO INSTALL");
}

static void render_job(void)
{
    sd_file_job_status_t job;
    char line[64];
    SD_FileJobGetStatus(&job);
    if (SD_ServiceOperation() == SD_SERVICE_OP_FORMAT) {
        UI_DrawTitle("FORMATTING SD", NULL);
        line[0] = 0;
        UI_AppendU(line, SD_ServiceProgressPct(), 1U); append(line, "%", sizeof(line));
        (void)GFX_TextCenter(0U, GFX_W, 64U, GFX_FONT_BODY, GFX_WHITE, line);
        UI_DrawBar(GFX_MARGIN, 88U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 14U, SD_ServiceProgressPct(), 100U, GFX_ACCENT);
        (void)GFX_TextCenter(0U, GFX_W, 120U, GFX_FONT_BODY, GFX_WARN, "DO NOT REMOVE POWER OR CARD");
        footer("FORMAT RUNS IN FOREGROUND SLICES", NULL);
        return;
    }
    if (job_is_format != 0U) {
        UI_DrawTitle(SD_IsMounted() ? "FORMAT COMPLETE" : "FORMAT FAILED", NULL);
        (void)GFX_TextCenter(0U, GFX_W, 80U, GFX_FONT_BODY, SD_IsMounted() ? GFX_OK : GFX_ALERT, SD_ServiceStatusString());
        footer(GFX_SYM_PRESS " BACK TO SD MENU", NULL);
        return;
    }
    UI_DrawTitle((job.kind == SD_FILE_JOB_DELETE) ? "DELETING" : "COPYING", NULL);
    (void)GFX_Text(GFX_MARGIN, 36U, GFX_FONT_BODY, GFX_WHITE, job.current_name, (uint16_t)(GFX_W - GFX_MARGIN));
    line[0] = 0;
    UI_AppendU(line, job.files_done, 1U); append(line, "   FOLDERS ", sizeof(line));
    UI_AppendU(line + strlen(line), job.dirs_done, 1U);
    UI_DrawPair(GFX_MARGIN, 64U, "FILES", line, GFX_TEXT);
    line[0] = 0;
    UI_AppendU(line, job.bytes_done, 1U);
    UI_DrawPair(GFX_MARGIN, 80U, "BYTES", line, GFX_TEXT);
    line[0] = 0;
    UI_AppendU(line, job.depth, 1U);
    UI_DrawPair(GFX_MARGIN, 96U, "DEPTH", line, GFX_TEXT);
    if ((job.kind == SD_FILE_JOB_FAILED) || (job.kind == SD_FILE_JOB_CANCELLED) || (job.kind == SD_FILE_JOB_DONE)) {
        (void)GFX_Text(GFX_MARGIN, 128U, GFX_FONT_BODY, (job.kind == SD_FILE_JOB_DONE) ? GFX_OK : GFX_WARN,
                       (job.kind == SD_FILE_JOB_DONE) ? "DONE" : job.error, GFX_W);
        footer(GFX_SYM_PRESS " BACK TO BROWSER", NULL);
    } else {
        footer(GFX_SYM_PRESS " CANCEL", NULL);
    }
}

void UI_SDRender(void)
{
    switch (screen) {
        case SDUI_HUB: render_hub(); break;
        case SDUI_BROWSER: render_browser(); break;
        case SDUI_FILE_MENU: render_file_menu(); break;
        case SDUI_PROPERTIES: render_properties(); break;
        case SDUI_VIEWER: render_viewer(); break;
        case SDUI_VIEWER_MENU: render_viewer_menu(); break;
        case SDUI_PLAYER: render_player(); break;
        case SDUI_BROWSER_SETTINGS: render_browser_settings(); break;
        case SDUI_PLAYER_SETTINGS: render_player_settings(); break;
        case SDUI_EDITOR: render_editor(); break;
        case SDUI_DELETE_CONFIRM: render_confirm(0U); break;
        case SDUI_FORMAT_CONFIRM: render_confirm(1U); break;
        case SDUI_FW_CONFIRM: render_fw_confirm(); break;
        case SDUI_JOB: render_job(); break;
        default: render_hub(); break;
    }
}

/* One level back without ever confirming anything: a confirmation page or
 * the editor cancels, a running job is asked to stop, and everything else
 * takes the long-select route it already has (close, stop, up). Returns 0 on
 * the hub, where back means leaving the SD pages. */
uint8_t UI_SDBack(void)
{
    switch (screen) {
        case SDUI_HUB:            return 0U;
        case SDUI_EDITOR:         set_screen(editor_return); break;
        case SDUI_DELETE_CONFIRM: set_screen(SDUI_FILE_MENU); break;
        case SDUI_FORMAT_CONFIRM:
        case SDUI_FW_CONFIRM:     set_screen(SDUI_HUB); break;
        case SDUI_JOB:            UI_SDHandle(0U, 0U, 1U, 0U, 0U, 0U); break;
        default:                  UI_SDHandle(0U, 0U, 0U, 0U, 0U, 1U); break;
    }
    return 1U;
}

uint8_t UI_SDAtHub(void)
{
    return (uint8_t)(screen == SDUI_HUB);
}

uint8_t UI_SDPlayerPageActive(void)
{
    return (screen == SDUI_PLAYER && SD_AudioPlayerIsOpen()) ? 1U : 0U;
}

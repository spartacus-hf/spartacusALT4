/**
 * @file    mod_sdcard_service.h
 * @brief   Background SD card service and FatFs integration.
 */
#ifndef MOD_SDCARD_SERVICE_H
#define MOD_SDCARD_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "mod_sdcard.h"
#include "sd_file_browser.h"

typedef enum {
    SD_SERVICE_OP_IDLE = 0,
    SD_SERVICE_OP_FORMAT,
    SD_SERVICE_OP_FILE_JOB
} sd_service_op_t;

/**
 * One-time initialization of the SD card subsystem.
 * Calls SD_SPI1_Init() and SD_Init(). Called from main() after
 * the SPI1 hardware is configured.
 */
void SD_ServiceInit(void);

/**
 * Background service tick — call from main loop.
 * Handles long SD operations in small chunks. This must only be called from
 * the non-realtime main-loop path.
 */
void SD_ServiceBackground(void);

/**
 * Re-probe the card and mount FAT16/FAT32 if present.
 */
void SD_ServiceRefresh(void);

/**
 * Enter/exit raw USB Mass Storage ownership of the SD card.
 * Enter unmounts FatFs and clears the file cache so only MSC block callbacks
 * touch the card. Exit re-probes and remounts FatFs.
 */
uint8_t SD_UsbMscEnter(void);
void SD_UsbMscExit(void);
uint8_t SD_UsbMscActive(void);

/**
 * Return 1 if the SD card is mounted and ready.
 */
uint8_t SD_IsMounted(void);

/**
 * Write a measurement export to EXPORTS/<stem><NNN>.CSV (first free number,
 * stem up to 3 characters). line() fills one text line (with its own line
 * ending) into buf and returns its length. Main loop only; returns 1 and the
 * path in name (24 bytes) when the file is closed successfully.
 */
typedef uint16_t (*sd_export_line_fn)(uint16_t index, char *buf, uint16_t cap);
uint8_t SD_ExportLines(const char *stem, char *name, uint16_t lines, sd_export_line_fn line);

/**
 * Bounded byte-level file access for the self test and the USB diagnostic
 * console. Paths are FatFs paths from the card root with no leading slash and
 * no parent traversal. Main loop only; every call refuses while the card is
 * owned by USB mass storage or a background job. Write with truncate starts
 * the file over; without it the write lands at offset in an existing or new
 * file, so a host can stream a file in blocks.
 */
/* The calls below would read the card now rather than refuse: a failure
 * while this holds is the card's or the file's. */
uint8_t SD_DiagIoAllowed(void);
uint8_t SD_DiagStat(const char *path, uint32_t *size, uint8_t *attr);
/* len_a bytes at off_a, then up to len_b at off_b (*got_b), one open. */
uint8_t SD_DiagReadSpans(const char *path, uint32_t off_a, void *a, uint32_t len_a,
                         uint32_t off_b, void *b, uint32_t len_b, uint32_t *got_b);
/* main.c, around the boot: card I/O allowed before the main loop runs. */
void SD_ServiceBootIo(uint8_t on);
uint8_t SD_DiagReadFile(const char *path, uint32_t offset, void *data, uint32_t len, uint32_t *out_len);
/* FNV-1a of a file from `offset` to its end, one open. */
uint8_t SD_DiagFileFnv1a(const char *path, uint32_t offset, uint32_t *hash);
/* len bytes from offset, to each() a chunk (at most 64 bytes) at a time,
 * one open; 0 if the file ends early. */
typedef void (*sd_chunk_fn)(const uint8_t *data, uint32_t len, void *ctx);
uint8_t SD_DiagReadEach(const char *path, uint32_t offset, uint32_t len, sd_chunk_fn each, void *ctx);
uint8_t SD_DiagWriteFile(const char *path, uint32_t offset, const void *data, uint32_t len, uint8_t truncate);
uint8_t SD_DiagDelete(const char *path);
uint8_t SD_DiagMkdir(const char *path);
uint8_t SD_DiagRename(const char *path, const char *to);
uint8_t SD_DiagFreeMiB(uint32_t *free_mib, uint32_t *total_mib);
/* One directory entry by position, for streaming a listing to a host without
 * holding a directory handle or touching the browser's name pool. Returns 0
 * once the listing is exhausted. */
uint8_t SD_DiagListEntry(const char *path, uint16_t index, char *name, uint16_t cap,
                         uint32_t *size, uint8_t *attr);

/**
 * Return the current service operation and percent progress.
 */
sd_service_op_t SD_ServiceOperation(void);
uint8_t SD_ServiceProgressPct(void);

/**
 * Start a background FAT32 factory format. Returns 1 if accepted.
 */
uint8_t SD_FormatFactoryStart(void);

/**
 * Capacity in whole MiB, or 0 when unknown.
 */
uint32_t SD_ServiceCapacityMiB(void);

/**
 * Human-readable mount/operation status.
 */
const char *SD_ServiceStatusString(void);

/**
 * Diagnostics for storage/MSC failure reporting.
 */
const char *SD_ServiceLastFatFsOp(void);
const char *SD_ServiceLastFatFsPath(void);
uint8_t SD_ServiceLastFResult(void);
uint8_t SD_ServiceLastSettingsStep(void);
uint8_t SD_UsbMscDirty(void);
uint8_t SD_UsbMscUnsafeExit(void);
uint8_t SD_UsbMscLastOpcode(void);
uint8_t SD_UsbMscLastSenseKey(void);
uint8_t SD_UsbMscLastSenseAsc(void);
uint32_t SD_UsbMscLastLba(void);
uint32_t SD_UsbMscLastBlocks(void);

void SD_UsbMscRecordCommand(uint8_t opcode, uint32_t lba, uint32_t blocks);
void SD_UsbMscRecordSense(uint8_t key, uint8_t asc);
void SD_UsbMscRecordRead(uint32_t lba, uint32_t blocks, uint8_t ok);
void SD_UsbMscRecordWrite(uint32_t lba, uint32_t blocks, uint8_t ok);
uint8_t SD_UsbMscSynchronizeCache(void);
uint8_t SD_UsbMscHostEject(void);

/* Borrow the service's 512-byte scratch sector for a foreground update while
 * SD_SERVICE_OP_IDLE. The service and updater run only on the main loop. */
uint8_t *SD_UpdateWorkSector(void);

/**
 * Compatibility wrappers for the full browser service.
 */
uint8_t SD_FileManagerRefresh(void);
uint8_t SD_FileManagerCount(void);
const sd_file_entry_t *SD_FileManagerEntry(uint8_t idx);
uint8_t SD_FileManagerDelete(uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif /* MOD_SDCARD_SERVICE_H */

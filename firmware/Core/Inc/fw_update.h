#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdint.h>

typedef enum {
    FW_UPDATE_OK = 0,
    FW_UPDATE_BUSY,
    FW_UPDATE_NO_CARD,
    FW_UPDATE_UNSAFE_MSC,
    FW_UPDATE_NO_FILE,
    FW_UPDATE_BAD_HEADER,
    FW_UPDATE_WRONG_BOARD,
    FW_UPDATE_BAD_IMAGE,
    FW_UPDATE_IO_ERROR,
    FW_UPDATE_NO_LOADER,
    FW_UPDATE_UNPROTECTED,
    FW_UPDATE_FLASH_ERROR
} fw_update_result_t;

/* Foreground only. Install resets on success and returns an error otherwise. */
fw_update_result_t FW_UpdateVerify(void);
fw_update_result_t FW_UpdateInstall(void);
const char *FW_UpdateResultText(fw_update_result_t result);
const char *FW_UpdateStateText(void);
/* NULL when a standalone reset can reach this exact application image. */
const char *FW_UpdateBootFault(void);
void FW_UpdateConfirmService(uint32_t now);

#endif

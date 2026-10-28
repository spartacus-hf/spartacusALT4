#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/**
 * @file    usb_disk_mode.c
 * @brief   Safe USB Audio <-> USB MSC mode switch orchestration.
 */

#include "usb_disk_mode.h"
#include "main.h"
#include "usb_device.h"
#include "modulator.h"
#include "mod_sdcard_service.h"

static uint8_t usb_disk_active = 0U;
static uint8_t usb_disk_prev_muted = 0U;

uint8_t USB_DiskModeEnter(void)
{
    if (usb_disk_active != 0U) return 1U;
    if (SD_ServiceOperation() != SD_SERVICE_OP_IDLE) return 0U;
    if (MOD_BackgroundIoAllowed(HAL_GetTick()) == 0U) return 0U;

    usb_disk_prev_muted = MOD_GetMuted();
    MOD_SetMuted(1U);
    MOD_TxCarrierTestStop();
    MOD_UsbStreamReset();
    MOD_FillAllNullBuffers();

    if (SD_UsbMscEnter() == 0U) {
        MOD_SetMuted(usb_disk_prev_muted);
        return 0U;
    }

    USB_Device_ReEnumerateMsc();
    /* Mass storage reads and writes the card from the USB interrupt, a few
     * hundred microseconds a block and up to tens of milliseconds a command.
     * At its usual priority 1 that held off the receiver (PendSV, 2) and the
     * audio DMA (1) for as long; below them the receiver keeps running and
     * only the copy slows. USB_Device_ReEnumerateAudio puts 1 back
     * (HAL_PCD_MspInit). */
    HAL_NVIC_SetPriority(USB_LP_IRQn, 3U, 0U);
    usb_disk_active = 1U;
    return 1U;
}

void USB_DiskModeExit(void)
{
    if (usb_disk_active == 0U) return;

    USB_Device_ReEnumerateAudio();
    SD_UsbMscExit();
    MOD_UsbStreamReset();
    MOD_SetMuted(usb_disk_prev_muted);
    usb_disk_active = 0U;
}

uint8_t USB_DiskModeIsActive(void)
{
    return usb_disk_active;
}

const char *USB_DiskModeStatusString(void)
{
    if (SD_IsReady() == 0U) return "NO CARD";
    return "ACTIVE";
}

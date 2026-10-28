#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/**
 * @file    usb_diag_mode.c
 * @brief   USB Audio <-> USB diagnostic console mode switch.
 */

#include "usb_diag_mode.h"
#include "usb_diag.h"
#include "main.h"
#include "usb_device.h"
#include "usb_disk_mode.h"
#include "modulator.h"

static uint8_t diag_active;
static uint8_t diag_prev_muted;
/* An `audio` window is open: the console lent the port and will take it
 * back, so diag_prev_muted still holds the state from before the console. */
static uint8_t diag_window_open;
/* Deadline for retaking the port after an `audio` window, 0 when none. */
static uint32_t diag_return_at;
#define DIAG_RETURN_GIVE_UP_MS 30000U

__attribute__((cold)) static uint8_t diag_enter(uint8_t mute)
{
    if (diag_active) return 1U;
    /* Never re-enumerate under the audio path's feet. */
    if (USB_DiskModeIsActive()) return 0U;
    if (MOD_AudioRealtimeCritical(HAL_GetTick())) return 0U;
    if (MOD_BackgroundIoAllowed(HAL_GetTick()) == 0U) return 0U;

    if (!diag_window_open) diag_prev_muted = MOD_GetMuted();
    diag_window_open = 0U;
    if (mute) MOD_SetMuted(1U);
    MOD_TxCarrierTestStop();
    MOD_UsbStreamReset();
    MOD_FillAllNullBuffers();

    USB_Device_ReEnumerateDiag();
    USB_DiagReset();
    diag_active = 1U;
    return 1U;
}

/* Opened from the panel or the console: TX is muted while it is up. */
__attribute__((cold)) uint8_t USB_DiagModeEnter(void) { return diag_enter(1U); }

/* Opened by the boot itself (DIAG ON BOOT, a card that needs files, no
 * card): the operator did not ask for a console session, so TX MUTE stays
 * as it was -- off by default. Muting here left every DIAG ON BOOT radio
 * muted after power-up (bench, 2026-10-01); CW is held by its own
 * power-up arming instead (mod_cw.c). */
__attribute__((cold)) uint8_t USB_DiagModeEnterAtBoot(void) { return diag_enter(0U); }

__attribute__((cold)) static void diag_leave(uint8_t restore_mute)
{
    /* A virtual paddle lever must not outlive the port it came in on. */
    USB_DiagRelease();
    USB_Device_ReEnumerateAudio();
    MOD_UsbStreamReset();
    if (restore_mute) MOD_SetMuted(diag_prev_muted);
    diag_active = 0U;
}

__attribute__((cold)) void USB_DiagModeExit(void)
{
    /* Leaving on purpose cancels any pending return: `exit` means exit. */
    diag_return_at = 0U;
    if (!diag_active) return;
    diag_leave(1U);
    diag_window_open = 0U;
}

/* The window keeps TX muted, as the console had it. It lends the port for a
 * capture (rev1_diag.py audio); restoring the pre-console mute there -- off,
 * on a board that boots into the console -- transmitted whatever the host
 * played into the radio for the length of the window. `exit` still restores
 * it. */
__attribute__((cold)) uint8_t USB_DiagModeAudioWindow(uint32_t seconds)
{
    if (!diag_active || !seconds || seconds > 600U) return 0U;
    diag_return_at = 0U;
    diag_leave(0U);
    diag_window_open = 1U;
    diag_return_at = HAL_GetTick() + seconds * 1000U;
    if (!diag_return_at) diag_return_at = 1U; /* 0 means "no window" */
    return 1U;
}

void USB_DiagModeTick(uint32_t now)
{
    if (!diag_return_at || diag_active) return;
    if ((int32_t)(now - diag_return_at) < 0) return;
    /* USB_DiagModeEnter refuses while the audio path is critical or storage
     * is busy, so keep asking; stop asking eventually rather than leaving a
     * deadline alive forever. The panel can always re-enter by hand. */
    if (USB_DiagModeEnter() || now - diag_return_at > DIAG_RETURN_GIVE_UP_MS) diag_return_at = 0U;
}

uint8_t USB_DiagModeIsActive(void) { return diag_active; }

const char *USB_DiagModeStatusString(void)
{
    if (!diag_active) return diag_return_at ? "AUDIO WINDOW" : "OFF";
    return USB_DiagIsOpen() ? "HOST ATTACHED" : "WAITING FOR HOST";
}

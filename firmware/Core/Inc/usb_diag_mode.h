#ifndef USB_DIAG_MODE_H
#define USB_DIAG_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* USB Audio <-> USB diagnostic console mode switch, the sibling of
 * usb_disk_mode.c. The console is a separate USB personality rather than a
 * composite interface for one reason: while it is enumerated the USB audio
 * class is not running at all, so the console cannot compete with the audio
 * path for interrupt time. It can only replace it. */

uint8_t USB_DiagModeEnter(void);
/* The same, opened by the boot (DIAG ON BOOT, card setup, no card): TX MUTE
 * is left as it is. */
uint8_t USB_DiagModeEnterAtBoot(void);
void    USB_DiagModeExit(void);
uint8_t USB_DiagModeIsActive(void);
const char *USB_DiagModeStatusString(void);
/* Hand the port back to USB audio for a while, then take it again without an
 * operator at the panel. Capturing the receiver's audio or raw I/Q on the host
 * needs the audio personality, and re-entering the console otherwise means
 * walking the LCD menu, which makes a scripted bench run impossible. Returns 0
 * if the console is not the active personality or the window is out of range
 * (1..600 s). */
uint8_t USB_DiagModeAudioWindow(uint32_t seconds);
/* Main loop. Retakes the port when the window has expired; gives up after
 * 30 s of the audio path never being idle enough to re-enumerate. While a
 * window is open USB_DiagModeStatusString reads AUDIO WINDOW, so the panel
 * says why the console went away. */
void    USB_DiagModeTick(uint32_t now);

#ifdef __cplusplus
}
#endif

#endif

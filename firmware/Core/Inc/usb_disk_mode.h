#ifndef USB_DISK_MODE_H
#define USB_DISK_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

uint8_t USB_DiskModeEnter(void);
void USB_DiskModeExit(void);
uint8_t USB_DiskModeIsActive(void);
const char *USB_DiskModeStatusString(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_DISK_MODE_H */

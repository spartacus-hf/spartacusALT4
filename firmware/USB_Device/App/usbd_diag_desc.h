#ifndef __USBD_DIAG_DESC_H__
#define __USBD_DIAG_DESC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_def.h"
#include "usbd_desc.h"

/* Descriptors for the diagnostic console personality. A distinct product
 * string and PID so a udev rule, or a person reading dmesg, can tell this
 * apart from the audio and mass-storage personalities of the same board. */
extern const USBD_DescriptorsTypeDef DIAG_Desc;

#ifdef __cplusplus
}
#endif

#endif

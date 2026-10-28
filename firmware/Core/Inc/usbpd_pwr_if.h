/* REV1 power-port adaptation; no controllable VBUS source, switch or VCONN. */
#ifndef REV1_USBPD_PWR_IF_H
#define REV1_USBPD_PWR_IF_H
#include "usbpd_def.h"
typedef enum { USBPD_PWR_BELOWVSAFE0V, USBPD_PWR_VSAFE5V, USBPD_PWR_SNKDETACH } USBPD_VBUSPOWER_STATUS;
typedef enum { VBUS_CONNECTED, VBUS_NOT_CONNECTED } USBPD_PWR_VBUSConnectionStatusTypeDef;
uint8_t USBPD_PWR_IF_GetVBUSStatus(uint8_t port, USBPD_VBUSPOWER_STATUS status);
int32_t BSP_USBPD_PWR_Init(uint32_t port);
int32_t BSP_USBPD_PWR_RegisterVBUSDetectCallback(uint32_t port,
    void (*callback)(uint32_t, USBPD_PWR_VBUSConnectionStatusTypeDef));
int32_t BSP_USBPD_PWR_VBUSInit(uint32_t port);
int32_t BSP_USBPD_PWR_VBUSDeInit(uint32_t port);
int32_t BSP_USBPD_PWR_VCONNInit(uint32_t port, uint32_t cc);
int32_t BSP_USBPD_PWR_VCONNDeInit(uint32_t port, uint32_t cc);
void RADIO_PdPortPoll(void);
uint8_t RADIO_PdTimerReady(void);
#endif

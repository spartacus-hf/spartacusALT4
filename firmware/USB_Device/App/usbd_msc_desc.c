/**
  ******************************************************************************
  * @file           : App/usbd_msc_desc.c
  * @brief          : USB Mass Storage device descriptors.
  ******************************************************************************
  */

#include "usbd_core.h"
#include "usbd_msc_desc.h"
#include "usbd_conf.h"
#include "modulator.h"

#define USBD_MSC_VID                  1155U
#define USBD_MSC_PID                  22337U
#define USBD_MSC_LANGID_STRING        1033U
#define USBD_MSC_MANUFACTURER_STRING  "Chireix"
#define USBD_MSC_PRODUCT_STRING       "Chireix TX SD Disk"
#define USBD_MSC_CONFIGURATION_STRING "MSC Config"
#define USBD_MSC_INTERFACE_STRING     "MSC Interface"

static void MSC_GetSerialNum(void);
static void MSC_IntToUnicode(uint32_t value, uint8_t *pbuf, uint8_t len);

uint8_t *USBD_MSC_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_MSC_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_MSC_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_MSC_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_MSC_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_MSC_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
uint8_t *USBD_MSC_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);

const USBD_DescriptorsTypeDef MSC_Desc =
{
  USBD_MSC_DeviceDescriptor,
  USBD_MSC_LangIDStrDescriptor,
  USBD_MSC_ManufacturerStrDescriptor,
  USBD_MSC_ProductStrDescriptor,
  USBD_MSC_SerialStrDescriptor,
  USBD_MSC_ConfigStrDescriptor,
  USBD_MSC_InterfaceStrDescriptor
};

__ALIGN_BEGIN static const uint8_t USBD_MSC_DeviceDesc[USB_LEN_DEV_DESC] __ALIGN_END =
{
  0x12,
  USB_DESC_TYPE_DEVICE,
  0x00,
  0x02,
  0x00,
  0x00,
  0x00,
  USB_MAX_EP0_SIZE,
  LOBYTE(USBD_MSC_VID),
  HIBYTE(USBD_MSC_VID),
  LOBYTE(USBD_MSC_PID),
  HIBYTE(USBD_MSC_PID),
  LOBYTE(FW_VERSION_BCD),
  HIBYTE(FW_VERSION_BCD),
  USBD_IDX_MFC_STR,
  USBD_IDX_PRODUCT_STR,
  USBD_IDX_SERIAL_STR,
  USBD_MAX_NUM_CONFIGURATION
};

__ALIGN_BEGIN static const uint8_t USBD_MSC_LangIDDesc[USB_LEN_LANGID_STR_DESC] __ALIGN_END =
{
  USB_LEN_LANGID_STR_DESC,
  USB_DESC_TYPE_STRING,
  LOBYTE(USBD_MSC_LANGID_STRING),
  HIBYTE(USBD_MSC_LANGID_STRING)
};

/* Shared with the diagnostic personality: only one of them is ever
 * enumerated, so they cannot both be mid-conversion with the host. */
__ALIGN_BEGIN uint8_t USBD_SharedStrDesc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;
USBD_STR_FITS(USBD_MSC_MANUFACTURER_STRING);
USBD_STR_FITS(USBD_MSC_PRODUCT_STRING);
USBD_STR_FITS(USBD_MSC_CONFIGURATION_STRING);
USBD_STR_FITS(USBD_MSC_INTERFACE_STRING);
#define USBD_MSC_StrDesc USBD_SharedStrDesc

__ALIGN_BEGIN static uint8_t USBD_MSC_StringSerial[USB_SIZ_STRING_SERIAL] __ALIGN_END =
{
  USB_SIZ_STRING_SERIAL,
  USB_DESC_TYPE_STRING,
};

uint8_t *USBD_MSC_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  *length = sizeof(USBD_MSC_DeviceDesc);
  return (uint8_t *)USBD_MSC_DeviceDesc;
}

uint8_t *USBD_MSC_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  *length = sizeof(USBD_MSC_LangIDDesc);
  return (uint8_t *)USBD_MSC_LangIDDesc;
}

uint8_t *USBD_MSC_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_MSC_MANUFACTURER_STRING, USBD_MSC_StrDesc, length);
  return USBD_MSC_StrDesc;
}

uint8_t *USBD_MSC_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_MSC_PRODUCT_STRING, USBD_MSC_StrDesc, length);
  return USBD_MSC_StrDesc;
}

uint8_t *USBD_MSC_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  *length = USB_SIZ_STRING_SERIAL;
  MSC_GetSerialNum();
  return USBD_MSC_StringSerial;
}

uint8_t *USBD_MSC_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_MSC_CONFIGURATION_STRING, USBD_MSC_StrDesc, length);
  return USBD_MSC_StrDesc;
}

uint8_t *USBD_MSC_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_MSC_INTERFACE_STRING, USBD_MSC_StrDesc, length);
  return USBD_MSC_StrDesc;
}

static void MSC_GetSerialNum(void)
{
  uint32_t deviceserial0 = *(uint32_t *)DEVICE_ID1;
  uint32_t deviceserial1 = *(uint32_t *)DEVICE_ID2;
  uint32_t deviceserial2 = *(uint32_t *)DEVICE_ID3;

  deviceserial0 += deviceserial2;

  if (deviceserial0 != 0U)
  {
    MSC_IntToUnicode(deviceserial0, &USBD_MSC_StringSerial[2], 8U);
    MSC_IntToUnicode(deviceserial1, &USBD_MSC_StringSerial[18], 4U);
  }
}

static void MSC_IntToUnicode(uint32_t value, uint8_t *pbuf, uint8_t len)
{
  for (uint8_t idx = 0U; idx < len; idx++)
  {
    if ((value >> 28) < 0xAU)
    {
      pbuf[2U * idx] = (uint8_t)((value >> 28) + '0');
    }
    else
    {
      pbuf[2U * idx] = (uint8_t)((value >> 28) + 'A' - 10U);
    }

    value <<= 4;
    pbuf[(2U * idx) + 1U] = 0U;
  }
}

/**
  * @file    App/usbd_diag_desc.c
  * @brief   USB descriptors for the REV1 diagnostic console personality.
  */

#include "usbd_core.h"
#include "usbd_diag_desc.h"
#include "usbd_conf.h"
#include "modulator.h"

#define USBD_DIAG_VID                  1155U
#define USBD_DIAG_PID                  22338U  /* one past the MSC personality */
#define USBD_DIAG_LANGID_STRING        1033U
#define USBD_DIAG_MANUFACTURER_STRING  "Chireix"
#define USBD_DIAG_PRODUCT_STRING       "SPARTACUS REV1 DIAG"
#define USBD_DIAG_CONFIGURATION_STRING "Diagnostic Console"
#define USBD_DIAG_INTERFACE_STRING     "CDC Console"

static uint8_t *diag_device(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *diag_langid(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *diag_manufacturer(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *diag_product(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *diag_configuration(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *diag_interface(USBD_SpeedTypeDef speed, uint16_t *length);

/* The board has one serial number whatever personality it is wearing, so the
 * MSC descriptor's UID-derived string is reused rather than duplicated. */
uint8_t *USBD_MSC_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);

const USBD_DescriptorsTypeDef DIAG_Desc =
{
  diag_device,
  diag_langid,
  diag_manufacturer,
  diag_product,
  USBD_MSC_SerialStrDescriptor,
  diag_configuration,
  diag_interface,
  NULL   /* no BOS descriptor: USB 2.0 device, no capabilities to report */
};

/* Class 0xEF/0x02/0x01 is the Interface Association defined class: it tells
 * the host the two CDC interfaces are one function. */
__ALIGN_BEGIN static const uint8_t diag_device_desc[USB_LEN_DEV_DESC] __ALIGN_END =
{
  0x12, USB_DESC_TYPE_DEVICE, 0x00, 0x02,
  0xEF, 0x02, 0x01,
  USB_MAX_EP0_SIZE,
  LOBYTE(USBD_DIAG_VID), HIBYTE(USBD_DIAG_VID),
  LOBYTE(USBD_DIAG_PID), HIBYTE(USBD_DIAG_PID),
  LOBYTE(FW_VERSION_BCD), HIBYTE(FW_VERSION_BCD),
  USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR, USBD_IDX_SERIAL_STR,
  USBD_MAX_NUM_CONFIGURATION
};

__ALIGN_BEGIN static uint8_t diag_langid_desc[USB_LEN_LANGID_STR_DESC] __ALIGN_END =
{
  USB_LEN_LANGID_STR_DESC, USB_DESC_TYPE_STRING,
  LOBYTE(USBD_DIAG_LANGID_STRING), HIBYTE(USBD_DIAG_LANGID_STRING)
};

/* Shared with the mass-storage personality (see usbd_msc_desc.c). */
extern uint8_t USBD_SharedStrDesc[USBD_MAX_STR_DESC_SIZ];
#define diag_str_desc USBD_SharedStrDesc
USBD_STR_FITS(USBD_DIAG_MANUFACTURER_STRING);
USBD_STR_FITS(USBD_DIAG_PRODUCT_STRING);
USBD_STR_FITS(USBD_DIAG_CONFIGURATION_STRING);
USBD_STR_FITS(USBD_DIAG_INTERFACE_STRING);

static uint8_t *diag_device(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  *length = sizeof(diag_device_desc);
  return (uint8_t *)diag_device_desc;
}

static uint8_t *diag_langid(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  *length = sizeof(diag_langid_desc);
  return diag_langid_desc;
}

static uint8_t *diag_manufacturer(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_DIAG_MANUFACTURER_STRING, diag_str_desc, length);
  return diag_str_desc;
}

static uint8_t *diag_product(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_DIAG_PRODUCT_STRING, diag_str_desc, length);
  return diag_str_desc;
}

static uint8_t *diag_configuration(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_DIAG_CONFIGURATION_STRING, diag_str_desc, length);
  return diag_str_desc;
}

static uint8_t *diag_interface(USBD_SpeedTypeDef speed, uint16_t *length)
{
  UNUSED(speed);
  USBD_GetString((uint8_t *)USBD_DIAG_INTERFACE_STRING, diag_str_desc, length);
  return diag_str_desc;
}

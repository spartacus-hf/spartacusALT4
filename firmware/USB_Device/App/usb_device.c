#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_device.c
  * @version        : v3.0_Cube
  * @brief          : This file implements the USB Device
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/

#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_audio.h"
#include "usbd_audio_if.h"
#include "usbd_msc.h"
#include "usbd_msc_desc.h"
#include "usbd_msc_if.h"
#include "usbd_cdc_min.h"
#include "usbd_diag_desc.h"
#include "usbd_cat.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

extern void Error_Handler(void);
/* USB Device Core handle declaration. */
USBD_HandleTypeDef hUsbDeviceFS;
extern const USBD_DescriptorsTypeDef AUDIO_Desc;
extern const USBD_DescriptorsTypeDef MSC_Desc;
extern const USBD_DescriptorsTypeDef DIAG_Desc;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */
static volatile usb_device_mode_t usb_device_mode = USB_DEVICE_MODE_AUDIO;
static uint8_t usb_device_started = 0U;

static void usb_stop_deinit(void)
{
  if (usb_device_started != 0U) {
    (void)USBD_Stop(&hUsbDeviceFS);
    (void)USBD_DeInit(&hUsbDeviceFS);
    usb_device_started = 0U;
  }
}

static void usb_dp_disconnect_pulse(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();

  g.Pin = GPIO_PIN_12;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_PULLDOWN;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &g);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
  HAL_Delay(80U);
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_12);
  HAL_Delay(20U);
}

static void usb_start_audio(void)
{
  usb_device_mode = USB_DEVICE_MODE_AUDIO;

  if (USBD_Init(&hUsbDeviceFS, (USBD_DescriptorsTypeDef *)&AUDIO_Desc, DEVICE_FS) != USBD_OK) {
    Error_Handler();
  }
  /* The sound card and its two CAT/terminal ports, as QMX (usbd_cat.h). */
  if (USBD_RegisterClass(&hUsbDeviceFS, (USBD_ClassTypeDef *)&USBD_AUDIO_CAT) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_AUDIO_RegisterInterface(&hUsbDeviceFS, (USBD_AUDIO_ItfTypeDef *)&USBD_AUDIO_fops_FS) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_Start(&hUsbDeviceFS) != USBD_OK) {
    Error_Handler();
  }

  usb_device_started = 1U;
}

static void usb_start_msc(void)
{
  usb_device_mode = USB_DEVICE_MODE_MSC;

  if (USBD_Init(&hUsbDeviceFS, (USBD_DescriptorsTypeDef *)&MSC_Desc, DEVICE_FS) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceFS, (USBD_ClassTypeDef *)&USBD_MSC) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_MSC_RegisterStorage(&hUsbDeviceFS, (USBD_StorageTypeDef *)&USBD_MSC_fops_FS) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_Start(&hUsbDeviceFS) != USBD_OK) {
    Error_Handler();
  }

  usb_device_started = 1U;
}

static void usb_start_diag(void)
{
  usb_device_mode = USB_DEVICE_MODE_DIAG;

  if (USBD_Init(&hUsbDeviceFS, (USBD_DescriptorsTypeDef *)&DIAG_Desc, DEVICE_FS) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceFS, (USBD_ClassTypeDef *)&USBD_CDC_MIN) != USBD_OK) {
    Error_Handler();
  }
  if (USBD_Start(&hUsbDeviceFS) != USBD_OK) {
    Error_Handler();
  }

  usb_device_started = 1U;
}

/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * Init USB device Library, add supported class and start the library
  * @retval None
  */
void MX_USB_Device_Init(void)
{
  /* USER CODE BEGIN USB_Device_Init_PreTreatment */

  /* USER CODE END USB_Device_Init_PreTreatment */

  /* Init Device Library, add supported class and start the library. */
  usb_start_audio();
  /* USER CODE BEGIN USB_Device_Init_PostTreatment */

  /* USER CODE END USB_Device_Init_PostTreatment */
}

void MX_USB_MSC_Init(void)
{
  usb_start_msc();
}

void USB_Device_ReEnumerateAudio(void)
{
  usb_stop_deinit();
  usb_dp_disconnect_pulse();
  usb_start_audio();
}

void USB_Device_ReEnumerateMsc(void)
{
  usb_stop_deinit();
  usb_dp_disconnect_pulse();
  usb_start_msc();
}

void USB_Device_ReEnumerateDiag(void)
{
  usb_stop_deinit();
  usb_dp_disconnect_pulse();
  usb_start_diag();
}

usb_device_mode_t USB_DeviceCurrentMode(void)
{
  return usb_device_mode;
}

uint8_t USB_DeviceIsMscActive(void)
{
  return (usb_device_mode == USB_DEVICE_MODE_MSC) ? 1U : 0U;
}

/**
  * @}
  */

/**
  * @}
  */


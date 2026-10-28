/**
  ******************************************************************************
  * @file    usbd_audio.h
  * @author  MCD Application Team
  * @brief   header file for the usbd_audio.c file.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2015 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under Ultimate Liberty license
  * SLA0044, the "License"; You may not use this file except in compliance with
  * the License. You may obtain a copy of the License at:
  *                      www.st.com/SLA0044
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USB_AUDIO_H
#define __USB_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include  "usbd_ioreq.h"

/** @addtogroup STM32_USB_DEVICE_LIBRARY
  * @{
  */

/** @defgroup USBD_AUDIO
  * @brief This file is the Header file for usbd_audio.c
  * @{
  */


/** @defgroup USBD_AUDIO_Exported_Defines
  * @{
  */
#ifndef USBD_AUDIO_FREQ
/* AUDIO Class Config */
#define USBD_AUDIO_FREQ                               48000U
#endif /* USBD_AUDIO_FREQ */

#ifndef USBD_MAX_NUM_INTERFACES
#define USBD_MAX_NUM_INTERFACES                       3U
#endif /* USBD_AUDIO_FREQ */

#ifndef AUDIO_HS_BINTERVAL
#define AUDIO_HS_BINTERVAL                            0x01U
#endif /* AUDIO_HS_BINTERVAL */

#ifndef AUDIO_FS_BINTERVAL
#define AUDIO_FS_BINTERVAL                            0x01U
#endif /* AUDIO_FS_BINTERVAL */

#define AUDIO_OUT_EP                                  0x01U
#define AUDIO_FEEDBACK_EP                             0x82U
#define AUDIO_CAPTURE_EP                              0x83U
#define AUDIO_CAPTURE_PACKET                          196U
#define USB_AUDIO_CONFIG_DESC_SIZ                     0xC1U
#define AUDIO_INTERFACE_DESC_SIZE                     0x09U
#define USB_AUDIO_DESC_SIZ                            0x0AU
#define AUDIO_STANDARD_ENDPOINT_DESC_SIZE             0x09U
#define AUDIO_STREAMING_ENDPOINT_DESC_SIZE            0x07U

#define AUDIO_DESCRIPTOR_TYPE                         0x21U
#define USB_DEVICE_CLASS_AUDIO                        0x01U
#define AUDIO_SUBCLASS_AUDIOCONTROL                   0x01U
#define AUDIO_SUBCLASS_AUDIOSTREAMING                 0x02U
#define AUDIO_PROTOCOL_UNDEFINED                      0x00U
#define AUDIO_STREAMING_GENERAL                       0x01U
#define AUDIO_STREAMING_FORMAT_TYPE                   0x02U

/* Audio Descriptor Types */
#define AUDIO_INTERFACE_DESCRIPTOR_TYPE               0x24U
#define AUDIO_ENDPOINT_DESCRIPTOR_TYPE                0x25U

/* Audio Control Interface Descriptor Subtypes */
#define AUDIO_CONTROL_HEADER                          0x01U
#define AUDIO_CONTROL_INPUT_TERMINAL                  0x02U
#define AUDIO_CONTROL_OUTPUT_TERMINAL                 0x03U
#define AUDIO_CONTROL_FEATURE_UNIT                    0x06U

#define AUDIO_INPUT_TERMINAL_DESC_SIZE                0x0CU
#define AUDIO_OUTPUT_TERMINAL_DESC_SIZE               0x09U
#define AUDIO_STREAMING_INTERFACE_DESC_SIZE           0x07U

#define AUDIO_CONTROL_MUTE                            0x0001U

#define AUDIO_FORMAT_TYPE_I                           0x01U
#define AUDIO_FORMAT_TYPE_III                         0x03U

#define AUDIO_ENDPOINT_GENERAL                        0x01U

#define AUDIO_REQ_GET_CUR                             0x81U
#define AUDIO_REQ_SET_CUR                             0x01U

#define AUDIO_OUT_STREAMING_CTRL                      0x02U

#define AUDIO_OUT_TC                                  0x01U
#define AUDIO_IN_TC                                   0x02U


#define AUDIO_OUT_CHANNELS                            2U
#define AUDIO_OUT_BYTES_PER_SAMPLE                    2U
#define AUDIO_OUT_BYTES_PER_FRAME                     (AUDIO_OUT_CHANNELS * AUDIO_OUT_BYTES_PER_SAMPLE)
#define AUDIO_OUT_NOMINAL_PACKET                      (uint16_t)(((USBD_AUDIO_FREQ * AUDIO_OUT_BYTES_PER_FRAME) / 1000U))
#define AUDIO_OUT_PACKET                              (uint16_t)(AUDIO_OUT_NOMINAL_PACKET + AUDIO_OUT_BYTES_PER_FRAME)
#define AUDIO_FEEDBACK_PACKET                         0x0003U
/* UAC1 4.10.1.1: bRefresh is the exponent of the feedback interval in
 * milliseconds, so the host expects a new value every 2^bRefresh frames and
 * polls the synch endpoint at that rate. This firmware recomputes feedback
 * from the buffer fill every other SOF, so it must advertise bRefresh = 1.
 *
 * It previously advertised 9 -- 512 ms -- while the period macro computed
 * 1 << (10 - 9) = 2 SOFs, which inverted the spec's meaning. Linux reads
 * bRefresh straight out of the descriptor and polled the endpoint every
 * 512 ms, so a queued feedback packet sat uncollected and the busy watchdog
 * below reported it as a feedback transmit failure on the first stream open. */
#define AUDIO_FEEDBACK_REFRESH                        1U
#define AUDIO_FEEDBACK_REFRESH_SOF_PERIOD             (1U << AUDIO_FEEDBACK_REFRESH)
/* Four refresh periods of grace before a queued packet is dropped as stale,
 * derived from the cadence so the two cannot drift apart again. */
#define AUDIO_FEEDBACK_BUSY_CLEAR_SOF                 (4U * AUDIO_FEEDBACK_REFRESH_SOF_PERIOD)
#define AUDIO_DEFAULT_VOLUME                          70U
#define AUDIO_RX_BUFFER_SIZE                          AUDIO_OUT_PACKET

#define USBD_AUDIO_EVENT_ZERO_PACKET                  0U
#define USBD_AUDIO_EVENT_OUT_INCOMPLETE               1U
#define USBD_AUDIO_EVENT_REARM_FAILED                 2U
#define USBD_AUDIO_EVENT_FEEDBACK_TX_FAILED           3U
#define USBD_AUDIO_EVENT_STREAM_ACTIVE                4U
#define USBD_AUDIO_EVENT_STREAM_INACTIVE              5U

/* Number of sub-packets in the audio transfer buffer. You can modify this value but always make sure
  that it is an even number and higher than 3 */
#define AUDIO_OUT_PACKET_NUM                          80U
/* Total size of the audio transfer buffer */
#define AUDIO_TOTAL_BUF_SIZE                          ((uint16_t)(AUDIO_OUT_PACKET * AUDIO_OUT_PACKET_NUM))

/* Audio Commands enumeration */
typedef enum
{
  AUDIO_CMD_START = 1,
  AUDIO_CMD_PLAY,
  AUDIO_CMD_STOP,
} AUDIO_CMD_TypeDef;


typedef enum
{
  AUDIO_OFFSET_NONE = 0,
  AUDIO_OFFSET_HALF,
  AUDIO_OFFSET_FULL,
  AUDIO_OFFSET_UNKNOWN,
} AUDIO_OffsetTypeDef;
/**
  * @}
  */


/** @defgroup USBD_CORE_Exported_TypesDefinitions
  * @{
  */
typedef struct
{
  uint8_t cmd;
  uint8_t data[USB_MAX_EP0_SIZE];
  uint8_t len;
  uint8_t unit;
} USBD_AUDIO_ControlTypeDef;


typedef struct
{
  uint32_t alt_setting;
  int16_t capture_data[AUDIO_CAPTURE_PACKET / 2U];
  uint8_t capture_alt, capture_busy, capture_age;
  uint8_t buffer[2][AUDIO_RX_BUFFER_SIZE];
  uint8_t feedback_data[AUDIO_FEEDBACK_PACKET];
  AUDIO_OffsetTypeDef offset;
  uint8_t rd_enable;
  uint8_t feedback_busy;
  uint8_t feedback_busy_sofs;
  uint8_t feedback_refresh_sofs;
  uint8_t rx_buffer_idx;
  uint8_t rx_rearm_pending;
  uint8_t rx_rearm_attempts;
  uint16_t rd_ptr;
  uint16_t wr_ptr;
  USBD_AUDIO_ControlTypeDef control;
} USBD_AUDIO_HandleTypeDef;


typedef struct
{
  int8_t (*Init)(uint32_t AudioFreq, uint32_t Volume, uint32_t options);
  int8_t (*DeInit)(uint32_t options);
  int8_t (*AudioCmd)(uint8_t *pbuf, uint32_t size, uint8_t cmd);
  int8_t (*VolumeCtl)(uint8_t vol);
  int8_t (*MuteCtl)(uint8_t cmd);
  int8_t (*PeriodicTC)(uint8_t *pbuf, uint32_t size, uint8_t cmd);
  int8_t (*GetState)(void);
  uint32_t (*GetFeedback10_14)(void);
  void (*Notify)(uint8_t event, uint32_t value);
  /* max_frames == 0 resets capture; otherwise returns up to max_frames. */
  uint16_t (*Capture)(int16_t *stereo, uint16_t max_frames);
} USBD_AUDIO_ItfTypeDef;
/**
  * @}
  */



/** @defgroup USBD_CORE_Exported_Macros
  * @{
  */

/**
  * @}
  */

/** @defgroup USBD_CORE_Exported_Variables
  * @{
  */

#define AUDIO_SAMPLE_FREQ(frq)         (uint8_t)(frq), (uint8_t)((frq >> 8)), (uint8_t)((frq >> 16))
#define AUDIO_PACKET_SZE(bytes)        (uint8_t)((bytes) & 0xFFU), \
                                       (uint8_t)(((bytes) >> 8) & 0xFFU)

/* The audio function's interfaces as they follow the configuration header:
 * the bare audio class (usbd_audio.c) and the composite with the CAT ports
 * (usbd_cat.c) both put them in their descriptors, which are constant and
 * live in flash. USB_AUDIO_CONFIG_DESC_SIZ is the 9-byte header plus these. */
#define USBD_AUDIO_FUNCTION_DESC \
 \
  /* USB Speaker Standard interface descriptor */ \
  AUDIO_INTERFACE_DESC_SIZE,            /* bLength */ \
  USB_DESC_TYPE_INTERFACE,              /* bDescriptorType */ \
  0x00,                                 /* bInterfaceNumber */ \
  0x00,                                 /* bAlternateSetting */ \
  0x00,                                 /* bNumEndpoints */ \
  USB_DEVICE_CLASS_AUDIO,               /* bInterfaceClass */ \
  AUDIO_SUBCLASS_AUDIOCONTROL,          /* bInterfaceSubClass */ \
  AUDIO_PROTOCOL_UNDEFINED,             /* bInterfaceProtocol */ \
  0x00,                                 /* iInterface */ \
  /* 09 byte*/ \
 \
  /* USB Speaker Class-specific AC Interface Descriptor */ \
  0x0A,            /* bLength */ \
  AUDIO_INTERFACE_DESCRIPTOR_TYPE,      /* bDescriptorType */ \
  AUDIO_CONTROL_HEADER,                 /* bDescriptorSubtype */ \
  0x00,          /* 1.00 */             /* bcdADC */ \
  0x01, \
  0x3E,                                 /* wTotalLength = 62 */ \
  0x00, \
  0x02,                                 /* bInCollection */ \
  0x01, 0x02,                           /* baInterfaceNr: playback, capture */ \
  /* 09 byte*/ \
 \
  /* USB Speaker Input Terminal Descriptor */ \
  AUDIO_INPUT_TERMINAL_DESC_SIZE,       /* bLength */ \
  AUDIO_INTERFACE_DESCRIPTOR_TYPE,      /* bDescriptorType */ \
  AUDIO_CONTROL_INPUT_TERMINAL,         /* bDescriptorSubtype */ \
  0x01,                                 /* bTerminalID */ \
  0x01,                                 /* wTerminalType AUDIO_TERMINAL_USB_STREAMING */ \
  0x01, \
  0x00,                                 /* bAssocTerminal */ \
  0x02,                                 /* bNrChannels */ \
  0x03,                                 /* wChannelConfig: Left Front + Right Front */ \
  0x00, \
  0x00,                                 /* iChannelNames */ \
  0x00,                                 /* iTerminal */ \
  /* 12 byte*/ \
 \
  /* USB Speaker Audio Feature Unit Descriptor */ \
  0x0A,                                 /* bLength */ \
  AUDIO_INTERFACE_DESCRIPTOR_TYPE,      /* bDescriptorType */ \
  AUDIO_CONTROL_FEATURE_UNIT,           /* bDescriptorSubtype */ \
  AUDIO_OUT_STREAMING_CTRL,             /* bUnitID */ \
  0x01,                                 /* bSourceID */ \
  0x01,                                 /* bControlSize */ \
  AUDIO_CONTROL_MUTE,                   /* bmaControls(0) master */ \
  0x00,                                 /* bmaControls(1) left */ \
  0x00,                                 /* bmaControls(2) right */ \
  0x00,                                 /* iTerminal */ \
  /* 10 byte*/ \
 \
  /*USB Speaker Output Terminal Descriptor */ \
  0x09,      /* bLength */ \
  AUDIO_INTERFACE_DESCRIPTOR_TYPE,      /* bDescriptorType */ \
  AUDIO_CONTROL_OUTPUT_TERMINAL,        /* bDescriptorSubtype */ \
  0x03,                                 /* bTerminalID */ \
  0x01,                                 /* wTerminalType  0x0301*/ \
  0x03, \
  0x00,                                 /* bAssocTerminal */ \
  0x02,                                 /* bSourceID */ \
  0x00,                                 /* iTerminal */ \
  /* 09 byte*/ \
 \
  /* REV1 receiver input terminal 4 (radio receiver, stereo I/Q or demodulated). */ \
  12, 0x24, 2, 4, 0x10, 0x07, 0, 2, 3, 0, 0, 0, \
  /* USB capture output terminal 5, sourced from terminal 4. */ \
  9, 0x24, 3, 5, 1, 1, 0, 4, 0, \
 \
  /* USB Speaker Standard AS Interface Descriptor - Audio Streaming Zero Bandwidth */ \
  /* Interface 1, Alternate Setting 0                                             */ \
  AUDIO_INTERFACE_DESC_SIZE,            /* bLength */ \
  USB_DESC_TYPE_INTERFACE,              /* bDescriptorType */ \
  0x01,                                 /* bInterfaceNumber */ \
  0x00,                                 /* bAlternateSetting */ \
  0x00,                                 /* bNumEndpoints */ \
  USB_DEVICE_CLASS_AUDIO,               /* bInterfaceClass */ \
  AUDIO_SUBCLASS_AUDIOSTREAMING,        /* bInterfaceSubClass */ \
  AUDIO_PROTOCOL_UNDEFINED,             /* bInterfaceProtocol */ \
  0x00,                                 /* iInterface */ \
  /* 09 byte*/ \
 \
  /* USB Speaker Standard AS Interface Descriptor - Audio Streaming Operational */ \
  /* Interface 1, Alternate Setting 1                                           */ \
  AUDIO_INTERFACE_DESC_SIZE,            /* bLength */ \
  USB_DESC_TYPE_INTERFACE,              /* bDescriptorType */ \
  0x01,                                 /* bInterfaceNumber */ \
  0x01,                                 /* bAlternateSetting */ \
  0x02,                                 /* bNumEndpoints */ \
  USB_DEVICE_CLASS_AUDIO,               /* bInterfaceClass */ \
  AUDIO_SUBCLASS_AUDIOSTREAMING,        /* bInterfaceSubClass */ \
  AUDIO_PROTOCOL_UNDEFINED,             /* bInterfaceProtocol */ \
  0x00,                                 /* iInterface */ \
  /* 09 byte*/ \
 \
  /* USB Speaker Audio Streaming Interface Descriptor */ \
  AUDIO_STREAMING_INTERFACE_DESC_SIZE,  /* bLength */ \
  AUDIO_INTERFACE_DESCRIPTOR_TYPE,      /* bDescriptorType */ \
  AUDIO_STREAMING_GENERAL,              /* bDescriptorSubtype */ \
  0x01,                                 /* bTerminalLink */ \
  0x01,                                 /* bDelay */ \
  0x01,                                 /* wFormatTag AUDIO_FORMAT_PCM  0x0001 */ \
  0x00, \
  /* 07 byte*/ \
 \
  /* USB Speaker Audio Type III Format Interface Descriptor */ \
  0x0B,                                 /* bLength */ \
  AUDIO_INTERFACE_DESCRIPTOR_TYPE,      /* bDescriptorType */ \
  AUDIO_STREAMING_FORMAT_TYPE,          /* bDescriptorSubtype */ \
  AUDIO_FORMAT_TYPE_I,                  /* bFormatType */ \
  0x02,                                 /* bNrChannels */ \
  0x02,                                 /* bSubFrameSize :  2 Bytes per frame (16bits) */ \
  16,                                   /* bBitResolution (16-bits per sample) */ \
  0x01,                                 /* bSamFreqType only one frequency supported */ \
  AUDIO_SAMPLE_FREQ(USBD_AUDIO_FREQ),   /* Audio sampling frequency coded on 3 bytes */ \
  /* 11 byte*/ \
 \
  /* Endpoint 1 - Standard Descriptor */ \
  AUDIO_STANDARD_ENDPOINT_DESC_SIZE,    /* bLength */ \
  USB_DESC_TYPE_ENDPOINT,               /* bDescriptorType */ \
  AUDIO_OUT_EP,                         /* bEndpointAddress 1 out endpoint */ \
  0x05,                                 /* bmAttributes: Isochronous, Asynchronous */ \
  AUDIO_PACKET_SZE(AUDIO_OUT_PACKET),   /* wMaxPacketSize in Bytes: nominal 48 plus async feedback headroom */ \
  AUDIO_FS_BINTERVAL,                   /* bInterval */ \
  0x00,                                 /* bRefresh */ \
  AUDIO_FEEDBACK_EP,                    /* bSynchAddress */ \
  /* 09 byte*/ \
 \
  /* Endpoint - Audio Streaming Descriptor*/ \
  AUDIO_STREAMING_ENDPOINT_DESC_SIZE,   /* bLength */ \
  AUDIO_ENDPOINT_DESCRIPTOR_TYPE,       /* bDescriptorType */ \
  AUDIO_ENDPOINT_GENERAL,               /* bDescriptor */ \
  0x00,                                 /* bmAttributes */ \
  0x00,                                 /* bLockDelayUnits */ \
  0x00,                                 /* wLockDelay */ \
  0x00, \
  /* 07 byte*/ \
 \
  /* Explicit feedback endpoint (UAC1 asynchronous sink, 10.14 format) */ \
  AUDIO_STANDARD_ENDPOINT_DESC_SIZE,    /* bLength */ \
  USB_DESC_TYPE_ENDPOINT,               /* bDescriptorType */ \
  AUDIO_FEEDBACK_EP,                    /* bEndpointAddress */ \
  0x11,                                 /* bmAttributes: Isochronous, Feedback */ \
  LOBYTE(AUDIO_FEEDBACK_PACKET),        /* wMaxPacketSize */ \
  HIBYTE(AUDIO_FEEDBACK_PACKET), \
  AUDIO_FS_BINTERVAL,                   /* bInterval */ \
  AUDIO_FEEDBACK_REFRESH,               /* bRefresh */ \
  0x00,                                 /* bSynchAddress */ \
 \
  /* Interface 2: capture alt 0 and stereo 16-bit 48 kHz alt 1. */ \
  9, 4, 2, 0, 0, 1, 2, 0, 0, \
  9, 4, 2, 1, 1, 1, 2, 0, 0, \
  7, 0x24, 1, 5, 1, 1, 0, \
  11, 0x24, 2, 1, 2, 2, 16, 1, AUDIO_SAMPLE_FREQ(USBD_AUDIO_FREQ), \
  9, 5, AUDIO_CAPTURE_EP, 0x05, AUDIO_PACKET_SZE(AUDIO_CAPTURE_PACKET), 1, 0, 0, \
  7, 0x25, 1, 0, 0, 0, 0

extern USBD_ClassTypeDef USBD_AUDIO;
/* The class callbacks, for the composite device (usbd_cat.c) to call
 * directly: no function pointers on the USB interrupt's paths, so the
 * real-time and stack analyses see every call. */
uint8_t USBD_AUDIO_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
uint8_t USBD_AUDIO_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
uint8_t USBD_AUDIO_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
uint8_t USBD_AUDIO_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum);
uint8_t USBD_AUDIO_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum);
uint8_t USBD_AUDIO_EP0_RxReady(USBD_HandleTypeDef *pdev);
uint8_t USBD_AUDIO_EP0_TxReady(USBD_HandleTypeDef *pdev);
uint8_t USBD_AUDIO_SOF(USBD_HandleTypeDef *pdev);
uint8_t USBD_AUDIO_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum);
uint8_t USBD_AUDIO_IsoOutIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum);
uint8_t *USBD_AUDIO_GetCfgDesc(uint16_t *length);
uint8_t *USBD_AUDIO_GetDeviceQualifierDesc(uint16_t *length);
#define USBD_AUDIO_CLASS &USBD_AUDIO

typedef struct
{
  uint32_t sofs;
  uint32_t submits;
  uint32_t completions;
  uint32_t incomplete;
  uint32_t submit_failures;
  uint32_t submitted_frames;
} USBD_AUDIO_CaptureStatsTypeDef;
/**
  * @}
  */

/** @defgroup USB_CORE_Exported_Functions
  * @{
  */
uint8_t USBD_AUDIO_RegisterInterface(USBD_HandleTypeDef *pdev,
                                     USBD_AUDIO_ItfTypeDef *fops);

void USBD_AUDIO_Sync(USBD_HandleTypeDef *pdev, AUDIO_OffsetTypeDef offset);
void USBD_AUDIO_GetCaptureStats(USBD_AUDIO_CaptureStatsTypeDef *out);
/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif  /* __USB_AUDIO_H */
/**
  * @}
  */

/**
  * @}
  */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/

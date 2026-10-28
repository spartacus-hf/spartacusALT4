/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_HRTIM_MspPostInit(HRTIM_HandleTypeDef *hhrtim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);
void APP_WatchdogProgress(void);
extern volatile uint32_t g_error_handler_count;

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/


/* USER CODE BEGIN Private defines */

#include "board_rev1.h"

#define LCD_USE_CS_GPIO 1
#define LCD_USE_BL_GPIO 0
#define TFT_CS_Pin GPIO_PIN_11
#define TFT_CS_GPIO_Port GPIOE
#define TFT_SCK_Pin GPIO_PIN_12
#define TFT_SCK_GPIO_Port GPIOE
#define TFT_MOSI_Pin GPIO_PIN_14
#define TFT_MOSI_GPIO_Port GPIOE
#define TFT_SPI_AF GPIO_AF5_SPI4
#define TFT_DC_Pin GPIO_PIN_13
#define TFT_DC_GPIO_Port GPIOE
#define TFT_RST_Pin GPIO_PIN_10
#define TFT_RST_GPIO_Port GPIOE
#define SD_CS_Pin GPIO_PIN_4
#define SD_CS_GPIO_Port GPIOA
#define SD_SCK_Pin GPIO_PIN_5
#define SD_SCK_GPIO_Port GPIOA
#define SD_MISO_Pin GPIO_PIN_6
#define SD_MISO_GPIO_Port GPIOA
#define SD_MOSI_Pin GPIO_PIN_7
#define SD_MOSI_GPIO_Port GPIOA
#define SD_SPI_AF GPIO_AF5_SPI1
#define BTN_UP_Pin GPIO_PIN_4
#define BTN_UP_GPIO_Port GPIOD
#define BTN_DN_Pin GPIO_PIN_2
#define BTN_DN_GPIO_Port GPIOD
#define BTN_SEL_Pin GPIO_PIN_3
#define BTN_SEL_GPIO_Port GPIOD
#define LED_Pin GPIO_PIN_7
#define LED_GPIO_Port GPIOB
#define CPU_SCOPE_Pin LED_Pin
#define CPU_SCOPE_GPIO_Port LED_GPIO_Port
#define CW_DIT_Pin GPIO_PIN_0
#define CW_DIT_GPIO_Port GPIOE
#define CW_DAH_Pin GPIO_PIN_1
#define CW_DAH_GPIO_Port GPIOE

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

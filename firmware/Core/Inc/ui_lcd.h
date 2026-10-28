#ifndef UI_LCD_H
#define UI_LCD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

/* ST7789 240x320 panel driven in landscape (320x240). The frame itself is
 * ui_gfx.c; this module streams it to the panel in low-priority SPI4 DMA
 * stripes, only for 8-row pages whose pixels or colours changed, and never
 * while the RF/audio path needs the CPU. */

typedef void (*lcdui_build_fn_t)(void *ctx, uint32_t now);

void LCDUI_Init(DMA_HandleTypeDef *hdma_spi_tx);
/* DMA1 channel 3 (the panel's SPI4 TX): from its interrupt handler. */
void LCDUI_DmaIrq(void);
/* Boot log: blocking, used before the main loop and by Error_Handler. */
void LCDUI_BootBegin(void);
void LCDUI_BootStep(const char *name, uint8_t status, const char *detail);
/* Worst boot status so far: 1 every stage passed, 2 a warning, 0 a failure. */
uint8_t LCDUI_BootWorst(void);
/* Milliseconds the last frame took from its build to its last page. */
uint16_t LCDUI_FlushMs(void);
/* 0 = landscape 90 degrees, 1 = 270 (panel mounted the other way round).
 * Applied at once when the SPI is idle, otherwise at the next service;
 * either way the whole frame is sent again. */
void LCDUI_SetRotation(uint8_t rot270);
uint8_t LCDUI_Rotation(void);
/* While the RF path holds LCDUI_Service off (CW keying), the frame may
 * still change in place -- a badge, no build -- and go out page by page as
 * usual: LCDUI_LightReady says the framebuffer is not being sent, and
 * LCDUI_LightFlush (changed = it was drawn on) queues what differs and sends
 * the next stripe. A few tens of microseconds of CPU a stripe. */
uint8_t LCDUI_LightReady(void);
void LCDUI_LightFlush(uint8_t changed);
/* The next frame answers the operator (a detent, a key): build it at the
 * next page boundary instead of after the whole frame being sent. */
void LCDUI_Preempt(void);
void LCDUI_Service(uint32_t now,
                   uint8_t *ui_dirty,
                   uint16_t idle_refresh_ms,
                   uint16_t busy_refresh_ms,
                   lcdui_build_fn_t build_fn,
                   void *ctx);

#ifdef __cplusplus
}
#endif

#endif

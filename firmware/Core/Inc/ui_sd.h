/**
 * @file    ui_sd.h
 * @brief   SD browser, viewer, file-operation, and local-player UI controller.
 */
#ifndef UI_SD_H
#define UI_SD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void UI_SDInit(void);
void UI_SDEnter(void);
void UI_SDExit(void);
void UI_SDService(uint32_t now);
void UI_SDHandle(uint8_t up, uint8_t down, uint8_t select,
                 uint8_t up_long, uint8_t down_long, uint8_t select_long);
void UI_SDRender(void);
uint8_t UI_SDPlayerPageActive(void);
/* 1 on the SD hub, where a long press leaves the SD pages. */
uint8_t UI_SDAtHub(void);
/* The FUNC key's back: never confirms; 0 on the hub. */
uint8_t UI_SDBack(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_SD_H */

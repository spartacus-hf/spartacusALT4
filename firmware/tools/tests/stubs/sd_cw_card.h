#ifndef SD_CW_CARD_H
#define SD_CW_CARD_H
/* The host suites' SD card for the CW pack (sd_cw_card.c). */
#include "mod_sdcard_service.h"
#include <stdint.h>

typedef struct {
    uint8_t *pack;          /* CWPACK.BIN as on the card, editable */
    uint32_t pack_len;      /* 0: no such file */
    const char *custom;     /* CW/CUSTOM.TXT, NULL: none */
    uint8_t mounted, io;    /* a card; I/O allowed now */
    uint32_t opens, hashes; /* file opens, whole-file checks */
} sd_cw_card_t;

extern sd_cw_card_t sd_cw_card;
/* The tree's sd/CWPACK.BIN on a mounted card, I/O allowed. */
void sd_cw_card_reset(void);

#endif

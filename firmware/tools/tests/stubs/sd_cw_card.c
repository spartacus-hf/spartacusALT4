/* A card for the CW pack and trainer suites: CWPACK.BIN is sd/CWPACK.BIN
 * from the tree (tools/gen_cw_pack.py writes it), held in memory so a suite
 * can damage it, cut it short or take the card away, and CW/CUSTOM.TXT is
 * whatever the suite puts there. The SD service calls behave as the
 * radio's: nothing while the card is gone or I/O is not allowed. */
#include "sd_cw_card.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CWPACK_BIN_PATH
#error "CWPACK_BIN_PATH names sd/CWPACK.BIN"
#endif

sd_cw_card_t sd_cw_card;
static uint8_t pack_bytes[256U * 1024U];

void sd_cw_card_reset(void)
{
    FILE *f = fopen(CWPACK_BIN_PATH, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", CWPACK_BIN_PATH);
        exit(2);
    }
    memset(&sd_cw_card, 0, sizeof(sd_cw_card));
    sd_cw_card.pack = pack_bytes;
    sd_cw_card.pack_len = (uint32_t)fread(pack_bytes, 1U, sizeof(pack_bytes), f);
    fclose(f);
    sd_cw_card.mounted = 1U;
    sd_cw_card.io = 1U;
}

static const uint8_t *file_of(const char *path, uint32_t *len)
{
    if (!sd_cw_card.mounted || !sd_cw_card.io) return NULL;
    if (strcmp(path, "CWPACK.BIN") == 0 && sd_cw_card.pack_len) {
        *len = sd_cw_card.pack_len;
        return sd_cw_card.pack;
    }
    if (strcmp(path, "CW/CUSTOM.TXT") == 0 && sd_cw_card.custom) {
        *len = (uint32_t)strlen(sd_cw_card.custom);
        return (const uint8_t *)sd_cw_card.custom;
    }
    return NULL;
}

uint8_t SD_IsMounted(void) { return sd_cw_card.mounted; }
uint8_t SD_DiagIoAllowed(void) { return (uint8_t)(sd_cw_card.mounted && sd_cw_card.io); }

uint8_t SD_DiagStat(const char *path, uint32_t *size, uint8_t *attr)
{
    uint32_t len = 0U;
    if (!file_of(path, &len)) return 0U;
    if (size) *size = len;
    if (attr) *attr = 0U;
    return 1U;
}

uint8_t SD_DiagReadFile(const char *path, uint32_t offset, void *data, uint32_t len, uint32_t *out_len)
{
    uint32_t size = 0U;
    const uint8_t *f = file_of(path, &size);
    if (out_len) *out_len = 0U;
    if (!f) return 0U;
    ++sd_cw_card.opens;
    const uint32_t n = offset >= size ? 0U : (size - offset < len ? size - offset : len);
    memcpy(data, f + offset, n);
    if (out_len) *out_len = n;
    return 1U;
}

uint8_t SD_DiagFileFnv1a(const char *path, uint32_t offset, uint32_t *hash)
{
    uint32_t size = 0U, h = 0x811C9DC5UL;
    const uint8_t *f = file_of(path, &size);
    if (!f) return 0U;
    ++sd_cw_card.opens;
    ++sd_cw_card.hashes;
    for (uint32_t i = offset; i < size; ++i) h = (h ^ f[i]) * 0x01000193UL;
    *hash = h;
    return 1U;
}

uint8_t SD_DiagReadEach(const char *path, uint32_t offset, uint32_t len, sd_chunk_fn each, void *ctx)
{
    uint32_t size = 0U;
    const uint8_t *f = file_of(path, &size);
    if (!f) return 0U;
    ++sd_cw_card.opens;
    while (len) {
        if (offset >= size) return 0U;
        uint32_t n = len < 64U ? len : 64U;
        if (n > size - offset) n = size - offset;
        each(f + offset, n, ctx);
        offset += n;
        len -= n;
    }
    return 1U;
}

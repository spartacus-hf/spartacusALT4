/* A card for the host suites that read the settings help: SD_DiagReadFile
 * serves sd/HELP.BIN from the tree (tools/gen_help_text.py writes it), as the
 * radio reads it from the SD card since v8.08. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef HELP_BIN_PATH
#error "HELP_BIN_PATH names sd/HELP.BIN"
#endif

__attribute__((weak)) uint32_t HAL_GetTick(void) { static uint32_t t; return t += 10U; }
uint8_t SD_IsMounted(void) { return 1U; }
/* Set: the card is not read now (transmitting or streaming, on the radio). */
uint8_t sd_help_card_busy;
uint8_t MOD_BlockingIoAllowed(uint32_t now) { (void)now; return (uint8_t)!sd_help_card_busy; }

/* The radio memories' file (UI_CwMemoryText): CR LF lines, a blank one. */
const char sd_help_card_memories[] = "CQ CQ DE TEST K\r\n\r\nTEST <SK>\n";

uint8_t SD_DiagReadFile(const char *path, uint32_t offset, void *data, uint32_t len, uint32_t *out_len)
{
    if (out_len) *out_len = 0U;
    if (strcmp(path, "CW/MEMORIES.TXT") == 0) {
        const uint32_t size = (uint32_t)sizeof(sd_help_card_memories) - 1U;
        const uint32_t n = offset < size ? (size - offset < len ? size - offset : len) : 0U;
        memcpy(data, sd_help_card_memories + (offset < size ? offset : size), n);
        if (out_len) *out_len = n;
        return 1U;
    }
    if (strcmp(path, "HELP.BIN") != 0) return 0U;
    FILE *f = fopen(HELP_BIN_PATH, "rb");
    if (!f) return 0U;
    size_t got = 0U;
    if (fseek(f, (long)offset, SEEK_SET) == 0) got = fread(data, 1U, len, f);
    fclose(f);
    if (out_len) *out_len = (uint32_t)got;
    return 1U;
}

uint8_t SD_DiagReadSpans(const char *path, uint32_t off_a, void *a, uint32_t len_a,
                         uint32_t off_b, void *b, uint32_t len_b, uint32_t *got_b)
{
    uint32_t got = 0U;
    if (got_b) *got_b = 0U;
    if (!SD_DiagReadFile(path, off_a, a, len_a, &got) || got != len_a) return 0U;
    return SD_DiagReadFile(path, off_b, b, len_b, got_b);
}

/* FNV-1a of the file from `offset` on, as the firmware checks HELP.BIN. */
uint8_t SD_DiagFileFnv1a(const char *path, uint32_t offset, uint32_t *hash)
{
    if (strcmp(path, "HELP.BIN") != 0) return 0U;
    FILE *f = fopen(HELP_BIN_PATH, "rb");
    if (!f) return 0U;
    uint32_t h = 0x811C9DC5UL;
    int c;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return 0U; }
    while ((c = fgetc(f)) != EOF) h = (h ^ (uint32_t)c) * 0x01000193UL;
    fclose(f);
    *hash = h;
    return 1U;
}

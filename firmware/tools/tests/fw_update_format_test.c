#include "fw_update_format.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t reference_crc(const uint8_t *bytes, uint32_t count)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < count; i++) {
        crc ^= bytes[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
        }
    }
    return ~crc;
}

int main(void)
{
    assert(fw_crc32("123456789", 9U) == 0xCBF43926UL);
    uint8_t sample[1031];
    for (uint32_t i = 0U; i < sizeof(sample); i++) {
        sample[i] = (uint8_t)((i * 73U + (i >> 2U)) & 255U);
    }
    for (uint32_t count = 0U; count <= sizeof(sample); count++) {
        assert(fw_crc32(sample, count) == reference_crc(sample, count));
        uint32_t split = count / 3U;
        uint32_t crc = fw_crc32_continue(0xFFFFFFFFUL, sample, split);
        crc = fw_crc32_continue(crc, sample + split, count - split);
        assert(~crc == reference_crc(sample, count));
    }
    assert(FW_APP_BASE == FW_LOADER_BASE + FW_LOADER_BYTES);
    assert(FW_FOOTER_ADDR + sizeof(fw_image_footer_t) == FW_JOURNAL_ADDR);
    assert((FW_JOURNAL_CONFIRM_ADDR & 7U) == 0U);
    assert(FW_JOURNAL_CONFIRM_ADDR + 8U <= FW_JOURNAL_ADDR + FW_PAGE_BYTES);

    fw_package_header_t header = {0};
    header.magic = FW_PACKAGE_MAGIC;
    header.board_id = FW_BOARD_ID;
    header.image_base = FW_APP_BASE;
    header.image_bytes = 4096U;
    uint32_t crc = fw_crc32(&header, sizeof(header));
    header.header_crc32 = crc;
    header.header_crc32 = 0U;
    assert(fw_crc32(&header, sizeof(header)) == crc);
    header.board_id ^= 1U;
    assert(fw_crc32(&header, sizeof(header)) != crc);

    /* Layout 2 (v8.09): the application runs to the footer at the end of
     * bank-2 page 126, the journal is the last page, nothing after it. */
    assert(FW_JOURNAL_ADDR + FW_PAGE_BYTES == FW_FLASH_END);
    assert((FW_JOURNAL_ADDR % FW_PAGE_BYTES) == 0U);
    assert(FW_FOOTER_ADDR - FW_APP_BASE == 518112UL);
    assert(FW_FLASH_END - FW_LOADER_BASE == 512UL * 1024UL);
    assert((FW_JOURNAL_ADDR - 0x08040000UL) / FW_PAGE_BYTES == FW_BANK2_LAST_PAGE);
    /* Packages: layout 1's (ABI 1) refused, this layout's taken, a later
     * loader's refused. */
    assert(FW_LAYOUT_ABI == 2UL && FW_LOADER_ABI >= FW_LAYOUT_ABI);
    assert(!fw_abi_fits_layout(0UL));
    assert(!fw_abi_fits_layout(1UL));
    assert(fw_abi_fits_layout(FW_LAYOUT_ABI));
    assert(fw_abi_fits_layout(FW_LOADER_ABI));
    assert(!fw_abi_fits_layout(FW_LOADER_ABI + 1UL));

    assert(fw_wrp_overlaps(0x00010000UL, 0U, 1U));
    assert(!fw_wrp_overlaps(0x00010000UL, 2U, 127U));
    assert(!fw_wrp_overlaps(0x0000007FUL, 0U, 127U));
    /* A window over layout 1's settings pages (bank 2, 106..127) now covers
     * application and journal pages: the loader refuses to run with it. */
    assert(fw_wrp_overlaps(0x007F006AUL, 0U, FW_BANK2_LAST_PAGE));
    assert(!fw_wrp_overlaps(0x007F006AUL, 0U, 105U));
    puts("firmware format, layout 2, ABI, CRC and WRP range checks passed");
    return 0;
}

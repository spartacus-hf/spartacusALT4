#include "stm32g4xx.h"
#include "fw_update_format.h"
#include <stddef.h>

#define GPIO_PIN_4  (1UL << 4U)
#define GPIO_PIN_6  (1UL << 6U)
#define GPIO_PIN_7  (1UL << 7U)
#define GPIO_PIN_8  (1UL << 8U)
#define GPIO_PIN_9  (1UL << 9U)
#define GPIO_PIN_10 (1UL << 10U)
#define GPIO_PIN_12 (1UL << 12U)

#define FLASH_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | \
                      FLASH_SR_PGAERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR | \
                      FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR)
#define FLASH_CLEAR (FLASH_ERRORS | FLASH_SR_EOP)

static uint8_t sector[FW_SECTOR_BYTES];
static uint8_t map_sector[FW_SECTOR_BYTES];
static uint32_t mapped_sector = 0xFFFFFFFFUL;
static uint8_t sd_high_capacity;

__attribute__((used, section(".loader_info")))
const fw_loader_info_t loader_info = {
    FW_LOADER_INFO_MAGIC, FW_LOADER_ABI, FW_APP_BASE, FW_LOADER_BYTES,
    0xDEDD7027UL, {0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL}
};

/* Called before the C runtime. No state or HAL calls are permitted here. */
void loader_early_safe(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN |
                    RCC_AHB2ENR_GPIOCEN | RCC_AHB2ENR_GPIODEN;
    (void)RCC->AHB2ENR;
    GPIOB->BSRR = GPIO_BSRR_BS10;        /* Q13 holds board power */
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 20U)) | (1UL << 20U);
    GPIOA->BRR = GPIO_PIN_8 | GPIO_PIN_9;
    GPIOC->BRR = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9;
    GPIOC->BSRR = GPIO_PIN_6 | GPIO_PIN_10; /* driver disable nodes */
    GPIOD->BRR = GPIO_PIN_12;             /* bias buck off */
    GPIOA->MODER = (GPIOA->MODER & ~(15UL << 16U)) | (5UL << 16U);
    GPIOC->MODER = (GPIOC->MODER & ~((3UL << 12U) | (15UL << 16U) |
                                      (3UL << 20U))) |
                   (1UL << 12U) | (5UL << 16U) | (1UL << 20U);
    GPIOD->MODER = (GPIOD->MODER & ~(3UL << 24U)) | (1UL << 24U);
}

static void watchdog_kick(void)
{
    IWDG->KR = 0xAAAAU;
}

static void watchdog_start(void)
{
    IWDG->KR = 0xCCCCU;
    IWDG->KR = 0x5555U;
    IWDG->PR = 6U;               /* /256, about 20 seconds at nominal LSI */
    IWDG->RLR = 2499U;
    for (uint32_t n = 0U; n < 100000U && IWDG->SR != 0U; n++) { }
    watchdog_kick();
}

static void led_init(void)
{
    GPIOB->BRR = GPIO_PIN_7;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 14U)) | (1UL << 14U);
}

static void wait_blink(uint8_t rapid)
{
    const uint32_t count = rapid ? 500000UL : 3000000UL;
    for (;;) {
        GPIOB->ODR ^= GPIO_PIN_7;
        for (volatile uint32_t n = 0U; n < count; n++) {
            if ((n & 0xFFFFUL) == 0U) watchdog_kick();
        }
    }
}

static uint8_t supply_ok(void)
{
    return ((PWR->SR2 & PWR_SR2_PVDO) == 0U) ? 1U : 0U;
}

static uint8_t flash_wait(void)
{
    uint32_t count = 2000000UL;
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
        if (--count == 0U) return 0U;
    }
    if ((FLASH->SR & FLASH_ERRORS) != 0U) return 0U;
    FLASH->SR = FLASH_CLEAR;
    watchdog_kick();
    return 1U;
}

static uint8_t flash_unlock(void)
{
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) == 0U ||
        (FLASH->OPTR & FLASH_OPTR_BFB2) != 0U) return 0U;
    if ((FLASH->WRP1AR & FLASH_WRP1AR_WRP1A_STRT) != 0U ||
        ((FLASH->WRP1AR & FLASH_WRP1AR_WRP1A_END) >>
         FLASH_WRP1AR_WRP1A_END_Pos) != 1U) return 0U;
    if (fw_wrp_overlaps(FLASH->WRP1BR, 2U, 127U) ||
        fw_wrp_overlaps(FLASH->WRP2AR, 0U, FW_BANK2_LAST_PAGE) ||
        fw_wrp_overlaps(FLASH->WRP2BR, 0U, FW_BANK2_LAST_PAGE)) return 0U;
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = 0x45670123UL;
        FLASH->KEYR = 0xCDEF89ABUL;
    }
    return ((FLASH->CR & FLASH_CR_LOCK) == 0U) ? 1U : 0U;
}

static uint8_t flash_erase_page(uint32_t addr)
{
    uint32_t bank = (addr >= 0x08040000UL) ? FLASH_CR_BKER : 0U;
    uint32_t base = bank ? 0x08040000UL : FW_LOADER_BASE;
    uint32_t page = (addr - base) / FW_PAGE_BYTES;
    if (!supply_ok() || !flash_wait()) return 0U;
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_PNB | FLASH_CR_BKER | FLASH_CR_PG)) |
                FLASH_CR_PER | bank | (page << FLASH_CR_PNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;
    uint8_t ok = flash_wait();
    FLASH->CR &= ~FLASH_CR_PER;
    return ok;
}

static uint8_t flash_write8(uint32_t addr, const uint8_t *bytes)
{
    uint32_t lo = 0U, hi = 0U;
    if (!supply_ok() || !flash_wait()) return 0U;
    for (uint8_t n = 0U; n < 4U; n++) {
        lo |= (uint32_t)bytes[n] << (8U * n);
        hi |= (uint32_t)bytes[n + 4U] << (8U * n);
    }
    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)addr = lo;
    __ISB();
    *(volatile uint32_t *)(addr + 4U) = hi;
    uint8_t ok = flash_wait();
    FLASH->CR &= ~FLASH_CR_PG;
    return (ok && *(volatile uint32_t *)addr == lo &&
            *(volatile uint32_t *)(addr + 4U) == hi) ? 1U : 0U;
}

static uint8_t footer_fields_valid(const fw_image_footer_t *f)
{
    if (f->magic != FW_FOOTER_MAGIC || f->board_id != FW_BOARD_ID ||
        f->image_base != FW_APP_BASE || !fw_abi_fits_layout(f->loader_abi) ||
        f->image_bytes < 512U || f->image_bytes > FW_FOOTER_ADDR - FW_APP_BASE ||
        fw_crc32(f, offsetof(fw_image_footer_t, footer_crc32)) != f->footer_crc32) return 0U;
    return 1U;
}

static uint8_t footer_valid(void)
{
    const fw_image_footer_t *f = (const fw_image_footer_t *)FW_FOOTER_ADDR;
    if (!footer_fields_valid(f)) return 0U;
    uint32_t sp = *(const uint32_t *)FW_APP_BASE;
    uint32_t pc = *(const uint32_t *)(FW_APP_BASE + 4U);
    if (sp < 0x20000000UL || sp > 0x20018000UL || (sp & 7U) != 0U ||
        (pc & 1U) == 0U || (pc & ~1UL) < FW_APP_BASE ||
        (pc & ~1UL) >= FW_APP_BASE + f->image_bytes) return 0U;
    if (fw_crc32((const void *)FW_APP_BASE, f->image_bytes) != f->image_crc32) return 0U;
    return 1U;
}

static uint8_t journal_valid(const fw_journal_t **out)
{
    const fw_journal_t *j = (const fw_journal_t *)FW_JOURNAL_ADDR;
    if (j->commit != FW_JOURNAL_COMMIT || j->magic != FW_JOURNAL_MAGIC ||
        j->format != FW_FORMAT_VERSION || j->board_id != FW_BOARD_ID ||
        j->image_bytes < 512U || j->image_bytes > FW_FOOTER_ADDR - FW_APP_BASE ||
        j->package_sectors != 1U + (j->image_bytes + 511U) / 512U ||
        j->package_sectors > FW_MAP_ENTRIES ||
        fw_crc32(j, offsetof(fw_journal_t, record_crc32)) != j->record_crc32) return 0U;
    *out = j;
    return 1U;
}

static uint8_t spi_transfer(uint8_t output)
{
    uint32_t limit = 100000UL;
    while ((SPI1->SR & SPI_SR_TXE) == 0U && --limit != 0U) { }
    if (limit == 0U) return 0xFFU;
    *(__IO uint8_t *)&SPI1->DR = output;
    limit = 100000UL;
    while ((SPI1->SR & SPI_SR_RXNE) == 0U && --limit != 0U) { }
    return limit ? (uint8_t)SPI1->DR : 0xFFU;
}

static void sd_deselect(void)
{
    GPIOA->BSRR = GPIO_PIN_4;
    (void)spi_transfer(0xFFU);
}

/* Every command carries its real CRC7: the application turns the card's CRC
 * checks on (CMD59), and a card keeps its own power across an MCU reset. */
static uint8_t sd_command(uint8_t cmd, uint32_t arg)
{
    const uint8_t frame[5] = {(uint8_t)(0x40U | cmd), (uint8_t)(arg >> 24U), (uint8_t)(arg >> 16U),
                              (uint8_t)(arg >> 8U), (uint8_t)arg};
    uint8_t crc = 0U;
    GPIOA->BRR = GPIO_PIN_4;
    (void)spi_transfer(0xFFU);
    for (uint8_t i = 0U; i < 5U; i++) {
        uint8_t d = frame[i];
        (void)spi_transfer(d);
        for (uint8_t b = 0U; b < 8U; b++) {
            crc = (uint8_t)(crc << 1U);
            if (((d ^ crc) & 0x80U) != 0U) crc ^= 0x09U;
            d = (uint8_t)(d << 1U);
        }
    }
    (void)spi_transfer((uint8_t)((crc << 1U) | 1U));
    for (uint8_t n = 0U; n < 12U; n++) {
        uint8_t r = spi_transfer(0xFFU);
        if ((r & 0x80U) == 0U) return r;
    }
    return 0xFFU;
}

static void sd_gpio_spi_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    GPIOA->BSRR = GPIO_PIN_4;
    GPIOA->MODER = (GPIOA->MODER & ~(0xFFUL << 8U)) |
                   (1UL << 8U) | (2UL << 10U) | (2UL << 12U) | (2UL << 14U);
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~((15UL << 20U) | (15UL << 24U) |
                                         (15UL << 28U))) |
                    (5UL << 20U) | (5UL << 24U) | (5UL << 28U);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3UL << 12U)) | (1UL << 12U);
    SPI1->CR1 = 0U;
    SPI1->CR2 = SPI_CR2_DS_0 | SPI_CR2_DS_1 | SPI_CR2_DS_2 | SPI_CR2_FRXTH;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM |
                SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0 | SPI_CR1_SPE;
}

static uint8_t sd_init(void)
{
    sd_gpio_spi_init();
    sd_deselect();
    for (uint8_t n = 0U; n < 10U; n++) (void)spi_transfer(0xFFU);
    uint8_t r = 0xFFU;
    for (uint8_t n = 0U; n < 16U && r != 1U; n++) {
        r = sd_command(0U, 0U);
        sd_deselect();
    }
    if (r != 1U) return 0U;
    r = sd_command(8U, 0x1AAU);
    uint8_t v1 = (r == 5U) ? 1U : 0U;
    if (r == 1U) {
        uint8_t bytes[4];
        for (uint8_t n = 0U; n < 4U; n++) bytes[n] = spi_transfer(0xFFU);
        if (bytes[2] != 1U || bytes[3] != 0xAAU) return 0U;
    } else if (!v1) return 0U;
    sd_deselect();
    for (uint16_t tries = 0U; tries < 2000U; tries++) {
        r = sd_command(55U, 0U);
        sd_deselect();
        if (r > 1U) return 0U;
        r = sd_command(41U, v1 ? 0U : 0x40000000UL);
        sd_deselect();
        if (r == 0U) break;
        if (r != 1U || tries == 1999U) return 0U;
        watchdog_kick();
    }
    r = sd_command(58U, 0U);
    if (r != 0U) return 0U;
    uint8_t ocr0 = spi_transfer(0xFFU);
    for (uint8_t n = 0U; n < 3U; n++) (void)spi_transfer(0xFFU);
    sd_high_capacity = (ocr0 & 0x40U) ? 1U : 0U;
    sd_deselect();
    if (!sd_high_capacity) {
        r = sd_command(16U, 512U);
        sd_deselect();
        if (r != 0U) return 0U;
    }
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 = (SPI1->CR1 & ~(SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0)) |
                SPI_CR1_BR_0 | SPI_CR1_SPE; /* HSI16 /4 = 4 MHz */
    return 1U;
}

static uint8_t sd_read(uint32_t lba, uint8_t *dst)
{
    if (!sd_high_capacity && lba > UINT32_MAX / 512U) return 0U;
    uint8_t r = sd_command(17U, sd_high_capacity ? lba : lba * 512U);
    if (r != 0U) { sd_deselect(); return 0U; }
    uint32_t count = 100000UL;
    do {
        r = spi_transfer(0xFFU);
        if (r == 0xFEU) break;
    } while (--count != 0U);
    if (count == 0U) { sd_deselect(); return 0U; }
    for (uint32_t n = 0U; n < 512U; n++) dst[n] = spi_transfer(0xFFU);
    (void)spi_transfer(0xFFU);
    (void)spi_transfer(0xFFU);
    sd_deselect();
    watchdog_kick();
    return 1U;
}

static uint32_t map_entry(const fw_journal_t *j, uint32_t index)
{
    uint32_t part = index / 128U;
    if (index >= j->package_sectors || part >= FW_MAP_SECTORS) return 0U;
    if (mapped_sector != part) {
        if (!sd_read(j->map_lba[part], map_sector)) return 0U;
        mapped_sector = part;
    }
    uint32_t off = (index % 128U) * 4U;
    return (uint32_t)map_sector[off] | ((uint32_t)map_sector[off + 1U] << 8U) |
           ((uint32_t)map_sector[off + 2U] << 16U) |
           ((uint32_t)map_sector[off + 3U] << 24U);
}

static uint8_t package_valid(const fw_journal_t *j)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t n = 0U; n < FW_MAP_SECTORS; n++) {
        if (j->map_lba[n] == 0U || !sd_read(j->map_lba[n], map_sector)) return 0U;
        crc = fw_crc32_continue(crc, map_sector, 512U);
    }
    if (~crc != j->map_crc32) return 0U;
    mapped_sector = 0xFFFFFFFFUL;
    if (!sd_read(map_entry(j, 0U), sector)) return 0U;
    fw_package_header_t *h = (fw_package_header_t *)sector;
    uint32_t header_crc = h->header_crc32;
    h->header_crc32 = 0U;
    uint8_t header_ok = (h->magic == FW_PACKAGE_MAGIC &&
                         h->format == FW_FORMAT_VERSION &&
                         h->board_id == FW_BOARD_ID &&
                         fw_abi_fits_layout(h->min_loader_abi) &&
                         h->image_base == FW_APP_BASE &&
                         h->image_bytes == j->image_bytes &&
                         h->image_crc32 == j->image_crc32 &&
                         h->firmware_version == j->firmware_version &&
                         header_crc == j->header_crc32 &&
                         fw_crc32(h, 512U) == header_crc);
    if (!header_ok) return 0U;
    crc = 0xFFFFFFFFUL;
    uint32_t remaining = j->image_bytes;
    for (uint32_t n = 1U; n < j->package_sectors; n++) {
        uint32_t lba = map_entry(j, n);
        if (lba == 0U || !sd_read(lba, sector)) return 0U;
        if (n == 1U) {
            uint32_t sp = *(const uint32_t *)&sector[0];
            uint32_t pc = *(const uint32_t *)&sector[4];
            if (sp < 0x20000000UL || sp > 0x20018000UL || (sp & 7U) != 0U ||
                (pc & 1U) == 0U || (pc & ~1UL) < FW_APP_BASE ||
                (pc & ~1UL) >= FW_APP_BASE + j->image_bytes) return 0U;
        }
        uint32_t take = remaining > 512U ? 512U : remaining;
        crc = fw_crc32_continue(crc, sector, take);
        remaining -= take;
    }
    return (remaining == 0U && ~crc == j->image_crc32) ? 1U : 0U;
}

static uint8_t program_candidate(const fw_journal_t *j)
{
    if (!flash_unlock()) return 0U;
    for (uint32_t addr = FW_APP_BASE; addr < FW_JOURNAL_ADDR; addr += FW_PAGE_BYTES) {
        if (!flash_erase_page(addr)) return 0U;
        GPIOB->ODR ^= GPIO_PIN_7;
    }
    uint32_t remaining = j->image_bytes;
    uint32_t dst = FW_APP_BASE;
    for (uint32_t n = 1U; n < j->package_sectors; n++) {
        uint32_t lba = map_entry(j, n);
        if (lba == 0U || !sd_read(lba, sector)) return 0U;
        uint32_t take = remaining > 512U ? 512U : remaining;
        for (uint32_t off = 0U; off < take; off += 8U) {
            uint8_t word[8];
            for (uint8_t k = 0U; k < 8U; k++) {
                word[k] = (off + k < take) ? sector[off + k] : 0xFFU;
            }
            if (dst + off + 8U > FW_FOOTER_ADDR || !flash_write8(dst + off, word)) return 0U;
        }
        dst += take;
        remaining -= take;
    }
    if (remaining != 0U || fw_crc32((const void *)FW_APP_BASE, j->image_bytes) != j->image_crc32) return 0U;
    fw_image_footer_t footer = {
        .magic = FW_FOOTER_MAGIC,
        .board_id = FW_BOARD_ID,
        .image_base = FW_APP_BASE,
        .image_bytes = j->image_bytes,
        .firmware_version = j->firmware_version,
        .image_crc32 = j->image_crc32,
        .loader_abi = FW_LOADER_ABI,
        .footer_crc32 = 0U,
    };
    footer.footer_crc32 = fw_crc32(&footer, offsetof(fw_image_footer_t, footer_crc32));
    const uint8_t *p = (const uint8_t *)&footer;
    for (uint32_t off = 0U; off < sizeof(footer); off += 8U) {
        if (!flash_write8(FW_FOOTER_ADDR + off, p + off)) return 0U;
    }
    FLASH->CR |= FLASH_CR_LOCK;
    return footer_valid();
}

__attribute__((naked, noreturn)) static void app_branch(void)
{
    __asm volatile(
        "ldr r0, =0x08001000\n"
        "ldr r1, [r0]\n"
        "ldr r2, [r0, #4]\n"
        "msr msp, r1\n"
        "bx r2\n");
}

static void app_jump(void)
{
    /* Leave GPIO power hold and RF inhibit latched. The application repeats
     * early safe GPIO setup before touching clocks or C runtime. */
    SPI1->CR1 &= ~SPI_CR1_SPE;
    RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
    SCB->VTOR = FW_APP_BASE;
    __DSB();
    __ISB();
    app_branch();
}

void loader_main(void)
{
    led_init();
    watchdog_start();
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    /* FW_PVD_LEVEL, not level 7: led_init() above drives PB7 low, and PB7 is
     * also PVD_IN, so level 7 arms the brownout guard against this loader's
     * own status LED and refuses every erase and program below. */
    PWR->CR2 = (PWR->CR2 & ~PWR_CR2_PLS) | PWR_CR2_PVDE | FW_PVD_LEVEL;
    const fw_journal_t *j = NULL;
    if (!journal_valid(&j)) {
        if (footer_valid()) app_jump();
        wait_blink(0U);
    }
    const fw_image_footer_t *current = (const fw_image_footer_t *)FW_FOOTER_ADDR;
    if (footer_fields_valid(current) && current->image_crc32 == j->image_crc32 &&
        current->image_bytes == j->image_bytes &&
        current->firmware_version == j->firmware_version && footer_valid()) {
        if (*(const uint64_t *)FW_JOURNAL_CONFIRM_ADDR != FW_CONFIRM_MARK) {
            uint32_t slot = 0U;
            while (slot < FW_BOOT_ATTEMPTS &&
                   *(const uint64_t *)(FW_JOURNAL_ATTEMPTS_ADDR + slot * 8U) != UINT64_MAX) slot++;
            if (slot == FW_BOOT_ATTEMPTS) wait_blink(0U);
            const uint64_t mark = 0xB007A77E4D4F4445ULL;
            if (!flash_unlock() || !flash_write8(FW_JOURNAL_ATTEMPTS_ADDR + slot * 8U,
                                                (const uint8_t *)&mark)) wait_blink(0U);
            FLASH->CR |= FLASH_CR_LOCK;
        }
        app_jump();
    }
    if (!supply_ok() || !sd_init() || !package_valid(j)) {
        if (footer_valid()) app_jump();
        wait_blink(0U);
    }
    if (!program_candidate(j)) wait_blink(1U);
    NVIC_SystemReset();
    wait_blink(0U);
}

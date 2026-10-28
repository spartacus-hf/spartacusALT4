/**
 * @file    mod_sdcard.c
 * @brief   Low-level SD card block driver over SPI1.
 *
 * Implementation notes:
 *  - Card init is bit-banged below 400 kHz, then SPI1 switches to full speed.
 *  - All transfers are blocking (polling) - no interrupts, no DMA.
 *    This keeps the SD path entirely outside the RF/USB real-time domain.
 *  - Every command carries its CRC7, and CMD59 turns the card's CRC checks
 *    on: it checks commands and written blocks, the driver checks the CRC16
 *    of every block it reads.
 *  - Deadlines count CPU cycles (DWT), so they also run out inside an
 *    interrupt -- USB mass storage reads and writes the card from one, where
 *    HAL_GetTick stands still.
 *  - A failed block transfer is retried (mod_sdcard_policy.h); a write is
 *    checked with CMD13 once the card has finished programming it.
 *  - Only SDSC/SDHC cards are supported (SDv1, SDv2, SDHC).
 *
 * Pin map:
 *   PA4 = SD_CS (software GPIO, active low)
 *   PA5 = SD_SCK (SPI1 alternate function)
 *   PA6 = SD_MISO (SPI1 alternate function)
 *   PA7 = SD_MOSI (SPI1 alternate function)
 */

#include "mod_sdcard.h"
#include "mod_sdcard_policy.h"
#include "modulator.h"
#include "main.h"
#include <string.h>

/* ═══════════════ Internal state ═════════════════════════ */
#define CMD0    0U   /* GO_IDLE_STATE */
#define CMD1    1U   /* SEND_OP_COND (SDC) */
#define CMD8    8U   /* SEND_IF_COND */
#define CMD9    9U   /* SEND_CSD */
#define CMD10   10U  /* SEND_CID */
#define CMD12   12U  /* STOP_TRANSMISSION */
#define CMD16   16U  /* SET_BLOCKLEN */
#define CMD17   17U  /* READ_SINGLE_BLOCK */
#define CMD18   18U  /* READ_MULTIPLE_BLOCK */
#define CMD23   23U  /* SET_BLOCK_COUNT */
#define CMD24   24U  /* WRITE_BLOCK */
#define CMD25   25U  /* WRITE_MULTIPLE_BLOCK */
#define CMD41   41U  /* SD_SEND_OP_COND (ACMD, prefix with CMD55) */
#define CMD13   13U  /* SEND_STATUS */
#define CMD55   55U  /* APP_CMD */
#define CMD58   58U  /* READ_OCR */
#define CMD59   59U  /* CRC_ON_OFF */

/* Data tokens */
#define SD_TOKEN_START_BLOCK     0xFEU
#define SD_TOKEN_START_BLOCK_WR  0xFCU   /* multi-block write */
#define SD_TOKEN_STOP_TRAN       0xFDU

/* Data response masks (write) */
#define SD_DATA_OK               0x05U
#define SD_DATA_CRC_ERR          0x0BU
#define SD_DATA_WRITE_ERR        0x0DU

/* ═══════════════ Timing constants ═══════════════════════ */
#define SD_INIT_ATTEMPTS         200U    /* ~800 ms at 4 ms per CMD55+ACMD41 */
#define SD_WRITE_TIMEOUT_MS      500U
#define SD_READ_TIMEOUT_MS       300U
#define SD_LOCAL_READ_TIMEOUT_MS 4U
#define SD_ERASE_TIMEOUT_MS      2000U
#define SD_INIT_GPIO_DELAY       240U
#define SD_SPI_FLAG_TIMEOUT_MS   10U

/* ═══════════════ Internal state ═════════════════════════ */
static uint8_t   sd_ready         = 0U;
static sd_type_t sd_type          = SD_TYPE_UNKNOWN;
static uint32_t  sd_block_count   = 0U;
static uint8_t   sd_status_byte   = 0U;
static sd_error_t sd_last_error   = SD_ERR_NOT_READY;
static uint8_t   sd_slow_gpio     = 0U;
static const char *sd_last_wait    = "NONE";
static uint8_t   sd_crc_on        = 0U;
static sd_stats_t sd_stats;

/* ═══════════════ Time: CPU cycles ═══════════════════════ */

static void sd_clock_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t sd_now(void)
{
    return DWT->CYCCNT;
}

/* A deadline in cycles; at 170 MHz the counter wraps after 25 s, far beyond
 * the longest wait here (2 s). */
static uint32_t sd_cycles(uint32_t ms)
{
    return ms * (SystemCoreClock / 1000U);
}

static void sd_busy_wait_ms(uint32_t ms)
{
    const uint32_t start = sd_now(), limit = sd_cycles(ms);
    while ((sd_now() - start) < limit) { }
}

/* ═══════════════ GPIO helpers ═══════════════════════════ */

static inline void sd_cs_low(void)
{
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET);
}

static inline void sd_cs_high(void)
{
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
}

static void sd_set_last_error(sd_error_t err)
{
    sd_last_error = err;
}

static sd_error_t sd_fail(sd_error_t err)
{
    sd_set_last_error(err);
    return err;
}

static sd_error_t sd_wait_fail(void)
{
    return (sd_last_error == SD_ERR_ABORTED) ? sd_fail(SD_ERR_ABORTED)
                                             : sd_fail(SD_ERR_TIMEOUT);
}

/* A block write is never cut short. Aborted in its busy phase -- the card
 * still programming the sector -- the driver dropped to not-ready, and the
 * remount that followed sent CMD0 into the programming: the sector was left
 * erased. With the settings on the card that sector was the root directory,
 * rewritten by every autosave, and the bench card lost every file in its
 * root (2026-10-01). A write is a few milliseconds of main loop, which is
 * not realtime; the receive and RF paths run in interrupts and are not
 * held up by it. Reads may still be cut short: that loses nothing. */
static volatile uint8_t sd_write_active;
/* Set through the boot (SD_DriverBootIo): nothing realtime runs yet, and the
 * I/O gate's refusals then (the first carrier apply) are not real. */
static uint8_t sd_boot_io;

void SD_DriverBootIo(uint8_t on) { sd_boot_io = on ? 1U : 0U; }

/* A whole file operation that writes is held from start to end the same way
 * (mod_sdcard_service.c), its reads included. Nests. */
static volatile uint8_t sd_hold_depth;

void SD_DriverHold(uint8_t on)
{
    if (on) ++sd_hold_depth;
    else if (sd_hold_depth) --sd_hold_depth;
}

/* The I/O gate costs ~20 us of main loop (CW keyer state included), and the
 * SPI waits asked it on every byte: 43 ms a sector on the bench, so every
 * settings help text froze the main loop -- the knob with it -- for 150-240
 * ms (2026-10-01). It is asked afresh at the start of each block and command,
 * then every SD_ABORT_POLLS polls, a few tens of microseconds of transfer. */
#define SD_ABORT_POLLS 64U
static uint8_t sd_abort_polls;
static uint8_t sd_abort_last;

static uint8_t sd_realtime_abort_requested(void)
{
    if (sd_write_active || sd_boot_io || sd_hold_depth) return 0U;
    if (MOD_LocalAudioIoScopeActive() != 0U) return 0U;
    if (sd_abort_polls != 0U) {
        --sd_abort_polls;
        return sd_abort_last;
    }
    sd_abort_polls = SD_ABORT_POLLS - 1U;
    sd_abort_last = (MOD_BlockingIoAllowed(HAL_GetTick()) == 0U) ? 1U : 0U;
    return sd_abort_last;
}

/* The gate as it is now, for the start of an operation. */
static uint8_t sd_realtime_abort_now(void)
{
    sd_abort_polls = 0U;
    return sd_realtime_abort_requested();
}

/* A transfer was abandoned part way: the card may still be in the middle of
 * a data block (SD_Init lets it finish before CMD0). Set from the start: a
 * reset (a flash, the watchdog) can come in the middle of one too, and the
 * card keeps its power -- after a flash the radio met its card still sending
 * and found no card (bench, 2026-10-01). */
static uint8_t sd_abandoned = 1U;

/* What a transfer left in SPI1. CS raised in the middle of a byte (the I/O
 * gate cutting a transfer short, a timeout) does not stop the SPI: the byte
 * in flight is clocked out, and what came back lands in the RX FIFO after the
 * driver stopped reading. Neither SPE off nor a new SD_Init empties it, so
 * every byte read from then on was the one before: reads still worked (they
 * read on), but a write's data response came a byte late and every write
 * failed until a reset -- the card "lost" on the bench (2026-10-01). Emptied
 * at every command and after every transfer abandoned. Bounded: the shift
 * ends within a byte time (~380 ns), the FIFO holds four. */
static void spi_drain(void)
{
    if (sd_slow_gpio != 0U || (SPI1->CR1 & SPI_CR1_SPE) == 0U) return;
    /* A byte just written sits in the TX FIFO for a few clocks before BSY
     * rises: wait for both, then give its reply time to arrive (it came in
     * after a drain that looked at BSY alone; bench, 2026-10-01). */
    for (uint32_t i = 0U; i < 512U && (SPI1->SR & (SPI_SR_FTLVL | SPI_SR_BSY)) != 0U; ++i) {
    }
    for (uint32_t i = 0U, n = 0U; i < 64U && n < 8U; ++i) {
        if ((SPI1->SR & SPI_SR_RXNE) != 0U) {
            (void)*(__IO uint8_t *)&SPI1->DR;
            ++n;
        }
    }
    if ((SPI1->SR & SPI_SR_OVR) != 0U) {   /* cleared by DR then SR */
        (void)*(__IO uint8_t *)&SPI1->DR;
        (void)SPI1->SR;
    }
}

static uint8_t sd_spi_timeout(const char *site)
{
    sd_last_wait = site;
    sd_ready = 0U;
    sd_cs_high();
    spi_drain();
    sd_abandoned = 1U;
    sd_set_last_error(SD_ERR_TIMEOUT);
    return 0U;
}

static uint8_t sd_audio_abort(const char *site)
{
    sd_last_wait = site;
    if (sd_ready) ++sd_stats.aborts;
    sd_ready = 0U;
    sd_cs_high();
    spi_drain();
    sd_abandoned = 1U;
    sd_set_last_error(SD_ERR_ABORTED);
    return 0U;
}

static sd_error_t sd_audio_abort_fail(const char *site)
{
    (void)sd_audio_abort(site);
    return sd_fail(SD_ERR_ABORTED);
}

static uint8_t sd_delay_abortable(uint32_t ms, const char *site)
{
    for (uint32_t i = 0U; i < ms; i++) {
        if (sd_realtime_abort_requested() != 0U) return sd_audio_abort(site);
        sd_busy_wait_ms(1U);
        if (sd_realtime_abort_requested() != 0U) return sd_audio_abort(site);
    }
    return 1U;
}

static void sd_gpio_delay(void)
{
    for (volatile uint32_t i = 0U; i < SD_INIT_GPIO_DELAY; i++) {
        __NOP();
    }
}

static uint8_t sd_gpio_slow_mode(void)
{
    GPIO_InitTypeDef g = {0};

    if (sd_slow_gpio == 0U && (SPI1->CR1 & SPI_CR1_SPE) != 0U) {
        const uint32_t start = sd_now(), limit = sd_cycles(SD_SPI_FLAG_TIMEOUT_MS);
        while ((SPI1->SR & SPI_SR_BSY) != 0U) {
            if (sd_realtime_abort_requested() != 0U) {
                return sd_audio_abort("GPIO_AUDIO");
            }
            if ((sd_now() - start) > limit) {
                return sd_spi_timeout("GPIO_BSY");
            }
        }
    }
    SPI1->CR1 &= ~SPI_CR1_SPE;

    g.Pin = SD_SCK_Pin | SD_MOSI_Pin;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);

    g.Pin = SD_MISO_Pin;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);

    HAL_GPIO_WritePin(SD_SCK_GPIO_Port, SD_SCK_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(SD_MOSI_GPIO_Port, SD_MOSI_Pin, GPIO_PIN_SET);
    sd_slow_gpio = 1U;
    return 1U;
}

static void sd_gpio_spi_mode(void)
{
    GPIO_InitTypeDef g = {0};

    g.Pin = SD_SCK_Pin | SD_MOSI_Pin;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = SD_SPI_AF;
    HAL_GPIO_Init(GPIOA, &g);

    g.Pin = SD_MISO_Pin;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = SD_SPI_AF;
    HAL_GPIO_Init(GPIOA, &g);

    sd_slow_gpio = 0U;
}

__attribute__((noinline, cold)) static uint8_t sd_gpio_txrx(uint8_t tx) /* init only, < 400 kHz */
{
    uint8_t rx = 0U;
    for (uint8_t bit = 0U; bit < 8U; bit++) {
        HAL_GPIO_WritePin(SD_MOSI_GPIO_Port, SD_MOSI_Pin,
                          (tx & 0x80U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        sd_gpio_delay();
        HAL_GPIO_WritePin(SD_SCK_GPIO_Port, SD_SCK_Pin, GPIO_PIN_SET);
        sd_gpio_delay();
        rx <<= 1;
        if (HAL_GPIO_ReadPin(SD_MISO_GPIO_Port, SD_MISO_Pin) == GPIO_PIN_SET) {
            rx |= 1U;
        }
        HAL_GPIO_WritePin(SD_SCK_GPIO_Port, SD_SCK_Pin, GPIO_PIN_RESET);
        sd_gpio_delay();
        tx <<= 1;
    }
    return rx;
}

/* ═══════════════ SPI1 low-level helpers ═════════════════ */

/* Poll SPI1->SR until (SR & mask) == want, giving way to the real-time gate,
 * up to SD_SPI_FLAG_TIMEOUT_MS. One loop for the three waits below: each
 * inlined its own copy at every call site. */
__attribute__((noinline)) static uint8_t spi_wait_flag(uint32_t mask, uint32_t want,
                                                       const char *gate_site, const char *timeout_site)
{
    const uint32_t start = sd_now(), limit = sd_cycles(SD_SPI_FLAG_TIMEOUT_MS);
    while ((SPI1->SR & mask) != want) {
        if (sd_realtime_abort_requested() != 0U) {
            return sd_audio_abort(gate_site);
        }
        if ((sd_now() - start) > limit) {
            return sd_spi_timeout(timeout_site);
        }
    }
    return 1U;
}

/**
 * Wait until SPI TX buffer is empty.
 */
static inline uint8_t spi_wait_tx(void)
{
    if ((SPI1->SR & SPI_SR_TXE) != 0U) return 1U;
    return spi_wait_flag(SPI_SR_TXE, SPI_SR_TXE, "SPI_TX_AUDIO", "SPI_TXE");
}

/**
 * Wait until SPI RX buffer has data.
 */
static inline uint8_t spi_wait_rx(void)
{
    return spi_wait_flag(SPI_SR_RXNE, SPI_SR_RXNE, "SPI_RX_AUDIO", "SPI_RXNE");
}

/**
 * Wait until SPI is not busy.
 */
static inline uint8_t spi_wait_bsy(void)
{
    return spi_wait_flag(SPI_SR_BSY, 0U, "SPI_BSY_AUDIO", "SPI_BSY");
}

/**
 * Transmit one byte and return the received byte.
 */
static uint8_t spi_txrx(uint8_t tx)
{
    if (sd_slow_gpio != 0U) {
        return sd_gpio_txrx(tx);
    }
    if (spi_wait_tx() == 0U) return 0xFFU;
    *(__IO uint8_t *)&SPI1->DR = tx;
    if (spi_wait_rx() == 0U) return 0xFFU;
    return (uint8_t)SPI1->DR;
}

/**
 * Transmit one byte (discard received).
 */
static uint8_t spi_tx(uint8_t tx)
{
    (void)spi_txrx(tx);
    return ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) ? 0U : 1U;
}

/**
 * Receive one byte (transmit 0xFF as dummy).
 */
static uint8_t spi_rx(void)
{
    return spi_txrx(0xFFU);
}

/**
 * Transmit a buffer, discard received data.
 */
static uint8_t spi_tx_buf(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0U; i < len; i++) {
        if (spi_tx(buf[i]) == 0U) return 0U;
    }
    return 1U;
}

/**
 * Receive into a buffer (transmit 0xFF as dummy).
 */
static uint8_t spi_rx_buf(uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0U; i < len; i++) {
        buf[i] = spi_rx();
        if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) return 0U;
    }
    return 1U;
}

static const uint16_t crc16_table[256];

/**
 * Receive a data block (0xFF out) and its CRC16 as it arrives: the next byte
 * is already on the wire while this one is stored and added to the CRC, so
 * checking the CRC costs no time. One byte is in flight at a time, as in
 * spi_txrx: RXNE for it means the TX FIFO is empty again.
 */
static uint8_t spi_rx_block(uint8_t *buf, uint16_t len, uint16_t *crc_out)
{
    uint16_t crc = 0U;
    if (sd_slow_gpio != 0U) {
        for (uint16_t i = 0U; i < len; i++) {
            buf[i] = sd_gpio_txrx(0xFFU);
            crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ buf[i]) & 0xFFU]);
        }
        *crc_out = crc;
        return 1U;
    }
    if (len == 0U) { *crc_out = 0U; return 1U; }
    if (spi_wait_tx() == 0U) return 0U;
    *(__IO uint8_t *)&SPI1->DR = 0xFFU;
    for (uint16_t i = 0U; i < len; i++) {
        if (spi_wait_rx() == 0U) return 0U;
        const uint8_t b = (uint8_t)SPI1->DR;
        if ((uint16_t)(i + 1U) < len) *(__IO uint8_t *)&SPI1->DR = 0xFFU;
        buf[i] = b;
        crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ b) & 0xFFU]);
    }
    *crc_out = crc;
    return 1U;
}

/**
 * Send dummy clocks. Required after CS deassert and for
 * initial 74+ clocks during power-up sequence.
 */
static uint8_t spi_send_clocks(uint16_t n)
{
    for (uint16_t i = 0U; i < n; i++) {
        if (spi_tx(0xFFU) == 0U) return 0U;
    }
    return 1U;
}

/* With CS low, a data block and its CRC clocked out (0xFF in): a card still
 * sending one finishes it, one waiting for a write's data gets a block whose
 * CRC it refuses, an idle one ignores it. Init speed only (~40 ms). */
static void sd_flush_block(void)
{
    sd_cs_low();
    for (uint16_t i = 0U; i < 520U; ++i) (void)spi_rx();
}

/**
 * Set SPI1 clock speed.
 * speed=0: slow (~300 kHz for init)
 * speed=1: fast (full speed for data)
 */
static uint8_t sd_set_speed(uint8_t speed)
{
    if (speed == 0U) {
        return sd_gpio_slow_mode();
    }

    if (sd_slow_gpio != 0U) {
        sd_gpio_spi_mode();
    }

    /* Disable SPI before changing baud rate (RM0440: BR bits must be changed with SPE=0) */
    if (spi_wait_bsy() == 0U) return 0U;
    SPI1->CR1 &= ~SPI_CR1_SPE;
    /* Wait for any ongoing frame to complete */
    if (spi_wait_bsy() == 0U) return 0U;

    uint32_t cr1 = SPI1->CR1;
    cr1 &= ~(SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0);
    /* /8 prescaler: 170 MHz / 8 = 21.25 MHz (conservative for SPI-mode SD). */
    cr1 |= SPI_CR1_BR_1;
    SPI1->CR1 = cr1;
    SPI1->CR1 |= SPI_CR1_SPE;
    spi_drain(); /* nothing from before the switch */
    return 1U;
}

/* ═══════════════ CRC7 for SD commands ══════════════════ */

static uint8_t sd_crc7(const uint8_t *data, uint8_t n)
{
    uint8_t crc = 0U;
    for (uint8_t i = 0U; i < n; i++) {
        uint8_t d = data[i];
        for (uint8_t b = 0U; b < 8U; b++) {
            crc <<= 1;
            if ((uint8_t)((d ^ crc) & 0x80U) != 0U) {
                crc ^= 0x09U;
            }
            d <<= 1;
        }
    }
    return (crc << 1) | 1U;
}

/* ═══════════════ CRC16 for SD data blocks ══════════════ */

/* Pre-computed CRC-16/CCITT-XMODEM table */
static const uint16_t crc16_table[256] = {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,
    0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
    0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,
    0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
    0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,
    0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
    0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,
    0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
    0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,
    0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
    0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,
    0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
    0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,
    0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
    0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,
    0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
    0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,
    0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
    0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,
    0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
    0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,
    0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,
    0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
    0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,
    0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
    0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,
    0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
    0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,
    0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
    0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,
    0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0
};

static uint16_t sd_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0U;
    for (uint16_t i = 0U; i < len; i++) {
        crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ data[i]) & 0xFFU]);
    }
    return crc;
}

/* ═══════════════ SD command transport ═════════════════ */

/**
 * Send an SD command (6 bytes) and read R1 response.
 *
 * @param cmd   Command index (0-63).
 * @param arg   32-bit argument.
 * @return      R1 response byte.
 */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t buf[6];
    uint8_t r1;

    /* Build command packet */
    buf[0] = (uint8_t)(0x40U | (cmd & 0x3FU));
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)(arg);
    buf[5] = sd_crc7(buf, 5);

    spi_drain(); /* a command starts with the RX FIFO empty */
    /* Send command */
    if (spi_tx_buf(buf, 6) == 0U) return 0xFFU;

    /* Wait for non-FF response (up to 8 bytes of Ncr) */
    uint16_t timeout = 8U;
    do {
        r1 = spi_rx();
        if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) return 0xFFU;
    } while ((r1 & 0x80U) != 0U && --timeout > 0U);

    return r1;
}

/**
 * Send an application-specific command (prefix with CMD55).
 *
 * @param cmd   ACMD index (0-63).
 * @param arg   32-bit argument.
 * @return      R1 response byte from the ACMD.
 */
static uint8_t sd_send_acmd(uint8_t cmd, uint32_t arg)
{
    uint8_t r1 = sd_send_cmd(CMD55, 0U);
    if (r1 > 1U) return r1;
    return sd_send_cmd(cmd, arg);
}

/* ═══════════════ SD initialization ════════════════════ */

/**
 * SPI mode initialization sequence for SD card.
 *
 * Sequence:
 *   1. 74+ dummy clocks with CS high
 *   2. CMD0 (GO_IDLE_STATE) → enter SPI mode
 *   3. CMD8 (SEND_IF_COND) → check voltage range
 *   4. ACMD41 loop → wait for card to leave idle state
 *   5. CMD58 (READ_OCR) → check CCS bit for SDHC
 *   6. CMD16 (SET_BLOCKLEN = 512) for SDSC only
 */
__attribute__((cold)) sd_error_t SD_Init(void)
{
    uint8_t  r1;
    uint32_t start, timeout;
    uint8_t  ocr_buf[4];

    if (sd_realtime_abort_now() != 0U) return sd_fail(SD_ERR_ABORTED);

    /* Every wait below is bounded, but together they come near the 2 s
     * watchdog (500 ms for a card still busy, ~1 s of ACMD41 for a card just
     * powered, 300 ms for the CSD), and a caller may have waited already: a
     * card going in or out reset the radio on the bench (2026-10-01). Fed at
     * each stage, so the watchdog still catches a hang anywhere else. */
    APP_WatchdogProgress();
    sd_clock_enable();
    sd_ready = 0U;
    sd_crc_on = 0U;
    sd_type = SD_TYPE_UNKNOWN;
    sd_block_count = 0U;
    sd_status_byte = 0U;
    sd_set_last_error(SD_ERR_NOT_READY);

    /* Start with card deselected, slow speed */
    sd_cs_high();
    if (sd_set_speed(0) == 0U) return sd_wait_fail();

    /* A card already in SPI mode may still be programming a block -- the
     * driver re-initialising after an error, or the MCU reset (a flash, the
     * watchdog) in the middle of a write: the card keeps its own power. It
     * holds DO low while selected, and CMD0 now would cut the programming
     * short; on the bench that left the card answering blank sectors until
     * it was power-cycled (2026-10-01). Let it finish first (bounded). A card
     * fresh from power-up leaves DO released and this ends at once. */
    sd_cs_low();
    start = sd_now();
    while ((spi_rx() == 0x00U) && ((sd_now() - start) < sd_cycles(500U))) {
    }
    /* A read cut short left the card sending the rest of its block, and it
     * takes no command until that is out: clock a block's worth (a card
     * receiving a write ends it with a CRC the card refuses). */
    if (sd_abandoned) {
        sd_flush_block();
        sd_abandoned = 0U;
    }
    sd_cs_high();
    APP_WatchdogProgress();

    /* Send 80 dummy clocks with CS high (>= 74 required) */
    if (spi_send_clocks(80U) == 0U) return sd_wait_fail();

    /* ── CMD0: GO_IDLE_STATE ── once more after a block's worth clocked out
     * if unanswered: a card in some state of its own (left by another radio,
     * a transfer nobody here knew of) takes it then. */
    sd_cs_low();
    r1 = sd_send_cmd(CMD0, 0U);
    if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
    if (r1 != 0x01U) {
        sd_flush_block();
        sd_cs_high();
        if (spi_send_clocks(16U) == 0U) return sd_wait_fail();
        APP_WatchdogProgress();
        sd_cs_low();
        r1 = sd_send_cmd(CMD0, 0U);
        if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
    }
    if (r1 != 0x01U) {
        sd_cs_high();
        return sd_fail(SD_ERR_NO_CARD);
    }

    /* ── CMD8: SEND_IF_COND ──
     * VHS = 0x1 (2.7-3.6V), check pattern = 0xAA */
    r1 = sd_send_cmd(CMD8, 0x000001AAUL);
    if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
    if (r1 == 0x01U) {
        /* Read R7 response (4 bytes after R1) */
        uint8_t r7[4];
        if (spi_rx_buf(r7, 4) == 0U) {
            sd_cs_high();
            return sd_wait_fail();
        }
        if (r7[2] != 0x01U || r7[3] != 0xAAU) {
            sd_cs_high();
            return sd_fail(SD_ERR_PARAM);
        }
        /* SDv2 card */
        sd_type = SD_TYPE_SDV2;
    } else if (r1 == 0x05U) {
        /* Illegal command — SDv1 or MMC */
        sd_type = SD_TYPE_SDV1;
    } else {
        sd_cs_high();
        return sd_fail(SD_ERR_NO_CARD);
    }
    sd_cs_high();
    if (spi_send_clocks(8U) == 0U) return sd_wait_fail();

    /* ── ACMD41: SD_SEND_OP_COND ──
     * For SDHC: arg = 0x40000000 (HCS bit set)
     * For SDSC: arg = 0x00000000
     * Wait until card leaves idle state or timeout. */
    sd_cs_low();
    timeout = SD_INIT_ATTEMPTS;
    do {
        uint32_t acmd41_arg = (sd_type == SD_TYPE_SDV1) ? 0U : 0x40000000UL;
        r1 = sd_send_acmd(CMD41, acmd41_arg);
        if (r1 == 0x00U) break;
        if (sd_last_error == SD_ERR_ABORTED) {
            sd_cs_high();
            return sd_fail(SD_ERR_ABORTED);
        }
        if (r1 > 0x01U) {
            sd_cs_high();
            return sd_fail(SD_ERR_TIMEOUT);
        }
        /* ~4 ms delay between retries */
        sd_cs_high();
        if (spi_send_clocks(8U) == 0U) return sd_wait_fail();
        APP_WatchdogProgress();
        if (sd_delay_abortable(4U, "INIT_AUDIO") == 0U) {
            return sd_fail(SD_ERR_ABORTED);
        }
        sd_cs_low();
    } while (--timeout > 0U);

    if (timeout == 0U) {
        sd_cs_high();
        return sd_fail(SD_ERR_TIMEOUT);
    }

    /* ── CMD58: READ_OCR ── */
    r1 = sd_send_cmd(CMD58, 0U);
    if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
    if (r1 != 0x00U) {
        sd_cs_high();
        return sd_fail(SD_ERR_STATE);
    }
    if (spi_rx_buf(ocr_buf, 4) == 0U) {
        sd_cs_high();
        return sd_wait_fail();
    }
    /* Check CCS (bit 30): if set, card is SDHC/SDXC (block addressed) */
    if ((ocr_buf[0] & 0x40U) != 0U) {
        sd_type = SD_TYPE_SDV2HC;
        /* SDHC cards are block-addressed; CMD16 is not needed */
    } else {
        /* SDSC: set block length to 512 */
        r1 = sd_send_cmd(CMD16, 512U);
        if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
        if (r1 != 0x00U) {
            sd_cs_high();
            return sd_fail(SD_ERR_STATE);
        }
    }

    /* ── Read CSD to determine capacity ── */
    {
        uint8_t csd[16];
        r1 = sd_send_cmd(CMD9, 0U);  /* SEND_CSD */
        if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
        if (r1 != 0x00U) {
            sd_cs_high();
            return sd_fail(SD_ERR_STATE);
        }
        /* Wait for data token */
        APP_WatchdogProgress();
        start = sd_now();
        while (spi_rx() != SD_TOKEN_START_BLOCK) {
            if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) {
                sd_cs_high();
                return sd_wait_fail();
            }
            if (sd_realtime_abort_requested() != 0U) {
                return sd_audio_abort_fail("CSD_AUDIO");
            }
            if ((sd_now() - start) > sd_cycles(SD_READ_TIMEOUT_MS)) {
                sd_cs_high();
                return sd_fail(SD_ERR_TIMEOUT);
            }
        }
        uint16_t csd_crc = 0U;
        if (spi_rx_block(csd, 16U, &csd_crc) == 0U) {
            sd_cs_high();
            return sd_wait_fail();
        }
        const uint16_t csd_sent = (uint16_t)((uint16_t)spi_rx() << 8);
        const uint16_t csd_got = (uint16_t)(csd_sent | spi_rx());
        if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) {
            sd_cs_high();
            return sd_wait_fail();
        }
        /* The capacity decides every address the card is given: a CSD that
         * arrived damaged is not used. */
        if (csd_got != csd_crc) {
            ++sd_stats.crc_errors;
            sd_cs_high();
            return sd_fail(SD_ERR_CRC);
        }

        /* CSD structure version */
        uint8_t csd_ver = (csd[0] >> 6) & 0x03U;
        if (csd_ver == 1U) {
            /* CSD v2.0 (SDHC/SDXC): C_SIZE is 22 bits */
            uint32_t csize = ((uint32_t)(csd[7] & 0x3FU) << 16)
                           | ((uint32_t)csd[8] << 8)
                           | (uint32_t)csd[9];
            sd_block_count = (csize + 1U) * 1024U;
        } else {
            /* CSD v1.0 (SDSC) */
            uint32_t csize = ((uint32_t)(csd[6] & 0x03U) << 10)
                           | ((uint32_t)csd[7] << 2)
                           | ((uint32_t)(csd[8] >> 6) & 0x03U);
            uint8_t  cmult = ((csd[9] & 0x03U) << 1)
                           | ((csd[10] >> 7) & 0x01U);
            uint32_t blen  = (csd[5] & 0x0FU);
            blen = 1UL << blen;
            uint32_t mult  = 1UL << (cmult + 2U);
            sd_block_count = (csize + 1U) * mult * (blen / 512UL);
        }
    }

    sd_cs_high();
    if (spi_send_clocks(8U) == 0U) return sd_wait_fail();

    /* CRC on: the card then rejects a command or a written block that
     * arrived damaged, and the block reads below check theirs. A card that
     * refuses CMD59 still works, without the checks (counted). */
    sd_cs_low();
    r1 = sd_send_cmd(CMD59, 1U);
    if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
    sd_crc_on = (r1 == 0x00U) ? 1U : 0U;
    if (!sd_crc_on) ++sd_stats.crc_refused;
    sd_cs_high();
    if (spi_send_clocks(8U) == 0U) return sd_wait_fail();

    /* Switch to full speed */
    if (sd_set_speed(1) == 0U) return sd_wait_fail();

    ++sd_stats.inits;
    sd_ready = 1U;
    sd_status_byte = 0U;
    sd_set_last_error(SD_OK);
    return SD_OK;
}

/* ═══════════════ Retries ══════════════════════════════ */

/* One transfer that failed: count it, and feed the watchdog before the next
 * attempt -- the attempts are bounded (three), and a re-initialisation feeds
 * it at each of its stages (SD_Init). A card that still fails is left not
 * ready, so what follows fails at once instead of waiting again. */
static sd_error_t sd_with_retries(sd_error_t (*once)(uint32_t, void *), uint32_t block, void *buf,
                                  uint8_t is_write)
{
    sd_error_t e = SD_ERR_NOT_READY;
    for (uint8_t attempt = 0U; ; ++attempt) {
        e = once(block, buf);
        if (e == SD_ERR_CRC) ++sd_stats.crc_errors;
        else if (e == SD_ERR_TIMEOUT) ++sd_stats.timeouts;
        else if (e == SD_ERR_IO) ++sd_stats.io_errors;
        const sd_retry_t next = sd_retry_policy(e, attempt, MOD_LocalAudioIoScopeActive());
        if (next == SD_RETRY_NONE) break;
        ++sd_stats.retries;
        APP_WatchdogProgress();
        if (next == SD_RETRY_REINIT) {
            ++sd_stats.reinits;
            MOD_EventLog(MOD_EVENT_SD, 1U, (uint16_t)e, block, is_write);
            if (SD_Init() != SD_OK) break;
        }
    }
    if (e == SD_OK) return e;
    if (e != SD_ERR_ABORTED && e != SD_ERR_NOT_READY) {
        ++sd_stats.failures;
        MOD_EventLog(MOD_EVENT_SD, is_write ? 3U : 2U, (uint16_t)e, block, sd_stats.retries);
        /* A card that stopped answering is left not ready (the storage
         * service mounts it again); one that answered with an error -- a bad
         * sector, say -- stays usable for the rest of the card. */
        if (e == SD_ERR_TIMEOUT) sd_ready = 0U;
    }
    return sd_fail(e);
}

/* ═══════════════ Block read ═══════════════════════════ */

static sd_error_t sd_read_once(uint32_t block, void *buf)
{
    uint8_t *dst = (uint8_t *)buf;
    if (sd_ready == 0U) return sd_fail(SD_ERR_NOT_READY);
    if (sd_realtime_abort_now() != 0U) return sd_fail(SD_ERR_ABORTED);
    /* The waits below test sd_last_error; an earlier operation's must not
     * fail this one before it starts. */
    sd_set_last_error(SD_OK);

    uint8_t  r1;
    uint32_t start;

    sd_cs_low();

    /* For SDSC, convert block number to byte address */
    uint32_t addr = block;
    if (sd_type != SD_TYPE_SDV2HC) {
        addr = block * 512UL;
    }

    r1 = sd_send_cmd(CMD17, addr);
    if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
    if (r1 != 0x00U) {
        sd_cs_high();
        return sd_fail((r1 & 0x08U) ? SD_ERR_CRC : SD_ERR_IO); /* bit 3: command CRC */
    }

    /* Wait for data token 0xFE; anything else but 0xFF is the card's error
     * token (out of range, ECC, controller error), not a reason to wait on. */
    const uint32_t limit = sd_cycles(MOD_LocalAudioIoScopeActive() ? SD_LOCAL_READ_TIMEOUT_MS
                                                                    : SD_READ_TIMEOUT_MS);
    start = sd_now();
    for (;;) {
        const uint8_t token = spi_rx();
        if (token == SD_TOKEN_START_BLOCK) break;
        if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) {
            sd_cs_high();
            return sd_wait_fail();
        }
        if (token != 0xFFU) {
            sd_cs_high();
            return sd_fail(SD_ERR_IO);
        }
        if (sd_realtime_abort_requested() != 0U) {
            return sd_audio_abort_fail("READ_AUDIO");
        }
        if ((sd_now() - start) > limit) {
            sd_cs_high();
            return sd_fail(SD_ERR_TIMEOUT);
        }
    }

    /* 512 bytes, then the card's CRC16 of them */
    uint16_t crc = 0U;
    if (spi_rx_block(dst, 512U, &crc) == 0U) {
        sd_cs_high();
        return sd_wait_fail();
    }
    const uint16_t sent_hi = (uint16_t)((uint16_t)spi_rx() << 8);
    const uint16_t sent = (uint16_t)(sent_hi | spi_rx());
    if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) {
        sd_cs_high();
        return sd_wait_fail();
    }

    sd_cs_high();
    if (spi_send_clocks(8U) == 0U) return sd_wait_fail();
    if (sd_crc_on && sent != crc) return sd_fail(SD_ERR_CRC);

    return sd_fail(SD_OK);
}

sd_error_t SD_ReadBlock(uint32_t block, uint8_t *dst)
{
    if (sd_ready == 0U || dst == NULL) {
        return sd_fail(SD_ERR_NOT_READY);
    }
    return sd_with_retries(sd_read_once, block, dst, 0U);
}

/* ═══════════════ Block write ════════════════════════ */

static sd_error_t sd_write_block(uint32_t block, const uint8_t *src);

/* What the card refused, for `sd stats`: a card that will not take writes says
 * why only here (the CMD13 status: write protect, ECC, controller error). */
static void sd_note_reject(uint8_t stage, uint8_t a, uint8_t b, uint32_t block)
{
    sd_stats.reject = ((uint32_t)stage << 24) | ((uint32_t)a << 8) | (uint32_t)b;
    sd_stats.reject_lba = block;
}

static sd_error_t sd_write_once(uint32_t block, void *buf)
{
    if (sd_ready == 0U) return sd_fail(SD_ERR_NOT_READY);
    sd_set_last_error(SD_OK);
    return sd_write_block(block, (const uint8_t *)buf);
}

sd_error_t SD_WriteBlock(uint32_t block, const uint8_t *src)
{
    if (sd_ready == 0U || src == NULL) {
        return sd_fail(SD_ERR_NOT_READY);
    }
    /* Refused before it starts, never once it has (retries included). */
    if (sd_realtime_abort_now() != 0U) return sd_fail(SD_ERR_ABORTED);
    sd_write_active = 1U;
    const sd_error_t e = sd_with_retries(sd_write_once, block, (void *)(uintptr_t)src, 1U);
    sd_write_active = 0U;
    if (e == SD_OK) sd_stats.write_fail_run = 0U;
    else if (e != SD_ERR_ABORTED && e != SD_ERR_NOT_READY && sd_stats.write_fail_run < 0xFFU) ++sd_stats.write_fail_run;
    return e;
}

uint8_t SD_WritesFailing(void)
{
    return (uint8_t)(sd_stats.write_fail_run >= SD_WRITE_FAIL_RUN);
}

static sd_error_t sd_write_block(uint32_t block, const uint8_t *src)
{

    uint8_t  r1, dr;
    uint32_t start;

    sd_cs_low();

    /* For SDSC, convert block number to byte address */
    uint32_t addr = block;
    if (sd_type != SD_TYPE_SDV2HC) {
        addr = block * 512UL;
    }

    r1 = sd_send_cmd(CMD24, addr);
    if (sd_last_error == SD_ERR_ABORTED) return sd_fail(SD_ERR_ABORTED);
    if (r1 != 0x00U) {
        sd_cs_high();
        sd_note_reject(1U, r1, 0U, block);
        return sd_fail((r1 & 0x08U) ? SD_ERR_CRC : SD_ERR_IO);
    }

    /* Send data start token */
    if (spi_tx(SD_TOKEN_START_BLOCK) == 0U) {
        sd_cs_high();
        return sd_wait_fail();
    }

    /* Send 512 bytes of data */
    if (spi_tx_buf(src, 512U) == 0U) {
        sd_cs_high();
        return sd_wait_fail();
    }

    /* CRC16 of the block: checked by the card once CMD59 turned CRC on */
    {
        uint16_t crc = sd_crc16(src, 512U);
        if (spi_tx((uint8_t)(crc >> 8)) == 0U ||
            spi_tx((uint8_t)(crc & 0xFFU)) == 0U) {
            sd_cs_high();
            return sd_wait_fail();
        }
    }

    /* Read data response token */
    dr = spi_rx();
    if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) {
        sd_cs_high();
        return sd_wait_fail();
    }
    if ((dr & 0x1FU) != SD_DATA_OK) {
        sd_cs_high();
        sd_note_reject(2U, dr, 0U, block);
        if ((dr & 0x1FU) == SD_DATA_CRC_ERR) return sd_fail(SD_ERR_CRC);
        return sd_fail(SD_ERR_IO);
    }

    /* Wait for write to complete (card drives busy=0) */
    start = sd_now();
    while (spi_rx() == 0x00U) {
            if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) {
                sd_cs_high();
                return sd_wait_fail();
            }
            if (sd_realtime_abort_requested() != 0U) {
                return sd_audio_abort_fail("WRITE_AUDIO");
            }
        if ((sd_now() - start) > sd_cycles(SD_WRITE_TIMEOUT_MS)) {
            sd_cs_high();
            return sd_fail(SD_ERR_TIMEOUT);
        }
    }

    sd_cs_high();
    if (spi_send_clocks(8U) == 0U) return sd_wait_fail();

    /* A programming error is reported only after the busy phase: ask. */
    sd_cs_low();
    r1 = sd_send_cmd(CMD13, 0U);
    const uint8_t r2 = spi_rx();
    sd_cs_high();
    if ((sd_last_error == SD_ERR_TIMEOUT) || (sd_last_error == SD_ERR_ABORTED)) return sd_wait_fail();
    if (spi_send_clocks(8U) == 0U) return sd_wait_fail();
    if (!sd_status_ok(r1, r2)) {
        sd_status_byte = r2;
        sd_note_reject(3U, r1, r2, block);
        return sd_fail(SD_ERR_IO);
    }

    return sd_fail(SD_OK);
}

/* ═══════════════ Status queries ══════════════════════ */

void SD_GetStats(sd_stats_t *out)
{
    if (out) {
        *out = sd_stats;
        out->crc_on = sd_crc_on;
    }
}

sd_type_t SD_GetType(void)
{
    return sd_type;
}

uint32_t SD_GetBlockCount(void)
{
    return sd_block_count;
}

uint8_t SD_IsReady(void)
{
    return sd_ready;
}

sd_error_t SD_GetLastError(void)
{
    return sd_last_error;
}

const char *SD_GetLastWaitString(void)
{
    return sd_last_wait;
}

const char *SD_GetStatusString(void)
{
    if (sd_ready == 0U) {
        return SD_ErrorString(sd_last_error);
    }
    switch (sd_type) {
        case SD_TYPE_SDV1:   return "SDv1";
        case SD_TYPE_SDV2:   return "SDv2";
        case SD_TYPE_SDV2HC: return "SDHC";
        default:             return "UNK";
    }
}

const char *SD_ErrorString(sd_error_t err)
{
    switch (err) {
        case SD_OK:            return "OK";
        case SD_ERR_NO_CARD:   return "NO CARD";
        case SD_ERR_TIMEOUT:   return "TIMEOUT";
        case SD_ERR_CRC:       return "CRC";
        case SD_ERR_WP:        return "WRITE PROT";
        case SD_ERR_PARAM:     return "PARAM";
        case SD_ERR_STATE:     return "STATE";
        case SD_ERR_IO:        return "IO";
        case SD_ERR_ABORTED:   return "AUDIO BUSY";
        case SD_ERR_NOT_READY:
        default:               return "NOT READY";
    }
}

void SD_Deinit(void)
{
    sd_cs_high();
    sd_ready = 0U;
    sd_type = SD_TYPE_UNKNOWN;
    sd_block_count = 0U;
    sd_set_last_error(SD_ERR_NOT_READY);
}

/* ═══════════════ Low-level SPI1 hardware init ═══════════════
 * Called once at boot from MX_GPIO_Init or MX_SPI1_Init. */

/**
 * Initialize SPI1 for SD card communication.
 *
 * SPI1: Master, full-duplex, 8-bit, MSB first, CPOL=0/CPHA=0,
 *       software NSS, slow clock for init.
 */
void SD_SPI1_Init(void)
{
    sd_clock_enable();
    __HAL_RCC_SPI1_CLK_ENABLE();

    SPI1->CR1 = 0U;
    SPI1->CR2 = 0U;

    /* Master, /256 prescaler (slow for init), CPOL=0, CPHA=0,
     * MSB first, software NSS, full duplex */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM
              | SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0;
    /* 8-bit data size, FIFO reception threshold = 8-bit */
    SPI1->CR2 = SPI_CR2_DS_0 | SPI_CR2_DS_1 | SPI_CR2_DS_2 | SPI_CR2_FRXTH;

    SPI1->CR1 |= SPI_CR1_SPE;
}

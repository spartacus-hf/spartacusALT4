#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "ui_lcd.h"
#include "sctrl_link.h"
#include "ui_gfx.h"
#include "ui_state.h"
#include "mod_priv.h"
#include <string.h>

#define DISP_PWR_MS          120U
#define DISP_RETRY_MS        3000U
#define DISP_RSV_EXT         8U
#define DISP_RSV_INT         12U
/* One row per DMA transfer, from two row buffers: the main loop converts
 * the next row while the DMA sends this one, and the DMA interrupt starts a
 * converted row itself. Together they are v8.00's 1280-byte two-row stripe:
 * RAM, not flash, is this image's tight budget. SPI4 runs at HCLK/4 (42.5 MHz
 * at 170 MHz, inside the ST7789's 16 ns write cycle): a full frame is about
 * 30 ms of transfer, and the clock sits above the HF bands. */
#define LINE_BYTES           (GFX_W * 2U)
#define PAGE_LINES           8U
#define LINE_BUFS            2U
#define ALL_PAGES            ((1UL << GFX_PAGES) - 1UL)

#define ST7789_SWRESET       0x01U
#define ST7789_SLPOUT        0x11U
#define ST7789_NORON         0x13U
#define ST7789_DISPON        0x29U
#define ST7789_CASET         0x2AU
#define ST7789_RASET         0x2BU
#define ST7789_RAMWR         0x2CU
#define ST7789_MADCTL        0x36U
#define ST7789_COLMOD        0x3AU

typedef struct {
    uint8_t  ok;
    /* Shared with the DMA interrupt. */
    volatile uint8_t  dma_busy;
    volatile uint8_t  flush_active;
    /* The run being sent: dirty pages from run_page on, run_rows rows in
     * one address window. Its rows converted (main loop), handed to the DMA,
     * finished; row n is in line_buf[n % LINE_BUFS]. */
    uint8_t           run_page;
    volatile uint8_t  run_rows;
    volatile uint8_t  line_prep;
    volatile uint8_t  line_sent;
    volatile uint8_t  line_done;
    volatile uint32_t dirty_pages;
    volatile uint16_t flush_ms;     /* last frame, first page to last */
    uint8_t  rebuild_pending;
    uint8_t  rot270;
    uint8_t  rot_pending;
    uint8_t  boot_active;
    uint8_t  boot_row;
    uint8_t  boot_worst;            /* 1 all passed, 2 a warning, 0 a failure */
    uint8_t  preempt;               /* LCDUI_Preempt: the next build need not wait */
    uint32_t flush_start;
    uint32_t last_build;
    uint32_t retry_at;
    uint32_t last_mode;
    /* Frame builds never start while a page is being sent, so the
     * framebuffer is the transmit buffer; these hashes remember what the last
     * build drew, and dirty_pages what of it the panel does not show yet. */
    uint32_t page_hash[GFX_PAGES];
    uint8_t  line_buf[LINE_BUFS][LINE_BYTES] __attribute__((aligned(4)));
} display_t;

static DMA_HandleTypeDef *g_hdma_spi_tx = NULL;
static display_t disp;

#if LCD_USE_CS_GPIO
static inline void tft_select(void)   { HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET); }
static inline void tft_unselect(void) { HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);   }
#else
static inline void tft_select(void)   { (void)0; }
static inline void tft_unselect(void) { (void)0; }
#endif
static inline void tft_dc_command(void) { HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET); }
static inline void tft_dc_data(void)    { HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);   }

static void tft_tx_blocking(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len; i++) {
        while ((SPI4->SR & SPI_SR_TXE) == 0U) {
        }
        *(__IO uint8_t *)&SPI4->DR = data[i];
    }
    while ((SPI4->SR & SPI_SR_TXE) == 0U) {
    }
    while ((SPI4->SR & SPI_SR_BSY) != 0U) {
    }
}

static void tft_write_cmd(uint8_t cmd)
{
    tft_select();
    tft_dc_command();
    tft_tx_blocking(&cmd, 1U);
    tft_unselect();
}

static void tft_write_data(const uint8_t *data, uint16_t len)
{
    tft_select();
    tft_dc_data();
    tft_tx_blocking(data, len);
    tft_unselect();
}

static void tft_write_cmd_data(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    tft_write_cmd(cmd);
    tft_write_data(data, len);
}

static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t p[4];
    p[0] = (uint8_t)(x0 >> 8); p[1] = (uint8_t)(x0 & 0xFFU);
    p[2] = (uint8_t)(x1 >> 8); p[3] = (uint8_t)(x1 & 0xFFU);
    tft_write_cmd_data(ST7789_CASET, p, 4U);
    p[0] = (uint8_t)(y0 >> 8); p[1] = (uint8_t)(y0 & 0xFFU);
    p[2] = (uint8_t)(y1 >> 8); p[3] = (uint8_t)(y1 & 0xFFU);
    tft_write_cmd_data(ST7789_RASET, p, 4U);
    tft_write_cmd(ST7789_RAMWR);
}

static void tft_madctl(void)
{
    const uint8_t madctl = GFX_Madctl(disp.rot270);
    tft_write_cmd_data(ST7789_MADCTL, &madctl, 1U);
}

/* ---- Stripes ------------------------------------------------------------ */

/* The panel takes one address window per run of consecutive dirty pages;
 * their rows then stream into the same RAMWR with CS held, each converted
 * from the one-bit frame just before it is sent. */
static void page_window(uint8_t page, uint8_t pages)
{
    const uint16_t y = (uint16_t)(page * 8U);
    tft_set_window(0U, y, (uint16_t)(GFX_W - 1U), (uint16_t)(y + pages * 8U - 1U));
    tft_select();
    tft_dc_data();
}

static void flush_blocking(uint32_t pages)
{
    if (disp.ok == 0U) return;
    for (uint8_t p = 0U; p < GFX_PAGES; ++p) {
        if ((pages & (1UL << p)) == 0U) continue;
        page_window(p, 1U);
        for (uint8_t n = 0U; n < PAGE_LINES; ++n) {
            GFX_PrepareLines((uint16_t)(p * 8U + n), 1U, disp.line_buf[0]);
            tft_tx_blocking(disp.line_buf[0], LINE_BYTES);
        }
        tft_unselect();
    }
    disp.dirty_pages = 0U;
    disp.flush_active = 0U;
}

/* Pages whose content differs from what the panel shows. */
static uint32_t changed_pages(void)
{
    uint32_t mask = 0U;
    for (uint8_t p = 0U; p < GFX_PAGES; ++p) {
        const uint32_t h = GFX_PageHash(p);
        if (h != disp.page_hash[p]) {
            disp.page_hash[p] = h;
            mask |= 1UL << p;
        }
    }
    return mask;
}

static void disp_fail(void)
{
    disp.ok = 0U;
    disp.flush_active = 0U;
    disp.dirty_pages = 0U;
    disp.rebuild_pending = 1U;
    disp.retry_at = HAL_GetTick() + DISP_RETRY_MS;
}

/* The row DMA is driven at its registers (through the HAL handle's own
 * channel, base and index): HAL_DMA_Start_IT and HAL_DMA_IRQHandler took
 * their lock, state and callback bookkeeping on every row, and the
 * interrupt runs at priority 1, above the receive worker. HAL keeps the
 * channel's one-time setup (HAL_DMA_Init in main.c). */
static void line_dma_off(void)
{
    DMA_Channel_TypeDef *const ch = g_hdma_spi_tx->Instance;
    ch->CCR &= ~(DMA_CCR_EN | DMA_CCR_TCIE | DMA_CCR_HTIE | DMA_CCR_TEIE);
    g_hdma_spi_tx->DmaBaseAddress->IFCR = DMA_ISR_GIF1 << (g_hdma_spi_tx->ChannelIndex & 0x1FU);
}

/* Hand the next converted row to the DMA. The caller has found the DMA idle
 * with a converted row waiting: the main loop with interrupts masked, or the
 * DMA interrupt itself. */
static uint8_t start_line_dma(void)
{
    const uint8_t *buf = disp.line_buf[disp.line_sent % LINE_BUFS];
    DMA_Channel_TypeDef *const ch = g_hdma_spi_tx->Instance;
    disp.dma_busy = 1U;
    line_dma_off();
    ch->CNDTR = LINE_BYTES;
    ch->CPAR = (uint32_t)(uintptr_t)&SPI4->DR;
    ch->CMAR = (uint32_t)(uintptr_t)buf;
    ch->CCR |= DMA_CCR_TCIE | DMA_CCR_TEIE | DMA_CCR_EN;
    disp.line_sent++;
    SET_BIT(SPI4->CR2, SPI_CR2_TXDMAEN);
    return 1U;
}

/* The run's last row is out: release the panel. From the DMA interrupt, or
 * from the main loop with interrupts masked and the DMA idle. */
static void end_run(void)
{
    while ((SPI4->SR & SPI_SR_BSY) != 0U) {
    }
    tft_unselect();
    disp.dma_busy = 0U;
    disp.flush_active = 0U;
    if (disp.dirty_pages == 0U) disp.flush_ms = (uint16_t)(HAL_GetTick() - disp.flush_start);
}

/* DMA1 channel 3, priority 1, above the receive worker (PendSV): a row done,
 * start the next one if the main loop has converted it; at the end of a run
 * wait out the last FIFO bytes (a few hundred ns at 42.5 MHz) and release CS.
 * A few register writes, no pixel work. At priority 3 the interrupt -- and
 * the main loop -- waited out the worker after every transfer, so the panel
 * took about one transfer per 0.375 ms worker period; a whole-screen change
 * reached it 100 ms after its build (bench, 2026-10-01). */
static void disp_dma_tx_complete(void)
{
    CLEAR_BIT(SPI4->CR2, SPI_CR2_TXDMAEN);
    const uint8_t done = ++disp.line_done;
    if ((done % PAGE_LINES) == 0U) disp.dirty_pages &= ~(1UL << (disp.run_page + done / PAGE_LINES - 1U));
    if (done >= disp.run_rows) {
        end_run();
        return;
    }
    /* A failed start leaves the row to the main loop, which retries it. */
    if ((disp.line_sent < disp.line_prep) && start_line_dma()) return;
    disp.dma_busy = 0U;
}

static void disp_dma_tx_error(void)
{
    CLEAR_BIT(SPI4->CR2, SPI_CR2_TXDMAEN);
    tft_unselect();
    disp.dma_busy = 0U;
    disp_fail();
}

/* DMA1 channel 3's interrupt (stm32g4xx_it.c): a row out, or a bus error. */
void LCDUI_DmaIrq(void)
{
    if (g_hdma_spi_tx == NULL) return;
    const uint32_t shift = g_hdma_spi_tx->ChannelIndex & 0x1FU;
    const uint32_t isr = g_hdma_spi_tx->DmaBaseAddress->ISR >> shift;
    if ((isr & DMA_ISR_TEIF1) != 0U) {
        line_dma_off();
        disp_dma_tx_error();
    } else if ((isr & DMA_ISR_TCIF1) != 0U) {
        g_hdma_spi_tx->DmaBaseAddress->IFCR = DMA_ISR_GIF1 << shift;
        disp_dma_tx_complete();
    }
}

/* One row per call, converted here in thread context and only when
 * LCDUI_Service has found the RF path idle or with headroom -- the gate
 * v8.00 had per stripe. A run's first row opens its address window, up to
 * `max_pages` consecutive dirty pages, and the others continue the same RAMWR
 * with CS held: no main-loop turn between pages. */
static void flush_step(uint8_t max_pages)
{
    if (g_hdma_spi_tx == NULL) return;
    if (disp.flush_active == 0U) {
        if ((disp.dirty_pages == 0U) || (disp.dma_busy != 0U)) return;
        uint8_t p = 0U;
        while ((disp.dirty_pages & (1UL << p)) == 0U) p++;
        uint8_t n = 1U;
        while ((n < max_pages) && ((uint8_t)(p + n) < GFX_PAGES) && ((disp.dirty_pages & (1UL << (p + n))) != 0U)) n++;
        disp.run_page = p;
        disp.run_rows = (uint8_t)(n * PAGE_LINES);
        disp.line_prep = 0U;
        disp.line_sent = 0U;
        disp.line_done = 0U;
        disp.flush_active = 1U;
        page_window(p, n);
    }
    /* Row n goes into the buffer row n - LINE_BUFS left: free once sent. */
    const uint8_t prep = disp.line_prep;
    if ((prep < disp.run_rows) && ((uint8_t)(prep - disp.line_done) < LINE_BUFS)) {
        GFX_PrepareLines((uint16_t)(disp.run_page * 8U + prep), 1U, disp.line_buf[prep % LINE_BUFS]);
        __DMB(); /* the row is in memory before the interrupt can count it */
        disp.line_prep = (uint8_t)(prep + 1U);
    }
    if ((disp.dma_busy == 0U) && (disp.line_sent < disp.line_prep)) {
        const uint32_t pm = enter_critical();
        const uint8_t ok = (uint8_t)((disp.dma_busy != 0U) || (disp.line_sent >= disp.line_prep) ||
                                     start_line_dma());
        exit_critical(pm);
        if (!ok) {
            tft_unselect();
            disp_fail();
        }
    }
}

/* End the run at the page being converted, for a frame that takes over
 * (LCDUI_Preempt): what is converted goes out, the rest stays dirty. */
static void run_cut(void)
{
    if (disp.flush_active == 0U) return;
    const uint32_t pm = enter_critical();
    const uint8_t end = (uint8_t)((disp.line_prep + PAGE_LINES - 1U) / PAGE_LINES * PAGE_LINES);
    if (end < disp.run_rows) {
        disp.run_rows = end;
        if ((disp.dma_busy == 0U) && (disp.line_done >= end)) end_run(); /* already there */
    }
    exit_critical(pm);
}

/* Stop a row in flight so blocking writes can take the bus (boot log
 * written from Error_Handler while the UI was running). */
static void abort_dma(void)
{
    if ((disp.dma_busy != 0U) && (g_hdma_spi_tx != NULL)) {
        const uint32_t pm = enter_critical(); /* the interrupt chains rows */
        line_dma_off();
        exit_critical(pm);
        CLEAR_BIT(SPI4->CR2, SPI_CR2_TXDMAEN);
        while ((SPI4->SR & SPI_SR_BSY) != 0U) {
        }
        tft_unselect();
    }
    disp.dma_busy = 0U;
    disp.flush_active = 0U;
}

/* ---- Rotation ----------------------------------------------------------- */

static void apply_rotation(void)
{
    disp.rot_pending = 0U;
    if (disp.ok == 0U) return;
    tft_madctl();
    if (disp.boot_active) {
        flush_blocking(ALL_PAGES);
    } else {
        disp.dirty_pages = ALL_PAGES;
        disp.flush_active = 0U;
    }
}

void LCDUI_SetRotation(uint8_t rot270)
{
    rot270 = rot270 ? 1U : 0U;
    if (rot270 == disp.rot270) return;
    disp.rot270 = rot270;
    disp.rot_pending = 1U;
    if ((disp.dma_busy == 0U) && (disp.flush_active == 0U)) apply_rotation();
}

uint8_t LCDUI_Rotation(void) { return disp.rot270; }

/* ---- Boot log ------------------------------------------------------------ */

#define BOOT_ROWS 24U /* two columns of UI_DrawBootRow */

void LCDUI_BootBegin(void)
{
    if (disp.ok == 0U) return;
    disp.boot_active = 1U;
    disp.boot_row = 0U;
    disp.boot_worst = 1U;
    UI_DrawBootHeader();
    flush_blocking(changed_pages());
}

uint8_t LCDUI_BootWorst(void) { return disp.boot_worst; }

uint16_t LCDUI_FlushMs(void) { return disp.flush_ms; }

void LCDUI_BootStep(const char *name, uint8_t status, const char *detail)
{
    if (disp.ok == 0U) return;
    abort_dma();
    if ((disp.boot_active == 0U) || (disp.boot_row >= BOOT_ROWS)) {
        disp.boot_active = 1U;
        disp.boot_row = 0U;
        UI_DrawBootHeader();
    }
    if (status == 0U) disp.boot_worst = 0U;
    else if ((status != 1U) && (disp.boot_worst != 0U)) disp.boot_worst = 2U;
    UI_DrawBootRow(disp.boot_row++, name, status, detail);
    flush_blocking(changed_pages());
}

/* ---- Panel bring-up -------------------------------------------------------- */

static void disp_try_init(uint32_t now)
{
    static const uint8_t pixfmt = 0x55U;
    disp.ok = 0U;
    disp.retry_at = now + DISP_RETRY_MS;

    HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(20U);
    HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(120U);
    tft_write_cmd(ST7789_SWRESET);
    HAL_Delay(150U);
    tft_write_cmd(ST7789_SLPOUT);
    HAL_Delay(120U);
    tft_write_cmd_data(ST7789_COLMOD, &pixfmt, 1U);
    tft_madctl();
    /* No INVON and RGB order (GFX_Madctl): this panel is not the inverted IPS
     * type. v7.x sent INVON with the BGR bit, which showed every colour
     * inverted with red and blue swapped -- black as white. */
    tft_write_cmd(ST7789_NORON);
    HAL_Delay(10U);
    tft_write_cmd(ST7789_DISPON);
    HAL_Delay(20U);

    disp.ok = 1U;
    disp.dma_busy = 0U;
    disp.flush_active = 0U;
    disp.rot_pending = 0U;
    /* Whatever the panel holds now is unknown: send every page. */
    memset(disp.page_hash, 0, sizeof(disp.page_hash));
    disp.dirty_pages = ALL_PAGES;
    disp.rebuild_pending = 1U;
}

void LCDUI_Init(DMA_HandleTypeDef *hdma_spi_tx)
{
    g_hdma_spi_tx = hdma_spi_tx;
    memset(&disp, 0, sizeof(disp));
    disp.boot_worst = 1U; /* no panel, no boot log: nothing failed on it */
    HAL_Delay(DISP_PWR_MS);
    GFX_Clear(GFX_BG);
    disp_try_init(HAL_GetTick());
    (void)changed_pages();
    flush_blocking(ALL_PAGES);
}

/* ---- Cooperative service ------------------------------------------------- */

/* The display may only take the CPU while the RF render queue has headroom. */
static uint8_t display_rf_busy(uint32_t now, uint32_t *reserve, uint32_t *render_count, uint32_t *config)
{
    modulator_status_fast_t st;
    MOD_GetStatusFast(&st);
    const uint8_t cw_mode = (st.mod_type == MOD_TYPE_CW) ? 1U : 0U;
    const uint8_t tx_test = (st.config == MOD_CFG_TX_CARRIER_TEST) ? 1U : 0U;
    const uint8_t internal_bulk = (uint8_t)((st.internal_source != 0U) && !cw_mode && !tx_test);
    *reserve = internal_bulk ? DISP_RSV_INT : DISP_RSV_EXT;
    *render_count = st.render_count;
    *config = (uint32_t)st.config;
    if (internal_bulk) return 1U;
    if (st.transport_started == 0U) return 0U;
    return (uint8_t)(((st.usb_audio_seen != 0U) &&
                      ((now - st.last_usb_audio_tick_ms) <= USB_AUDIO_CRITICAL_HOLDOFF_MS)) ||
                     ((now - st.last_audio_tick_ms) <= USB_AUDIO_CRITICAL_HOLDOFF_MS));
}

static void build_frame(uint32_t now, uint32_t config, uint8_t *ui_dirty,
                        lcdui_build_fn_t build_fn, void *ctx)
{
    disp.last_mode = config;
    disp.last_build = now;
    disp.rebuild_pending = 0U;
    disp.preempt = 0U;
    disp.boot_active = 0U;
    if (ui_dirty != NULL) *ui_dirty = 0U;
    if (build_fn != NULL) build_fn(ctx, now);
    const uint32_t changed = changed_pages();
    disp.dirty_pages |= changed;
    SCTRL_LinkScreenChanged(changed);
    disp.flush_start = now;
}

/* Milliseconds of row sending one LCDUI_Service call may spend while the RF
 * path is idle (HAL ticks: 10 is 9-10 ms). Receiving, the worker and the
 * headphone renderer leave the main loop about a tenth of the CPU; the rest
 * of the loop waits at most this long, the CW decoder's ring holds 128 ms. */
#define LCD_FLUSH_BUDGET_MS 10U

void LCDUI_Preempt(void) { disp.preempt = 1U; }

uint8_t LCDUI_LightReady(void)
{
    return (uint8_t)(disp.ok && !disp.boot_active && !disp.dma_busy && !disp.flush_active);
}

void LCDUI_LightFlush(uint8_t changed)
{
    if (disp.ok == 0U) return;
    if (changed && (disp.flush_active == 0U) && (disp.dma_busy == 0U)) disp.dirty_pages |= changed_pages();
    /* Keyed: a page at a time, as v8.00, so the cue waits one page at most
     * -- a run started while receiving ends at the page it is on. */
    run_cut();
    flush_step(1U);
}

void LCDUI_Service(uint32_t now,
                   uint8_t *ui_dirty,
                   uint16_t idle_refresh_ms,
                   uint16_t busy_refresh_ms,
                   lcdui_build_fn_t build_fn,
                   void *ctx)
{
    MOD_CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_LCD_SERVICE);

    if (MOD_AudioRealtimeCritical(now) != 0U) {
        MOD_CPU_DIAG_FINISH(MOD_CPU_DIAG_LCD_SERVICE, cpu_diag_start);
        return;
    }
    if (disp.ok == 0U) {
        if ((int32_t)(now - disp.retry_at) >= 0) disp_try_init(now);
        MOD_CPU_DIAG_FINISH(MOD_CPU_DIAG_LCD_SERVICE, cpu_diag_start);
        return;
    }

    uint32_t reserve, render_count, config;
    const uint8_t rf_busy = display_rf_busy(now, &reserve, &render_count, &config);
    const uint8_t headroom = (uint8_t)((rf_busy == 0U) || (render_count >= reserve + 2U));
    const uint8_t idle = (uint8_t)((disp.dma_busy == 0U) && (disp.flush_active == 0U));

    if ((disp.rot_pending != 0U) && idle) apply_rotation();

    const uint8_t force = (uint8_t)(((ui_dirty != NULL) && (*ui_dirty != 0U)) || (disp.last_mode != config));
    const uint32_t period = rf_busy ? busy_refresh_ms : idle_refresh_ms;
    if (force || ((period != 0U) && ((now - disp.last_build) >= period))) disp.rebuild_pending = 1U;
    /* A frame the operator asked for (LCDUI_Preempt) takes over at the next
     * page boundary: what is left of the frame being sent stays queued and
     * goes out with the new one's changes, top page first. Waiting out the
     * whole previous frame put a scrolled menu a frame behind the knob. */
    const uint8_t preempt = (uint8_t)((disp.preempt != 0U) && (disp.rebuild_pending != 0U) && headroom);
    const uint8_t run_max = rf_busy ? 1U : (uint8_t)GFX_PAGES;
    if (preempt) run_cut();
    if (preempt && (rf_busy == 0U)) {
        const uint32_t t0 = HAL_GetTick();
        while ((disp.flush_active != 0U) && (disp.ok != 0U) && ((uint32_t)(HAL_GetTick() - t0) < LCD_FLUSH_BUDGET_MS)) {
            flush_step(run_max); /* the cut run's remaining rows */
        }
    }
    /* Rebuild only between pages: the framebuffer is also the send buffer. */
    const uint8_t between = (uint8_t)((disp.dma_busy == 0U) && (disp.flush_active == 0U));
    if ((disp.rebuild_pending != 0U) && between && ((disp.dirty_pages == 0U) || preempt) && headroom) {
        build_frame(now, config, ui_dirty, build_fn, ctx);
    }
    /* A takeover still waiting for its page boundary starts no new page. */
    const uint8_t hold = (uint8_t)((disp.preempt != 0U) && (disp.rebuild_pending != 0U) && (disp.flush_active == 0U));

    if (!hold && ((rf_busy == 0U) || (render_count >= reserve))) flush_step(run_max);
    /* Receiving, the main loop has the time: convert row after row here as
     * buffers come free, for up to LCD_FLUSH_BUDGET_MS a call. One transfer a
     * pass, as under TX, took a whole-screen change -- a settings menu
     * category -- 200-240 ms to reach the panel, and the menu lagged the knob
     * by that much (bench, 2026-10-01). */
    if ((rf_busy == 0U) && !hold) {
        const uint32_t t0 = HAL_GetTick();
        while ((disp.flush_active != 0U || disp.dirty_pages != 0U) && disp.ok != 0U &&
               (uint32_t)(HAL_GetTick() - t0) < LCD_FLUSH_BUDGET_MS) {
            flush_step(run_max);
        }
    }
    MOD_CPU_DIAG_FINISH(MOD_CPU_DIAG_LCD_SERVICE, cpu_diag_start);
}

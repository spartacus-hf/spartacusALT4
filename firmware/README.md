# Firmware internals

Current firmware: `v9.00`.

This directory is the STM32CubeIDE project of the SPARTACUS-HF REV1
firmware. The project and its ELF are named `alt2`, the firmware's
development name. Building, installing a release and the documentation index
are in the [repository README](../README.md); this file is the developer's
guide to the firmware's structure and rules.

- Install and recovery: [REV1 USB firmware update](docs/USB_FIRMWARE_UPDATE.md).
  For ST-LINK, use **alt2 Factory Debug** once to install the loader and
  footer, then **alt2 Debug** for later application flashes. The application
  ELF alone cannot boot after a hardware reset.
- A radio still on v8.08 or earlier (flash layout 1) needs a one-time move
  first, which keeps its settings and calibration: see
  [Moving to layout 2](docs/USB_FIRMWARE_UPDATE.md#moving-to-layout-2-v809).
  Do not flash **alt2 Debug** or `rev1_recovery.*` onto such a radio first.
- First power-up, the self test and the diagnostic console:
  [REV1 bring-up](docs/BRINGUP.md).
- The board contract: [REV1](docs/REV1.md).

The RF/audio path has priority over every UI task. LCD/menu work must remain
cooperative, low priority, and bounded so it cannot starve USB audio, render
FIFO refill, TIM6/HRTIM DMA, CW timing, TX TEST gating, or carrier retune
safety.

## Current Architecture

- `Core/Src/main.c` owns hardware init and the cooperative main loop. RF rendering runs in priority-2 PendSV so UI and storage cannot starve it.
- USB audio enters through `USB_Device/App/usbd_audio_if.c`, converts stereo S16_LE to selected mono mode, and submits frame-numbered packets through `MOD_SubmitUsbPacket()`.
- The audio personality is composite, as QMX: `USB_Device/App/usbd_cat.c` adds two CDC-ACM ports beside the sound card; `Core/Src/cat.c` (with the host-tested `cat_logic.c`) speaks QMX's Kenwood TS-480 CAT on them, or the console after a line ending. USB audio transmits only during CAT transmit unless `USB VOX` is on (docs/REV1.md, "USB serial ports").
- `Core/Src/mod_transport.c` owns the PCM ring, render FIFO, TIM6 scheduler, direct phase-checked DMA IRQ path, hard lockout, and RF_EN alignment with accepted DMA frames.
- Production builds compile deep diagnostics out with `MOD_DEEP_DIAG_ENABLE=0`; `tools/build_diagnostic.sh` creates the separately flashed O2 diagnostic image.
- `Core/Src/modulator.c` initializes the modulation subsystem;
  `Core/Src/mod_tx_pipeline.c` owns SSB/FM/AM/TX TEST rendering and DSP policy,
  while `Core/Src/mod_transport.c` owns RF-critical scheduling.
- `Core/Src/mod_cw_keyer.c` is the hardware-free keyer core (one step per 48 kHz sample, integer timing, Morserino-32-style memories per mode, the 1 ms lever sampler and the radio memories' sequencer); `tools/tests/cw_keyer_test.c` drives the same code. `Core/Src/mod_cw.c` runs it on the radio: the SysTick sampler, the paddle guard, timing published between marks, render_cw and the sidetone keyer, the trainer's RF inhibit, SPOT and the memories.
- `Core/Src/mod_cw_player.c` plays the trainer's prompts, feedback beeps and SPOT on the sidetone only (stepped by the headphone renderer, never by render_cw). `Core/Src/mod_morse_trainer.c` is the trainer's controller, `Core/Src/cw_pack.c` reads its SD pack (`CWPACK.BIN`, `tools/gen_cw_pack.py`), `Core/Src/ui_trainer.c` draws its page. While the trainer owns the keyer nothing it keys reaches RF. Operator guide: `docs/CW_TRAINER.md`.
- `Core/Src/sctrl_link.c` is the S-CTRL LINK (v8.11): `link 1` turns CAT port 1 of the audio personality into a framed channel (SLIP + CRC-16) for the S-CTRL desktop app -- STATUS at 10 Hz, the screen mirror (changed pages, PackBits), the waterfall line by line, console and CAT tunnels, panel input, and keying under a transmit guard (ARM, heartbeat, key-down caps, one owner). Its pure logic is `sctrl_link_logic.c` (host suite `sctrl_link_logic`); packets leave from the SOF interrupt, at most two a frame in its first 600 us. Protocol: [`docs/SCTRL_LINK.md`](docs/SCTRL_LINK.md); the app: github.com/spartacus-hf/spartacusControl.

## Pipeline Loss Protection

The production image detects loss only at boundaries that already execute: USB hardware IRQ/SOF/packet delivery, PCM and render queue operations, DMA half/full/error IRQs, rendered HRTIM compare validation, HRTIM fault IRQs, and a constant-time 1 ms DMA progress check. It does not add per-sample tracing, CRC passes, background DMA polling, logging, or diagnostic runtime branches. The direct RF DMA handler verifies flag order, inactive-half phase, paired-channel CNDTR skew, and remaining refill slack before accepting a frame.

One or two consecutive missing/malformed USB frames are replaced by a static 48-sample silence frame and reported immediately. A third consecutive USB failure, any internal queue/sequence failure, DMA anomaly, invalid HRTIM compare, or HRTIM/USB hardware fault inhibits RF (driver inputs forced GPIO-low, PC6/PC10 parked high), stops TIM6/DMA, parks valid idle compares, and latches a global LED/LCD notification. Hold the knob or SELECT for one second to acknowledge. Hard faults rearm only after carrier geometry, render reserve, and—when USB is the source—three valid USB frames are healthy again.

`tools/build_diagnostic.sh` builds the optional `Diagnostic/alt2.elf` with deep CPU/DWT and sample diagnostics compiled in at `-O2`. Normal Debug and Release-Speed images default to `MOD_DEEP_DIAG_ENABLE=0`; diagnostic implementation and hot-path call sites preprocess out. Firmware can observe the digital pipeline through HRTIM compare delivery. It cannot prove the analog RF waveform after the MCU pins without external feedback hardware.
- `Core/Src/ui_gfx.c` is the 320x240 landscape frame: a 1-bit framebuffer plus one colour attribute per 8x8 tile, clipped text in the fonts generated by `tools/gen_ui_fonts.py`, and the RGB565 stripe conversion. `Core/Src/ui_lcd.c` is the ST7789 driver: landscape rotation (90/270), changed-page detection by hash, 2-line low-priority SPI DMA stripes, and the boot log.
- `Core/Src/ui_nav.c` owns navigation (home zones: tuning, picker and settings row; menu, pages, unified mode, the coalesced tuning applier); `ui_tune.c` holds its pure arithmetic and the settings-row contents; `ui_diag.c` turns live status into the worded diagnostics summary and pages. `ui_menu.c` is the category model over the setting groups of `ui_settings_map.c`. `ui_render.c` (home, widgets, fault, boot log), `ui_render_menu.c` and `ui_render_pages.c` draw.
- `Core/Src/ui_waterfall.c` is the WATERFALL page (opened only from the settings row's second cell): layouts, the colour raster behind `ui_gfx.c`'s band, the frequency scale and its settings list. `Core/Src/radio_spectrum.c` is its engine -- capture requests to the receive worker, the 512-point FFT of `radio_spectrum_dsp.h`, averaging, the 4-bit history, the experimental LO-hopping scanner and the TX guard -- and runs only while the page is open.
- `Core/Src/ui_input.c` turns the knob (turn, hold-and-turn, click, hold) and the three panel buttons into one event set per pass. `Core/Src/board_rev1.c` samples the encoder at 1 kHz from `SysTick_Handler`.
- **Settings and calibration store** (v8.09): `Core/Src/mod_sd_store.c` keeps them in raw sectors 256-767 of the SD card, before the first partition and outside every file system, as two copies of every record; `mod_storage.c` (settings, autosave, event log), `mod_cal_storage.c` (calibration per band, sideband and analytic backend) and `mod_storage_import.c` (the one-time v8.08 import, backups and restores) sit on it. The MCU's flash holds no settings any more. See *Storage And Calibration* below.
- **SD Card Subsystem**: `Core/Src/mod_sdcard.c` provides the SPI1 block driver and `sd_diskio.c` bridges it to official FatFs R0.16. `sd_file_browser.c` implements paged UTF-8 LFN browsing, nested file management, streamed viewers, and incremental recursive jobs. `sd_audio_decoder.c` and `sd_audio_player.c` decode local PCM WAV and MP3 into the same frame-aligned RF transport used by USB audio; a `.flac` file is still listed and labelled by the browser but is not playable since v7.10 freed that decoder's flash for the receive bench. `ui_sd.c` owns the SD pages; the knob maps onto their button grammar. See [`docs/REV1.md`](docs/REV1.md) for the active board contract.

## ⇢⇢ SD Card Framework

### Overview

The SD card is on a dedicated SPI1 bus and accessed through a three-layer stack:

```
[Application Code]                       [Settings store — mod_sd_store.c]
       │ f_open / f_read / f_write ...           │ raw sectors 256-767, no FatFs
       ▼                                         │
[FatFs — ff.c]          Middlewares/Third_Party/FatFs/src/
       │ disk_read / disk_write / disk_ioctl     │
       ▼                                         │
[diskio bridge — sd_diskio.c]                    │      [USB MSC — usbd_msc_if.c]
       │ SD_ReadBlock / SD_WriteBlock  ◄─────────┘──────── (refuses writes to 256-767)
       ▼
[SPI1 block driver — mod_sdcard.c]  CRC7 on commands, CRC16 on data (CMD59), retries
       │ SPI1 direct-register access
       ▼
[SD Card Hardware]
```

The card's layout: the MBR at LBA 0, the radio's system area at LBA
256-767 (two 256-sector copies, A and B, of the settings and calibration
store), the FAT volume from LBA 2048 (the in-radio formatter; a computer's
formatter usually starts at 2048 or 8192). A card whose MBR is missing, GPT,
or has a partition over the system area reads `CARD LAYOUT - FORMAT IT`; the
in-radio format fixes it (some small cards come from the factory with their
partition starting before sector 768).

### Pin Mapping

| Signal   | MCU Pin | Function              | Notes |
|----------|---------|-----------------------|-------|
| SD CS    | PA4     | GPIO output (manual)  | Active low. Toggled per-command frame by software. |
| SD SCK   | PA5     | SPI1_SCK (AF5)        | REV1 SD clock. |
| SD MISO  | PA6     | SPI1_MISO (AF5)       | REV1 SD data input. |
| SD MOSI  | PA7     | SPI1_MOSI (AF5)       | REV1 SD data output. |
| SD VCC   | 5V rail | —                     | For the pictured adapter module with onboard LDO. |
| SD GND   | GND     | —                     | |

### REV1 LCD bus

The LCD uses SPI4 (PE12 SCK, PE14 MOSI, AF5), with PE10 reset, PE11 chip
select, PE13 data/command and PB9 TIM17 backlight (AF1). It has no shared
signals with the SD bus.

### SPI1 Configuration

- **Bus**: SPI1, clocked from the REV1 APB2 clock.
- **Mode**: Master, full-duplex, 8-bit, MSB first, CPOL=0/CPHA=0, software NSS.
- **Init speed**: PA5/PA6/PA7 are temporarily switched to GPIO and bit-banged below 400 kHz for the SD SPI-mode reset/identification sequence.
- **Data speed**: /8 prescaler = 21.25 MHz (conservative; SD cards in SPI mode typically support up to 25 MHz. RM0440 requires SPE=0 before changing BR bits — this is done correctly in `sd_set_speed()`).
- **Access method**: Direct register access (`SPI1->DR`, `SPI1->SR`, `SPI1->CR1`). No HAL SPI API, no interrupts, no DMA.

### Safety: Audio/RF Pipeline Protection

The SD card subsystem is designed to be **completely invisible** to the audio-to-RF pipeline:

| Guarantee | How |
|-----------|-----|
| **No interrupts** | SPI1 uses polling only (`while (SPI1->SR & flag) {}`). No SPI1 interrupt is enabled anywhere. |
| **No DMA** | All SD transfers are blocking byte-by-byte polling. No DMA channel is allocated for SPI1. |
| **No ISR contamination** | No SD code runs in an RF, audio or DMA interrupt, a USB audio callback (`USBD_AUDIO_*`) or the HRTIM fault handler. The one exception is USB mass storage in USB DISK mode, which reads and writes blocks from the USB interrupt; since v8.09 that interrupt drops to priority 3 in disk mode (below PendSV's receiver and the audio DMA) and returns to 1 on exit. |
| **Deadlines that run in interrupts** | Every driver deadline counts DWT cycles, not `HAL_GetTick` (SysTick stands still inside the USB interrupt), so a stuck card times out in mass storage too. |
| **Checked transfers** | Every command carries its CRC7; CMD59 turns the card's checks on, and the driver checks the CRC16 of every block it reads. A write is followed by CMD13. A failed transfer is retried (3 attempts: a CRC error again at once; a timeout or card error once more, then after a re-initialisation; never inside a local-audio read's 4 ms budget, never one the real-time gate refused). `sd stats` counts it all (with the card's last refusal: `reject=` stage, bytes, block) and `LOGS/EVENTS.LOG` gets an `SD` line per re-initialisation and per transfer that failed for good. A transfer the real-time gate cuts short leaves nothing behind: SPI1's receive FIFO is emptied at every command and after every abort or timeout (a byte left there once made every later write fail until a reset), and before the card is initialised again -- also at the first initialisation after any reset -- it is clocked through the rest of a block it may still be sending (`sd stats` `aborts`). |
| **Foreground-only execution** | Browser, viewer, format, and file jobs run only from the non-critical main-loop path. Local playback is the sole exception: it performs one bounded foreground read only through `MOD_LocalAudioIoBegin()` after transport headroom and render/DMA state have been checked. |
| **Controlled probing only** | SD card init and FAT mount happen during boot before the IWDG watchdog starts. Later re-probe is explicit through the SD UI refresh action, the NO SD CARD boot hold (once a second) and, with the card as the settings store, the autosave's remount after a lost card (every 3 s, outside TX; a card that is there but does not answer is tried less and less often, up to 30 s apart, and one that takes no writes once a minute). SD_Init feeds the watchdog at each of its bounded stages, and a mount initialises the card once (v8.09's first remount ran it twice and reset the radio when a card went back in). |
| **Blocking calls are opt-in** | All FatFs operations (`f_open`, `f_read`, `f_write`, etc.) are synchronous and block the main loop. Application code MUST NOT call them while transmitting. Typical write latency is <50 ms for a few sectors; worst-case SD internal write can take up to 500 ms (bounded by `SD_WRITE_TIMEOUT_MS`). |
| **Isolated SPI bus** | SPI1 (SD) and SPI4 (LCD) are independent. SD activity cannot stall LCD DMA or the RF/audio buses. |

**Rule for future developers**: Never call any `SD_*`, `disk_*`, or `f_*` function from:
- Any interrupt handler (HRTIM, TIM6, DMA, USB, EXTI)
- `MOD_ServiceBackground()`
- Any render function (`render_ssb_*`, `render_cw`, `render_tx_carrier_test`)
- `MOD_SubmitUsbPacket()` or the USB audio data path
- `rfifo_pop_to_dma()` or any DMA callback
- Any code path reachable while `MOD_AudioRealtimeCritical()` returns true, except the bounded local-player read scope already implemented in `sd_audio_player.c`

The only safe call sites are: the main loop's non-critical section, boot init, UI event handlers, and the existing guarded local-player read scope.

### SD Card Initialization Sequence

The driver follows the standard SD SPI initialization per the Physical Layer Simplified Specification:

1. **Power-on**: 80 dummy clocks with CS high (≥74 required by spec).
2. **CMD0** (GO_IDLE_STATE): Puts the card into SPI mode. R1=0x01 expected.
3. **CMD8** (SEND_IF_COND): VHS=0x1 (2.7-3.6V), check pattern=0xAA. If R1=0x01, card is SDv2 — read R7 to verify voltage/pattern. If R1=0x05, card is SDv1/MMC — fall back to CMD1/ACMD41.
4. **ACMD41** loop (CMD55 + ACMD41): Send with HCS=1 for SDv2 (supports SDHC/SDXC) or HCS=0 for SDv1. Poll up to 200 times with 4ms delays (~800ms max). Exit when R1=0x00 (card ready).
5. **CMD58** (READ_OCR): Read OCR to check CCS bit (bit 30). If set, card is SDHC/SDXC (block-addressed). If not set, issue CMD16 to set block length to 512 bytes.
6. **CMD9** (SEND_CSD): Read CSD to determine capacity. CSD v2.0: capacity = (C_SIZE+1)×1024 sectors. CSD v1.0: capacity = (C_SIZE+1)×MULT×(BLEN/512). The CSD's CRC16 is checked.
7. **CMD59** (CRC_ON_OFF, since v8.09): the card checks the CRC of every command and written block from here on. A card that refuses still works, without the checks (`sd stats` `crc_refused`). The card keeps this across an MCU reset, so the loader sends real CRC7s too.
8. **Speed switch**: Disable SPI (SPE=0), wait for BSY, change BR bits to /8 (21.25 MHz), re-enable SPI.

**Card type detection**:
- `SD_TYPE_SDV1`: Pre-2006 cards, byte-addressed, ≤2 GB.
- `SD_TYPE_SDV2`: SDv2 cards ≤2 GB (block-addressed after CMD16).
- `SD_TYPE_SDV2HC`: SDHC/SDXC cards >2 GB (block-addressed natively).

**Known limitation**: Some ultra-new SDXC cards (UHS-I class, post-2020) may not support SPI mode at all per the SD specification. These cards will fail CMD0 and return `NO CARD` on boot. SDSC and SDHC cards are universally supported.

### FatFs Filesystem

This firmware vendors official Elm-Chan FatFs R0.16. Its embedded configuration is:

| Feature | Decision | Rationale |
|---------|----------|-----------|
| FAT type | FAT16/FAT32 | Both are mounted; factory format creates FAT32. |
| exFAT | Not supported | Would add ~6 KB flash and complex directory handling. Cards >32 GB ship with exFAT but can be reformatted to FAT32. |
| Long filenames (LFN) | UTF-8 enabled | Names are preserved internally up to 255 bytes; supported Latin glyphs are displayed and unsupported glyphs use `?`. |
| Memory profile | Tiny/static LFN | One sector window and bounded static workspaces; no heap is used by application code or codecs. |
| RTC timestamps | Not used | `FF_FS_NORTC=1`; newly created files use the configured fixed build-year date. |
| Thread safety | None | Single-threaded access from main loop only. No `FF_FS_REENTRANT`. |

**Internal design notes**:

- `f_open` stores the directory entry's byte offset within its sector in `fp->dir_ptr` (cast to pointer). `f_sync` recovers this offset to update the correct entry — it does NOT search by name. This is critical for correctness when multiple files share the same directory sector.
- `alloc_cluster()` scans the FAT linearly from the last-allocated cluster, wrapping around. This is simple but slow for large FATs. Acceptable for embedded use where files are usually small and few.
- `remove_chain()` walks a cluster chain marking each entry as free (0x00000000). No orphan cleanup needed since each FAT entry is independently freed.

### Directory Entry Layout (FAT32, 32 bytes per entry)

```
 Offset  Size  Field
 ------  ----  -----
  0      11    Short filename (8.3, space-padded, uppercase)
 11       1    Attributes (0x10=dir, 0x20=archive, 0x01=read-only, 0x0F=LFN)
 12       1    Reserved (NT flags)
 13       1    Create time tenths
 14-15    2    Create time
 16-17    2    Create date
 18-19    2    Last access date
 20-21    2    First cluster HIGH (FAT32 only)
 22-23    2    Last write time
 24-25    2    Last write date
 26-27    2    First cluster LOW
 28-31    4    File size (bytes)
```

### File Structure Summary

| File | Role | Lines |
|------|------|-------|
| `Core/Inc/mod_sdcard.h` | Public API for the SPI1 block driver | ~80 |
| `Core/Src/mod_sdcard.c` | SPI1 direct-register SD driver: init, read/write blocks, CRC7/16, CMD59/CMD13, retries, DWT deadlines, speed switching, CSD parsing | ~1150 |
| `Core/Inc/mod_sdcard_policy.h` | The driver's retry decisions and CRC helpers, pure for the host suite | ~50 |
| `Core/Inc/mod_sdcard_service.h` | Background service API, file-manager helpers, and audio-plan staging | ~160 |
| `Core/Src/mod_sdcard_service.c` | Global `FATFS g_sd_fs`, init/mount, chunked FAT32 format, the store's device hooks, file cache | ~1050 |
| `Core/Src/mod_sd_store.c` | Settings and calibration records in the system area: two copies, CRC, UID, scrub | ~540 |
| `Core/Src/mod_storage_import.c` | v8.08 import (flash dump and card files), store backup and restore | ~670 |
| `Core/Src/sd_diskio.c` | diskio bridge: `disk_read`/`disk_write`/`disk_ioctl` → `SD_ReadBlock`/`SD_WriteBlock` | ~90 |
| `Middlewares/Third_Party/FatFs/src/ff.h` | FatFs public API: `f_mount`, `f_open`, `f_read`, `f_write`, etc. | ~170 |
| `Middlewares/Third_Party/FatFs/src/ff.c` | Minimal FAT32 implementation | ~1050 |
| `Middlewares/Third_Party/FatFs/src/ffconf.h` | FatFs configuration | ~65 |
| `Middlewares/Third_Party/FatFs/src/diskio.h` | Disk I/O interface | ~80 |
| `Middlewares/Third_Party/FatFs/src/integer.h` | FatFs integer type definitions | ~30 |

### Usage Example

```c
// Include the header
#include "mod_sdcard_service.h"

// The global FATFS object (declared in mod_sdcard_service.c)
extern FATFS g_sd_fs;

// ---- Settings and calibration: the store in the system area ----
MOD_SaveSettings();             // settings record (slot 0) and the band's calibration
                                // record, each to the copy without the newest version,
                                // read back before it counts; autosave calls it too
MOD_StorageBackup();            // SYSTEM/STOREBAK.BIN, built as .TMP and renamed
SD_StoreRead(slot, buf, cap, &len, &seq);   // the newest valid copy of a record

// ---- Factory-format SD card ----
SD_FormatFactoryStart();        // background FAT32 format; pump SD_ServiceBackground()

// ---- Listing directory ----
DIR dir;
FILINFO info;
if (f_opendir(&dir, "") == FR_OK) {
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
        // info.fname = 8.3 filename, info.fsize = size in bytes
    }
    f_closedir(&dir);
}

// ---- Checking space ----
DWORD free_clusters;
if (f_getfree("", &free_clusters, NULL) == FR_OK) {
    // free_clusters * sectors_per_cluster * 512 = free bytes
}
```

### Adding SD Features: Rules for Developers

1. **Always check `SD_IsMounted()`** before calling any FatFs function. No card = no filesystem.
2. **Always check return values**: Every `f_*` function returns `FRESULT`. Ignoring errors will cause silent data corruption (e.g., writing to a full card returns `FR_DENIED`).
3. **Never call FatFs from an ISR or audio/RF path**: See the safety guarantees section above.
4. **Close files after use**: `f_close()` writes back the directory entry with final file size. If you don't close, the file will have size 0 or stale metadata.
5. **Format cards as FAT32**: Cards >32 GB ship with exFAT. Use a PC to reformat them as FAT32. The firmware will detect but refuse to mount non-FAT32 filesystems (`FR_NO_FILESYSTEM`).
6. **8.3 filenames only**: `BACKUP.CFG` works. `backup_config_2024.cfg` will be silently truncated to `BACKUP~1.CFG` or rejected.
7. **One file open at a time**: The current FIL object design supports only one open file. Opening a second file with the same FIL object abandons the first.
8. **Write latency**: An `f_write` of N sectors blocks the main loop for N×~2ms (SPI transfer) + up to 500ms (SD internal write). Do not write during TX. Consider splitting large writes into small chunks with `MOD_ServiceBackground()` calls between them.
9. **Power-loss safety**: The FAT filesystem is NOT journaled. Pulling power during an `f_write` or `f_close` can corrupt the FAT or directory. Always close files and sync before power-off. The bootloader screen shows SD status — wait for "OK" before removing power. None of this reaches the settings and calibration: they are in the store outside the file system, written one copy at a time (see *Storage And Calibration*). Keep new persistent radio state in the store, not in files.
10. **Format programmatically through the service only**: use `SD_FormatFactoryStart()`. It writes a FAT32 MBR/VBR in background chunks; do not call block writes directly from UI code.

## RF And Timing Rules

- HRTIM compare DMA and TIM6 scheduling are the hard realtime path.
- The current two-channel MCMP DMA path has a documented atomic-update lab
  candidate and acceptance procedure in
  [`docs/hrtim_atomic_dma_experiment.md`](docs/hrtim_atomic_dma_experiment.md).
  Keep the production path unchanged until that procedure passes on hardware.
- DMA channels for HRTIM compare updates use priority `HIGH`; SPI LCD DMA uses priority `LOW`.
- CW and TX TEST are realtime-critical while keyed or recently keyed. LCD service exits before display work in those windows.
- TX TEST is paddle-gated, independently debounced for DIT/DAH, and uses a low fixed outphase carrier. CW also uses a low fixed carrier theta by default; the board RF gate (`BOARD_WriteRf`) following keying is the RF on/off authority, with only the shaped key edge allowed to use larger transient theta for click control.
- Outphasing is bounded by both mode policy and the selected HRTIM carrier geometry. SSB/AM are compressed when the band cannot represent the requested range; FM/CW/TX TEST automatically reduce fixed theta on high bands. The diagnostics page reports `VALID`, `LIMITED`, `COMPRESSED`, or `NOT REC`.
- `PA GEOMETRY` is the branch-polarity control for output transformers (WTST) that are not wound alike: `NORMAL` drives both push-pull pairs as wired, `A INVERTED` swaps PA8/PA9 at the HRTIM output level, turning A by 180 degrees in every drain-modulation mode. Inverting C instead would give the same combined output (only the absolute phase turns), so it is not offered. Judge it by supply current and output on the scope: the wrong polarity makes the branches fight, drawing current for little output.
- `THETA DIRECTION` picks which branch takes `+theta` when the branches outphase (`A +THETA`: A `+theta`, C `-theta`; `C +THETA` the reverse). It does nothing in `ENVELOPE` drain modulation, where both branches run in phase.
- Through v8.05 the PA WIRING MAP had nine entries and the debug menu per-branch sign/RF180 flags and a sign scope. The mirrored maps (the old default `A MIR`) negated one branch's theta, which leaves no amplitude control whatever the transformer polarity; the RF180 flags turned compare values that `ENVELOPE` mode discards; the sign flags conjugated one branch, which no wiring does. They are gone. Saved settings convert on load: a single swapped branch, or a single RF180 flag under `EXPERT`/`V633`, becomes `A INVERTED`, and everything else `NORMAL`.
- SSB/CW outphasing laws, branch routing, calibration, and RF gate decisions must not be changed as part of UI-only work. RF must be inhibited through `BOARD_PermitRf(0)`/`BOARD_EmergencyStop()` before retunes, HRTIM/DMA failures, reset paths, or unsafe carrier reconfiguration.
- Carrier retunes are idle-only. TX remains inhibited while a frequency/clock apply is pending, waiting, or applying.

## LCD/UI Constraints

- Display target is an ST7789-class 240x320 SPI LCD driven in **landscape, 320x240**. `SCREEN ROTATION` (DISPLAY) selects 90 or 270 degrees; it lives in `radio_config_t.boot_flags` bit 1, survives a settings factory reset and is applied right after the settings load. The portrait UI was removed in v8.00.
- Frame model: 1-bit framebuffer (9600 bytes) plus one foreground/background colour nibble per 8x8 tile (1200 bytes) over a fixed 16-colour palette. No RGB framebuffer. Two colours never share a tile, so every layout sits on the 8-pixel grid; text keeps 8 px from the glass edge and is clipped with an ellipsis, never drawn past its box.
- Fonts are generated tables (`tools/gen_ui_fonts.py`, checked by `--check` in validation): Terminus Bold 6x12 and 8x16, DejaVu Sans Condensed Bold digits for the frequency. Licences in `docs/THIRD_PARTY_FONTS.md`.
- Keep LCD updates cooperative. Frames are built only between flushes (the framebuffer is also the send buffer), only pages whose hash changed are sent, and `LCDUI_Service` yields to the RF path exactly as before. Each 2-line stripe is converted in thread context, one per service pass and only through the RF headroom gate; the SPI4 DMA interrupt only ends a transfer. Never draw from RF/DMA/USB interrupt context. SPI4 runs at HCLK/4 (42.5 MHz): a full frame is about 30 ms and the clock sits above HF.
- The knob is the primary control and the buttons must remain sufficient on their own (no encoder fitted). The FUNC key (default PC14, the second key on the encoder board) is BACK everywhere and does nothing on the tuning screen. Every screen's footer names what the knob and buttons do there.
- Colours: the phosphor palette in `ui_gfx.c` -- true black background, saturated greens for structure and captions (no greys), near-white text, a bright green frequency, cyan for focus, amber/red only for warnings and TX. Corners are square. The panel runs without INVON and in RGB order.
- Settings that write in place (volume, mic gain, USB/FM gain, drive, CW drive, WPM, backlight, tuning step) apply on every detent; anything that retunes or resets the modulator applies on SET, behind `MOD_BackgroundIoAllowed`. Cancel restores the value the edit started from.
- Menu: nine categories (DIAG adds a tenth with the diagnostic image's debug UI). Every `setting_id_t` must be reachable through them; `tools/tests/ui_menu_model_test.c` enforces that, and `ui_settings_policy_test.c` that each is in some group. REV1 commissioning stays under BOARD as ASSEMBLY, CONTROLS, FILTERS, BIAS + PROTECT, RX TRIM, POWER / PD and CALIBRATE.
- `python3 tools/ui_preview.py --out DIR` renders every screen on the host from the production UI sources, as PNGs of exactly the pixels sent to the panel. Look at it after any UI change.

## Main Loop Priority Order

```
while (1):
    poll buttons
    MOD_ServiceBackground()           // config apply, retune, CW guard, render trigger

    if MOD_AudioRealtimeCritical():    // CW/TX_TEST active OR USB audio streaming
        consume UI events              // process button presses only
        MOD_ServiceBackground() ×2     // service modulator (render, FIFO)
        kick watchdog
        continue                       // ← SKIP everything below (no LCD, no SD)

    UI_AppHandle()                     // menus, settings, navigation
    UI_AppServiceDisplay()             // throttled LCD dirty-stripe flush
    MOD_ServiceBackground()            // background modulator housekeeping
    SD_ServiceBackground()             // background SD format/file service
    kick watchdog
```

## Storage And Calibration

Since v8.09 settings and calibration live only on the SD card, in the store
of `Core/Src/mod_sd_store.c`: raw sectors 256-767, between the MBR and the
first partition. Nothing there is a file, so nothing a computer, the file
manager, a FAT repair or a format in the radio does to the files can touch
them, and USB mass storage refuses writes into those sectors.

- **Layout.** Two copies, A (256-511) and B (512-767). Each starts with a header: magic, layout version, the MCU's 96-bit UID, a random store instance, the firmware that created it, where its contents came from (new, v8.08 flash dump, v8.08 card files, restored, adopted; and an IMPORTING/RESTORING mark while an import or restore is under way), CRC-32. Then one slot per record: slot 0 the settings (`settings_record_t`, v27 blob, up to 8 sectors), slots 1-192 one sector each for the calibration records (`ssb_cal_record_t`, slot 1 + band x 6 + sideband x 3 + analytic key; 138 used). A record carries a 24-byte header with its slot, a sequence number, the store instance, its length and a CRC-32 over header and payload.
- **Saving.** A record is written to the copy that does not hold its newest valid version, with the next sequence number, read back and only then counted; a failure is retried once, then logged (`STORAGE WRITE GAVE UP`) and the store marked degraded. Nothing is written while the supply is under the PVD threshold (about 2.9 V). Both copies are never written by one save, so whatever cuts a save short -- power, a pulled card, a reset -- leaves the previous version readable. The settings and the band's calibration are saved by `SAVE` and by the autosave, 2 s after the last change (at most every 5 s, never while transmitting or streaming).
- **Reading.** The newest valid copy wins. A copy whose read was cut short (the I/O gate, a timeout: the card went not ready under it) is no answer -- it may be the newer one -- and the read is an error, retried later, never the older copy; a copy the card itself will not read is passed over. A band change reads that band's two sideband records once (2 sectors; v8.08 scanned up to 40 KB of `M32CAL.BIN`, 134 ms); a sideband change reads nothing. A band whose records cannot be read is never written over, and **TX is locked** until they read (a TX block reason, `tx_block=CAL NOT LOADED`: no unmute gets past it) -- the radio never transmits on another band's calibration, nor on none by surprise. A band change reads while its own retune holds the I/O gate shut, and a USB audio stream holds it shut for as long as it runs: that read waits without a word and goes past the gate once TX is locked, the transmitter down and the retune done (milliseconds after the band change); `CAL NOT LOADED - TX LOCKED` is shown for a read that failed or one still waiting after 3 s. RUN WITHOUT CARD is the operator's way to transmit uncalibrated, knowingly. A band with no record runs uncalibrated, as before.
- **Background check.** Every 500 ms, outside TX, the scrub reads both copies of one slot (a pass about every 100 s). A record saved once gets its second copy (a mirror); a copy of this store that does not check, or that the card will not read, is rewritten from the good one and logged (`COPY REPAIRED`); a slot whose read was cut short is left for the next pass. Records an earlier store left on the card (after `storage new wipe`) are ignored.
- **Which card.** The store belongs to the MCU that made it. At boot, a card that is missing, blank, another radio's, damaged (both headers gone), of the wrong layout or unreadable, a store whose import or restore was cut short, or one whose settings record will not read (`SETTINGS READ ERROR`: read again every second and on a click; before v8.09's bench review the failed read let the first autosave write the defaults over it), stops the boot before RF and the watchdog start, with the USB console up: a click does what the card needs (import v8.08's settings, create a store, use another radio's, repair, import or restore again), a hold runs the radio without a card until the next boot (`NO SD CARD - SETTINGS NOT SAVED` on the home screen); console `storage nocard|new|adopt|import|restore|keep` does the same. A store is never created, adopted or replaced silently.
- **Backups.** `SYSTEM/STOREBAK.BIN` holds every record with its CRC, under a header with the MCU's UID. It is written by the SD menu (BACK UP STORE), by console `storage backup`, before a factory reset, and by itself a quiet minute after a calibration change or an import (at most every ten minutes, a few slots per main-loop step). It is built as `STOREBAK.TMP` and renamed over the old one only when whole. `storage restore [path]` (default `SYSTEM/RESTORE.BIN`) checks the whole file -- this radio's UID, every CRC -- before it writes anything. `tools/rev1_diag.py storage-backup` and `storage-restore FILE` do both from a PC.
- **Factory reset** backs the store up, then erases the settings and every band's calibration (v8.08 erased the flash pages but left the card's records).
- **The v8.08 import.** v8.08 kept settings and calibration in flash (`0x08075000`, 44 KiB) and, with the card as its store, in `M32CFG.BIN`/`M32BAK.BIN`/`M32CAL.BIN`. `tools/flash_rev1.py --factory` saves those flash pages before the new image covers them and puts them on the card as `SYSTEM/NVM808.BIN`; the boot hold imports them with v8.08's own rules (the flash record says where the settings lived; a key's card record wins where the card was the store; within a source the highest sequence number wins; every record checked as v8.08 checked it) and writes `SYSTEM/IMPORT.LOG`. The old files are left on the card; `storage import` can be run again. The script then compares every calibration record the radio stored with its own reading of v8.08's.
- Current settings layout is defined in `Core/Inc/mod_storage_layout_priv.h` (v29: the Morse trainer's preferences and the memories' repeat; a v28 record moves the keyer once to Iambic B, Curtis 75/45, weight 50, no auto space, latency 0, debounce 3 ms; v26 records still load); the store's in `Core/Inc/mod_sd_store.h` (194 slots: 0 settings, 1-192 calibration, 193 the trainer's learner record; a backup made with 194 slots does not restore on v8.09). The calibration record stores typed differential-skew, theta-linearity, common-phase, branch-tick-offset, validity, and APPLY state per band, sideband, and analytic backend.
- `cfg_knee` and several `legacy` names are retained only for stored-settings compatibility.

**Power loss, by what was happening:**

| Interrupted | Afterwards |
|---|---|
| A settings or calibration save (autosave, SAVE) | The previous version of that record is read; the next save writes the same copy again. |
| A firmware update | As in the update guide: the loader resumes from the card or keeps the old image. |
| The v8.08 import, or a restore | The store is marked IMPORTING/RESTORING: the next boot holds with `IMPORT CUT SHORT`/`RESTORE CUT SHORT`, and a click runs it again (or keeps the store when the source file is gone). |
| A backup | `STOREBAK.TMP` is left half-written; the previous `STOREBAK.BIN` is intact. |
| A file write (event log, export, file manager, mass storage) | FAT is not journaled: the last lines may be lost or the volume need a check on a computer. The store is not affected. |
| An in-radio format | The volume is gone; the store, before it, is not. |

**The card in trouble while the radio runs** -- the running settings stay as they are, nothing falls back to defaults, and TX is never let through on values the radio did not mean:

| What happens | What the radio does |
|---|---|
| The card is pulled, or stops answering | Settings and the active band's calibration stay in RAM; `SD CARD LOST - SETTINGS NOT SAVED`; the card is mounted again every 3 s outside TX. A band change whose calibration cannot be read **locks TX** (`CAL NOT LOADED - TX LOCKED`); the SD menu's SETTINGS STORE, clicked twice, runs without a card until the next boot (TX uncalibrated, nothing saved), as the boot hold offers. |
| It goes back in | Mounted within 3 s; the store reads its headers before anything else (another radio's card is FOREIGN and waits for the operator); what changed meanwhile is saved. |
| The driver initialises the card again (a retry, FatFs after an error, a transfer the gate cut short) | The store reads nothing and writes nothing until it has read the card's headers again (it may be another card); FatFs is mounted afresh when it is. |
| The card refuses writes but reads | `SD CARD WRITE ERRORS - REPLACE CARD` after three writes failed in a row; the event log backs off (2 s up to a minute) and remounts so files keep reading; autosave asks once a minute. Reads, the store's settings and calibration keep working. |
| The settings record will not read at boot | The boot holds (`SETTINGS READ ERROR`) and reads again every second; nothing is saved meanwhile, so the defaults never land over the record. A hold runs without a card. |
| The card hangs and drives nothing | `NO CARD` (and the boot hold after a restart): only taking it out restores it -- REV1 has no switch on the card's supply. |
| A transfer or a remount the real-time I/O gate cuts short (a USB audio stream starting, a retune) | Nothing visible: the driver is initialised again at the next pass the gate allows; the store's header reads and the FAT mount are held so they are never cut, and a store read cut short is an error, retried, never the older copy. |
| A card half inserted, or slow to answer | No watchdog reset: SD_Init feeds it at each bounded stage; the remount backs off to 30 s. |

## Current Resource Baseline

Memory-budget check at v8.09 (`tools/memory_budget_check.py`; flash layout 2,
the policy budget is 515,000 bytes of the 518,112-byte `FLASH_APP` region,
which runs from `0x08001000` to the image footer at `0x0807F7E0`; the update
journal is the last page, `0x0807F800`):

| Build | FLASH_APP | RAM | CCMRAM |
|---|---:|---:|---:|
| Debug / Release-Speed | 478224 | 89344 | 28664 |
| Diagnostic | 467192 | 89096 | 28664 |

That leaves 39.0 KB of the region free (35.9 KB under the policy budget) and
8.75 KB of RAM unallocated against the 8 KB floor. v8.08 used 469,100 of
473,056 bytes (900 bytes under its 470,000 budget) with 8.2 KB of RAM. The
landscape UI's frame (framebuffer, tile colours, page hashes, one DMA stripe)
takes about 12.2 KB of RAM.

What v8.09 measured, kept and left (Release-Speed bytes):

| Change | Flash | Kept |
|---|---:|---|
| Flash layout 2: settings/calibration pages to the card | +45,056 of region | yes |
| dr_mp3's assert strings (`DRMP3_ASSERT` keeps every check) | -792 | yes |
| `-Os` on `usb_diag_mode.c` | -11 | yes |
| `-Os` on `stm32g4xx_hal_msp.c` | -11 | no: generated file, not worth it |
| `-Os` on `radio_bpf_fit.c`, `radio_iq_fit.c`, `mod_api_cw.c` | | no: per-sample receive accumulators or interrupt-reachable functions (call graph of the linked image) |
| Console help/usage, menu category text, export headers to `HELP.BIN` (57 texts, 2,919 B of text) | -2,420 | yes |
| SD driver CRC/CMD59/CMD13/retries/DWT deadlines | +1,576 | yes |
| Store, import, backups, boot hold, console (net of the flash backend removed) | +8,559 | yes |
| LTO over the cold objects | -5,736 | no: it re-optimises interrupt-reachable functions, which breaks the object identity proof and the stack checks |
| `-Os` on the MP3 decoder, SD player, SD driver and FatFs (~3.5 KB) | | no: they feed SD-audio transmit |
| RX decimation table from the card (6.5 KB) | | no: a card read at receiver start |
| `Os` to `Oz` | 0 | no |

`tools/rt_identity_check.py --base <v8.08 build> --new Release-Speed --allow 'sd_audio_decoder:*'`
compares every real-time object (receiver, transmit pipeline, transport, CW,
USB audio and PD, SD audio, FatFs, HAL, interrupt handlers) and every
function reachable from an interrupt vector, section by section with symbolic
relocations: at v8.09 all are byte-identical to v8.08 apart from the MP3
decoder's assert calls.

**Where the flash goes, and how it is kept down.** Modules that no interrupt,
the PendSV receive worker or SysTick reaches are built for size with
`#pragma GCC optimize ("Os")` at the top of the file (the HAL's init-time
drivers, FatFs, the MSC and CDC classes and USB control requests included);
configuration-time functions inside the speed-built files carry
`__attribute__((cold))`. The TX renderers (`HOT_RENDER`, O3), the receive
front end and worker, the transport, SD block transfers, the MP3 decoder and
the USB audio path keep their speed code, and so does anything reached through
a function table. Keep new code in the same shape: a new real-time function
belongs in a speed-built file, and a new menu, console or storage feature in a
size-built one. The image carries no double-precision arithmetic -- use
`MOD_PI_F`, not `M_PI`, which `<math.h>` defines as a double -- and no newlib
stdio: `snprintf`/`vsnprintf` are the only formatted-output entry points, and
`assert()` goes through the stdio-free `__assert_func` in `syscalls.c`.

## Full Hardware Pinout

### REV1 board pin map

The complete 86-entry GPIO table is [`docs/rev1_pins.csv`](docs/rev1_pins.csv).
The most timing-sensitive assignments are:

| Group | Pins | Assignment |
|---|---|---|
| TX A/B | PA8/PA9 AF13; PC8/PC9 AF3 | HRTIM A1/A2 and E1/E2; GPIO-low until RF permit |
| RX/helper/test | PB12/PB13 AF13; PB14 AF13 | HRTIM C1/C2 quadrature and D1 calibration injection |
| Headphones | PA0/PA1 AF1 | TIM2 CH1/CH2 PWM, ~295–380 kHz chosen per RF frequency (no on-board reconstruction filter) |
| Microphones | PA2, PA3, PE9 | ADC1 IN3/IN4 and ADC3 IN2 |
| SD | PA4 CS; PA5/PA6/PA7 AF5 | SPI1 |
| LCD | PE10..PE14; PB9 AF1 | SPI4 plus reset/CS/DC/backlight |
| PCM1821 | PA15/PB3/PB5 AF6 | I2S3 WS/CK/SD |
| USB data | PA11/PA12 | U17 USB FS D−/D+ |
| USB-C PD | PB4/PB6 | U18 CC2/CC1, external Rd, UCPD1 |
| RF gates | PC6/PC10 | Driver enables via BSS123 inverters: MCU high = idle/fault for UCC27524 and ACT541; low only for an active UCC27524 branch |
| controls | PD0..PD6, PE0/PE1 | RX BPF, five buttons (polarity learned from first press), DIT/DAH |
| encoder | 3 spare JP20/JP7 pins, default PC11/PC12/PC13 | Optional rotary encoder: A/B plus push; pins selectable in settings |
| analog | PC0..PC5, PB1/PB2/PB11 | detectors, VIN, envelope DAC/OPAMP paths |
| power/recovery | PB10, PA13/PA14, PG10, PB8, PF0 | MCU hold, SWD, NRST, BOOT0, 8 MHz TXCO |

## Validation

Use the STM32 toolchain path that contains `arm-none-eabi-*`, then build and validate from this directory:

```sh
make -C Debug -j16 all
tools/build_release_speed.sh
tools/build_diagnostic.sh
python3 tools/version_tool.py check
python3 tools/defaults_consistency_check.py
python3 tools/validate_current.py --quick
python3 tools/host_tests.py --sanitize
python3 tools/validate_current.py
python3 tools/validate_current.py --release
python3 tools/gen_ui_fonts.py --check
python3 tools/ui_preview.py --out /tmp/alt2-ui
```

`validate_current.py` is a stable, version-independent entry point. `--quick`
compiles and executes host tests against the production transport/RF, SD audio,
Morse, CRC, and storage-migration implementations. It also applies strict STM32
target type/conversion checks to the safety-critical RF, transport, storage,
and SD-audio modules; it needs no firmware ELF.
The default tier builds Debug first when it is missing or stale (`--no-build`
fails instead), then checks the compiler-expanded
firmware version plus the actual USB/MSC descriptor bytes, programmed
flash/RAM/CCMRAM use, stack call chains, cyclomatic-complexity baselines, build
metadata, deep-diagnostic flavor, and forbidden real-time call paths. The ELF
policy follows reviewed Audio-class function-pointer targets, including Thumb
`BX` tail calls. `--release` applies the linked checks to Debug,
Release-Speed, and Diagnostic images, building those that are stale first;
everything after the builds is read-only. Add `--json` for automation.

Validation and reporting scripts are source-tree read-only. They compile probes
only in temporary directories and verify source/configuration hashes before
returning. The two build wrappers are intentional writers restricted to their
marked generated build directories; `update_version.py vMAJOR.MINOR` is the
only intentional source writer. Shared thresholds and the persisted ABI
manifest live in `tools/validation_policy.json`.

Hardware checks that still matter:

- All five LPFs are populated, so the usable range is 1.0-28.3 MHz. Above
  28.3 MHz the home screen must read `NO LPF FOR FREQ`, the receiver must stay
  off, and PE2..PE6 must all be low. A board whose settings were saved while a
  slot was out of service needs `QMX+ LPF EDGES` under BOARD > FILTERS once,
  then a save; the compiled defaults already carry all five.
- First power-on with no antenna: the status line must read `SETUP REQUIRED`,
  PC7 (RX_EN) low, PC6/PC10 high, PA8/PA9/PC8/PC9 and PB12..PB14 low, PD12 low,
  PB1/PB11 at 0 V. Nothing may move until `ASSEMBLY CONFIRM`.
- Encoder: turn and press, then reassign the pins under BOARD > CONTROLS and
  confirm the change takes effect without a reboot. Hold and turn must move
  the tuning step without also opening the menu.
- Display: both SCREEN ROTATION settings upright for their mounting, colours
  correct, nothing within 8 px of the glass edge; the boot log turns once,
  after SETTINGS/RF.
- Home: the knob tunes with no dead receiver after a spin; a click cycles the
  step; hold, turn to the settings row, click, click VOLUME and the level
  changes while turning, with no audio drop; MODE moves the receiver with the
  transmitter. The boot log stays readable (4 s, 12 s with a warning).
- Enter DRAIN A and DRAIN B deliberately mismatched and confirm DIAGNOSTICS
  › TRANSMIT reports `DRAIN A/B DIFFER` on its TX row.
- TX TEST: brief paddle bounce must not enable the PA.
- TX TEST: while the paddle keys the carrier the page reads CARRIER ON and the RF lamp lights (redrawn in place; the full frame resumes after release). On home, the badge reads TX through a CW over.
- CW trainer (`docs/CW_TRAINER.md`): START LEARNING with a CW newcomer finds the paddle sides without ADVANCED CW and reaches audible M/K echo within 2 minutes; the first lesson runs on to lesson 3 on the paddle alone, no knob press; speed and volume change from the trainer page; a wrong answer is understood; leaving and START LEARNING again resumes; RF stays off throughout (dummy load and meter). Paddle-to-sidetone and paddle-to-RF latency, first-element length, sidetone/RF alignment and break-in return on the scope or an SWD trace.
- CW memories: a line of `CW/MEMORIES.TXT` keys on the dummy load with the paddle's timing; a lever stops it; REPEAT EVERY S repeats it; a mode change drops it. SPOT sounds at CW PITCH; the tuning bar reads ZERO on a carrier at the dial.
- Menu usability: every row shows its help; edit mode shows either the neighbouring choices or a range bar; HOLD always goes back.
- PA geometry: start with `PA GEOMETRY = NORMAL`, `THETA DIRECTION = A +THETA` and a low `DRAIN LIMIT MV` or `CW THETA`. Try `A INVERTED` if the supply current is high for the output seen.
- Outphasing sweep: `INT THETA SWEEP` now respects the same hardware/mode theta cap as other test modes, so it verifies the legal reduced range rather than forcing a deep null.
- Dynamic clock policies: verify RF stays inhibited (driver inputs low, enables parked high) during pending/applying retunes and TIM6 stays at 48 kHz after retune.
- SSB/CW: verify PA current, spectrum, and IMD after any phase/calibration/outphasing change.
- SD card: verify SD detection and mount on boot screen. Verify file read/write/list/delete works while NOT transmitting. Verify `SETTINGS STORE`, `BACK UP STORE`, `FORMAT SD CARD`, and `WRITE AUDIO PLAN`. Verify factory reset leaves `SYSTEM/STOREBAK.BIN` and zero calibration.
- SD settings store (v8.09): boot without a card stops at NO SD CARD with the USB console up; a hold (or `storage nocard`) runs without a card, inserting one continues the boot. `storage` shows `store=READY`, headers `AB`, `sd stats` `crc_on=1` and no errors; a change is in the store about 2 s after the knob stops (`storage list` sequence numbers move); `LOGS/EVENTS.LOG` gains a boot line per power-up and no `COPY REPAIRED` in normal use. Mass storage refuses a write into sectors 256-767 (`dd` from the PC fails), deleting or formatting the card's files from the PC leaves the store as it was. Power pulled during autosave loops: every boot comes up READY with the last or the one before. After every flash run `tools/flash_rev1.py` (or `--card-only`) so the card holds the image's HELP.BIN; the console's `card` lists what the image needs. Card trouble: pull the card while receiving -- `SD CARD LOST`, settings unchanged; change band -- `tx_block=CAL NOT LOADED`, no TX on any key; put it back -- READY within 3 s, TX unlocks, no watchdog reset. Reads cut short (a bench script that closes the I/O gate over SWD at random): `sd stats` `aborts` rises, `crc`/`io` stay 0, every write between still lands, `storage` `bad_copies`/`repairs`/`io` stay 0. Receive in every mode on 160-10 m: `rx` `overruns` stays 0 after a band change and over 3 s (FM ran out of time before v8.09; `rxprof on` gives rxavg ~42-45k cycles a half, headphones ~9.3k).

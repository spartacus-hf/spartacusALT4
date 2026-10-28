# REV1 USB firmware update

The STM32G474VET6 boots through a 4 KiB first stage at `0x08000000`.
The application begins at `0x08001000`. The first stage uses only SPI1 SD
sector reads, CRC32, flash programming, and the watchdog. It does not need
the application's USB stack or FatFs to recover an interrupted install.

Since v8.09 the flash uses **layout 2** (loader ABI 2): the application runs
to its footer at `0x0807F7E0` (518,112 bytes), the update journal is the last
page (`0x0807F800`), and there are no settings or calibration pages in flash
any more -- they live on the SD card. Layout 1 (v7.xx-v8.08) ended the
application at `0x080747E0` and kept settings at `0x08075000` and calibration
at `0x08076000`. The two do not mix: a layout-1 loader refuses v8.09
packages (they ask for loader ABI 2), a layout-2 loader refuses packages of
layout 1 (no downgrade through the SD card), and an application on the other
layout's loader stops at `FLASH SETUP`. Moving a radio to layout 2 is a
one-time job: see [Moving to layout 2](#moving-to-layout-2-v809).

## Build the files

From `firmware/`:

```sh
make -C Bootloader all
make -C Debug all
tools/build_release_speed.sh
python3 tools/package_firmware.py --build-dir Release-Speed --output-dir dist/rev1
```

Use the STM32 GNU tools from CubeIDE in `PATH` if `arm-none-eabi-gcc` is not
already available. The packer rejects a stale ELF, wrong reset vectors,
oversized image, and an incompatible loader ABI record. It writes:

| File | Use | ROM DFU start address |
|---|---|---|
| `REV1FW.UPD` | Copy to the SD root through USB Disk mode | — |
| `rev1_factory.bin` | One time USB DFU install, including loader | `0x08000000` |
| `rev1_recovery.bin` | USB DFU recovery with protected loader intact | `0x08001000` |

The packer also writes `rev1_factory.elf` and `rev1_recovery.elf` with the
same flash bytes and explicit load addresses. These are for STM32CubeIDE and
ST-Link; the `.bin` files are for ROM DFU.
The Debug pre-build step removes old flash images, and post-build runs from
`Debug/` to write fresh files to `dist/debug/`. A compiler or packaging
failure therefore leaves no old factory ELF for CubeIDE to download.
The release and diagnostic build
wrappers package their own ELFs into `dist/release-speed/` and
`dist/diagnostic/` after each build.

Since v8.09 the factory image covers the whole flash, `0x08000000` to
`0x08080000`, and leaves the update journal page erased; the recovery image
runs from `0x08001000` to the end. Both overwrite the pages where v8.08 kept
its settings and calibration -- on a layout-1 radio, read them first (below).

## One time setup without an ST Link

1. Power the board through U18 USB-C PD or the DC input. Connect the **data**
   USB cable to U17. Hold SW8 for the whole ROM DFU session because the ST
   ROM cannot drive PB10 `MCU_PWR_HOLD`.
2. Hold BOOT0/SW12, press and release RESET/SW13, then release SW12. Confirm
   the host sees an STM32 USB DFU device.
3. Read option bytes with `STM32_Programmer_CLI -c port=usb1 -ob displ`.
   This layout requires `DBANK=1`, `BFB2=0`, and RDP level 0. Set the first
   two if needed, then reconnect to ROM DFU before programming.
4. Program and verify the factory file:

   ```sh
   STM32_Programmer_CLI -c port=usb1 -d dist/rev1/rev1_factory.bin 0x08000000 -v
   ```

5. Protect only Bank 1 pages 0 and 1, then read the option bytes back:

   ```sh
   STM32_Programmer_CLI -c port=usb1 -ob WRP1A_STRT=0x00 WRP1A_END=0x01
   STM32_Programmer_CLI -c port=usb1 -ob displ
   ```

   Confirm that other WRP windows do not cover application pages or Bank 2
   pages 0–127 (layout 1: 0-105). The application refuses to stage an install
   if the loader is missing or protection/layout is wrong.
6. Release BOOT0, reset, and keep holding SW8 until the application starts
   and takes over the power latch. Then release SW8.

The initial factory flash is a one time migration from the old application
address. An old unrelocated image cannot boot through this loader.

## STM32CubeIDE and ST-Link flashing

The application's `Debug/alt2.elf` starts at `0x08001000` and does **not**
contain the first-stage loader or image footer. Loading only that ELF can
appear to work when the debugger starts at its application entry point, then
fail on the next hardware reset: the processor starts at `0x08000000` and
cannot assert `MCU_PWR_HOLD` without a valid loader. Reflashing only the
application ELF also leaves a stale footer, which the loader rejects.
The application now checks for its loader and matching footer during the boot
splash. A debugger launch with either missing stops on `FLASH SETUP` while
keeping PB10 asserted, before RF or USB starts.
On every reset the loader verifies the entire application CRC before jumping
to it. This happens before the LCD is initialized, so the screen may remain
blank or white during the check. v7.08 uses a compact lookup table to shorten
that check without weakening verification. LCD reset and wake delays also
contribute to the startup time.
The faster CRC lives in the first-stage loader. A normal SD/USB update or
`rev1_recovery.elf` changes only the application, so it cannot speed an
existing v7.07 loader. Install the v7.08 factory image once while loader
pages are still unprotected. If they are write protected, preserve that
protection unless you deliberately use ST-Link or ROM DFU to replace the
loader and restore protection afterward.

1. For an unprotected board without this first stage, select **alt2 Factory
   Debug** in CubeIDE. Hold SW8 while connecting/programming if the board
   does not stay powered. The Debug post-build step builds the loader and
   packages `dist/debug/rev1_factory.elf`; the launch downloads that complete
   image at `0x08000000` and loads debug symbols from `Debug/alt2.elf`.
   Do not use this launch after write-protecting loader pages 0 and 1.
   The equivalent ST-Link CLI command, after packaging the Debug build, is:

   ```sh
   STM32_Programmer_CLI -c port=SWD mode=UR -d dist/debug/rev1_factory.elf -v
   ```
2. Stop the debugger and press RESET/SW13 with BOOT0/SW12 released. Hold SW8
   until the application takes over PB10, then release it. Confirm it boots
   again without the debugger. Only then set WRP on Bank 1 pages 0 and 1.
3. For later ST-Link debugging, select **alt2 Debug**. It downloads
   `dist/debug/rev1_recovery.elf` at `0x08001000`, including the matching
   footer and an erased update journal, while using `Debug/alt2.elf` only
   for symbols. Its ELF entry starts through the protected loader. The
   protected first stage remains intact. Since v8.09 that image runs to the
   end of the flash: on a radio still on layout 1 it would overwrite v8.08's
   settings and calibration pages before they were read -- move the radio
   first (`tools/flash_rev1.py --factory`, which refuses an application-only
   flash onto a layout-1 loader).

After importing or pulling the project, refresh the CubeIDE project so its
tracked launch configurations and Debug post-build step are applied. For a
command-line build, run `make -C Bootloader all`, `make -C Debug all`, then
`python3 tools/package_firmware.py --build-dir Debug --output-dir dist/debug`
before flashing the images. A generic CubeIDE launch configuration that
downloads `Debug/alt2.elf` alone is unsafe for this layout.

## Moving to layout 2 (v8.09)

A radio on v8.08 or earlier has its settings and calibration in the flash
pages from `0x08075000` (44 KiB) and, if the card was its store, in
`M32CFG.BIN`/`M32BAK.BIN`/`M32CAL.BIN` on the card. v8.09's image covers those
pages, so they are read out first and imported into the card's store. Keep
the SD card in the radio throughout.

**With an ST-Link** (one command):

```sh
tools/flash_rev1.py --factory
```

It checks the option bytes (`DBANK=1`, `BFB2=0`, RDP level 0), saves the whole
flash, the option bytes and the MCU UID under `backups/`, keeps v8.08's pages
as `backups/rev1_<uid>_<time>_before-factory.NVM808.BIN`, lifts the loader's
write protection (`WRP1A_STRT=0x7F WRP1A_END=0x0`), writes and verifies the
factory image, reads the whole flash back, protects pages 0-1 again
(`WRP1A_STRT=0x0 WRP1A_END=0x1`) and resets. The radio boots into its NO
SETTINGS STORE hold with the console up; the script puts the pages on the
card as `SYSTEM/NVM808.BIN`, the radio imports them (with v8.08's card files,
by v8.08's own rules), and the script compares every calibration record in
the store with its own reading of v8.08's. Then it provisions `HELP.BIN`.
`tools/flash_rev1.py --restore backups/rev1_..._before-factory.flash.bin`
puts the old loader, image and settings pages back byte for byte.

**With ROM DFU only** (SW8/SW12/SW13 and U17 as in the one-time setup):

1. Read the whole flash and keep it -- it is the only copy of v8.08's
   settings and calibration pages, and the way back:

   ```sh
   STM32_Programmer_CLI -c port=usb1 -u 0x08000000 0x80000 rev1_v808_flash.bin
   STM32_Programmer_CLI -c port=usb1 -ob displ > rev1_v808_ob.txt
   ```
2. Lift the loader's protection, program the factory image, protect again
   (an option-byte change reloads the option bytes and resets the MCU:
   enter ROM DFU again before the next command):

   ```sh
   STM32_Programmer_CLI -c port=usb1 -ob WRP1A_STRT=0x7F WRP1A_END=0x0
   STM32_Programmer_CLI -c port=usb1 -d dist/rev1/rev1_factory.bin 0x08000000 -v
   STM32_Programmer_CLI -c port=usb1 -ob WRP1A_STRT=0x00 WRP1A_END=0x01
   ```
3. Reset with BOOT0 released. The radio stops at NO SETTINGS STORE with the
   USB console up (`/dev/ttyACM*`). Then:

   ```sh
   tools/flash_rev1.py --import-nvm rev1_v808_flash.bin
   ```

   It reads the radio's UID from the console, puts the pages on the card as
   `SYSTEM/NVM808.BIN`, lets the radio import them and checks every record,
   as `--factory` does. (A click on the panel instead imports v8.08's card
   files only, or starts a new store.)

To go back, program `rev1_v808_flash.bin` at `0x08000000` the same way
(protection lifted, then restored).

## Normal update through U17 USB

1. Build `REV1FW.UPD` for this REV1 board. Open **SD CARD → USB DISK MODE**,
   copy the file into the SD root, safely eject the drive on the computer,
   then select **EXIT USB DISK** on the radio. Keep the SD card inserted.
   Copying a file never starts an install.
2. On the radio, select **SD CARD → FIRMWARE UPDATE**. It checks the package
   before showing the confirmation page. Hold the knob (or SELECT) to install. Alternatively,
   switch to the separate **USB DIAG CONSOLE** CDC personality and send
   `fw verify` followed by `fw install`. A successful `fw install` resets
   the board and disconnects the CDC port.
3. Keep board power and the SD card connected until the new firmware boots.
   The first stage rechecks the SD map, header, image CRC, and image vectors
   before erasing; it verifies programmed flash and writes the footer last.
   After two seconds of healthy foreground running, the application confirms
   the new image during an idle audio/storage interval.

The package is integrity checked, including a REV1 board ID and minimum
loader ABI: a layout-2 loader takes ABI 2 packages only (v8.09 and later), a
layout-1 loader none of them. It is not signed; custom firmware can be
packaged and installed.
The application will not run an update while USB MSC owns the card, after an
unsafe MSC exit, during transmission/calibration, or while audio/storage work
is real time critical.

## A board running v7.09 or earlier cannot install at all

`FW_UpdateInstall` guards the journal transaction with the programmable
voltage detector, and up to v7.09 it asked for `PWR_CR2_PLS_LEV7`. On STM32G4
that coding is not the highest supply threshold: it disconnects the comparator
from VDD and points it at the external `PVD_IN` pin, compared against VREFINT.
`PVD_IN` is PB7, which this board uses as `STATUS_LED` -- a push-pull output
driven low in `BOARD_EarlyInit` and never driven high. The guard read an unlit
LED, called it a brownout, and returned `FLASH ERROR` before touching flash.
`fw verify` passed every time because verification only reads the card.

`FW_UpdateConfirmService` had the same line, so an install that did reach
flash would never have been confirmed and the loader would have rolled it back
after `FW_BOOT_ATTEMPTS` boots.

v7.10 uses `PWR_CR2_PLS_LEV6`, about 2.9 V against this board's 3.3 V VDD, and
a `_Static_assert` in `Core/Src/fw_update.c` fails the build if level 7 comes
back. Note that the pin is shared: anything that drives PB7 as a status LED
also drives the PVD input, which is harmless only while `PLS` names a VDD
threshold.

Because the fix has to be installed by the broken installer, **one board
running v7.09 or earlier needs a single ROM DFU session** to reach v7.10:

```sh
STM32_Programmer_CLI -c port=usb1 -d dist/rev1/rev1_recovery.bin 0x08001000 -v
```

with SW8/SW12/SW13 as in "One time setup without an ST Link" above. The
protected loader, settings and calibration are untouched. From v7.10 onwards
`fw install` and **SD CARD -> FIRMWARE UPDATE** work as documented.

## Failure and recovery behavior

| Situation | Result |
|---|---|
| Missing, wrong-board, corrupt, or incomplete package | No application flash erase; current firmware runs. |
| Package of the other flash layout (v8.08 or earlier on a v8.09 loader, or the reverse) | Refused before anything is erased; current firmware runs. |
| Power loss before the journal commit | Current firmware still boots. |
| SD absent or changed before erase | First stage keeps the current valid firmware. |
| Power loss after application erase starts | On reset, first stage retries the CRC-checked staged SD image. Keep the original card and files inserted. |
| SD becomes unreadable after erase, or programming/verify fails | First stage keeps RF disabled and blinks the LED. Restore the card and reset, or use ROM USB DFU. |
| New firmware repeatedly fails before confirmation | After eight boot attempts the first stage stays in safe recovery. Use ROM USB DFU. |
| Install returns `FLASH ERROR` while `fw verify` passes on v7.09 or earlier | The PVD guard read PB7 instead of VDD. Fixed in v7.10; that one board needs ROM USB DFU to get there. |

There is no old-image copy and no automatic rollback. The application footer
and journal are separate; an interrupted journal erase leaves the current
valid image bootable. The first stage is write protected and never erases its
own pages. Since v8.09 there is no settings or calibration region in flash:
an install cannot touch them.

For final recovery, enter ROM USB DFU using SW8/SW12/SW13 and U17 as above,
then program only the application/recovery region:

```sh
STM32_Programmer_CLI -c port=usb1 -d dist/rev1/rev1_recovery.bin 0x08001000 -v
```

This works while Bank 1 pages 0 and 1 remain write protected, on a radio
whose loader is already layout 2 (v8.09 or later; a layout-1 radio needs the
move above). Reset with BOOT0 released and keep SW8 held until the
application takes over power.

The flash layout and package fields are in `Core/Inc/fw_update_format.h`.
`Bootloader/bootloader.c` is the small recovery implementation;
`Core/Src/fw_update.c` creates and commits the SD sector map in the main-loop
foreground. Flash/RF/card timing still needs a physical REV1 power-cut and
USB DFU test before field deployment.

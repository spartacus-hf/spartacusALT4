TOOL_PREFIX ?= arm-none-eabi-
CC := $(TOOL_PREFIX)gcc
OBJCOPY := $(TOOL_PREFIX)objcopy
SIZE := $(TOOL_PREFIX)size
CFLAGS := -std=gnu11 -Os -g0 -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -mcpu=cortex-m4 -mthumb -DSTM32G474xx \
          -I../Core/Inc -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include
LDFLAGS := -nostdlib -Wl,--gc-sections,-Map=bootloader.map -Tbootloader.ld

.PHONY: all clean
all: bootloader.elf bootloader.bin

bootloader.elf: startup.o bootloader.o bootloader.ld
	$(CC) $(CFLAGS) $(LDFLAGS) startup.o bootloader.o -lgcc -o $@
	$(SIZE) $@

bootloader.bin: bootloader.elf
	$(OBJCOPY) --gap-fill=0xFF -O binary $< $@

startup.o: startup.s
	$(CC) $(CFLAGS) -c $< -o $@

bootloader.o: bootloader.c ../Core/Inc/fw_update_format.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) startup.o bootloader.o bootloader.elf bootloader.bin bootloader.map

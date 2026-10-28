ENTRY(Reset_Handler)
MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 0x0FE0
  LOADER_INFO (rx) : ORIGIN = 0x08000FE0, LENGTH = 32
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 96K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) } > FLASH
  .loader_info : { KEEP(*(.loader_info)) } > LOADER_INFO
  .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT> FLASH
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
  . = ALIGN(8);
  ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH), "loader exceeds 4 KiB")
  ASSERT(_ebss + 4096 <= _estack, "loader RAM stack headroom")
}

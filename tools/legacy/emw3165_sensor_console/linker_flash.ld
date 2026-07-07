ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
  .isr_vector ORIGIN(FLASH) :
  {
    KEEP(*(.isr_vector))
  } > FLASH

  .text :
  {
    *(.text.Reset_Handler)
    *(.text*)
    *(.rodata*)
  } > FLASH

  .data : AT(ADDR(.text) + SIZEOF(.text))
  {
    __data_start__ = .;
    *(.data*)
    __data_end__ = .;
  } > RAM
  __data_load__ = LOADADDR(.data);

  .bss (NOLOAD) :
  {
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    __bss_end__ = .;
  } > RAM

  . = ALIGN(8);
  __stack_top__ = ORIGIN(RAM) + LENGTH(RAM);
}

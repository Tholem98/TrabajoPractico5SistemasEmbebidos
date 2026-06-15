/* linker.ld — completo para STM32F103C8T6 con libopencm3 */
ENTRY(reset_handler)

MEMORY
{
  rom (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
  ram (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

_stack = ORIGIN(ram) + LENGTH(ram);

SECTIONS
{
  .text : {
    KEEP(*(.vectors))      /* vector table */
    *(.text*)              /* código */
    *(.rodata*)            /* const */
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
  } >rom

  /* Sección .ARM.exidx para soporte de excepciones (opcional) */
  /*
  .ARM.exidx : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >rom
  */

  /* .data en RAM, pero viene desde ROM */
  .data : AT (_etext) {
    _data = .;
    _data_loadaddr = LOADADDR(.data);
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >ram

  /* .bss: RAM sin inicializar */
  .bss : {
    _bss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } >ram

  /* .init_array / .fini_array para inicializar objetos globales (opcional, pero requerido) */
  .init_array : {
    __init_array_start = .;
    KEEP(*(.init_array*))
    __init_array_end = .;
  } >ram

  .fini_array : {
    __fini_array_start = .;
    KEEP(*(.fini_array*))
    __fini_array_end = .;
  } >ram

    /* Soporte para arrays de inicialización */
  .preinit_array : {
    __preinit_array_start = .;
    KEEP(*(.preinit_array*))
    __preinit_array_end = .;
  } >ram


  /* Símbolos auxiliares */
  _etext = LOADADDR(.text) + SIZEOF(.text);

  /DISCARD/ :
  {
    *(.ARM.exidx*)
  }
}

/* Provide symbol for newlib's _sbrk implementation */
PROVIDE(end = _ebss);

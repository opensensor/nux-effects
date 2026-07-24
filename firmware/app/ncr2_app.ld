ENTRY(Reset_Handler)

MEMORY
{
    SDRAM (rx)  : ORIGIN = 0x80000000, LENGTH = 0x00200000
    DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00020000
}

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > SDRAM

    .text :
    {
        *(.text*)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        *(.vfp11_veneer)
        *(.v4_bx)
        *(.iplt)
        *(.igot.plt)
        *(.rel.iplt)
        . = ALIGN(4);
    } > SDRAM

    .data :
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > DTCM AT > SDRAM
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DTCM

    __stack_top = ORIGIN(DTCM) + LENGTH(DTCM);
    __sdram_image_end = LOADADDR(.data) + SIZEOF(.data);

    /*
     * Audio buffers too large for the 128 KiB DTCM live here. The region is
     * NOLOAD, so it never enters the flashed image, and it is placed one
     * megabyte into SDRAM so it cannot collide with the loaded application
     * however much that grows. Startup does not clear it; anything placed
     * here must tolerate arbitrary power-on contents or zero itself.
     */
    .sdram_bss (ORIGIN(SDRAM) + 0x00100000) (NOLOAD) :
    {
        . = ALIGN(32);
        __sdram_bss_start = .;
        *(.sdram_bss*)
        . = ALIGN(32);
        __sdram_bss_end = .;
    } > SDRAM

    .ARM.attributes 0 :
    {
        *(.ARM.attributes)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }

    ASSERT(__sdram_image_end <= ORIGIN(SDRAM) + LENGTH(SDRAM),
           "application exceeds slot load budget")
    ASSERT(__sdram_image_end <= ORIGIN(SDRAM) + 0x00100000,
           "loaded application overruns the SDRAM audio buffer region")
    ASSERT(__sdram_bss_end <= ORIGIN(SDRAM) + LENGTH(SDRAM),
           "SDRAM audio buffers exceed the mapped region")
}

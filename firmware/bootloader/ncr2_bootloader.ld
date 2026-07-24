ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x60002000, LENGTH = 0x0001E000
    ITCM  (rwx) : ORIGIN = 0x00000000, LENGTH = 0x00020000
    DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00020000
}

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /*
     * Any call graph that can execute while the XIP NOR is busy belongs
     * here. This section must precede the catch-all .text rule. The
     * explicit MCUX sections prevent an accidental call back into flash
     * from the RAM-tagged W25Q64 primitives.
     */
    .ramfunc :
    {
        . = ALIGN(8);
        __ramfunc_start = .;
        *(.ramfunc*)
        *fsl_flexspi.c.obj(.text.FLEXSPI_UpdateLUT)
        . = ALIGN(8);
        __ramfunc_end = .;
    } > ITCM AT > FLASH
    __ramfunc_load = LOADADDR(.ramfunc);

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
    } > FLASH

    .ram_recovery_blob :
    {
        . = ALIGN(8);
        __ram_recovery_blob_start = .;
        KEEP(*(.ram_recovery_blob))
        . = ALIGN(8);
        __ram_recovery_blob_end = .;
    } > FLASH

    .data :
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > DTCM AT > FLASH
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

    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        *(.noinit*)
        . = ALIGN(4);
    } > DTCM

    __stack_top = ORIGIN(DTCM) + LENGTH(DTCM);
    __flash_image_end = LOADADDR(.data) + SIZEOF(.data);

    .ARM.attributes 0 :
    {
        *(.ARM.attributes)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }

    ASSERT(__flash_image_end <= ORIGIN(FLASH) + LENGTH(FLASH),
           "bootloader exceeds protected flash region")
    ASSERT(__ramfunc_end <= ORIGIN(ITCM) + LENGTH(ITCM),
           "RAM-resident flash call graph exceeds ITCM")
    ASSERT(DEFINED(FLEXSPI_UpdateLUT) ?
           (FLEXSPI_UpdateLUT >= ORIGIN(ITCM) &&
            FLEXSPI_UpdateLUT < ORIGIN(ITCM) + LENGTH(ITCM)) : 1,
           "FLEXSPI_UpdateLUT must execute from ITCM")
}

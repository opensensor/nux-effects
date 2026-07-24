ENTRY(Reset_Handler)

/*
 * This image is loaded by the open bootloader as an ordinary slot payload,
 * then executes entirely from SDRAM/DTCM. It must remain independent of the
 * FlexSPI XIP window after reset because its purpose is to erase that window.
 */
MEMORY
{
    SDRAM (rwx) : ORIGIN = 0x80000000, LENGTH = 0x00200000
    DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00020000
}

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > SDRAM

    .ramfunc :
    {
        . = ALIGN(8);
        __ramfunc_start = .;
        *(.ramfunc*)
        *fsl_flexspi.c.obj(.text.FLEXSPI_UpdateLUT)
        . = ALIGN(8);
        __ramfunc_end = .;
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

    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        *(.noinit*)
        . = ALIGN(4);
    } > DTCM

    __stack_top = ORIGIN(DTCM) + LENGTH(DTCM);
    __ram_recovery_image_end = LOADADDR(.data) + SIZEOF(.data);

    .ARM.attributes 0 :
    {
        *(.ARM.attributes)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }

    ASSERT(__ram_recovery_image_end <= ORIGIN(SDRAM) + LENGTH(SDRAM),
           "RAM recovery image exceeds slot load budget")
    ASSERT(__bss_end <= ORIGIN(DTCM) + LENGTH(DTCM),
           "RAM recovery data exceeds DTCM")
}

ENTRY(Reset_Handler)

MEMORY
{
    SDRAM (rx)  : ORIGIN = 0x80000000, LENGTH = 0x00200000
    MAILBOX (rw): ORIGIN = 0x20000000, LENGTH = 0x00000010
    DTCM  (rwx) : ORIGIN = 0x20000010, LENGTH = 0x0001FFF0
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

    /*
     * Reserved for the bootloader/application one-shot recovery request.
     * The application does not initialize or clear this range.
     */
    .boot_mailbox (NOLOAD) :
    {
        . = ALIGN(8);
        KEEP(*(.boot_mailbox))
        . = ALIGN(8);
    } > MAILBOX

    __stack_top = ORIGIN(DTCM) + LENGTH(DTCM);
    __sdram_image_end = LOADADDR(.data) + SIZEOF(.data);

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
    ASSERT(SIZEOF(.boot_mailbox) <= LENGTH(MAILBOX),
           "boot mailbox exceeds its fixed DTCM reservation")
}

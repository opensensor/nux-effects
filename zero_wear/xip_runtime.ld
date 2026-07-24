ENTRY(xip_copy_handoff)

MEMORY
{
    XIP (rx) : ORIGIN = 0x60081000, LENGTH = 0x00000200
}

SECTIONS
{
    .xip_loader 0x60081000 :
    {
        KEEP(*(.xip_loader))
    } > XIP

    /DISCARD/ :
    {
        *(.ARM.attributes)
        *(.comment)
        *(.data*)
        *(.bss*)
    }

    ASSERT(SIZEOF(.xip_loader) <= LENGTH(XIP),
           "XIP loader exceeds its reserved window")
}


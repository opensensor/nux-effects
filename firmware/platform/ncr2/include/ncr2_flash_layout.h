#ifndef NCR2_FLASH_LAYOUT_H
#define NCR2_FLASH_LAYOUT_H

#include <stdint.h>

#define NCR2_FLASH_XIP_BASE UINT32_C(0x60000000)
#define NCR2_FLASH_SIZE UINT32_C(0x00800000)

#define NCR2_BOOTLOADER_OFFSET UINT32_C(0x00000000)
#define NCR2_BOOTLOADER_SIZE UINT32_C(0x00020000)
#define NCR2_BOOT_HEADER_SIZE UINT32_C(0x00002000)

#define NCR2_FACTORY_OFFSET UINT32_C(0x00020000)
#define NCR2_FACTORY_SIZE UINT32_C(0x003D0000)

#define NCR2_BOOT_METADATA_OFFSET UINT32_C(0x003F0000)
#define NCR2_BOOT_METADATA_SIZE UINT32_C(0x00010000)

#define NCR2_APPLICATION_A_OFFSET UINT32_C(0x00400000)
#define NCR2_APPLICATION_B_OFFSET UINT32_C(0x00600000)
#define NCR2_APPLICATION_SLOT_SIZE UINT32_C(0x00200000)
#define NCR2_APPLICATION_MANIFEST_SIZE UINT32_C(0x00001000)
#define NCR2_APPLICATION_LOAD_ADDRESS UINT32_C(0x80000000)

#define NCR2_APPLICATION_A_ADDRESS \
    (NCR2_FLASH_XIP_BASE + NCR2_APPLICATION_A_OFFSET)
#define NCR2_APPLICATION_B_ADDRESS \
    (NCR2_FLASH_XIP_BASE + NCR2_APPLICATION_B_OFFSET)

#define NCR2_DTCM_START UINT32_C(0x20000000)
#define NCR2_DTCM_END UINT32_C(0x20020000)

/*
 * SRC GPR3 through GPR6. The bootloader publishes the pending slot and
 * journal sequence here. A healthy application turns the handoff token
 * into a one-shot confirmation before warm reset.
 */
#define NCR2_BOOT_TRIAL_MAILBOX_ADDRESS UINT32_C(0x400F8028)
#define NCR2_BOOT_TRIAL_MAILBOX_SIZE UINT32_C(0x00000010)

/*
 * SRC GPR8 and GPR9. The RT1051 reference SDK documents SRC GPR values
 * as retained during reset. They form the magic/inverse warm-reset
 * recovery request and are unrelated to flash layout despite living in
 * this shared platform-address header.
 */
#define NCR2_BOOT_MAILBOX_ADDRESS UINT32_C(0x400F803C)
#define NCR2_BOOT_MAILBOX_SIZE UINT32_C(0x00000008)

/*
 * SRC GPR10. The one-word signature and three-bit engine-slot selector
 * request a one-shot handoff after warm reset. Slots 1-4 launch preserved
 * factory engines; slots 5-8 select open engines. GPR1..GPR10 is the complete
 * SRC GPR range on RT1051, so this deliberately does not assume a GPR11.
 */
#define NCR2_FACTORY_REQUEST_MAILBOX_ADDRESS UINT32_C(0x400F8044)
#define NCR2_FACTORY_REQUEST_MAILBOX_SIZE UINT32_C(0x00000004)

#endif

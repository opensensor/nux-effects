#ifndef NCR2_BOARD_H
#define NCR2_BOARD_H

#include <stdint.h>

#include "boot_recovery_request.h"

#define NCR2_BOARD_XTAL_HZ UINT32_C(24000000)
#define NCR2_BOARD_USB_IRQ_PRIORITY UINT32_C(3)

#define NCR2_RECOVERY_PRIMARY_GPIO_PIN UINT32_C(21)
#define NCR2_RECOVERY_GUARD_GPIO_PIN UINT32_C(2)

enum ncr2_board_status {
    NCR2_BOARD_OK = 0,
    NCR2_BOARD_USB_CLOCK_FAILED = 1,
    NCR2_BOARD_USB_PHY_FAILED = 2,
};

/*
 * Configure only the two recovered boot-selector inputs. This function
 * does not configure LEDs, relays, mute, audio, or any runtime control.
 */
void ncr2_board_recovery_input_init(void);
int ncr2_board_recovery_requested(void *context);
void ncr2_board_make_recovery_request(
    boot_recovery_request_t *request);

/*
 * Prepare USB1 for the already compile-checked recovery HID adapter.
 * The project VID/PID guard remains inside ncr2_recovery_usb_start().
 */
uint16_t ncr2_board_usb_clock_init(void);
void ncr2_board_usb_irq_enable(void);

/* Retains SRC GPRs and requests a Cortex-M system reset. */
__attribute__((noreturn))
void ncr2_board_warm_reset(void *context);

#endif

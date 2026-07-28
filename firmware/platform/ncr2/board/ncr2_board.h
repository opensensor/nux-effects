#ifndef NCR2_BOARD_H
#define NCR2_BOARD_H

#include <stdint.h>

#include "boot_recovery_request.h"

#define NCR2_BOARD_XTAL_HZ UINT32_C(24000000)
#define NCR2_BOARD_USB_IRQ_PRIORITY UINT32_C(3)

#define NCR2_RECOVERY_PRIMARY_GPIO_PIN UINT32_C(21)
#define NCR2_RECOVERY_GUARD_GPIO_PIN UINT32_C(2)

/* Four front-panel controls; index 2 is the stepped Type ladder. */
#define NCR2_BOARD_KNOB_COUNT 4U
#define NCR2_BOARD_KNOB_SELECTOR_INDEX 2U
#define NCR2_BOARD_KNOB_BURST 16U

enum ncr2_board_status {
    NCR2_BOARD_OK = 0,
    NCR2_BOARD_USB_CLOCK_FAILED = 1,
    NCR2_BOARD_USB_PHY_FAILED = 2,
};

/* Configure the two recovered boot-selector inputs. */
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

/*
 * Blink the factory runtime indicator bank while recovery is active.
 * The first hardware diagnostic identifies the visible status LED within
 * this five-pin bank so later revisions can narrow the mask.
 */
void ncr2_board_recovery_indicator_init(void);

/* Retains SRC GPRs and requests a Cortex-M system reset. */
__attribute__((noreturn))
void ncr2_board_warm_reset(void *context);

/*
 * Non-mutating front-panel capture used by the recovery READ_KNOBS command.
 * The converter is left disabled until the first call, so a recovery session
 * that never asks behaves exactly as it did before this existed.
 *
 * Returns the number of payload bytes written, or -1 if the capture failed.
 * Matches the recovery_backend_t read_knobs signature.
 */
int ncr2_board_recovery_read_knobs(
    void *context,
    void *destination,
    uint32_t capacity);

#endif

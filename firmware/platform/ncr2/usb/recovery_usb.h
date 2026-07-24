#ifndef NCR2_RECOVERY_USB_H
#define NCR2_RECOVERY_USB_H

#include <stdint.h>

#include "recovery_engine.h"

enum ncr2_recovery_usb_status {
    NCR2_RECOVERY_USB_OK = 0,
    NCR2_RECOVERY_USB_UNASSIGNED_ID = 1,
    NCR2_RECOVERY_USB_INIT_FAILED = 2,
};

uint16_t ncr2_recovery_usb_start(recovery_engine_t *engine);
void ncr2_recovery_usb_isr(void);

#endif

#ifndef NCR2_USB_AUDIO_CAPTURE_H
#define NCR2_USB_AUDIO_CAPTURE_H

#include <stdint.h>

#ifndef NCR2_HARDWARE_APP_USB_AUDIO
#define NCR2_HARDWARE_APP_USB_AUDIO 0
#endif

enum
{
    NCR2_USB_AUDIO_OK = 0,
    NCR2_USB_AUDIO_DISABLED = 1,
    NCR2_USB_AUDIO_UNASSIGNED_ID = 2,
    NCR2_USB_AUDIO_CLOCK_FAILED = 3,
    NCR2_USB_AUDIO_INIT_FAILED = 4,
};

#if NCR2_HARDWARE_APP_USB_AUDIO
uint16_t ncr2_usb_audio_capture_start(void);
void ncr2_usb_audio_capture_push(int32_t dry_sample);
void USB_OTG1_IRQHandler(void);
#else
static inline uint16_t ncr2_usb_audio_capture_start(void)
{
    return NCR2_USB_AUDIO_DISABLED;
}

static inline void ncr2_usb_audio_capture_push(int32_t dry_sample)
{
    (void)dry_sample;
}
#endif

#endif

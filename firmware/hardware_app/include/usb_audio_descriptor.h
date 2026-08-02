#ifndef NCR2_USB_AUDIO_DESCRIPTOR_H
#define NCR2_USB_AUDIO_DESCRIPTOR_H

#include <stdint.h>

#include "usb_device_config.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_device_audio.h"

#ifndef NCR2_OPEN_USB_VID
#define NCR2_OPEN_USB_VID 0U
#endif

#ifndef NCR2_OPEN_USB_PID
#define NCR2_OPEN_USB_PID 0U
#endif

enum
{
    NCR2_USB_AUDIO_CONFIGURATION_INDEX = 1,
    NCR2_USB_AUDIO_INTERFACE_COUNT = 2,
    NCR2_USB_AUDIO_CONTROL_INTERFACE = 0,
    NCR2_USB_AUDIO_STREAM_INTERFACE = 1,
    NCR2_USB_AUDIO_STREAM_ALTERNATE_OFF = 0,
    NCR2_USB_AUDIO_STREAM_ALTERNATE_ON = 1,
    NCR2_USB_AUDIO_STREAM_ENDPOINT = 1,
};

extern usb_device_class_struct_t g_ncr2_usb_audio_class;

usb_status_t USB_DeviceGetDeviceDescriptor(
    usb_device_handle handle,
    usb_device_get_device_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceGetConfigurationDescriptor(
    usb_device_handle handle,
    usb_device_get_configuration_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceGetStringDescriptor(
    usb_device_handle handle,
    usb_device_get_string_descriptor_struct_t *descriptor);
usb_status_t ncr2_usb_audio_descriptor_set_speed(uint8_t speed);

#endif

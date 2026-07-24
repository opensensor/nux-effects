#ifndef NCR2_RECOVERY_USB_DESCRIPTOR_H
#define NCR2_RECOVERY_USB_DESCRIPTOR_H

#include <stdint.h>

#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"

#ifndef NCR2_OPEN_USB_VID
#define NCR2_OPEN_USB_VID 0U
#endif

#ifndef NCR2_OPEN_USB_PID
#define NCR2_OPEN_USB_PID 0U
#endif

#define NCR2_USB_CONFIGURATION_INDEX 1U
#define NCR2_USB_INTERFACE_COUNT 1U
#define NCR2_USB_INTERFACE_INDEX 0U
#define NCR2_USB_INTERFACE_ALTERNATE 0U
#define NCR2_USB_ENDPOINT_COUNT 2U
#define NCR2_USB_ENDPOINT_IN 1U
#define NCR2_USB_ENDPOINT_OUT 2U
#define NCR2_USB_REPORT_SIZE 64U

extern usb_device_class_struct_t g_ncr2_recovery_hid_class;

usb_status_t USB_DeviceGetDeviceDescriptor(
    usb_device_handle handle,
    usb_device_get_device_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceGetConfigurationDescriptor(
    usb_device_handle handle,
    usb_device_get_configuration_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceGetStringDescriptor(
    usb_device_handle handle,
    usb_device_get_string_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceGetHidDescriptor(
    usb_device_handle handle,
    usb_device_get_hid_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceGetHidReportDescriptor(
    usb_device_handle handle,
    usb_device_get_hid_report_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceGetHidPhysicalDescriptor(
    usb_device_handle handle,
    usb_device_get_hid_physical_descriptor_struct_t *descriptor);
usb_status_t USB_DeviceSetSpeed(usb_device_handle handle, uint8_t speed);

#endif

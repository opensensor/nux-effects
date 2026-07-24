#include "recovery_usb_descriptor.h"

#include <stddef.h>
#include <stdint.h>

#include "usb.h"
#include "usb_device.h"
#include "usb_device_config.h"
#include "usb_device_hid.h"

#define NCR2_USB_SPEC_BCD UINT16_C(0x0200)
#define NCR2_USB_DEVICE_BCD UINT16_C(0x0001)
#define NCR2_USB_CONFIG_TOTAL_LENGTH 41U
#define NCR2_USB_HID_DESCRIPTOR_OFFSET 18U
#define NCR2_USB_HID_REPORT_DESCRIPTOR_LENGTH 34U
#define NCR2_USB_FULL_SPEED_INTERVAL 1U
#define NCR2_USB_HIGH_SPEED_INTERVAL 4U

static usb_device_endpoint_struct_t g_endpoints[NCR2_USB_ENDPOINT_COUNT] = {
    {
        NCR2_USB_ENDPOINT_IN |
            (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_INTERRUPT,
        NCR2_USB_REPORT_SIZE,
        NCR2_USB_FULL_SPEED_INTERVAL,
    },
    {
        NCR2_USB_ENDPOINT_OUT |
            (USB_OUT << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_INTERRUPT,
        NCR2_USB_REPORT_SIZE,
        NCR2_USB_FULL_SPEED_INTERVAL,
    },
};

static usb_device_interface_struct_t g_interface[] = {
    {
        NCR2_USB_INTERFACE_ALTERNATE,
        {
            NCR2_USB_ENDPOINT_COUNT,
            g_endpoints,
        },
        NULL,
    },
};

static usb_device_interfaces_struct_t g_interfaces[] = {
    {
        USB_DEVICE_CONFIG_HID_CLASS_CODE,
        0U,
        0U,
        NCR2_USB_INTERFACE_INDEX,
        g_interface,
        1U,
    },
};

static usb_device_interface_list_t g_interface_list[] = {
    {
        NCR2_USB_INTERFACE_COUNT,
        g_interfaces,
    },
};

usb_device_class_struct_t g_ncr2_recovery_hid_class = {
    g_interface_list,
    kUSB_DeviceClassTypeHid,
    1U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_report_descriptor[] = {
    0x06U, 0x00U, 0xFFU, /* Usage Page (vendor-defined 0xff00). */
    0x09U, 0x01U,       /* Usage 1. */
    0xA1U, 0x01U,       /* Collection (Application). */
    0x09U, 0x02U,       /* Input usage. */
    0x15U, 0x00U,       /* Logical minimum 0. */
    0x26U, 0xFFU, 0x00U, /* Logical maximum 255. */
    0x75U, 0x08U,       /* Report size 8. */
    0x95U, 0x40U,       /* Report count 64. */
    0x81U, 0x02U,       /* Input (Data, Variable, Absolute). */
    0x09U, 0x03U,       /* Output usage. */
    0x15U, 0x00U,       /* Logical minimum 0. */
    0x26U, 0xFFU, 0x00U, /* Logical maximum 255. */
    0x75U, 0x08U,       /* Report size 8. */
    0x95U, 0x40U,       /* Report count 64. */
    0x91U, 0x02U,       /* Output (Data, Variable, Absolute). */
    0xC0U,              /* End collection. */
};

_Static_assert(
    sizeof(g_report_descriptor) == NCR2_USB_HID_REPORT_DESCRIPTOR_LENGTH,
    "HID report descriptor length constant is stale");

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_device_descriptor[] = {
    USB_DESCRIPTOR_LENGTH_DEVICE,
    USB_DESCRIPTOR_TYPE_DEVICE,
    USB_SHORT_GET_LOW(NCR2_USB_SPEC_BCD),
    USB_SHORT_GET_HIGH(NCR2_USB_SPEC_BCD),
    0U,
    0U,
    0U,
    USB_CONTROL_MAX_PACKET_SIZE,
    USB_SHORT_GET_LOW(NCR2_OPEN_USB_VID),
    USB_SHORT_GET_HIGH(NCR2_OPEN_USB_VID),
    USB_SHORT_GET_LOW(NCR2_OPEN_USB_PID),
    USB_SHORT_GET_HIGH(NCR2_OPEN_USB_PID),
    USB_SHORT_GET_LOW(NCR2_USB_DEVICE_BCD),
    USB_SHORT_GET_HIGH(NCR2_USB_DEVICE_BCD),
    1U,
    2U,
    0U,
    1U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_configuration_descriptor[] = {
    9U,
    USB_DESCRIPTOR_TYPE_CONFIGURE,
    USB_SHORT_GET_LOW(NCR2_USB_CONFIG_TOTAL_LENGTH),
    USB_SHORT_GET_HIGH(NCR2_USB_CONFIG_TOTAL_LENGTH),
    NCR2_USB_INTERFACE_COUNT,
    NCR2_USB_CONFIGURATION_INDEX,
    0U,
    0xC0U,
    0U,

    9U,
    USB_DESCRIPTOR_TYPE_INTERFACE,
    NCR2_USB_INTERFACE_INDEX,
    NCR2_USB_INTERFACE_ALTERNATE,
    NCR2_USB_ENDPOINT_COUNT,
    USB_DEVICE_CONFIG_HID_CLASS_CODE,
    0U,
    0U,
    0U,

    9U,
    USB_DESCRIPTOR_TYPE_HID,
    0x11U,
    0x01U,
    0U,
    1U,
    USB_DESCRIPTOR_TYPE_HID_REPORT,
    USB_SHORT_GET_LOW(NCR2_USB_HID_REPORT_DESCRIPTOR_LENGTH),
    USB_SHORT_GET_HIGH(NCR2_USB_HID_REPORT_DESCRIPTOR_LENGTH),

    7U,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    NCR2_USB_ENDPOINT_IN |
        (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
    USB_ENDPOINT_INTERRUPT,
    USB_SHORT_GET_LOW(NCR2_USB_REPORT_SIZE),
    USB_SHORT_GET_HIGH(NCR2_USB_REPORT_SIZE),
    NCR2_USB_FULL_SPEED_INTERVAL,

    7U,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    NCR2_USB_ENDPOINT_OUT |
        (USB_OUT << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
    USB_ENDPOINT_INTERRUPT,
    USB_SHORT_GET_LOW(NCR2_USB_REPORT_SIZE),
    USB_SHORT_GET_HIGH(NCR2_USB_REPORT_SIZE),
    NCR2_USB_FULL_SPEED_INTERVAL,
};

_Static_assert(
    sizeof(g_configuration_descriptor) == NCR2_USB_CONFIG_TOTAL_LENGTH,
    "USB configuration descriptor length constant is stale");

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_string_language[] = {
    4U,
    USB_DESCRIPTOR_TYPE_STRING,
    0x09U,
    0x04U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_string_manufacturer[] = {
    22U,
    USB_DESCRIPTOR_TYPE_STRING,
    'O', 0U, 'p', 0U, 'e', 0U, 'n', 0U, 'S', 0U,
    'e', 0U, 'n', 0U, 's', 0U, 'o', 0U, 'r', 0U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_string_product[] = {
    50U,
    USB_DESCRIPTOR_TYPE_STRING,
    'N', 0U, 'U', 0U, 'X', 0U, ' ', 0U, 'E', 0U, 'f', 0U,
    'f', 0U, 'e', 0U, 'c', 0U, 't', 0U, 's', 0U, ' ', 0U,
    'O', 0U, 'p', 0U, 'e', 0U, 'n', 0U, ' ', 0U, 'R', 0U,
    'e', 0U, 'c', 0U, 'o', 0U, 'v', 0U, 'e', 0U, 'r', 0U,
    'y', 0U,
};

usb_status_t USB_DeviceGetDeviceDescriptor(
    usb_device_handle handle,
    usb_device_get_device_descriptor_struct_t *descriptor)
{
    (void)handle;
    descriptor->buffer = g_device_descriptor;
    descriptor->length = sizeof(g_device_descriptor);
    return kStatus_USB_Success;
}

usb_status_t USB_DeviceGetConfigurationDescriptor(
    usb_device_handle handle,
    usb_device_get_configuration_descriptor_struct_t *descriptor)
{
    (void)handle;
    if (descriptor->configuration != 0U) {
        return kStatus_USB_InvalidRequest;
    }
    descriptor->buffer = g_configuration_descriptor;
    descriptor->length = sizeof(g_configuration_descriptor);
    return kStatus_USB_Success;
}

usb_status_t USB_DeviceGetStringDescriptor(
    usb_device_handle handle,
    usb_device_get_string_descriptor_struct_t *descriptor)
{
    (void)handle;
    if (descriptor->stringIndex == 0U) {
        descriptor->buffer = g_string_language;
        descriptor->length = sizeof(g_string_language);
    } else if (descriptor->languageId != UINT16_C(0x0409)) {
        return kStatus_USB_InvalidRequest;
    } else if (descriptor->stringIndex == 1U) {
        descriptor->buffer = g_string_manufacturer;
        descriptor->length = sizeof(g_string_manufacturer);
    } else if (descriptor->stringIndex == 2U) {
        descriptor->buffer = g_string_product;
        descriptor->length = sizeof(g_string_product);
    } else {
        return kStatus_USB_InvalidRequest;
    }
    return kStatus_USB_Success;
}

usb_status_t USB_DeviceGetHidDescriptor(
    usb_device_handle handle,
    usb_device_get_hid_descriptor_struct_t *descriptor)
{
    (void)handle;
    if (descriptor->interfaceNumber != NCR2_USB_INTERFACE_INDEX) {
        return kStatus_USB_InvalidRequest;
    }
    descriptor->buffer =
        &g_configuration_descriptor[NCR2_USB_HID_DESCRIPTOR_OFFSET];
    descriptor->length = 9U;
    return kStatus_USB_Success;
}

usb_status_t USB_DeviceGetHidReportDescriptor(
    usb_device_handle handle,
    usb_device_get_hid_report_descriptor_struct_t *descriptor)
{
    (void)handle;
    if (descriptor->interfaceNumber != NCR2_USB_INTERFACE_INDEX) {
        return kStatus_USB_InvalidRequest;
    }
    descriptor->buffer = g_report_descriptor;
    descriptor->length = sizeof(g_report_descriptor);
    return kStatus_USB_Success;
}

usb_status_t USB_DeviceGetHidPhysicalDescriptor(
    usb_device_handle handle,
    usb_device_get_hid_physical_descriptor_struct_t *descriptor)
{
    (void)handle;
    (void)descriptor;
    return kStatus_USB_InvalidRequest;
}

usb_status_t USB_DeviceSetSpeed(usb_device_handle handle, uint8_t speed)
{
    uint8_t interval;

    (void)handle;
    interval =
        speed == USB_SPEED_HIGH
            ? NCR2_USB_HIGH_SPEED_INTERVAL
            : NCR2_USB_FULL_SPEED_INTERVAL;
    g_configuration_descriptor[33] = interval;
    g_configuration_descriptor[40] = interval;
    g_endpoints[0].interval = interval;
    g_endpoints[1].interval = interval;
    return kStatus_USB_Success;
}

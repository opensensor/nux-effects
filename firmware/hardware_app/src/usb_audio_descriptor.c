#include "usb_audio_descriptor.h"

#include <stddef.h>
#include <stdint.h>

#include "usb.h"
#include "usb_audio_ring.h"
#include "usb_device_config.h"

#define NCR2_USB_SPEC_BCD UINT16_C(0x0200)
#define NCR2_USB_DEVICE_BCD UINT16_C(0x0100)
#define NCR2_USB_AUDIO_CONFIG_LENGTH 108U
#define NCR2_USB_AUDIO_ENDPOINT_DESCRIPTOR_OFFSET 92U
#define NCR2_USB_AUDIO_FULL_SPEED_INTERVAL 1U
#define NCR2_USB_AUDIO_HIGH_SPEED_INTERVAL 4U
#define NCR2_USB_AUDIO_INPUT_TERMINAL_ID 1U
#define NCR2_USB_AUDIO_OUTPUT_TERMINAL_ID 2U

static usb_device_endpoint_struct_t g_stream_endpoints[] = {
    {
        NCR2_USB_AUDIO_STREAM_ENDPOINT |
            (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_ISOCHRONOUS,
        NCR2_USB_AUDIO_MAX_PACKET_BYTES,
        NCR2_USB_AUDIO_FULL_SPEED_INTERVAL,
    },
};

static usb_device_audio_entity_struct_t g_audio_entities[] = {
    {
        NCR2_USB_AUDIO_INPUT_TERMINAL_ID,
        USB_DESCRIPTOR_SUBTYPE_AUDIO_CONTROL_INPUT_TERMINAL,
        UINT16_C(0x0201),
    },
    {
        NCR2_USB_AUDIO_OUTPUT_TERMINAL_ID,
        USB_DESCRIPTOR_SUBTYPE_AUDIO_CONTROL_OUTPUT_TERMINAL,
        UINT16_C(0x0101),
    },
};

static usb_device_audio_entities_struct_t g_entity_list = {
    g_audio_entities,
    sizeof(g_audio_entities) / sizeof(g_audio_entities[0]),
};

static usb_device_interface_struct_t g_control_alternates[] = {
    {
        0U,
        { 0U, NULL },
        &g_entity_list,
    },
};

static usb_device_interface_struct_t g_stream_alternates[] = {
    {
        NCR2_USB_AUDIO_STREAM_ALTERNATE_OFF,
        { 0U, NULL },
        NULL,
    },
    {
        NCR2_USB_AUDIO_STREAM_ALTERNATE_ON,
        {
            sizeof(g_stream_endpoints) /
                sizeof(g_stream_endpoints[0]),
            g_stream_endpoints,
        },
        NULL,
    },
};

static usb_device_interfaces_struct_t g_interfaces[] = {
    {
        USB_DEVICE_CONFIG_AUDIO_CLASS_CODE,
        USB_DEVICE_AUDIO_CONTROL_SUBCLASS,
        0U,
        NCR2_USB_AUDIO_CONTROL_INTERFACE,
        g_control_alternates,
        sizeof(g_control_alternates) /
            sizeof(g_control_alternates[0]),
    },
    {
        USB_DEVICE_CONFIG_AUDIO_CLASS_CODE,
        USB_DEVICE_AUDIO_STREAM_SUBCLASS,
        0U,
        NCR2_USB_AUDIO_STREAM_INTERFACE,
        g_stream_alternates,
        sizeof(g_stream_alternates) /
            sizeof(g_stream_alternates[0]),
    },
};

static usb_device_interface_list_t g_interface_list[] = {
    {
        sizeof(g_interfaces) / sizeof(g_interfaces[0]),
        g_interfaces,
    },
};

usb_device_class_struct_t g_ncr2_usb_audio_class = {
    g_interface_list,
    kUSB_DeviceClassTypeAudio,
    1U,
};

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
    9U, USB_DESCRIPTOR_TYPE_CONFIGURE,
    USB_SHORT_GET_LOW(NCR2_USB_AUDIO_CONFIG_LENGTH),
    USB_SHORT_GET_HIGH(NCR2_USB_AUDIO_CONFIG_LENGTH),
    NCR2_USB_AUDIO_INTERFACE_COUNT,
    NCR2_USB_AUDIO_CONFIGURATION_INDEX,
    0U, 0xC0U, 0U,

    /* Audio function: control plus one capture stream. */
    8U, USB_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION,
    NCR2_USB_AUDIO_CONTROL_INTERFACE, 2U,
    USB_DEVICE_CONFIG_AUDIO_CLASS_CODE, 0U, 0U, 0U,

    9U, USB_DESCRIPTOR_TYPE_INTERFACE,
    NCR2_USB_AUDIO_CONTROL_INTERFACE, 0U, 0U,
    USB_DEVICE_CONFIG_AUDIO_CLASS_CODE,
    USB_DEVICE_AUDIO_CONTROL_SUBCLASS, 0U, 0U,

    /* UAC1 control header, input terminal, and USB-streaming terminal. */
    9U, USB_DESCRIPTOR_TYPE_AUDIO_CS_INTERFACE,
    USB_DESCRIPTOR_SUBTYPE_AUDIO_CONTROL_HEADER,
    0x00U, 0x01U, 30U, 0U, 1U,
    NCR2_USB_AUDIO_STREAM_INTERFACE,

    12U, USB_DESCRIPTOR_TYPE_AUDIO_CS_INTERFACE,
    USB_DESCRIPTOR_SUBTYPE_AUDIO_CONTROL_INPUT_TERMINAL,
    NCR2_USB_AUDIO_INPUT_TERMINAL_ID,
    0x01U, 0x02U, 0U, 1U, 0U, 0U, 0U, 3U,

    9U, USB_DESCRIPTOR_TYPE_AUDIO_CS_INTERFACE,
    USB_DESCRIPTOR_SUBTYPE_AUDIO_CONTROL_OUTPUT_TERMINAL,
    NCR2_USB_AUDIO_OUTPUT_TERMINAL_ID,
    0x01U, 0x01U, 0U,
    NCR2_USB_AUDIO_INPUT_TERMINAL_ID, 0U,

    9U, USB_DESCRIPTOR_TYPE_INTERFACE,
    NCR2_USB_AUDIO_STREAM_INTERFACE,
    NCR2_USB_AUDIO_STREAM_ALTERNATE_OFF,
    0U, USB_DEVICE_CONFIG_AUDIO_CLASS_CODE,
    USB_DEVICE_AUDIO_STREAM_SUBCLASS, 0U, 0U,

    9U, USB_DESCRIPTOR_TYPE_INTERFACE,
    NCR2_USB_AUDIO_STREAM_INTERFACE,
    NCR2_USB_AUDIO_STREAM_ALTERNATE_ON,
    1U, USB_DEVICE_CONFIG_AUDIO_CLASS_CODE,
    USB_DEVICE_AUDIO_STREAM_SUBCLASS, 0U, 0U,

    7U, USB_DESCRIPTOR_TYPE_AUDIO_CS_INTERFACE,
    USB_DESCRIPTOR_SUBTYPE_AUDIO_STREAMING_AS_GENERAL,
    NCR2_USB_AUDIO_OUTPUT_TERMINAL_ID,
    1U, 0x01U, 0x00U,

    11U, USB_DESCRIPTOR_TYPE_AUDIO_CS_INTERFACE,
    USB_DESCRIPTOR_SUBTYPE_AUDIO_STREAMING_FORMAT_TYPE,
    USB_AUDIO_FORMAT_TYPE_I, 1U,
    NCR2_USB_AUDIO_BYTES_PER_SAMPLE, 24U, 1U,
    0x80U, 0xBBU, 0x00U,

    /* Asynchronous source endpoint; 49 frames is the elastic maximum. */
    9U, USB_DESCRIPTOR_TYPE_ENDPOINT,
    NCR2_USB_AUDIO_STREAM_ENDPOINT |
        (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
    0x05U,
    USB_SHORT_GET_LOW(NCR2_USB_AUDIO_MAX_PACKET_BYTES),
    USB_SHORT_GET_HIGH(NCR2_USB_AUDIO_MAX_PACKET_BYTES),
    NCR2_USB_AUDIO_FULL_SPEED_INTERVAL,
    0U, 0U,

    7U, USB_DESCRIPTOR_TYPE_AUDIO_CS_ENDPOINT,
    1U, 0U, 0U, 0U, 0U,
};

_Static_assert(
    sizeof(g_configuration_descriptor) == NCR2_USB_AUDIO_CONFIG_LENGTH,
    "USB audio configuration descriptor length is stale");
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_string_language[] = {
    4U, USB_DESCRIPTOR_TYPE_STRING, 0x09U, 0x04U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_string_manufacturer[] = {
    22U, USB_DESCRIPTOR_TYPE_STRING,
    'O', 0U, 'p', 0U, 'e', 0U, 'n', 0U, 'S', 0U,
    'e', 0U, 'n', 0U, 's', 0U, 'o', 0U, 'r', 0U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_string_product[] = {
    46U, USB_DESCRIPTOR_TYPE_STRING,
    'N', 0U, 'C', 0U, 'R', 0U, '-', 0U, '2', 0U, ' ', 0U,
    'O', 0U, 'p', 0U, 'e', 0U, 'n', 0U, ' ', 0U,
    'P', 0U, 'e', 0U, 'd', 0U, 'a', 0U, 'l', 0U, ' ', 0U,
    'A', 0U, 'u', 0U, 'd', 0U, 'i', 0U, 'o', 0U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_string_input[] = {
    26U, USB_DESCRIPTOR_TYPE_STRING,
    'G', 0U, 'u', 0U, 'i', 0U, 't', 0U, 'a', 0U, 'r', 0U,
    ' ', 0U, 'I', 0U, 'n', 0U, 'p', 0U, 'u', 0U, 't', 0U,
};

static uint8_t *const g_strings[] = {
    g_string_language,
    g_string_manufacturer,
    g_string_product,
    g_string_input,
};

usb_status_t USB_DeviceGetDeviceDescriptor(
    usb_device_handle handle,
    usb_device_get_device_descriptor_struct_t *descriptor)
{
    (void)handle;
    if (descriptor == NULL) return kStatus_USB_InvalidParameter;
    descriptor->buffer = g_device_descriptor;
    descriptor->length = sizeof(g_device_descriptor);
    return kStatus_USB_Success;
}

usb_status_t USB_DeviceGetConfigurationDescriptor(
    usb_device_handle handle,
    usb_device_get_configuration_descriptor_struct_t *descriptor)
{
    (void)handle;
    if (descriptor == NULL || descriptor->configuration != 0U) {
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
    if (descriptor == NULL ||
        descriptor->stringIndex >=
            sizeof(g_strings) / sizeof(g_strings[0]) ||
        (descriptor->stringIndex != 0U &&
         descriptor->languageId != UINT16_C(0x0409))) {
        return kStatus_USB_InvalidRequest;
    }
    descriptor->buffer = g_strings[descriptor->stringIndex];
    descriptor->length = g_strings[descriptor->stringIndex][0];
    return kStatus_USB_Success;
}

usb_status_t ncr2_usb_audio_descriptor_set_speed(uint8_t speed)
{
    const uint8_t interval = speed == USB_SPEED_HIGH
        ? NCR2_USB_AUDIO_HIGH_SPEED_INTERVAL
        : NCR2_USB_AUDIO_FULL_SPEED_INTERVAL;

    g_stream_endpoints[0].interval = interval;
    g_configuration_descriptor[
        NCR2_USB_AUDIO_ENDPOINT_DESCRIPTOR_OFFSET + 6U] = interval;
    return kStatus_USB_Success;
}

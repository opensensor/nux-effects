#include "recovery_usb.h"

#include <stddef.h>
#include <stdint.h>

#include "recovery_usb_descriptor.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_device_config.h"
#include "usb_device_hid.h"

#define NCR2_USB_CONTROLLER kUSB_ControllerEhci0

_Static_assert(
    sizeof(recovery_packet_t) == NCR2_USB_REPORT_SIZE,
    "open recovery packet must occupy one HID report");

typedef struct ncr2_recovery_usb_context {
    recovery_engine_t *engine;
    usb_device_handle device;
    class_handle_t hid;
    uint8_t configuration;
    uint8_t attached;
} ncr2_recovery_usb_context_t;

USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static recovery_packet_t g_out_packet;
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static recovery_packet_t g_in_packet;
static ncr2_recovery_usb_context_t g_usb;

static usb_status_t arm_receive(void)
{
    if (g_usb.hid == NULL) {
        return kStatus_USB_InvalidHandle;
    }
    return USB_DeviceHidRecv(
        g_usb.hid,
        NCR2_USB_ENDPOINT_OUT,
        (uint8_t *)&g_out_packet,
        sizeof(g_out_packet));
}

static usb_status_t hid_callback(
    class_handle_t handle, uint32_t event, void *parameter)
{
    usb_device_endpoint_callback_message_struct_t *message =
        (usb_device_endpoint_callback_message_struct_t *)parameter;

    (void)handle;
    switch (event) {
    case kUSB_DeviceHidEventRecvResponse:
        if (g_usb.attached == 0U ||
            g_usb.engine == NULL ||
            message == NULL ||
            message->length != sizeof(g_out_packet)) {
            return arm_receive();
        }
        recovery_engine_process(
            g_usb.engine,
            &g_out_packet,
            &g_in_packet);
        return USB_DeviceHidSend(
            g_usb.hid,
            NCR2_USB_ENDPOINT_IN,
            (uint8_t *)&g_in_packet,
            sizeof(g_in_packet));
    case kUSB_DeviceHidEventSendResponse:
        return arm_receive();
    case kUSB_DeviceHidEventGetIdle:
    case kUSB_DeviceHidEventGetProtocol:
    case kUSB_DeviceHidEventSetIdle:
    case kUSB_DeviceHidEventSetProtocol:
        return kStatus_USB_Success;
    case kUSB_DeviceHidEventGetReport:
    case kUSB_DeviceHidEventSetReport:
    case kUSB_DeviceHidEventRequestReportBuffer:
    default:
        return kStatus_USB_InvalidRequest;
    }
}

static usb_status_t device_callback(
    usb_device_handle handle, uint32_t event, void *parameter)
{
    uint8_t *value8 = (uint8_t *)parameter;
    uint16_t *value16 = (uint16_t *)parameter;

    switch (event) {
    case kUSB_DeviceEventBusReset:
        g_usb.attached = 0U;
        g_usb.configuration = 0U;
        {
            uint8_t speed;
            if (USB_DeviceClassGetSpeed(
                    NCR2_USB_CONTROLLER,
                    &speed) == kStatus_USB_Success) {
                (void)USB_DeviceSetSpeed(handle, speed);
            }
        }
        return kStatus_USB_Success;
    case kUSB_DeviceEventSetConfiguration:
        if (value8 == NULL) {
            return kStatus_USB_InvalidParameter;
        }
        if (*value8 == 0U) {
            g_usb.attached = 0U;
            g_usb.configuration = 0U;
            return kStatus_USB_Success;
        }
        if (*value8 == NCR2_USB_CONFIGURATION_INDEX) {
            g_usb.attached = 1U;
            g_usb.configuration = *value8;
            return arm_receive();
        }
        return kStatus_USB_InvalidRequest;
    case kUSB_DeviceEventGetConfiguration:
        if (value8 == NULL) {
            return kStatus_USB_InvalidParameter;
        }
        *value8 = g_usb.configuration;
        return kStatus_USB_Success;
    case kUSB_DeviceEventSetInterface:
        if (value16 == NULL ||
            (*value16 >> 8U) != NCR2_USB_INTERFACE_INDEX ||
            (*value16 & UINT16_C(0xFF)) !=
                NCR2_USB_INTERFACE_ALTERNATE) {
            return kStatus_USB_InvalidRequest;
        }
        return arm_receive();
    case kUSB_DeviceEventGetInterface:
        if (value16 == NULL ||
            (*value16 >> 8U) != NCR2_USB_INTERFACE_INDEX) {
            return kStatus_USB_InvalidRequest;
        }
        *value16 =
            (uint16_t)(
                *value16 & UINT16_C(0xFF00));
        return kStatus_USB_Success;
    case kUSB_DeviceEventGetDeviceDescriptor:
        return USB_DeviceGetDeviceDescriptor(
            handle,
            (usb_device_get_device_descriptor_struct_t *)parameter);
    case kUSB_DeviceEventGetConfigurationDescriptor:
        return USB_DeviceGetConfigurationDescriptor(
            handle,
            (usb_device_get_configuration_descriptor_struct_t *)parameter);
    case kUSB_DeviceEventGetStringDescriptor:
        return USB_DeviceGetStringDescriptor(
            handle,
            (usb_device_get_string_descriptor_struct_t *)parameter);
    case kUSB_DeviceEventGetHidDescriptor:
        return USB_DeviceGetHidDescriptor(
            handle,
            (usb_device_get_hid_descriptor_struct_t *)parameter);
    case kUSB_DeviceEventGetHidReportDescriptor:
        return USB_DeviceGetHidReportDescriptor(
            handle,
            (usb_device_get_hid_report_descriptor_struct_t *)parameter);
    case kUSB_DeviceEventGetHidPhysicalDescriptor:
        return USB_DeviceGetHidPhysicalDescriptor(
            handle,
            (usb_device_get_hid_physical_descriptor_struct_t *)parameter);
    default:
        return kStatus_USB_InvalidRequest;
    }
}

static usb_device_class_config_struct_t g_class_config[] = {
    {
        hid_callback,
        NULL,
        &g_ncr2_recovery_hid_class,
    },
};

static usb_device_class_config_list_struct_t g_config_list = {
    g_class_config,
    device_callback,
    1U,
};

uint16_t ncr2_recovery_usb_start(recovery_engine_t *engine)
{
    if (NCR2_OPEN_USB_VID == 0U ||
        NCR2_OPEN_USB_PID == 0U ||
        NCR2_OPEN_USB_VID == UINT16_C(0x9527)) {
        return NCR2_RECOVERY_USB_UNASSIGNED_ID;
    }
    if (engine == NULL) {
        return NCR2_RECOVERY_USB_INIT_FAILED;
    }

    g_usb.engine = engine;
    g_usb.device = NULL;
    g_usb.hid = NULL;
    g_usb.configuration = 0U;
    g_usb.attached = 0U;
    if (USB_DeviceClassInit(
            NCR2_USB_CONTROLLER,
            &g_config_list,
            &g_usb.device) != kStatus_USB_Success) {
        return NCR2_RECOVERY_USB_INIT_FAILED;
    }
    g_usb.hid = g_class_config[0].classHandle;
    if (USB_DeviceRun(g_usb.device) != kStatus_USB_Success) {
        return NCR2_RECOVERY_USB_INIT_FAILED;
    }
    return NCR2_RECOVERY_USB_OK;
}

void ncr2_recovery_usb_isr(void)
{
    if (g_usb.device != NULL) {
        USB_DeviceEhciIsrFunction(g_usb.device);
    }
}

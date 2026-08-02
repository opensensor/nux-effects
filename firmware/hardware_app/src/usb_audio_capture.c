#include "usb_audio_capture.h"

#if NCR2_HARDWARE_APP_USB_AUDIO

#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "usb_device_config.h"
#include "usb.h"
#include "usb_audio_descriptor.h"
#include "usb_audio_ring.h"
#include "usb_device.h"
#include "usb_device_audio.h"
#include "usb_device_class.h"
#include "usb_phy.h"

#ifndef NCR2_ALLOW_BORROWED_NUX_DFU_ID
#define NCR2_ALLOW_BORROWED_NUX_DFU_ID 0
#endif

#define NCR2_USB_CONTROLLER kUSB_ControllerEhci0
#define NCR2_USB_XTAL_HZ UINT32_C(24000000)
#define NCR2_USB_PHY_D_CAL UINT8_C(0x0C)
#define NCR2_USB_PHY_TXCAL45DP UINT8_C(0x06)
#define NCR2_USB_PHY_TXCAL45DM UINT8_C(0x06)
#define NCR2_USB_IRQ_PRIORITY UINT32_C(3)

typedef struct ncr2_usb_audio_context
{
    usb_device_handle device;
    class_handle_t audio;
    uint8_t configuration;
    uint8_t speed;
    volatile uint8_t attached;
    volatile uint8_t streaming;
} ncr2_usb_audio_context_t;

USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t g_usb_packet[NCR2_USB_AUDIO_MAX_PACKET_BYTES];
static ncr2_usb_audio_ring_t g_capture_ring;
static ncr2_usb_audio_context_t g_usb;

volatile uint32_t g_hardware_app_usb_packets;
volatile uint32_t g_hardware_app_usb_send_failures;

static usb_status_t prime_audio_packet(void)
{
    const size_t length = ncr2_usb_audio_ring_packet(
        &g_capture_ring,
        g_usb_packet,
        sizeof(g_usb_packet));

    if (length == 0U || g_usb.audio == NULL ||
        g_usb.streaming == UINT8_C(0)) {
        return kStatus_USB_InvalidHandle;
    }
    return USB_DeviceAudioSend(
        g_usb.audio,
        NCR2_USB_AUDIO_STREAM_ENDPOINT,
        g_usb_packet,
        (uint32_t)length);
}

static usb_status_t audio_callback(
    class_handle_t handle,
    uint32_t event,
    void *parameter)
{
    (void)handle;
    if (event == kUSB_DeviceAudioEventStreamSendResponse) {
        usb_device_endpoint_callback_message_struct_t *const message =
            (usb_device_endpoint_callback_message_struct_t *)parameter;

        if (message == NULL || g_usb.attached == UINT8_C(0) ||
            g_usb.streaming == UINT8_C(0)) {
            return kStatus_USB_InvalidRequest;
        }
        ++g_hardware_app_usb_packets;
        if (prime_audio_packet() != kStatus_USB_Success) {
            ++g_hardware_app_usb_send_failures;
            return kStatus_USB_Error;
        }
        return kStatus_USB_Success;
    }
    /* The descriptor exposes no host-programmable feature or endpoint
     * controls. Standard enumeration and alternate settings are handled by
     * the common class layer. */
    return kStatus_USB_InvalidRequest;
}

static usb_status_t device_callback(
    usb_device_handle handle,
    uint32_t event,
    void *parameter)
{
    uint8_t *const value8 = (uint8_t *)parameter;
    uint16_t *const value16 = (uint16_t *)parameter;

    switch (event) {
    case kUSB_DeviceEventBusReset:
        g_usb.attached = UINT8_C(0);
        g_usb.streaming = UINT8_C(0);
        g_usb.configuration = UINT8_C(0);
        ncr2_usb_audio_ring_reset(&g_capture_ring);
        if (USB_DeviceClassGetSpeed(
                NCR2_USB_CONTROLLER,
                &g_usb.speed) == kStatus_USB_Success) {
            (void)ncr2_usb_audio_descriptor_set_speed(g_usb.speed);
        }
        return kStatus_USB_Success;
    case kUSB_DeviceEventSetConfiguration:
        if (value8 == NULL) return kStatus_USB_InvalidParameter;
        if (*value8 == UINT8_C(0)) {
            g_usb.attached = UINT8_C(0);
            g_usb.streaming = UINT8_C(0);
            g_usb.configuration = UINT8_C(0);
            return kStatus_USB_Success;
        }
        if (*value8 == NCR2_USB_AUDIO_CONFIGURATION_INDEX) {
            g_usb.attached = UINT8_C(1);
            g_usb.configuration = *value8;
            return kStatus_USB_Success;
        }
        return kStatus_USB_InvalidRequest;
    case kUSB_DeviceEventGetConfiguration:
        if (value8 == NULL) return kStatus_USB_InvalidParameter;
        *value8 = g_usb.configuration;
        return kStatus_USB_Success;
    case kUSB_DeviceEventSetInterface:
        if (value16 == NULL || g_usb.attached == UINT8_C(0)) {
            return kStatus_USB_InvalidParameter;
        }
        {
            const uint8_t interface = (uint8_t)(*value16 >> 8U);
            const uint8_t alternate = (uint8_t)(
                *value16 & UINT16_C(0xFF));

            if (interface == NCR2_USB_AUDIO_CONTROL_INTERFACE &&
                alternate == UINT8_C(0)) {
                return kStatus_USB_Success;
            }
            if (interface != NCR2_USB_AUDIO_STREAM_INTERFACE ||
                alternate > NCR2_USB_AUDIO_STREAM_ALTERNATE_ON) {
                return kStatus_USB_InvalidRequest;
            }
            g_usb.streaming = alternate ==
                NCR2_USB_AUDIO_STREAM_ALTERNATE_ON
                ? UINT8_C(1)
                : UINT8_C(0);
            ncr2_usb_audio_ring_reset(&g_capture_ring);
            if (g_usb.streaming != UINT8_C(0) &&
                prime_audio_packet() != kStatus_USB_Success) {
                g_usb.streaming = UINT8_C(0);
                ++g_hardware_app_usb_send_failures;
                return kStatus_USB_Error;
            }
            return kStatus_USB_Success;
        }
    case kUSB_DeviceEventGetInterface:
        if (value16 == NULL) return kStatus_USB_InvalidParameter;
        if ((*value16 >> 8U) == NCR2_USB_AUDIO_CONTROL_INTERFACE) {
            *value16 &= UINT16_C(0xFF00);
            return kStatus_USB_Success;
        }
        if ((*value16 >> 8U) == NCR2_USB_AUDIO_STREAM_INTERFACE) {
            *value16 = (uint16_t)(
                (*value16 & UINT16_C(0xFF00)) |
                (uint16_t)(g_usb.streaming != UINT8_C(0)
                    ? NCR2_USB_AUDIO_STREAM_ALTERNATE_ON
                    : NCR2_USB_AUDIO_STREAM_ALTERNATE_OFF));
            return kStatus_USB_Success;
        }
        return kStatus_USB_InvalidRequest;
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
    default:
        return kStatus_USB_InvalidRequest;
    }
}

static usb_device_class_config_struct_t g_class_config[] = {
    {
        audio_callback,
        NULL,
        &g_ncr2_usb_audio_class,
    },
};

static usb_device_class_config_list_struct_t g_config_list = {
    g_class_config,
    device_callback,
    sizeof(g_class_config) / sizeof(g_class_config[0]),
};

static uint16_t initialize_usb_clock(void)
{
    usb_phy_config_struct_t phy = {
        .D_CAL = NCR2_USB_PHY_D_CAL,
        .TXCAL45DP = NCR2_USB_PHY_TXCAL45DP,
        .TXCAL45DM = NCR2_USB_PHY_TXCAL45DM,
    };

    if (!CLOCK_EnableUsbhs0PhyPllClock(
            kCLOCK_Usbphy480M,
            UINT32_C(480000000)) ||
        !CLOCK_EnableUsbhs0Clock(
            kCLOCK_Usb480M,
            UINT32_C(480000000))) {
        return NCR2_USB_AUDIO_CLOCK_FAILED;
    }
    if (USB_EhciPhyInit(
            (uint8_t)NCR2_USB_CONTROLLER,
            NCR2_USB_XTAL_HZ,
            &phy) != (uint32_t)kStatus_USB_Success) {
        return NCR2_USB_AUDIO_CLOCK_FAILED;
    }
    return NCR2_USB_AUDIO_OK;
}

uint16_t ncr2_usb_audio_capture_start(void)
{
    uint16_t status;

    if (NCR2_OPEN_USB_VID == 0U || NCR2_OPEN_USB_PID == 0U ||
        (NCR2_OPEN_USB_VID == UINT16_C(0x9527) &&
         NCR2_ALLOW_BORROWED_NUX_DFU_ID == 0)) {
        return NCR2_USB_AUDIO_UNASSIGNED_ID;
    }
    g_usb.device = NULL;
    g_usb.audio = NULL;
    g_usb.configuration = UINT8_C(0);
    g_usb.speed = USB_SPEED_FULL;
    g_usb.attached = UINT8_C(0);
    g_usb.streaming = UINT8_C(0);
    g_hardware_app_usb_packets = UINT32_C(0);
    g_hardware_app_usb_send_failures = UINT32_C(0);
    ncr2_usb_audio_ring_reset(&g_capture_ring);

    status = initialize_usb_clock();
    if (status != NCR2_USB_AUDIO_OK) return status;
    if (USB_DeviceClassInit(
            NCR2_USB_CONTROLLER,
            &g_config_list,
            &g_usb.device) != kStatus_USB_Success) {
        return NCR2_USB_AUDIO_INIT_FAILED;
    }
    g_usb.audio = g_class_config[0].classHandle;
    if (g_usb.audio == NULL) return NCR2_USB_AUDIO_INIT_FAILED;

    NVIC_ClearPendingIRQ(USB_OTG1_IRQn);
    NVIC_SetPriority(USB_OTG1_IRQn, NCR2_USB_IRQ_PRIORITY);
    EnableIRQ(USB_OTG1_IRQn);
    if (USB_DeviceRun(g_usb.device) != kStatus_USB_Success) {
        return NCR2_USB_AUDIO_INIT_FAILED;
    }
    return NCR2_USB_AUDIO_OK;
}

void ncr2_usb_audio_capture_push(int32_t dry_sample)
{
    if (g_usb.streaming != UINT8_C(0)) {
        ncr2_usb_audio_ring_push(&g_capture_ring, dry_sample);
    }
}

void USB_OTG1_IRQHandler(void)
{
    if (g_usb.device != NULL) {
        USB_DeviceEhciIsrFunction(g_usb.device);
    }
}

#endif

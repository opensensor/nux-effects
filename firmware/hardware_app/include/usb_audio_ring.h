#ifndef NCR2_USB_AUDIO_RING_H
#define NCR2_USB_AUDIO_RING_H

#include <stddef.h>
#include <stdint.h>

enum
{
    NCR2_USB_AUDIO_SAMPLE_RATE_HZ = 48000,
    NCR2_USB_AUDIO_NOMINAL_PACKET_FRAMES = 48,
    NCR2_USB_AUDIO_MIN_PACKET_FRAMES = 47,
    NCR2_USB_AUDIO_MAX_PACKET_FRAMES = 49,
    NCR2_USB_AUDIO_BYTES_PER_SAMPLE = 3,
    NCR2_USB_AUDIO_MAX_PACKET_BYTES =
        NCR2_USB_AUDIO_MAX_PACKET_FRAMES *
        NCR2_USB_AUDIO_BYTES_PER_SAMPLE,
    NCR2_USB_AUDIO_RING_CAPACITY = 1024,
    NCR2_USB_AUDIO_RING_MASK = NCR2_USB_AUDIO_RING_CAPACITY - 1,
};

typedef struct ncr2_usb_audio_ring
{
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t overrun_frames;
    volatile uint32_t underrun_packets;
    volatile uint32_t captured_frames;
    volatile uint32_t streamed_frames;
    volatile int32_t samples[NCR2_USB_AUDIO_RING_CAPACITY];
} ncr2_usb_audio_ring_t;

void ncr2_usb_audio_ring_reset(ncr2_usb_audio_ring_t *ring);
void ncr2_usb_audio_ring_push(
    ncr2_usb_audio_ring_t *ring,
    int32_t sample);
size_t ncr2_usb_audio_ring_available(
    const ncr2_usb_audio_ring_t *ring);

/* Build one 1 ms, mono, signed 24-bit little-endian USB Audio packet.
 * Packet length may be 47, 48, or 49 frames to absorb the independent SAI
 * and USB clock tolerances without ever back-pressuring the audio ISR. */
size_t ncr2_usb_audio_ring_packet(
    ncr2_usb_audio_ring_t *ring,
    uint8_t *packet,
    size_t capacity);

#endif

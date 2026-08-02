#include "usb_audio_ring.h"

_Static_assert(
    (NCR2_USB_AUDIO_RING_CAPACITY & NCR2_USB_AUDIO_RING_MASK) == 0,
    "USB audio ring capacity must be a power of two");

void ncr2_usb_audio_ring_reset(ncr2_usb_audio_ring_t *ring)
{
    if (ring == NULL) return;
    ring->read_index = UINT32_C(0);
    ring->write_index = UINT32_C(0);
    ring->overrun_frames = UINT32_C(0);
    ring->underrun_packets = UINT32_C(0);
    ring->captured_frames = UINT32_C(0);
    ring->streamed_frames = UINT32_C(0);
}

void ncr2_usb_audio_ring_push(
    ncr2_usb_audio_ring_t *ring,
    int32_t sample)
{
    uint32_t write;
    uint32_t next;

    if (ring == NULL) return;
    write = ring->write_index;
    next = (write + UINT32_C(1)) & NCR2_USB_AUDIO_RING_MASK;
    if (next == ring->read_index) {
        ++ring->overrun_frames;
        return;
    }
    ring->samples[write] = sample;
    ring->write_index = next;
    ++ring->captured_frames;
}

size_t ncr2_usb_audio_ring_available(
    const ncr2_usb_audio_ring_t *ring)
{
    uint32_t write;
    uint32_t read;

    if (ring == NULL) return 0U;
    write = ring->write_index;
    read = ring->read_index;
    return (size_t)((write - read) & NCR2_USB_AUDIO_RING_MASK);
}

static void store_pcm24(uint8_t *output, int32_t sample)
{
    const int32_t pcm = sample / INT32_C(256);
    const uint32_t bits = (uint32_t)pcm;

    output[0] = (uint8_t)(bits & UINT32_C(0xFF));
    output[1] = (uint8_t)((bits >> 8U) & UINT32_C(0xFF));
    output[2] = (uint8_t)((bits >> 16U) & UINT32_C(0xFF));
}

size_t ncr2_usb_audio_ring_packet(
    ncr2_usb_audio_ring_t *ring,
    uint8_t *packet,
    size_t capacity)
{
    const size_t available = ncr2_usb_audio_ring_available(ring);
    size_t frames = NCR2_USB_AUDIO_NOMINAL_PACKET_FRAMES;
    size_t consumed = available;

    if (ring == NULL || packet == NULL ||
        capacity < (size_t)NCR2_USB_AUDIO_MAX_PACKET_BYTES) {
        return 0U;
    }
    if (available >
        (size_t)(NCR2_USB_AUDIO_NOMINAL_PACKET_FRAMES * 3)) {
        frames = NCR2_USB_AUDIO_MAX_PACKET_FRAMES;
    } else if (available <
               (size_t)NCR2_USB_AUDIO_NOMINAL_PACKET_FRAMES) {
        frames = NCR2_USB_AUDIO_MIN_PACKET_FRAMES;
    }
    if (consumed > frames) consumed = frames;

    for (size_t frame = 0U; frame < consumed; ++frame) {
        const uint32_t read = ring->read_index;

        store_pcm24(
            &packet[frame * NCR2_USB_AUDIO_BYTES_PER_SAMPLE],
            ring->samples[read]);
        ring->read_index =
            (read + UINT32_C(1)) & NCR2_USB_AUDIO_RING_MASK;
    }
    for (size_t frame = consumed; frame < frames; ++frame) {
        store_pcm24(
            &packet[frame * NCR2_USB_AUDIO_BYTES_PER_SAMPLE],
            INT32_C(0));
    }
    if (consumed < frames) ++ring->underrun_packets;
    ring->streamed_frames += (uint32_t)frames;
    return frames * NCR2_USB_AUDIO_BYTES_PER_SAMPLE;
}

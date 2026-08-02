# Normal-application USB guitar capture

The source-native hardware application can be built to expose the guitar
connected to the pedal as a standard USB Audio Class 1 capture device. This is
the pedal-first editor path:

```text
guitar -> NCR-2 analog input -> AK4619/SAI1 -> USB-C -> local editor
                                      `-----> pedal DSP -> analog output
```

This is not Open Recover. Recovery remains the small HID updater and does not
start the codec or audio DMA. USB Audio belongs to the normal application,
where the guitar stream already exists.

## Stream contract

- USB Audio Class 1, capture only;
- one signed PCM channel at 48 kHz and 24 bits;
- asynchronous isochronous endpoint 1 IN;
- 47, 48, or 49 frames per 1 ms packet; and
- product string `NCR-2 Open Pedal Audio`.

The SAI/eDMA callback copies the selected dry TDM sample into a 1024-frame
single-producer/single-consumer ring. USB consumes that copy from its own IRQ.
Audio DMA has the higher interrupt priority. A full ring drops the new USB
copy, and an empty ring sends zeroes; neither condition can wait on or alter
the analog effects path.

The variable packet size is an elastic clock-domain bridge. SAI and USB derive
their timing independently, so sending a fixed 48 frames forever would
eventually overrun or underrun even though both clocks nominally represent
48 kHz.

## Build and identity gate

The feature is off by default. A build must explicitly enable hardware USB,
provide an assigned VID, provide one PID for Open Recover, and provide a
different PID for normal-app audio:

```sh
cmake -S firmware -B build/usb-audio \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DNCR2_BUILD_HARDWARE_AUDIO_APP=ON \
  -DNCR2_HARDWARE_APP_USB_AUDIO=ON \
  -DNCR2_ENABLE_HARDWARE_USB_ENUMERATION=ON \
  -DNCR2_OPEN_USB_VID=<assigned-vid> \
  -DNCR2_OPEN_USB_PID=<recovery-pid> \
  -DNCR2_OPEN_AUDIO_USB_PID=<different-audio-pid> \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/usb-audio --target ncr2_hardware_audio_app
```

CMake refuses zero IDs, identical recovery/audio PIDs, and the project's
bench-only borrowed NUX recovery identity. Using a distinct product identity
also prevents operating-system descriptor caches from confusing the updater
with the audio device.

The current code has passed strict ARM compilation, RAM-only post-link checks,
and host tests for PCM packing, clock-elastic packet sizing, underflow, and
overflow.

## First physical result

On 2026-08-02, v0.26.0/build 58 was installed to inactive slot B of the Verb
Core Deluxe through Open Recover. Its application payload SHA-256 was
`774a019d1e62af8e7872889c0fbec025e8fb2bff263501fd58607fc91e121dbf`.
The normal app then:

- enumerated at high speed as bench identity `cafe:4e58`;
- reported manufacturer `OpenSensor` and product
  `NCR-2 Open Pedal Audio`;
- registered as an ALSA and PipeWire mono capture source;
- advertised only signed 24-bit little-endian, one channel, 48 kHz; and
- completed a continuous three-second/144,000-frame capture without a USB
  error or disconnect.

No guitar was played during that recording, so its approximately -89 dBFS
peak represents idle capture, not validation of musical signal level or sound
quality. The local editor has since passed direct recording and a persistent
native-DSP transport check against this physical device: ten consecutive
chunks totaling 10,240 frames were captured, processed, and returned without
a transport error. Audible guitar tone and latency remain listening checks,
not conclusions from that transport test. The `cafe:4e58` identity is
bench-only and must be replaced with a project-owned VID/PID before
distribution.

## Editor behavior

On Linux, the editor server resolves the pedal's dynamic ALSA card number from
USB identity `cafe:4e58` and product string, then exposes **Pedal USB — direct**
as the preferred input. Direct capture uses `arecord` with the device's exact
48 kHz, mono, packed-24-bit contract and does not depend on Chromium exposing
the pedal or granting microphone permission. Both six-second recording and
stateful live preview support this path.

The selector also lists browser `audioinput` devices for ordinary interfaces
and non-Linux fallback. Those requests disable browser gain control, echo
cancellation, and noise suppression. The direct pedal path converts packed
PCM24 to float32, duplicates mono only when the selected preview format is
stereo, applies a measured +18 dB pedal calibration, and then applies the
editor's explicit Input trim before native DSP. An eight-second physical
capture measured played-guitar peak/RMS at approximately 0.027/0.005 versus
0.204/0.033 for the bundled clean DI. It contained no large discontinuities;
one 47-frame zero pad occurred at capture startup.

ALSA otherwise chooses a 6,000-frame/125 ms capture period and a 24,000-frame
buffer for this endpoint. That default made localhost requests alternate
between sub-millisecond bursts and 120–132 ms stalls, producing severe
mute/refill modulation and accumulated delay in live playback. Direct editor
capture therefore requests a 256-frame period and 1,024-frame buffer. On the
physical pedal, 300 consecutive 1,024-frame reads then measured 21.0 ms median,
22.0 ms p95, and 28.5 ms maximum against the ideal 21.33 ms cadence.

USB Audio only transports dry guitar into the browser today. Effect parameters
still run in the editor's persistent native preview process. A later composite
normal-application HID interface can carry block-boundary-safe parameter and
engine updates without changing or enlarging Open Recover.

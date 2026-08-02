# Normal-application USB guitar capture

The source-native hardware application can be built to expose the guitar
connected to the pedal as a standard USB Audio Class 1 capture device. This is
the pedal-first editor path:

```text
guitar -> NCR-2 analog input -> AK4619/SAI1 -> USB-C -> browser editor
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
overflow. It has not yet been flashed for physical USB Audio enumeration. Do
not treat the build gate as hardware validation.

## Editor behavior

The editor lists all browser `audioinput` devices. Its automatic choice
prefers the product string above after capture permission reveals device
labels. The selector also supports the system default and ordinary audio
interfaces. Both six-second recording and stateful live preview use the same
selection and request raw capture with browser gain control, echo cancellation,
and noise suppression disabled.

USB Audio only transports dry guitar into the browser today. Effect parameters
still run in the editor's persistent native preview process. A later composite
normal-application HID interface can carry block-boundary-safe parameter and
engine updates without changing or enlarging Open Recover.

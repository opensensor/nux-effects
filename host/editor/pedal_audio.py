"""Direct ALSA capture for the normal-app NCR-2 USB Audio source."""

from __future__ import annotations

import math
import os
import select
import shutil
import struct
import subprocess
import time
from pathlib import Path
from typing import Any


SAMPLE_RATE = 48_000
BYTES_PER_SAMPLE = 3
CAPTURE_PERIOD_FRAMES = 256
CAPTURE_BUFFER_FRAMES = 1024
PEDAL_PRODUCT = "NCR-2 Open Pedal Audio"
BENCH_USB_ID = "cafe:4e58"
DEFAULT_ASOUND_ROOT = Path("/proc/asound")


class PedalAudioError(RuntimeError):
    """The direct pedal capture device is absent or stopped."""


def discover(asound_root: Path = DEFAULT_ASOUND_ROOT) -> dict[str, Any]:
    """Resolve the dynamic ALSA card number from its USB/product identity."""
    if shutil.which("arecord") is None:
        return {"available": False, "reason": "arecord is not installed"}
    for usb_id_path in sorted(asound_root.glob("card[0-9]*/usbid")):
        card_directory = usb_id_path.parent
        usb_id = usb_id_path.read_text(errors="replace").strip().lower()
        stream_path = card_directory / "stream0"
        stream = stream_path.read_text(errors="replace") if stream_path.is_file() else ""
        if usb_id != BENCH_USB_ID and PEDAL_PRODUCT not in stream:
            continue
        card_number = int(card_directory.name.removeprefix("card"))
        card_id_path = card_directory / "id"
        card_id = (
            card_id_path.read_text(errors="replace").strip()
            if card_id_path.is_file()
            else str(card_number)
        )
        return {
            "available": True,
            "name": f"{PEDAL_PRODUCT} Mono",
            "usb_id": usb_id,
            "card": card_number,
            "card_id": card_id,
            "device": f"hw:{card_number},0",
            "sample_rate": SAMPLE_RATE,
            "channels": 1,
            "sample_bits": 24,
        }
    return {
        "available": False,
        "reason": f"{PEDAL_PRODUCT} is not registered with ALSA",
    }


def pcm24_to_float32(
    payload: bytes,
    channels: int,
    gain_db: float = 0.0,
) -> bytes:
    """Convert mono packed signed PCM24 to interleaved float32 frames."""
    if len(payload) % BYTES_PER_SAMPLE:
        raise PedalAudioError("pedal capture returned a partial PCM24 frame")
    if channels not in (1, 2):
        raise PedalAudioError("direct pedal preview supports mono or stereo")
    if not math.isfinite(gain_db) or not -60.0 <= gain_db <= 24.0:
        raise PedalAudioError("pedal preview input gain is out of range")

    gain = math.pow(10.0, gain_db / 20.0)
    frames = len(payload) // BYTES_PER_SAMPLE
    output = bytearray(frames * channels * 4)
    output_offset = 0
    for offset in range(0, len(payload), BYTES_PER_SAMPLE):
        value = (
            payload[offset]
            | (payload[offset + 1] << 8)
            | (payload[offset + 2] << 16)
        )
        if value & 0x800000:
            value -= 0x1000000
        sample = max(-1.0, min(1.0, (value / 8388608.0) * gain))
        for _ in range(channels):
            struct.pack_into("<f", output, output_offset, sample)
            output_offset += 4
    return bytes(output)


class PedalCapture:
    """One bounded `arecord` stream from the pedal's capture endpoint."""

    def __init__(self, descriptor: dict[str, Any]) -> None:
        if not descriptor.get("available"):
            raise PedalAudioError(
                str(descriptor.get("reason") or "pedal audio is unavailable")
            )
        self.descriptor = descriptor
        self.process: subprocess.Popen[bytes] | None = None
        self.frames_read = 0

    @classmethod
    def open(cls) -> "PedalCapture":
        capture = cls(discover())
        capture.start()
        return capture

    def start(self) -> None:
        if self.process is not None:
            raise PedalAudioError("pedal capture is already running")
        command = [
            "arecord",
            "--quiet",
            "--device",
            str(self.descriptor["device"]),
            "--file-type",
            "raw",
            "--format",
            "S24_3LE",
            "--rate",
            str(SAMPLE_RATE),
            "--channels",
            "1",
            "--period-size",
            str(CAPTURE_PERIOD_FRAMES),
            "--buffer-size",
            str(CAPTURE_BUFFER_FRAMES),
            "-",
        ]
        try:
            self.process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
        except OSError as error:
            raise PedalAudioError(
                f"could not start direct pedal capture: {error}"
            ) from error

    def _read_exact(self, size: int, timeout: float) -> bytes:
        process = self.process
        if process is None or process.stdout is None:
            raise PedalAudioError("pedal capture is not running")
        deadline = time.monotonic() + timeout
        chunks: list[bytes] = []
        completed = 0
        while completed < size:
            if process.poll() is not None:
                detail = ""
                if process.stderr is not None:
                    detail = process.stderr.read().decode(errors="replace").strip()
                raise PedalAudioError(detail or "pedal capture process stopped")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise PedalAudioError("pedal USB capture timed out")
            ready, _, _ = select.select(
                [process.stdout.fileno()], [], [], remaining
            )
            if not ready:
                raise PedalAudioError("pedal USB capture timed out")
            chunk = os.read(process.stdout.fileno(), size - completed)
            if not chunk:
                detail = ""
                if process.stderr is not None:
                    detail = process.stderr.read().decode(errors="replace").strip()
                raise PedalAudioError(detail or "pedal USB capture ended")
            chunks.append(chunk)
            completed += len(chunk)
        return b"".join(chunks)

    def read_float32(
        self,
        frames: int,
        channels: int,
        gain_db: float,
        timeout: float = 2.0,
    ) -> bytes:
        if not 48 <= frames <= 4096:
            raise PedalAudioError("pedal capture chunk is out of range")
        try:
            packed = self._read_exact(frames * BYTES_PER_SAMPLE, timeout)
        except PedalAudioError:
            if self.frames_read:
                raise
            # PipeWire can release the hardware endpoint just after discovery.
            # Retry the first read once to make that handoff invisible in UI.
            self.stop()
            time.sleep(0.05)
            self.start()
            packed = self._read_exact(frames * BYTES_PER_SAMPLE, timeout)
        self.frames_read += frames
        return pcm24_to_float32(packed, channels, gain_db)

    def stop(self) -> None:
        process = self.process
        self.process = None
        if process is None:
            return
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=1.0)
        for pipe in (process.stdout, process.stderr):
            if pipe is not None:
                pipe.close()

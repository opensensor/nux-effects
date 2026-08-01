#!/usr/bin/env python3
"""Host utility for the open NCR-2 recovery protocol.

This utility targets the independent open bootloader protocol. It does not
send the proprietary NUX BINA format and must not be used against the stock
9527:c157 updater personality.
"""

from __future__ import annotations

import argparse
import dataclasses
import errno
import hashlib
import json
import os
import select
import struct
import sys
import time
import zlib
from pathlib import Path
from collections.abc import Callable
from typing import Protocol

import open_image
import ncr2_rom_recover


PACKET_MAGIC = 0x5846584E
PROTOCOL_VERSION = 2
PACKET_SIZE = 64
PAYLOAD_SIZE = 32
PACKET_STRUCT = struct.Struct("<IBBHIIIHHII32s")
INFO_STRUCT = struct.Struct("<IIIII4BII")
NOR_DIAGNOSTIC_STRUCT = struct.Struct("<IHBBIIIiHHI")
NOR_DIAGNOSTIC_MAGIC = 0x444F524E
KNOB_SAMPLE_STRUCT = struct.Struct("<I4HHH4B4BII")
KNOB_SAMPLE_MAGIC = 0x424F4E4B
KNOB_NAMES = ("amount", "character", "selector", "output")
KNOB_SELECTOR_INDEX = 2
EFFECT_COUNT = 8
CALIBRATION_WARNINGS = {
    "duplicate-detents": (
        "two detents measured identically; the selector cannot tell them "
        "apart. Check the knob and re-run."
    ),
    "non-monotonic": (
        "the ladder is not monotonic across positions. Nearest-detent "
        "matching still works, but verify the wiring."
    ),
    "gap-not-clear-of-noise": (
        "the smallest gap is not comfortably wider than the resting noise, "
        "so hysteresis margins must stay small."
    ),
}

SLOT_A = 0
SLOT_B = 1
SLOT_NONE = 0xFF
FLAG_FULL_FLASH = 0x8000
FULL_FLASH_UNLOCK = 0x45504957
CAPABILITY_FULL_FLASH_RAM = 0x20
CAPABILITY_PROGRESSIVE_FULL_ERASE = 0x40
CAPABILITY_VERIFIED_FULL_PROGRAM = 0x80
CAPABILITY_KNOB_SAMPLE = 0x100
FULL_FLASH_ERASE_CHUNK_SIZE = 0x10000
FULL_FLASH_CONFIRMATION = "WIPE-ALL-8MIB"
HANDOFF_CONFIRMATION = "ERASE-BOOT-REGION"
BOOT_REGION_SIZE = 0x10000
OPEN_RECOVERY_HID_ID = "HID_ID=0003:00009527:0000C157"
DEFAULT_HIDRAW_SYSFS = Path("/sys/class/hidraw")
DEFAULT_HIDRAW_DEVICES = Path("/dev")

COMMANDS = {
    "get-info": 1,
    "begin-image": 2,
    "erase-slot": 3,
    "write-chunk": 4,
    "read-chunk": 5,
    "finalize-image": 6,
    "set-pending": 7,
    "reboot": 8,
    "get-log": 9,
    "begin-full-flash": 10,
    "erase-full-flash": 11,
    "finalize-full-flash": 12,
    "read-knobs": 13,
}
COMMAND_NAMES = {value: name for name, value in COMMANDS.items()}

STATUSES = {
    0: "ok",
    1: "bad-magic",
    2: "bad-version",
    3: "bad-command",
    4: "bad-length",
    5: "bad-header-crc",
    6: "bad-payload-crc",
    7: "bad-session",
    8: "bad-sequence",
    9: "bad-slot",
    10: "range-denied",
    11: "backend-error",
    12: "invalid-state",
    13: "image-invalid",
    14: "active-slot",
    15: "not-finalized",
    16: "bad-flags",
    17: "write-order",
    18: "full-flash-disabled",
}


class RecoveryError(RuntimeError):
    """The host input, transport, or recovery response is invalid."""


class RecoveryTransportError(RecoveryError):
    """A report exchange failed at the operating-system transport layer."""

    def __init__(self, error: OSError):
        super().__init__(str(error))
        self.errno = error.errno


@dataclasses.dataclass(frozen=True)
class Packet:
    command: int
    flags: int = 0
    session: int = 0
    sequence: int = 0
    offset: int = 0
    status: int = 0
    payload: bytes = b""

    def encode(self) -> bytes:
        if self.command not in COMMAND_NAMES:
            raise RecoveryError(f"unsupported command {self.command}")
        if not 0 <= self.flags <= 0xFFFF:
            raise RecoveryError("flags exceed 16 bits")
        if not 0 <= self.session <= 0xFFFFFFFF:
            raise RecoveryError("session exceeds 32 bits")
        if not 0 <= self.sequence <= 0xFFFFFFFF:
            raise RecoveryError("sequence exceeds 32 bits")
        if not 0 <= self.offset <= 0xFFFFFFFF:
            raise RecoveryError("offset exceeds 32 bits")
        if not 0 <= self.status <= 0xFFFF:
            raise RecoveryError("status exceeds 16 bits")
        if len(self.payload) > PAYLOAD_SIZE:
            raise RecoveryError(
                f"payload is {len(self.payload)} bytes, maximum is "
                f"{PAYLOAD_SIZE}"
            )
        padded_payload = self.payload.ljust(PAYLOAD_SIZE, b"\0")
        payload_crc = zlib.crc32(self.payload) & 0xFFFFFFFF
        prefix = PACKET_STRUCT.pack(
            PACKET_MAGIC,
            PROTOCOL_VERSION,
            self.command,
            self.flags,
            self.session,
            self.sequence,
            self.offset,
            len(self.payload),
            self.status,
            payload_crc,
            0,
            padded_payload,
        )
        header_crc = zlib.crc32(prefix[:0x1C]) & 0xFFFFFFFF
        return prefix[:0x1C] + struct.pack("<I", header_crc) + prefix[0x20:]

    @classmethod
    def decode(cls, data: bytes) -> "Packet":
        if len(data) != PACKET_SIZE:
            raise RecoveryError(
                f"report is {len(data)} bytes, expected {PACKET_SIZE}"
            )
        (
            magic,
            version,
            command,
            flags,
            session,
            sequence,
            offset,
            length,
            status,
            payload_crc,
            header_crc,
            payload,
        ) = PACKET_STRUCT.unpack(data)
        if magic != PACKET_MAGIC:
            raise RecoveryError(f"bad packet magic {magic:#x}")
        if version != PROTOCOL_VERSION:
            raise RecoveryError(f"bad protocol version {version}")
        if command not in COMMAND_NAMES:
            raise RecoveryError(f"bad command {command}")
        if length > PAYLOAD_SIZE:
            raise RecoveryError(f"bad payload length {length}")
        expected_header = zlib.crc32(data[:0x1C]) & 0xFFFFFFFF
        if header_crc != expected_header:
            raise RecoveryError(
                f"header CRC {header_crc:#x} != {expected_header:#x}"
            )
        body = payload[:length]
        expected_payload = zlib.crc32(body) & 0xFFFFFFFF
        if payload_crc != expected_payload:
            raise RecoveryError(
                f"payload CRC {payload_crc:#x} != {expected_payload:#x}"
            )
        return cls(
            command=command,
            flags=flags,
            session=session,
            sequence=sequence,
            offset=offset,
            status=status,
            payload=body,
        )


@dataclasses.dataclass(frozen=True)
class RecoveryInfo:
    flash_size: int
    slot_size: int
    slot_a_offset: int
    slot_b_offset: int
    manifest_size: int
    confirmed_slot: int
    pending_slot: int
    selected_slot: int
    update_phase: int
    capabilities: int
    max_chunk_size: int

    @classmethod
    def decode(cls, payload: bytes) -> "RecoveryInfo":
        if len(payload) != INFO_STRUCT.size:
            raise RecoveryError(
                f"GET_INFO returned {len(payload)} bytes, expected "
                f"{INFO_STRUCT.size}"
            )
        return cls(*INFO_STRUCT.unpack(payload))


@dataclasses.dataclass(frozen=True)
class NorDiagnostics:
    magic: int
    version: int
    operation: int
    phase: int
    address: int
    length: int
    completed_units: int
    backend_status: int
    status: int
    reserved: int
    detail: int

    @classmethod
    def decode(cls, payload: bytes) -> "NorDiagnostics":
        if len(payload) != NOR_DIAGNOSTIC_STRUCT.size:
            raise RecoveryError(
                f"GET_LOG returned {len(payload)} bytes, expected "
                f"{NOR_DIAGNOSTIC_STRUCT.size}"
            )
        diagnostics = cls(*NOR_DIAGNOSTIC_STRUCT.unpack(payload))
        if diagnostics.magic != NOR_DIAGNOSTIC_MAGIC:
            raise RecoveryError(
                f"bad NOR diagnostic magic {diagnostics.magic:#x}"
            )
        return diagnostics


@dataclasses.dataclass(frozen=True)
class KnobSample:
    values: tuple[int, ...]
    selector_min: int
    selector_max: int
    channels: tuple[int, ...]
    burst: int
    valid: int
    adc_bits: int
    sample_index: int

    @property
    def selector(self) -> int:
        return self.values[KNOB_SELECTOR_INDEX]

    @property
    def selector_spread(self) -> int:
        return self.selector_max - self.selector_min

    @classmethod
    def decode(cls, payload: bytes) -> "KnobSample":
        if len(payload) != KNOB_SAMPLE_STRUCT.size:
            raise RecoveryError(
                f"READ_KNOBS returned {len(payload)} bytes, expected "
                f"{KNOB_SAMPLE_STRUCT.size}"
            )
        fields = KNOB_SAMPLE_STRUCT.unpack(payload)
        if fields[0] != KNOB_SAMPLE_MAGIC:
            raise RecoveryError(
                f"bad knob sample magic {fields[0]:#x}"
            )
        return cls(
            values=fields[1:5],
            selector_min=fields[5],
            selector_max=fields[6],
            channels=fields[7:11],
            burst=fields[11],
            valid=fields[12],
            adc_bits=fields[13],
            sample_index=fields[15],
        )

    def as_dict(self) -> dict[str, object]:
        return {
            "sample_index": self.sample_index,
            "valid": bool(self.valid),
            "adc_bits": self.adc_bits,
            "burst": self.burst,
            "selector_min": self.selector_min,
            "selector_max": self.selector_max,
            "selector_spread": self.selector_spread,
            "knobs": {
                name: {"channel": channel, "value": value}
                for name, channel, value in zip(
                    KNOB_NAMES, self.channels, self.values, strict=True
                )
            },
        }


class ReportTransport(Protocol):
    def exchange(self, report: bytes) -> bytes:
        """Send one 64-byte output report and receive one input report."""


class HidrawTransport:
    def __init__(
        self,
        device: Path,
        timeout: float = 2.0,
        allow_borrowed_nux_id: bool = False,
    ):
        self.device = device
        self.timeout = timeout
        self.allow_borrowed_nux_id = allow_borrowed_nux_id
        self.descriptor: int | None = None

    def __enter__(self) -> "HidrawTransport":
        sysfs_uevent = (
            Path("/sys/class/hidraw")
            / self.device.name
            / "device"
            / "uevent"
        )
        try:
            identity = sysfs_uevent.read_text().upper()
        except OSError:
            identity = ""
        if (
            "HID_ID=0003:00009527:0000C157" in identity
            and not self.allow_borrowed_nux_id
        ):
            raise RecoveryError(
                "refusing the stock NUX 9527:c157 updater; pedalctl speaks "
                "only the independent open recovery protocol; use "
                "--allow-borrowed-nux-id only with a known open build"
            )
        self.descriptor = os.open(
            self.device, os.O_RDWR | os.O_CLOEXEC | os.O_NONBLOCK
        )
        return self

    def __exit__(self, *_args: object) -> None:
        if self.descriptor is not None:
            os.close(self.descriptor)
            self.descriptor = None

    def exchange(self, report: bytes) -> bytes:
        if self.descriptor is None:
            raise RecoveryError("hidraw transport is not open")
        if len(report) != PACKET_SIZE:
            raise RecoveryError("refusing to send a non-64-byte report")

        deadline = time.monotonic() + self.timeout
        while True:
            try:
                written = os.write(self.descriptor, report)
                if written != PACKET_SIZE:
                    raise RecoveryError(
                        f"short HID write: {written} bytes"
                    )
                break
            except BlockingIOError:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RecoveryError("timed out writing HID report")
                select.select([], [self.descriptor], [], remaining)

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RecoveryError("timed out waiting for HID response")
            readable, _, _ = select.select(
                [self.descriptor], [], [], remaining
            )
            if not readable:
                continue
            try:
                response = os.read(self.descriptor, PACKET_SIZE)
            except BlockingIOError:
                continue
            if len(response) != PACKET_SIZE:
                raise RecoveryError(
                    f"short HID response: {len(response)} bytes"
                )
            return response


class RecoveryClient:
    def __init__(self, transport: ReportTransport, retries: int = 2):
        if retries < 0:
            raise RecoveryError("retry count cannot be negative")
        self.transport = transport
        self.retries = retries
        self.session = 0
        self.sequence = 0
        self.slot: int | None = None
        self.session_flags = 0

    def _exchange(self, request: Packet, *, begin: bool = False) -> Packet:
        encoded = request.encode()
        last_error: RecoveryError | None = None
        for _attempt in range(self.retries + 1):
            try:
                response = Packet.decode(self.transport.exchange(encoded))
            except OSError as error:
                last_error = RecoveryTransportError(error)
                continue
            except RecoveryError as error:
                last_error = error
                continue
            if response.command != request.command:
                raise RecoveryError(
                    f"response command {response.command} does not match "
                    f"{request.command}"
                )
            if response.flags != request.flags:
                raise RecoveryError("response slot flags do not match request")
            if response.sequence != request.sequence:
                raise RecoveryError("response sequence does not match request")
            if not begin and response.session != request.session:
                raise RecoveryError("response session does not match request")
            if response.status != 0:
                name = STATUSES.get(response.status, "unknown")
                raise RecoveryError(
                    f"{COMMAND_NAMES[request.command]} failed: "
                    f"{name} ({response.status})"
                )
            return response
        assert last_error is not None
        raise last_error

    def get_info(self) -> RecoveryInfo:
        response = self._exchange(Packet(COMMANDS["get-info"]))
        return RecoveryInfo.decode(response.payload)

    def get_log(self) -> NorDiagnostics:
        response = self._exchange(Packet(COMMANDS["get-log"]))
        return NorDiagnostics.decode(response.payload)

    def read_knobs(self) -> KnobSample:
        response = self._exchange(Packet(COMMANDS["read-knobs"]))
        return KnobSample.decode(response.payload)

    def begin(self, slot: int, image_size: int) -> int:
        if slot not in (SLOT_A, SLOT_B):
            raise RecoveryError(f"invalid target slot {slot}")
        layout = open_image.load_layout()
        minimum_size = layout.manifest_size + 8
        slot_size = layout.region("application_a").size
        if not minimum_size <= image_size <= slot_size:
            raise RecoveryError(
                f"image size {image_size:#x} is outside "
                f"{minimum_size:#x}..{slot_size:#x}"
            )
        response = self._exchange(
            Packet(
                COMMANDS["begin-image"],
                flags=slot,
                offset=image_size,
            ),
            begin=True,
        )
        if response.session == 0:
            raise RecoveryError("bootloader returned a zero session")
        self.slot = slot
        self.session_flags = slot
        self.session = response.session
        self.sequence = 1
        return self.session

    def begin_full_flash(
        self, image_size: int, image_sha256: bytes
    ) -> int:
        if image_size != ncr2_rom_recover.FLASH_SIZE:
            raise RecoveryError(
                f"full image must be exactly "
                f"{ncr2_rom_recover.FLASH_SIZE} bytes"
            )
        if len(image_sha256) != hashlib.sha256().digest_size:
            raise RecoveryError("full image SHA-256 must be 32 bytes")
        response = self._exchange(
            Packet(
                COMMANDS["begin-full-flash"],
                flags=FLAG_FULL_FLASH,
                session=FULL_FLASH_UNLOCK,
                offset=image_size,
                payload=image_sha256,
            ),
            begin=True,
        )
        if response.session == 0:
            raise RecoveryError("RAM recovery returned a zero session")
        self.slot = None
        self.session_flags = FLAG_FULL_FLASH
        self.session = response.session
        self.sequence = 1
        return self.session

    def _session_command(
        self,
        command: str,
        *,
        offset: int = 0,
        payload: bytes = b"",
    ) -> Packet:
        if self.session == 0:
            raise RecoveryError("no update transaction has begun")
        request = Packet(
            COMMANDS[command],
            flags=self.session_flags,
            session=self.session,
            sequence=self.sequence,
            offset=offset,
            payload=payload,
        )
        response = self._exchange(request)
        self.sequence += 1
        return response

    def erase(self) -> None:
        self._session_command("erase-slot")

    def erase_full_flash(
        self,
        progress: Callable[[int, int], None] | None = None,
        *,
        limit: int | None = None,
    ) -> None:
        """Erase the whole NOR, or only its first `limit` bytes.

        A bounded erase is how the boot-region handoff hands control to the
        immutable NXP ROM without touching the rest of the chip.
        """
        total = ncr2_rom_recover.FLASH_SIZE
        if limit is not None:
            if not 0 < limit <= total:
                raise RecoveryError(
                    f"erase limit {limit:#x} is outside 0..{total:#x}"
                )
            if limit % FULL_FLASH_ERASE_CHUNK_SIZE != 0:
                raise RecoveryError(
                    "erase limit must be a multiple of "
                    f"{FULL_FLASH_ERASE_CHUNK_SIZE:#x}"
                )
            total = limit
        for offset in range(0, total, FULL_FLASH_ERASE_CHUNK_SIZE):
            response = self._session_command(
                "erase-full-flash",
                offset=offset,
            )
            expected = min(
                offset + FULL_FLASH_ERASE_CHUNK_SIZE,
                total,
            )
            if response.offset != expected:
                raise RecoveryError(
                    "RAM recovery reported erase progress "
                    f"{response.offset:#x}, expected {expected:#x}"
                )
            if progress is not None:
                progress(expected, total)

    def write(
        self,
        image: bytes,
        progress: Callable[[int, int], None] | None = None,
        *,
        chunk_size: int = PAYLOAD_SIZE,
        start_offset: int = 0,
    ) -> None:
        if not 1 <= chunk_size <= PAYLOAD_SIZE:
            raise RecoveryError(
                f"write chunk size must be 1..{PAYLOAD_SIZE} bytes"
            )
        if not 0 <= start_offset <= len(image):
            raise RecoveryError("write start offset is outside the image")
        for offset in range(start_offset, len(image), chunk_size):
            chunk = image[offset : offset + chunk_size]
            self._session_command(
                "write-chunk",
                offset=offset,
                payload=chunk,
            )
            if progress is not None:
                progress(offset + len(chunk), len(image))

    def read(self, offset: int, length: int) -> bytes:
        if not 1 <= length <= PAYLOAD_SIZE:
            raise RecoveryError("read length must be 1..32 bytes")
        return self._session_command(
            "read-chunk",
            offset=offset,
            payload=bytes(length),
        ).payload

    def finalize(self) -> None:
        self._session_command("finalize-image")

    def finalize_full_flash(self) -> None:
        self._session_command("finalize-full-flash")

    def set_pending(self) -> None:
        self._session_command("set-pending")

    def reboot(self) -> bool:
        try:
            self._session_command("reboot")
        except RecoveryTransportError as error:
            #
            # The RT1051 resets synchronously from the REBOOT handler. On
            # Linux the hidraw node can disappear after accepting the OUT
            # report but before returning the IN acknowledgement. SET_PENDING
            # or FINALIZE_FULL_FLASH has already succeeded at this point, so
            # that specific USB disconnect is the expected reboot outcome.
            #
            if error.errno in {
                errno.ENODEV,
                errno.EIO,
                errno.ENXIO,
                errno.ESHUTDOWN,
            }:
                return False
            raise
        return True


def validate_slot_image(path: Path) -> tuple[bytes, open_image.Manifest]:
    image = path.read_bytes()
    layout = open_image.load_layout()
    if len(image) < layout.manifest_size:
        raise RecoveryError("slot image is shorter than one manifest sector")
    manifest = open_image.parse_manifest(
        image[: layout.manifest_size], layout=layout
    )
    expected_size = layout.manifest_size + manifest.image_size
    if len(image) != expected_size:
        raise RecoveryError(
            f"slot image is {len(image):#x} bytes, manifest requires "
            f"{expected_size:#x}"
        )
    payload = image[layout.manifest_size :]
    open_image.validate_vector(payload, manifest)
    digest = hashlib.sha256(payload).digest()
    if digest != manifest.image_sha256:
        raise RecoveryError(
            f"payload SHA-256 {digest.hex()} != "
            f"{manifest.image_sha256.hex()}"
        )
    return image, manifest


def slot_name(slot: int) -> str:
    return {SLOT_A: "A", SLOT_B: "B", SLOT_NONE: "none"}.get(
        slot, f"invalid-{slot}"
    )


def info_to_dict(info: RecoveryInfo) -> dict[str, object]:
    return {
        **dataclasses.asdict(info),
        "confirmed_slot_name": slot_name(info.confirmed_slot),
        "pending_slot_name": slot_name(info.pending_slot),
        "selected_slot_name": slot_name(info.selected_slot),
    }


def discover_recovery_device(
    sysfs_root: Path = DEFAULT_HIDRAW_SYSFS,
    device_root: Path = DEFAULT_HIDRAW_DEVICES,
) -> Path:
    candidates: list[Path] = []

    try:
        hidraw_entries = sorted(sysfs_root.glob("hidraw*"))
    except OSError as error:
        raise RecoveryError(
            f"cannot scan recovery devices in {sysfs_root}: {error}"
        ) from error
    for entry in hidraw_entries:
        try:
            identity = (entry / "device" / "uevent").read_text().upper()
        except OSError:
            continue
        if OPEN_RECOVERY_HID_ID in identity:
            candidates.append(device_root / entry.name)

    if not candidates:
        raise RecoveryError(
            "no 9527:c157 recovery device found; enter Open Recover "
            "or pass --device explicitly"
        )
    if len(candidates) != 1:
        raise RecoveryError(
            "multiple 9527:c157 recovery devices found: " +
            ", ".join(str(candidate) for candidate in candidates)
        )
    return candidates[0]


def recovery_transport(args: argparse.Namespace) -> HidrawTransport:
    auto_discovered = args.device is None
    device = (
        discover_recovery_device()
        if auto_discovered
        else args.device
    )
    if auto_discovered:
        print(
            f"Open Recover: {device}",
            file=sys.stderr,
            flush=True,
        )

    # Auto-discovery is restricted to the borrowed 9527:c157 HID identity.
    # The first command is still versioned GET_INFO; a stock updater cannot
    # pass packet decoding and therefore cannot reach a mutating command.
    return HidrawTransport(
        device,
        args.timeout,
        args.allow_borrowed_nux_id or auto_discovered,
    )


def command_inspect_slot(args: argparse.Namespace) -> None:
    image, manifest = validate_slot_image(args.image)
    print(
        json.dumps(
            {
                "path": str(args.image),
                "size": len(image),
                "sha256": hashlib.sha256(image).hexdigest(),
                "payload_size": manifest.image_size,
                "payload_sha256": manifest.image_sha256.hex(),
                "version": open_image.format_semantic_version(
                    manifest.semantic_version
                ),
                "build_number": manifest.build_number,
            },
            indent=2,
            sort_keys=True,
        )
    )


def command_info(args: argparse.Namespace) -> None:
    with recovery_transport(args) as transport:
        info = RecoveryClient(transport, args.retries).get_info()
    print(json.dumps(info_to_dict(info), indent=2, sort_keys=True))


def command_diagnostics(args: argparse.Namespace) -> None:
    with recovery_transport(args) as transport:
        diagnostics = RecoveryClient(
            transport, args.retries
        ).get_log()
    print(
        json.dumps(
            dataclasses.asdict(diagnostics),
            indent=2,
            sort_keys=True,
        )
    )


def require_knob_capability(client: RecoveryClient) -> None:
    info = client.get_info()
    if not info.capabilities & CAPABILITY_KNOB_SAMPLE:
        raise RecoveryError(
            "this recovery firmware does not advertise knob sampling "
            f"(capabilities {info.capabilities:#x}); flash a build that "
            "includes READ_KNOBS first"
        )


def format_knob_line(sample: KnobSample) -> str:
    columns = " ".join(
        f"{name}={value:>4}"
        for name, value in zip(KNOB_NAMES, sample.values, strict=True)
    )
    return (
        f"#{sample.sample_index:<6} {columns}  "
        f"selector_spread={sample.selector_spread:>3}"
        f"{'' if sample.valid else '  [INVALID]'}"
    )


def command_knobs(args: argparse.Namespace) -> None:
    with recovery_transport(args) as transport:
        client = RecoveryClient(transport, args.retries)
        require_knob_capability(client)
        if not args.watch:
            sample = client.read_knobs()
            print(json.dumps(sample.as_dict(), indent=2, sort_keys=True))
            return

        print(
            "Live front-panel capture. Rotate the Type knob through every "
            "detent.\nPress Ctrl-C to stop.",
            file=sys.stderr,
        )
        previous_index = None
        try:
            while True:
                sample = client.read_knobs()
                if sample.sample_index != previous_index:
                    previous_index = sample.sample_index
                    print(format_knob_line(sample), flush=True)
                time.sleep(args.interval)
        except KeyboardInterrupt:
            print(file=sys.stderr)


def capture_detent(
    client: RecoveryClient, repeats: int
) -> tuple[int, int, int]:
    """Return (median value, minimum seen, maximum seen) for one detent."""
    readings: list[int] = []
    low = 0xFFFF
    high = 0
    for _ in range(max(1, repeats)):
        sample = client.read_knobs()
        if not sample.valid:
            raise RecoveryError(
                "the recovery firmware reported an invalid ADC capture; "
                "the converter did not calibrate"
            )
        readings.append(sample.selector)
        low = min(low, sample.selector_min)
        high = max(high, sample.selector_max)
    readings.sort()
    return readings[len(readings) // 2], low, high


def selector_calibration_report(
    detents: list[int], spreads: list[int]
) -> dict[str, object]:
    """Summarise a detent sweep and name the ways it can be unusable.

    Two detents that measure the same value cannot be told apart at all. A
    smallest gap that is not clearly wider than the resting noise leaves no
    room for hysteresis, which is how a stepped knob ends up refusing to
    change program.
    """
    ordered = sorted(detents)
    gaps = [
        second - first
        for first, second in zip(ordered, ordered[1:], strict=False)
    ]
    minimum_gap = min(gaps) if gaps else 0
    worst_spread = max(spreads) if spreads else 0
    warnings: list[str] = []
    if len(set(detents)) != len(detents):
        warnings.append("duplicate-detents")
    elif detents not in (ordered, ordered[::-1]):
        warnings.append("non-monotonic")
    if minimum_gap <= 2 * worst_spread:
        warnings.append("gap-not-clear-of-noise")
    return {
        "detents": detents,
        "sorted": ordered,
        "gaps": gaps,
        "minimum_gap": minimum_gap,
        "maximum_rest_spread": worst_spread,
        "warnings": warnings,
    }


def render_selector_table(detents: list[int]) -> str:
    entries = ",\n".join(f"    UINT16_C({value})" for value in detents)
    return (
        "/* Measured on hardware by `pedalctl calibrate-selector`. */\n"
        "static const uint16_t g_selector_detent"
        f"[NCR2_EFFECT_COUNT] = {{\n{entries},\n}};"
    )


def command_calibrate_selector(args: argparse.Namespace) -> None:
    with recovery_transport(args) as transport:
        client = RecoveryClient(transport, args.retries)
        require_knob_capability(client)

        print(
            f"Measuring all {EFFECT_COUNT} Type detents.\n"
            "Turn the knob fully counter-clockwise to start, then step one "
            "detent clockwise each time.\n",
            file=sys.stderr,
        )
        detents: list[int] = []
        spreads: list[int] = []
        for position in range(EFFECT_COUNT):
            input(
                f"Position {position + 1}/{EFFECT_COUNT}: set the detent, "
                "then press Enter... "
            )
            value, low, high = capture_detent(client, args.repeats)
            spread = high - low
            spreads.append(spread)
            detents.append(value)
            print(
                f"  captured {value}  (burst range {low}..{high}, "
                f"spread {spread})",
                file=sys.stderr,
            )

    report = selector_calibration_report(detents, spreads)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return

    print()
    print(f"detent values   : {report['detents']}")
    print(f"adjacent gaps   : {report['gaps']}")
    print(f"smallest gap    : {report['minimum_gap']} counts")
    print(f"worst rest noise: {report['maximum_rest_spread']} counts")
    for warning in report["warnings"]:
        print(f"\nWARNING: {CALIBRATION_WARNINGS[warning]}")
    print()
    print(render_selector_table(detents))


def parse_slot(value: str) -> int | None:
    lowered = value.lower()
    if lowered == "auto":
        return None
    if lowered == "a":
        return SLOT_A
    if lowered == "b":
        return SLOT_B
    raise argparse.ArgumentTypeError("slot must be auto, A, or B")


def command_upload(args: argparse.Namespace) -> None:
    image, manifest = validate_slot_image(args.image)
    reboot_acknowledged: bool | None = None
    with recovery_transport(args) as transport:
        client = RecoveryClient(transport, args.retries)
        info = client.get_info()
        target = args.slot
        if target is None:
            if info.confirmed_slot not in (SLOT_A, SLOT_B):
                raise RecoveryError(
                    f"device reported invalid confirmed slot "
                    f"{info.confirmed_slot}"
                )
            target = SLOT_B if info.confirmed_slot == SLOT_A else SLOT_A
        if target == info.confirmed_slot or target == info.selected_slot:
            raise RecoveryError(
                f"refusing to overwrite confirmed/selected slot "
                f"{slot_name(target)}"
            )

        session = client.begin(target, len(image))
        print(
            f"session {session:#010x}: erasing inactive slot "
            f"{slot_name(target)}",
            file=sys.stderr,
        )
        client.erase()
        print(
            f"writing {len(image)} bytes in "
            f"{(len(image) + PAYLOAD_SIZE - 1) // PAYLOAD_SIZE} chunks",
            file=sys.stderr,
        )
        client.write(image)
        client.finalize()
        client.set_pending()
        if not args.no_reboot:
            reboot_acknowledged = client.reboot()

    print(
        json.dumps(
            {
                "result": "pending",
                "target_slot": slot_name(target),
                "slot_image_sha256": hashlib.sha256(image).hexdigest(),
                "payload_sha256": manifest.image_sha256.hex(),
                "reboot_requested": not args.no_reboot,
                "reboot_acknowledged": reboot_acknowledged,
            },
            indent=2,
            sort_keys=True,
        )
    )


def validate_full_restore_target(
    info: RecoveryInfo,
    image_size: int,
) -> None:
    if (
        info.capabilities & CAPABILITY_FULL_FLASH_RAM
    ) == 0:
        raise RecoveryError(
            "device is not the RAM-resident full-flash recovery "
            "personality; refusing destructive command"
        )
    if (
        info.capabilities &
        CAPABILITY_PROGRESSIVE_FULL_ERASE
    ) == 0:
        raise RecoveryError(
            "device lacks progressive full-flash erase; refusing the "
            "silent monolithic erase implementation"
        )
    if (
        info.capabilities &
        CAPABILITY_VERIFIED_FULL_PROGRAM
    ) == 0:
        raise RecoveryError(
            "device NOR programming is not physically verified for "
            "full restore; refusing before erase (use the NXP ROM "
            "flashloader recovery path)"
        )
    if info.flash_size != image_size:
        raise RecoveryError(
            f"device reports {info.flash_size} flash bytes, image has "
            f"{image_size}"
        )


def command_restore_full(args: argparse.Namespace) -> None:
    if args.confirm != FULL_FLASH_CONFIRMATION:
        raise RecoveryError(
            "full-chip restore requires "
            f"--confirm {FULL_FLASH_CONFIRMATION}"
        )
    image_sha256 = ncr2_rom_recover.validate_flash_image(
        args.image, args.expected_sha256
    )
    image = args.image.read_bytes()
    digest = bytes.fromhex(image_sha256)
    reboot_acknowledged: bool | None = None

    with recovery_transport(args) as transport:
        client = RecoveryClient(transport, args.retries)
        info = client.get_info()
        validate_full_restore_target(info, len(image))

        session = client.begin_full_flash(len(image), digest)
        print(
            f"session {session:#010x}: erasing the complete 8 MiB NOR; "
            "do not remove power",
            file=sys.stderr,
            flush=True,
        )
        erase_report_step = 512 * 1024

        def report_erase(completed: int, total: int) -> None:
            if (
                completed == total
                or completed % erase_report_step == 0
            ):
                print(
                    f"erased {completed // 1024} / "
                    f"{total // 1024} KiB",
                    file=sys.stderr,
                    flush=True,
                )

        client.erase_full_flash(report_erase)
        print(
            f"writing {len(image)} bytes in "
            f"{(len(image) + PAYLOAD_SIZE - 1) // PAYLOAD_SIZE} chunks",
            file=sys.stderr,
            flush=True,
        )
        write_report_step = 512 * 1024

        def report_write(completed: int, total: int) -> None:
            if (
                completed == total
                or completed % write_report_step == 0
            ):
                print(
                    f"wrote {completed // 1024} / "
                    f"{total // 1024} KiB",
                    file=sys.stderr,
                    flush=True,
                )

        client.write(image, report_write)
        print(
            "hashing the complete NOR in RAM recovery",
            file=sys.stderr,
            flush=True,
        )
        client.finalize_full_flash()
        if not args.no_reboot:
            reboot_acknowledged = client.reboot()

    print(
        json.dumps(
            {
                "result": "full-flash-restored",
                "image": str(args.image),
                "image_sha256": image_sha256,
                "reboot_requested": not args.no_reboot,
                "reboot_acknowledged": reboot_acknowledged,
            },
            indent=2,
            sort_keys=True,
        )
    )


def command_handoff_to_rom(args: argparse.Namespace) -> None:
    """Erase only the boot region so the next cold start enters NXP ROM.

    The open recovery NOR programmer cannot yet write reliably, so a new
    bootloader has to be installed through the immutable ROM flashloader.
    The ROM falls back to its serial downloader when the boot header at the
    start of flash is blank, and erasing that one 64 KiB region is the whole
    mechanism. Everything above it, including the factory compatibility
    region, is left untouched.
    """
    if args.confirm != HANDOFF_CONFIRMATION:
        raise RecoveryError(
            f"boot-region erase requires --confirm {HANDOFF_CONFIRMATION}"
        )
    # Validate the follow-up image before making the pedal unbootable, so a
    # bad path or hash cannot strand the device with nothing to install.
    image_sha256 = ncr2_rom_recover.validate_flash_image(
        args.image, args.expected_sha256
    )
    digest = bytes.fromhex(image_sha256)
    image_size = args.image.stat().st_size

    with recovery_transport(args) as transport:
        client = RecoveryClient(transport, args.retries)
        info = client.get_info()
        if not info.capabilities & CAPABILITY_FULL_FLASH_RAM:
            raise RecoveryError(
                "device is not the RAM-resident recovery personality; "
                "refusing destructive command"
            )
        if not info.capabilities & CAPABILITY_PROGRESSIVE_FULL_ERASE:
            raise RecoveryError(
                "device lacks progressive erase; refusing the silent "
                "monolithic erase implementation"
            )
        if info.flash_size != image_size:
            raise RecoveryError(
                f"device reports {info.flash_size} flash bytes, the "
                f"follow-up image has {image_size}"
            )

        client.begin_full_flash(image_size, digest)
        print(
            f"erasing the first {BOOT_REGION_SIZE // 1024} KiB only; "
            "do not remove power",
            file=sys.stderr,
            flush=True,
        )
        client.erase_full_flash(limit=BOOT_REGION_SIZE)

    print(
        json.dumps(
            {
                "result": "boot-region-erased",
                "erased_bytes": BOOT_REGION_SIZE,
                "next_image": str(args.image),
                "next_image_sha256": image_sha256,
                "next_step": (
                    "power-cycle with no footswitch held; the pedal should "
                    "enumerate as NXP ROM 1fc9:0130, then run "
                    "tools/ncr2_rom_recover.py flash --execute"
                ),
            },
            indent=2,
            sort_keys=True,
        )
    )


def resolved_path(value: str) -> Path:
    return Path(value).expanduser().resolve()


def add_transport_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--device",
        type=resolved_path,
        help=(
            "hidraw recovery node; omit to auto-discover the single "
            "9527:c157 Open Recover device"
        ),
    )
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--retries", type=int, default=2)
    parser.add_argument(
        "--allow-borrowed-nux-id",
        action="store_true",
        help=(
            "allow 9527:c157 only when the device is known to run the "
            "open recovery protocol"
        ),
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser(
        "inspect-slot",
        help="validate an open-recovery slot image without USB access",
    )
    inspect_parser.add_argument("image", type=resolved_path)
    inspect_parser.set_defaults(function=command_inspect_slot)

    info_parser = subparsers.add_parser(
        "info", help="read open bootloader recovery information"
    )
    add_transport_arguments(info_parser)
    info_parser.set_defaults(function=command_info)

    diagnostics_parser = subparsers.add_parser(
        "diagnostics",
        help="read the last open-bootloader NOR operation",
    )
    add_transport_arguments(diagnostics_parser)
    diagnostics_parser.set_defaults(function=command_diagnostics)

    knobs_parser = subparsers.add_parser(
        "knobs",
        help="read the four front-panel controls (non-mutating)",
    )
    add_transport_arguments(knobs_parser)
    knobs_parser.add_argument(
        "--watch",
        action="store_true",
        help="poll continuously and print each new capture",
    )
    knobs_parser.add_argument(
        "--interval",
        type=float,
        default=0.1,
        help="seconds between captures while watching (default 0.1)",
    )
    knobs_parser.set_defaults(function=command_knobs)

    calibrate_parser = subparsers.add_parser(
        "calibrate-selector",
        help="measure every Type detent and emit a firmware table",
    )
    add_transport_arguments(calibrate_parser)
    calibrate_parser.add_argument(
        "--repeats",
        type=int,
        default=5,
        help="captures averaged per detent (default 5)",
    )
    calibrate_parser.add_argument(
        "--json",
        action="store_true",
        help="emit the measurement report as JSON",
    )
    calibrate_parser.set_defaults(function=command_calibrate_selector)

    upload_parser = subparsers.add_parser(
        "upload",
        help="validate and upload an image to the inactive open A/B slot",
    )
    add_transport_arguments(upload_parser)
    upload_parser.add_argument("image", type=resolved_path)
    upload_parser.add_argument("--slot", type=parse_slot, default=None)
    upload_parser.add_argument("--no-reboot", action="store_true")
    upload_parser.set_defaults(function=command_upload)

    handoff_parser = subparsers.add_parser(
        "handoff-to-rom",
        help="erase only the 64 KiB boot region so the next cold start "
        "enters the NXP ROM downloader",
    )
    add_transport_arguments(handoff_parser)
    handoff_parser.add_argument(
        "image",
        type=resolved_path,
        help="the full image to be installed afterwards; validated before "
        "the erase so a bad path cannot strand the pedal",
    )
    handoff_parser.add_argument("--expected-sha256")
    handoff_parser.add_argument(
        "--confirm",
        required=True,
        help=f"must be exactly {HANDOFF_CONFIRMATION}",
    )
    handoff_parser.set_defaults(function=command_handoff_to_rom)

    restore_parser = subparsers.add_parser(
        "restore-full",
        help=(
            "erase and restore all 8 MiB through the RAM-resident recovery "
            "personality"
        ),
    )
    add_transport_arguments(restore_parser)
    restore_parser.set_defaults(timeout=600.0)
    restore_parser.add_argument("image", type=resolved_path)
    restore_parser.add_argument("--expected-sha256")
    restore_parser.add_argument(
        "--confirm",
        required=True,
        help=f"must be exactly {FULL_FLASH_CONFIRMATION}",
    )
    restore_parser.add_argument("--no-reboot", action="store_true")
    restore_parser.set_defaults(function=command_restore_full)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        args.function(args)
    except (
        OSError,
        RecoveryError,
        open_image.ImageError,
        json.JSONDecodeError,
    ) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())

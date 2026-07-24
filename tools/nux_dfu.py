#!/usr/bin/env python3
"""Inspect, build, query, and stream NUX Core Deluxe HID-DFU records.

The protocol was recovered from NUX Device Updater 2.1.11.30 and the
RTX_DFU image in the NCR-2 flash dump.

USB transport:
    VID:PID 9527:c157
    endpoint 0x02 OUT, endpoint 0x81 IN
    64-byte reports, no report ID on the wire

Each 540-byte BINA record is sent as nine reports:
    01 <sequence 0..8> 00 00 <60 record bytes>

The stock bootloader writes 512-byte pages contiguously beginning at flash
offset 0x60000. That base address is hardcoded in the bootloader.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import struct
import sys
import time
from pathlib import Path
from typing import Iterable, Iterator


VID = 0x9527
PID = 0xC157
EP_OUT = 0x02
EP_IN = 0x81
USB_REPORT_SIZE = 64
RECORD_SIZE = 540
RECORD_HEADER_SIZE = 28
PAGE_SIZE = 512
REPORT_DATA_SIZE = 60
REPORTS_PER_RECORD = 9
FLASH_BASE = 0x60000
ENGINE_SLOT_SIZE = 0x20000
SETUP_OPERATION = 1
PAGE_OPERATION = 4
PRODUCT_ID = b"COREDLX"


class DfuFormatError(ValueError):
    """The updater container or BINA stream is malformed."""


@dataclasses.dataclass(frozen=True)
class ContainerSection:
    tag: bytes
    size: int
    offset: int


@dataclasses.dataclass(frozen=True)
class BinaInfo:
    product: bytes
    version: bytes
    declared_length: int
    image: bytes
    record_count: int

    @property
    def page_count(self) -> int:
        return len(self.image) // PAGE_SIZE

    @property
    def flash_end(self) -> int:
        return FLASH_BASE + len(self.image)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_number(value: str) -> int:
    return int(value, 0)


def invert_bytes(data: bytes) -> bytes:
    return bytes(byte ^ 0xFF for byte in data)


def operation(record: bytes) -> int:
    return int.from_bytes(record[2:4], "big")


def page_marker(record: bytes) -> int:
    return int.from_bytes(record[12:16], "big")


def find_container_sections(data: bytes) -> list[ContainerSection]:
    if not data.startswith(b"NUX DFU"):
        return []

    sections: list[ContainerSection] = []
    # Current NUX containers keep a three-entry directory at 0x224. Scan the
    # header instead of assuming its exact entry count.
    for offset in range(0, min(len(data) - 12, 0x400), 4):
        tag = data[offset : offset + 4]
        if tag not in (b"TEXT", b"BINA", b"EXTR"):
            continue
        size, section_offset = struct.unpack_from("<II", data, offset + 4)
        if section_offset > len(data) or size > len(data) - section_offset:
            raise DfuFormatError(
                f"{tag.decode()} section points outside the container"
            )
        sections.append(ContainerSection(tag, size, section_offset))

    unique = {section.tag: section for section in sections}
    if b"BINA" not in unique:
        raise DfuFormatError("NUX DFU container has no BINA section")
    return [unique[tag] for tag in (b"TEXT", b"BINA", b"EXTR") if tag in unique]


def extract_bina(data: bytes) -> tuple[bytes, list[ContainerSection]]:
    sections = find_container_sections(data)
    if not sections:
        if len(data) % RECORD_SIZE:
            raise DfuFormatError(
                f"raw BINA length {len(data):#x} is not divisible by "
                f"{RECORD_SIZE}"
            )
        return data, []

    bina = next(section for section in sections if section.tag == b"BINA")
    return data[bina.offset : bina.offset + bina.size], sections


def records_from_bina(bina: bytes) -> list[bytes]:
    if not bina or len(bina) % RECORD_SIZE:
        raise DfuFormatError(
            f"BINA length {len(bina):#x} is not a positive multiple of "
            f"{RECORD_SIZE}"
        )
    return [
        bina[offset : offset + RECORD_SIZE]
        for offset in range(0, len(bina), RECORD_SIZE)
    ]


def decode_setup_record(record: bytes) -> tuple[bytes, bytes, tuple[int, int, int]]:
    if len(record) != RECORD_SIZE:
        raise DfuFormatError("setup record has the wrong length")
    if operation(record) != SETUP_OPERATION:
        raise DfuFormatError(
            f"first record operation is {operation(record)}, expected "
            f"{SETUP_OPERATION}"
        )
    payload = record[RECORD_HEADER_SIZE:]
    product = invert_bytes(payload[0:7])
    version = invert_bytes(payload[8:16])
    lengths = struct.unpack_from(">III", payload, 20)
    return product, version, lengths


def parse_bina(bina: bytes, *, strict: bool = True) -> BinaInfo:
    records = records_from_bina(bina)
    if len(records) < 2:
        raise DfuFormatError("BINA needs a setup record and at least one page")

    product, version, lengths = decode_setup_record(records[0])
    page_records = records[1:]
    image = b"".join(record[RECORD_HEADER_SIZE:] for record in page_records)
    declared_length = lengths[1]

    if strict:
        if product != PRODUCT_ID:
            raise DfuFormatError(
                f"setup product is {product!r}, expected {PRODUCT_ID!r}"
            )
        if declared_length != len(image):
            raise DfuFormatError(
                f"setup declares {declared_length:#x} bytes but carries "
                f"{len(image):#x}"
            )
        page_count = len(page_records)
        for page_index, record in enumerate(page_records):
            op = operation(record)
            if op != PAGE_OPERATION:
                raise DfuFormatError(
                    f"page {page_index} operation is {op}, expected "
                    f"{PAGE_OPERATION}"
                )
            # The vendor generator increments this display/progress byte once
            # per 64 complete 540-byte records. Record zero is the setup
            # record, so page 63 (BINA record 64) is the first value of 2.
            expected_progress = min((page_index + 1) // 64 + 1, 100)
            if record[0] != expected_progress:
                raise DfuFormatError(
                    f"page {page_index} progress is {record[0]}, expected "
                    f"{expected_progress}"
                )
            expected_marker = (
                0xFFFFFFFF if page_index == page_count - 1 else page_index
            )
            marker = page_marker(record)
            if marker != expected_marker:
                raise DfuFormatError(
                    f"page {page_index} marker is {marker:#x}, expected "
                    f"{expected_marker:#x}"
                )

    return BinaInfo(
        product=product,
        version=version,
        declared_length=declared_length,
        image=image,
        record_count=len(records),
    )


def build_bina(image: bytes, version: bytes) -> bytes:
    if not image or len(image) % PAGE_SIZE:
        raise DfuFormatError(
            f"image length {len(image):#x} must be a positive multiple of "
            f"{PAGE_SIZE}"
        )
    if len(version) != 8:
        raise DfuFormatError("the DFU version field must be exactly 8 bytes")

    setup_header = bytearray(RECORD_HEADER_SIZE)
    setup_header[0] = 1
    setup_header[2:4] = SETUP_OPERATION.to_bytes(2, "big")
    setup_payload = bytearray(PAGE_SIZE)
    setup_payload[0:7] = invert_bytes(PRODUCT_ID)
    setup_payload[8:16] = invert_bytes(version)
    struct.pack_into(">III", setup_payload, 20, 0, len(image), 0)

    records = [bytes(setup_header + setup_payload)]
    page_count = len(image) // PAGE_SIZE
    for page_index in range(page_count):
        header = bytearray(RECORD_HEADER_SIZE)
        header[0] = min((page_index + 1) // 64 + 1, 100)
        header[2:4] = PAGE_OPERATION.to_bytes(2, "big")
        marker = 0xFFFFFFFF if page_index == page_count - 1 else page_index
        header[12:16] = marker.to_bytes(4, "big")
        page = image[page_index * PAGE_SIZE : (page_index + 1) * PAGE_SIZE]
        records.append(bytes(header) + page)
    return b"".join(records)


def reports_from_record(record: bytes) -> Iterator[bytes]:
    if len(record) != RECORD_SIZE:
        raise DfuFormatError("record length is not 540 bytes")
    for sequence in range(REPORTS_PER_RECORD):
        start = sequence * REPORT_DATA_SIZE
        chunk = record[start : start + REPORT_DATA_SIZE]
        yield bytes((1, sequence, 0, 0)) + chunk


def reports_from_bina(bina: bytes) -> Iterator[bytes]:
    for record in records_from_bina(bina):
        yield from reports_from_record(record)


class UsbDfu:
    def __init__(self) -> None:
        try:
            import usb.core  # type: ignore
            import usb.util  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "PyUSB is required for live USB commands (python3-usb)"
            ) from exc
        self.usb_core = usb.core
        self.usb_util = usb.util
        self.device = None
        self.detached = False

    def __enter__(self) -> "UsbDfu":
        device = self.usb_core.find(idVendor=VID, idProduct=PID)
        if device is None:
            raise RuntimeError(
                f"NUX Core Deluxe DFU {VID:04x}:{PID:04x} was not found"
            )
        self.device = device
        if device.is_kernel_driver_active(0):
            device.detach_kernel_driver(0)
            self.detached = True
        self.usb_util.claim_interface(device, 0)
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        if self.device is None:
            return
        try:
            self.usb_util.release_interface(self.device, 0)
        except Exception:
            pass
        if self.detached:
            try:
                self.device.attach_kernel_driver(0)
            except Exception as reattach_error:
                print(
                    f"warning: could not reattach the kernel HID driver: "
                    f"{reattach_error}",
                    file=sys.stderr,
                )
        self.usb_util.dispose_resources(self.device)

    def drain(self, timeout_ms: int = 20) -> list[bytes]:
        assert self.device is not None
        reports: list[bytes] = []
        while True:
            try:
                reports.append(
                    bytes(
                        self.device.read(
                            EP_IN, USB_REPORT_SIZE, timeout=timeout_ms
                        )
                    )
                )
            except self.usb_core.USBTimeoutError:
                return reports

    def write_report(
        self, report: bytes, timeout_ms: int = 1000, retries: int = 5
    ) -> None:
        assert self.device is not None
        if len(report) != USB_REPORT_SIZE:
            raise ValueError("USB report must be exactly 64 bytes")
        last_error: Exception | None = None
        for attempt in range(1, retries + 1):
            try:
                written = self.device.write(
                    EP_OUT, report, timeout=timeout_ms
                )
                if written == USB_REPORT_SIZE:
                    return
                last_error = RuntimeError(
                    f"short HID write: {written}/{USB_REPORT_SIZE} bytes"
                )
            except self.usb_core.USBError as error:
                last_error = error
            if attempt != retries:
                time.sleep(0.01)
        raise RuntimeError(
            f"HID report failed after {retries} attempts: {last_error}"
        )

    def read_report(self, timeout_ms: int = 1000) -> bytes:
        assert self.device is not None
        return bytes(
            self.device.read(EP_IN, USB_REPORT_SIZE, timeout=timeout_ms)
        )

    def query_version(self) -> tuple[bytes, bytes]:
        self.drain()
        self.write_report(bytes((0x56,)) + bytes(USB_REPORT_SIZE - 1))
        response = self.read_report()
        if len(response) != USB_REPORT_SIZE or response[:3] != b"\0\0\0":
            raise RuntimeError(
                f"unexpected version-query response: {response.hex()}"
            )
        # The DFU handler copies its eight-byte version buffer at response+3.
        # The stock string begins with ASCII 'V' (0x56), which originally
        # made this look like an echoed query opcode.
        return response[3:11], response


def printable_version(version: bytes) -> str:
    return version.rstrip(b"\0\xff").decode("ascii", errors="replace")


def command_inspect(args: argparse.Namespace) -> None:
    data = args.path.read_bytes()
    bina, sections = extract_bina(data)
    info = parse_bina(bina, strict=not args.relaxed)

    print(f"path:             {args.path}")
    print(f"file size:        {len(data):#x} ({len(data):,})")
    print(f"file SHA-256:     {sha256(data)}")
    if sections:
        print("container sections:")
        for section in sections:
            print(
                f"  {section.tag.decode():4s} "
                f"offset={section.offset:#x} size={section.size:#x}"
            )
    else:
        print("container:        raw BINA")
    print(f"BINA size:        {len(bina):#x}")
    print(f"BINA SHA-256:     {sha256(bina)}")
    print(f"records:          {info.record_count:,}")
    print(f"pages:            {info.page_count:,}")
    print(f"product:          {info.product!r}")
    print(
        f"version:          {printable_version(info.version)!r} "
        f"({info.version.hex()})"
    )
    print(f"declared length:  {info.declared_length:#x}")
    print(
        f"flash mapping:    {FLASH_BASE:#x}..{info.flash_end - 1:#x} "
        f"({len(info.image):#x} bytes)"
    )
    print(f"image SHA-256:    {sha256(info.image)}")
    print(f"USB reports:      {info.record_count * REPORTS_PER_RECORD:,}")


def command_query(_args: argparse.Namespace) -> None:
    with UsbDfu() as device:
        version, response = device.query_version()
    print(f"device:           {VID:04x}:{PID:04x}")
    print(f"version:          {printable_version(version)!r}")
    print(f"raw response:     {response.hex()}")


def replace_engine_slot(
    stock: bytes, *, selected_slot: int, source_slot: int
) -> bytes:
    selected_offset = FLASH_BASE + selected_slot * ENGINE_SLOT_SIZE
    source_offset = FLASH_BASE + source_slot * ENGINE_SLOT_SIZE
    required = max(selected_offset, source_offset) + ENGINE_SLOT_SIZE
    if len(stock) < required:
        raise DfuFormatError(
            f"stock dump is too short for engine slot {source_slot}"
        )

    flash_end = selected_offset + ENGINE_SLOT_SIZE
    image = bytearray(stock[FLASH_BASE:flash_end])
    target_relative = selected_offset - FLASH_BASE
    image[target_relative : target_relative + ENGINE_SLOT_SIZE] = stock[
        source_offset : source_offset + ENGINE_SLOT_SIZE
    ]
    return bytes(image)


def write_bina(path: Path, bina: bytes) -> None:
    if path.exists():
        raise FileExistsError(f"refusing to overwrite existing file: {path}")
    path.write_bytes(bina)


def command_make_engine_test(args: argparse.Namespace) -> None:
    stock = args.stock.read_bytes()
    stock_sha = sha256(stock)
    if args.expected_stock_sha256 and stock_sha != args.expected_stock_sha256:
        raise DfuFormatError(
            f"stock SHA-256 is {stock_sha}, expected "
            f"{args.expected_stock_sha256}"
        )

    test_image = replace_engine_slot(
        stock,
        selected_slot=args.selected_slot,
        source_slot=args.source_slot,
    )
    restore_end = (
        FLASH_BASE + (args.selected_slot + 1) * ENGINE_SLOT_SIZE
    )
    restore_image = stock[FLASH_BASE:restore_end]

    test_bina = build_bina(test_image, args.test_version.encode("ascii"))
    restore_bina = build_bina(
        restore_image, args.restore_version.encode("ascii")
    )
    # Parse what we just built using all strict invariants before writing it.
    parse_bina(test_bina)
    parse_bina(restore_bina)
    write_bina(args.output, test_bina)
    write_bina(args.restore_output, restore_bina)

    target_start = FLASH_BASE + args.selected_slot * ENGINE_SLOT_SIZE
    target_end = target_start + ENGINE_SLOT_SIZE
    source_start = FLASH_BASE + args.source_slot * ENGINE_SLOT_SIZE
    print(f"stock SHA-256:    {stock_sha}")
    print(
        f"test mapping:     source slot {args.source_slot} "
        f"{source_start:#x}..{source_start + ENGINE_SLOT_SIZE - 1:#x}"
    )
    print(
        f"                  -> selected slot {args.selected_slot} "
        f"{target_start:#x}..{target_end - 1:#x}"
    )
    print(
        f"test BINA:        {args.output} ({len(test_bina):#x}, "
        f"SHA-256 {sha256(test_bina)})"
    )
    print(
        f"restore BINA:     {args.restore_output} ({len(restore_bina):#x}, "
        f"SHA-256 {sha256(restore_bina)})"
    )
    print(
        f"programmed range: {FLASH_BASE:#x}.."
        f"{FLASH_BASE + len(test_image) - 1:#x}"
    )


def command_dry_run(args: argparse.Namespace) -> None:
    data = args.path.read_bytes()
    bina, _sections = extract_bina(data)
    info = parse_bina(bina)
    reports = list(reports_from_bina(bina))
    if any(len(report) != USB_REPORT_SIZE for report in reports):
        raise AssertionError("generated a non-64-byte USB report")
    print(f"BINA SHA-256:     {sha256(bina)}")
    print(f"records:          {info.record_count:,}")
    print(f"USB reports:      {len(reports):,}")
    print(
        f"flash mapping:    {FLASH_BASE:#x}..{info.flash_end - 1:#x}"
    )
    print(f"first report:     {reports[0].hex()}")
    print(f"last report:      {reports[-1].hex()}")
    print("result:           all structural checks passed; nothing was sent")


def command_stream(args: argparse.Namespace) -> None:
    data = args.path.read_bytes()
    bina, _sections = extract_bina(data)
    info = parse_bina(bina)
    actual_sha = sha256(bina)
    expected_sha = args.confirm_sha256.lower()

    if not args.execute:
        raise RuntimeError(
            "refusing to write flash without --execute; use dry-run first"
        )
    if actual_sha != expected_sha:
        raise RuntimeError(
            f"BINA SHA-256 is {actual_sha}, not confirmation "
            f"{expected_sha}"
        )
    if info.flash_end > args.max_flash_end:
        raise RuntimeError(
            f"transfer ends at {info.flash_end:#x}, beyond approved limit "
            f"{args.max_flash_end:#x}"
        )

    reports = reports_from_bina(bina)
    total_reports = info.record_count * REPORTS_PER_RECORD
    with UsbDfu() as device:
        current_version, _response = device.query_version()
        current_version_text = printable_version(current_version)
        if (
            args.expected_device_version
            and current_version_text != args.expected_device_version
        ):
            raise RuntimeError(
                f"device reports {current_version_text!r}, expected "
                f"{args.expected_device_version!r}"
            )
        print(
            f"device version {current_version_text!r}; streaming "
            f"{total_reports:,} reports",
            flush=True,
        )
        for report_index, report in enumerate(reports, 1):
            device.write_report(report)
            if (
                report_index == total_reports
                or report_index % (REPORTS_PER_RECORD * 32) == 0
            ):
                percent = report_index * 100 / total_reports
                print(
                    f"\r{report_index:,}/{total_reports:,} "
                    f"reports ({percent:5.1f}%)",
                    end="",
                    flush=True,
                )
        print()
        time.sleep(args.settle_seconds)
        new_version, response = device.query_version()
        print(
            f"post-transfer version: {printable_version(new_version)!r} "
            f"({response.hex()})"
        )


def exact_ascii_8(value: str) -> str:
    encoded = value.encode("ascii")
    if len(encoded) != 8:
        raise argparse.ArgumentTypeError(
            "value must encode to exactly 8 ASCII bytes"
        )
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser(
        "inspect", help="inspect an official container or raw BINA stream"
    )
    inspect_parser.add_argument("path", type=Path)
    inspect_parser.add_argument(
        "--relaxed",
        action="store_true",
        help="show metadata without enforcing all recovered invariants",
    )
    inspect_parser.set_defaults(func=command_inspect)

    query_parser = subparsers.add_parser(
        "query", help="send the official non-destructive 0x56 version query"
    )
    query_parser.set_defaults(func=command_query)

    make_parser = subparsers.add_parser(
        "make-engine-test",
        help=(
            "build reversible BINA streams that copy one existing engine "
            "into the selected slot"
        ),
    )
    make_parser.add_argument("stock", type=Path)
    make_parser.add_argument("output", type=Path)
    make_parser.add_argument("restore_output", type=Path)
    make_parser.add_argument("--selected-slot", type=int, default=1)
    make_parser.add_argument("--source-slot", type=int, default=3)
    make_parser.add_argument(
        "--test-version", type=exact_ascii_8, default="ENG3TEST"
    )
    make_parser.add_argument(
        "--restore-version", type=exact_ascii_8, default="V1.2.4\0\0"
    )
    make_parser.add_argument("--expected-stock-sha256")
    make_parser.set_defaults(func=command_make_engine_test)

    dry_run_parser = subparsers.add_parser(
        "dry-run", help="expand every USB report without touching USB"
    )
    dry_run_parser.add_argument("path", type=Path)
    dry_run_parser.set_defaults(func=command_dry_run)

    stream_parser = subparsers.add_parser(
        "stream", help="stream a validated BINA image to the DFU bootloader"
    )
    stream_parser.add_argument("path", type=Path)
    stream_parser.add_argument(
        "--execute",
        action="store_true",
        help="required acknowledgement that this command writes NOR flash",
    )
    stream_parser.add_argument(
        "--confirm-sha256",
        required=True,
        help="required exact SHA-256 of the BINA bytes being sent",
    )
    stream_parser.add_argument("--expected-device-version")
    stream_parser.add_argument(
        "--max-flash-end",
        type=parse_number,
        default=0xA0000,
        help="refuse a transfer extending past this exclusive flash offset",
    )
    stream_parser.add_argument(
        "--settle-seconds", type=float, default=1.0
    )
    stream_parser.set_defaults(func=command_stream)

    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except (
        DfuFormatError,
        FileExistsError,
        RuntimeError,
        OSError,
    ) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

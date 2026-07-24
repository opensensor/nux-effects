#!/usr/bin/env python3
"""Build and inspect guarded open-firmware images for the NCR-2.

This tool never opens a USB device. Its full-image packer starts from the
verified stock dump, preserves the stock boot header and factory compatibility
region, and emits a byte-range diff report for review before programmer use.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LAYOUT = (
    ROOT / "firmware" / "platform" / "ncr2" / "flash_layout.json"
)

MANIFEST_MAGIC = 0x4E45504F
MANIFEST_BOARD_NCR2 = 0x3252434E
MANIFEST_FORMAT_VERSION = 1
MANIFEST_STRUCT = struct.Struct("<IHHIIIIII32sI")

BOOT_RECORD_MAGIC = 0x31545342
BOOT_RECORD_FORMAT_VERSION = 1
BOOT_RECORD_STRUCT = struct.Struct("<IHHIBBBBI8sI")
BOOT_SLOT_A = 0
BOOT_SLOT_NONE = 0xFF
BOOT_DEFAULT_MAX_TRIALS = 3

IVT_OFFSET = 0x1000
IVT_STRUCT = struct.Struct("<8I")
BOOT_DATA_STRUCT = struct.Struct("<3I")
XIP_BASE = 0x60000000


class ImageError(RuntimeError):
    """Raised when an image violates a format or range invariant."""


@dataclass(frozen=True)
class Region:
    name: str
    offset: int
    size: int
    policy: str

    @property
    def end(self) -> int:
        return self.offset + self.size


@dataclass(frozen=True)
class Layout:
    path: Path
    schema_version: int
    board_id: str
    flash_size: int
    xip_base: int
    erased_byte: int
    regions: tuple[Region, ...]
    boot_header_offset: int
    boot_header_size: int
    manifest_size: int
    application_load_address: int
    known_stock_sha256: str

    def region(self, name: str) -> Region:
        matches = [region for region in self.regions if region.name == name]
        if len(matches) != 1:
            raise ImageError(f"layout has {len(matches)} regions named {name!r}")
        return matches[0]


@dataclass(frozen=True)
class Manifest:
    image_size: int
    load_address: int
    vector_offset: int
    board_id: int
    semantic_version: int
    build_number: int
    image_sha256: bytes
    header_crc32: int


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_layout(path: Path = DEFAULT_LAYOUT) -> Layout:
    raw = json.loads(path.read_text())
    regions = tuple(
        Region(
            name=entry["name"],
            offset=int(entry["offset"]),
            size=int(entry["size"]),
            policy=entry["policy"],
        )
        for entry in raw["regions"]
    )
    layout = Layout(
        path=path,
        schema_version=int(raw["schema_version"]),
        board_id=raw["board_id"],
        flash_size=int(raw["flash_size"]),
        xip_base=int(raw["xip_base"]),
        erased_byte=int(raw["erased_byte"]),
        regions=regions,
        boot_header_offset=int(raw["boot_header"]["offset"]),
        boot_header_size=int(raw["boot_header"]["size"]),
        manifest_size=int(raw["application_manifest_size"]),
        application_load_address=int(raw["application_load_address"]),
        known_stock_sha256=raw["known_stock_sha256"],
    )
    validate_layout(layout)
    return layout


def validate_layout(layout: Layout) -> None:
    if layout.schema_version != 1:
        raise ImageError(f"unsupported layout schema {layout.schema_version}")
    if layout.board_id != "NCR2":
        raise ImageError(f"unexpected board ID {layout.board_id!r}")
    if layout.flash_size != 0x800000:
        raise ImageError(f"unexpected flash size {layout.flash_size:#x}")
    if layout.xip_base != XIP_BASE:
        raise ImageError(f"unexpected XIP base {layout.xip_base:#x}")
    if not 0 <= layout.erased_byte <= 0xFF:
        raise ImageError("erased byte is outside the byte range")
    if layout.manifest_size != 0x1000:
        raise ImageError("application manifest must occupy one 4 KiB sector")

    ordered = sorted(layout.regions, key=lambda region: region.offset)
    expected_offset = 0
    names: set[str] = set()
    for region in ordered:
        if region.name in names:
            raise ImageError(f"duplicate region name {region.name!r}")
        names.add(region.name)
        if region.offset != expected_offset:
            raise ImageError(
                f"gap/overlap before {region.name}: "
                f"{expected_offset:#x} != {region.offset:#x}"
            )
        if region.size <= 0 or region.size % 0x1000:
            raise ImageError(
                f"region {region.name} is empty or not sector aligned"
            )
        expected_offset = region.end
    if expected_offset != layout.flash_size:
        raise ImageError(
            f"regions end at {expected_offset:#x}, not {layout.flash_size:#x}"
        )

    required = {
        "bootloader",
        "boot_metadata",
        "factory_compatibility",
        "application_a",
        "application_b",
    }
    if names != required:
        raise ImageError(f"layout region set differs: {names ^ required}")
    if layout.boot_header_offset != 0:
        raise ImageError("boot header must begin at flash offset zero")
    if (
        layout.boot_header_size
        > layout.region("bootloader").size
    ):
        raise ImageError("boot header exceeds bootloader region")
    for slot_name in ("application_a", "application_b"):
        if layout.region(slot_name).size <= layout.manifest_size:
            raise ImageError(f"{slot_name} has no payload capacity")


def parse_semantic_version(value: str) -> int:
    try:
        major_text, minor_text, patch_text = value.split(".")
        major = int(major_text, 10)
        minor = int(minor_text, 10)
        patch = int(patch_text, 10)
    except (ValueError, TypeError) as error:
        raise ImageError("semantic version must be MAJOR.MINOR.PATCH") from error
    if not 0 <= major <= 0xFF:
        raise ImageError("major version exceeds 8 bits")
    if not 0 <= minor <= 0xFF:
        raise ImageError("minor version exceeds 8 bits")
    if not 0 <= patch <= 0xFFFF:
        raise ImageError("patch version exceeds 16 bits")
    return (major << 24) | (minor << 16) | patch


def format_semantic_version(value: int) -> str:
    return f"{value >> 24}.{(value >> 16) & 0xff}.{value & 0xffff}"


def build_manifest(
    payload: bytes,
    *,
    layout: Layout,
    semantic_version: int,
    build_number: int,
) -> bytes:
    if not payload:
        raise ImageError("application payload is empty")
    capacity = layout.region("application_a").size - layout.manifest_size
    if len(payload) > capacity:
        raise ImageError(
            f"application is {len(payload):#x}, capacity is {capacity:#x}"
        )
    if not 0 <= build_number <= 0xFFFFFFFF:
        raise ImageError("build number exceeds 32 bits")

    digest = hashlib.sha256(payload).digest()
    prefix = MANIFEST_STRUCT.pack(
        MANIFEST_MAGIC,
        MANIFEST_FORMAT_VERSION,
        layout.manifest_size,
        len(payload),
        layout.application_load_address,
        0,
        MANIFEST_BOARD_NCR2,
        semantic_version,
        build_number,
        digest,
        0,
    )
    crc = zlib.crc32(prefix[:-4]) & 0xFFFFFFFF
    header = prefix[:-4] + struct.pack("<I", crc)
    if len(header) != MANIFEST_STRUCT.size:
        raise AssertionError("manifest serialization changed unexpectedly")
    return header + bytes([layout.erased_byte]) * (
        layout.manifest_size - len(header)
    )


def build_slot_image(
    payload: bytes,
    *,
    layout: Layout,
    semantic_version: int,
    build_number: int,
) -> bytes:
    """Build the exact contiguous bytes written to one open A/B slot."""
    manifest = build_manifest(
        payload,
        layout=layout,
        semantic_version=semantic_version,
        build_number=build_number,
    )
    result = manifest + payload
    if len(result) > layout.region("application_a").size:
        raise AssertionError("validated slot image exceeds its partition")
    return result


def parse_manifest(data: bytes, *, layout: Layout) -> Manifest:
    if len(data) < layout.manifest_size:
        raise ImageError("manifest sector is truncated")
    fields = MANIFEST_STRUCT.unpack_from(data)
    (
        magic,
        format_version,
        header_size,
        image_size,
        load_address,
        vector_offset,
        board_id,
        semantic_version,
        build_number,
        digest,
        header_crc32,
    ) = fields
    expected_crc = zlib.crc32(data[: MANIFEST_STRUCT.size - 4]) & 0xFFFFFFFF
    if magic != MANIFEST_MAGIC:
        raise ImageError(f"bad manifest magic {magic:#x}")
    if format_version != MANIFEST_FORMAT_VERSION:
        raise ImageError(f"bad manifest version {format_version}")
    if header_size != layout.manifest_size:
        raise ImageError(f"bad manifest header size {header_size:#x}")
    if load_address != layout.application_load_address:
        raise ImageError(f"bad load address {load_address:#x}")
    if vector_offset != 0:
        raise ImageError(f"unsupported vector offset {vector_offset:#x}")
    if board_id != MANIFEST_BOARD_NCR2:
        raise ImageError(f"wrong manifest board ID {board_id:#x}")
    if header_crc32 != expected_crc:
        raise ImageError(
            f"manifest CRC {header_crc32:#x} != {expected_crc:#x}"
        )
    capacity = layout.region("application_a").size - layout.manifest_size
    if not 8 <= image_size <= capacity:
        raise ImageError(f"manifest image size {image_size:#x} is invalid")
    return Manifest(
        image_size=image_size,
        load_address=load_address,
        vector_offset=vector_offset,
        board_id=board_id,
        semantic_version=semantic_version,
        build_number=build_number,
        image_sha256=digest,
        header_crc32=header_crc32,
    )


def build_initial_boot_record() -> bytes:
    prefix = BOOT_RECORD_STRUCT.pack(
        BOOT_RECORD_MAGIC,
        BOOT_RECORD_FORMAT_VERSION,
        BOOT_RECORD_STRUCT.size,
        1,
        BOOT_SLOT_A,
        BOOT_SLOT_NONE,
        0,
        BOOT_DEFAULT_MAX_TRIALS,
        0,
        bytes(8),
        0,
    )
    crc = zlib.crc32(prefix[:-4]) & 0xFFFFFFFF
    return prefix[:-4] + struct.pack("<I", crc)


def parse_boot_record(data: bytes) -> dict[str, int]:
    if len(data) < BOOT_RECORD_STRUCT.size:
        raise ImageError("boot record is truncated")
    (
        magic,
        format_version,
        record_size,
        sequence,
        confirmed_slot,
        pending_slot,
        trial_count,
        max_trials,
        flags,
        _reserved,
        crc,
    ) = BOOT_RECORD_STRUCT.unpack_from(data)
    expected_crc = zlib.crc32(data[: BOOT_RECORD_STRUCT.size - 4])
    expected_crc &= 0xFFFFFFFF
    if magic != BOOT_RECORD_MAGIC:
        raise ImageError(f"bad boot-record magic {magic:#x}")
    if format_version != BOOT_RECORD_FORMAT_VERSION:
        raise ImageError(f"bad boot-record format {format_version}")
    if record_size != BOOT_RECORD_STRUCT.size:
        raise ImageError(f"bad boot-record size {record_size}")
    if confirmed_slot not in (0, 1):
        raise ImageError(f"bad confirmed slot {confirmed_slot}")
    if pending_slot not in (0, 1, BOOT_SLOT_NONE):
        raise ImageError(f"bad pending slot {pending_slot}")
    if pending_slot == confirmed_slot:
        raise ImageError("pending slot duplicates confirmed slot")
    if max_trials == 0 or trial_count > max_trials:
        raise ImageError("boot-record trial count is invalid")
    if crc != expected_crc:
        raise ImageError(f"boot-record CRC {crc:#x} != {expected_crc:#x}")
    return {
        "sequence": sequence,
        "confirmed_slot": confirmed_slot,
        "pending_slot": pending_slot,
        "trial_count": trial_count,
        "max_trials": max_trials,
        "flags": flags,
        "crc32": crc,
    }


def validate_vector(payload: bytes, manifest: Manifest) -> None:
    if len(payload) < manifest.image_size:
        raise ImageError("application payload is truncated")
    initial_stack, reset_handler = struct.unpack_from("<II", payload)
    reset_address = reset_handler & ~1
    image_end = manifest.load_address + manifest.image_size
    if not 0x20000000 <= initial_stack <= 0x20020000:
        raise ImageError(f"initial stack {initial_stack:#x} is outside DTCM")
    if initial_stack & 7:
        raise ImageError(f"initial stack {initial_stack:#x} is misaligned")
    if not reset_handler & 1:
        raise ImageError("application reset handler is not Thumb")
    if not manifest.load_address <= reset_address < image_end:
        raise ImageError(
            f"reset handler {reset_handler:#x} is outside application"
        )
    digest = hashlib.sha256(payload[: manifest.image_size]).digest()
    if digest != manifest.image_sha256:
        raise ImageError("application payload SHA-256 does not match manifest")


def validate_bootloader(payload: bytes, layout: Layout) -> None:
    capacity = (
        layout.region("bootloader").size - layout.boot_header_size
    )
    if not 8 <= len(payload) <= capacity:
        raise ImageError(
            f"bootloader size {len(payload):#x} exceeds {capacity:#x}"
        )
    initial_stack, reset_handler = struct.unpack_from("<II", payload)
    reset_address = reset_handler & ~1
    load_address = layout.xip_base + layout.boot_header_size
    if not 0x20000000 <= initial_stack <= 0x20020000:
        raise ImageError(f"boot stack {initial_stack:#x} is outside DTCM")
    if initial_stack & 7:
        raise ImageError(f"boot stack {initial_stack:#x} is misaligned")
    if not reset_handler & 1:
        raise ImageError("boot reset handler is not Thumb")
    if not load_address <= reset_address < load_address + len(payload):
        raise ImageError(
            f"boot reset handler {reset_handler:#x} is outside image"
        )


def require_stock_dump(stock: bytes, layout: Layout) -> None:
    if len(stock) != layout.flash_size:
        raise ImageError(
            f"stock dump is {len(stock):#x}, expected {layout.flash_size:#x}"
        )
    actual = sha256(stock)
    if actual != layout.known_stock_sha256:
        raise ImageError(
            f"stock dump SHA-256 {actual} != {layout.known_stock_sha256}"
        )


def changed_ranges(before: bytes, after: bytes) -> list[dict[str, Any]]:
    if len(before) != len(after):
        raise ImageError("cannot diff images of different sizes")
    ranges: list[dict[str, Any]] = []
    start: int | None = None
    for index, (old, new) in enumerate(zip(before, after, strict=True)):
        if old != new and start is None:
            start = index
        if old == new and start is not None:
            ranges.append(
                {
                    "offset": start,
                    "end_exclusive": index,
                    "size": index - start,
                }
            )
            start = None
    if start is not None:
        ranges.append(
            {
                "offset": start,
                "end_exclusive": len(before),
                "size": len(before) - start,
            }
        )
    return ranges


def containing_region(layout: Layout, offset: int, end: int) -> str:
    for region in layout.regions:
        if region.offset <= offset and end <= region.end:
            return region.name
    raise ImageError(f"changed range {offset:#x}..{end:#x} crosses regions")


def split_ranges_at_regions(
    layout: Layout, ranges: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    split: list[dict[str, Any]] = []
    for entry in ranges:
        cursor = entry["offset"]
        end = entry["end_exclusive"]
        while cursor < end:
            region = next(
                (
                    candidate
                    for candidate in layout.regions
                    if candidate.offset <= cursor < candidate.end
                ),
                None,
            )
            if region is None:
                raise ImageError(f"changed byte {cursor:#x} is outside layout")
            part_end = min(end, region.end)
            split.append(
                {
                    "offset": cursor,
                    "end_exclusive": part_end,
                    "size": part_end - cursor,
                    "region": region.name,
                }
            )
            cursor = part_end
    return split


def summarize_changed_ranges(
    ranges: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    summaries: dict[str, dict[str, Any]] = {}
    for entry in ranges:
        name = entry["region"]
        if name not in summaries:
            summaries[name] = {
                "region": name,
                "offset": entry["offset"],
                "end_exclusive": entry["end_exclusive"],
                "changed_bytes": entry["size"],
            }
        else:
            summary = summaries[name]
            summary["offset"] = min(summary["offset"], entry["offset"])
            summary["end_exclusive"] = max(
                summary["end_exclusive"], entry["end_exclusive"]
            )
            summary["changed_bytes"] += entry["size"]
    for summary in summaries.values():
        summary["span_size"] = (
            summary["end_exclusive"] - summary["offset"]
        )
    return sorted(summaries.values(), key=lambda entry: entry["offset"])


def build_full_image(
    stock: bytes,
    bootloader: bytes,
    application: bytes,
    *,
    layout: Layout,
    semantic_version: int,
    build_number: int,
    verify_stock: bool = True,
) -> tuple[bytes, dict[str, Any]]:
    if verify_stock:
        require_stock_dump(stock, layout)
    elif len(stock) != layout.flash_size:
        raise ImageError("synthetic stock image has the wrong size")

    validate_bootloader(bootloader, layout)
    manifest_sector = build_manifest(
        application,
        layout=layout,
        semantic_version=semantic_version,
        build_number=build_number,
    )
    parsed_manifest = parse_manifest(manifest_sector, layout=layout)
    validate_vector(application, parsed_manifest)

    image = bytearray(stock)
    erased = bytes([layout.erased_byte])
    boot_region = layout.region("bootloader")
    metadata_region = layout.region("boot_metadata")
    slot_a = layout.region("application_a")
    slot_b = layout.region("application_b")

    boot_payload_offset = layout.boot_header_size
    image[boot_payload_offset : boot_region.end] = erased * (
        boot_region.end - boot_payload_offset
    )
    image[
        boot_payload_offset : boot_payload_offset + len(bootloader)
    ] = bootloader

    image[metadata_region.offset : metadata_region.end] = (
        erased * metadata_region.size
    )
    initial_boot_record = build_initial_boot_record()
    image[
        metadata_region.offset :
        metadata_region.offset + len(initial_boot_record)
    ] = initial_boot_record
    image[slot_a.offset : slot_a.end] = erased * slot_a.size
    image[slot_a.offset : slot_a.offset + layout.manifest_size] = (
        manifest_sector
    )
    payload_offset = slot_a.offset + layout.manifest_size
    image[payload_offset : payload_offset + len(application)] = application
    image[slot_b.offset : slot_b.end] = erased * slot_b.size

    output = bytes(image)
    factory = layout.region("factory_compatibility")
    if (
        output[layout.boot_header_offset : layout.boot_header_size]
        != stock[layout.boot_header_offset : layout.boot_header_size]
    ):
        raise ImageError("stock boot header changed")
    if output[factory.offset : factory.end] != stock[
        factory.offset : factory.end
    ]:
        raise ImageError("factory compatibility region changed")

    allowed = {
        "bootloader",
        "boot_metadata",
        "application_a",
        "application_b",
    }
    ranges = split_ranges_at_regions(
        layout, changed_ranges(stock, output)
    )
    for entry in ranges:
        name = entry["region"]
        containing_region(layout, entry["offset"], entry["end_exclusive"])
        if name not in allowed:
            raise ImageError(f"unexpected changed region {name}")
        if (
            name == "bootloader"
            and entry["offset"] < layout.boot_header_size
        ):
            raise ImageError("changed range overlaps stock boot header")

    report: dict[str, Any] = {
        "format": "ncr2-open-full-image-report-v1",
        "stock_sha256": sha256(stock),
        "bootloader_sha256": sha256(bootloader),
        "application_sha256": sha256(application),
        "output_sha256": sha256(output),
        "output_size": len(output),
        "semantic_version": format_semantic_version(semantic_version),
        "build_number": build_number,
        "manifest": {
            "slot": "application_a",
            "offset": slot_a.offset,
            "payload_offset": payload_offset,
            "image_size": parsed_manifest.image_size,
            "image_sha256": parsed_manifest.image_sha256.hex(),
            "header_crc32": f"0x{parsed_manifest.header_crc32:08x}",
        },
        "boot_metadata": parse_boot_record(initial_boot_record),
        "preserved": {
            "boot_header_sha256": sha256(
                output[
                    layout.boot_header_offset : layout.boot_header_size
                ]
            ),
            "factory_sha256": sha256(
                output[factory.offset : factory.end]
            ),
        },
        "changed_ranges": summarize_changed_ranges(ranges),
    }
    return output, report


def pointer_to_offset(pointer: int, image_size: int) -> int:
    if not XIP_BASE <= pointer < XIP_BASE + image_size:
        raise ImageError(f"pointer {pointer:#x} is outside XIP image")
    return pointer - XIP_BASE


def parse_dcd(stock: bytes, dcd_pointer: int) -> dict[str, Any]:
    offset = pointer_to_offset(dcd_pointer, len(stock))
    if offset + 4 > len(stock):
        raise ImageError("DCD header is truncated")
    tag = stock[offset]
    length = int.from_bytes(stock[offset + 1 : offset + 3], "big")
    version = stock[offset + 3]
    if tag != 0xD2 or length < 4 or offset + length > len(stock):
        raise ImageError("DCD header is invalid")

    commands: list[dict[str, Any]] = []
    position = offset + 4
    end = offset + length
    while position < end:
        if position + 4 > end:
            raise ImageError("truncated DCD command header")
        command_tag = stock[position]
        command_length = int.from_bytes(
            stock[position + 1 : position + 3], "big"
        )
        parameter = stock[position + 3]
        if command_length < 4 or position + command_length > end:
            raise ImageError("DCD command length is invalid")
        body = stock[position + 4 : position + command_length]
        command: dict[str, Any] = {
            "offset": f"0x{position:06x}",
            "tag": f"0x{command_tag:02x}",
            "length": command_length,
            "parameter": f"0x{parameter:02x}",
        }
        if command_tag == 0xCC and len(body) % 8 == 0:
            writes = []
            for item in range(0, len(body), 8):
                address = int.from_bytes(body[item : item + 4], "big")
                value = int.from_bytes(body[item + 4 : item + 8], "big")
                writes.append(
                    {
                        "address": f"0x{address:08x}",
                        "value": f"0x{value:08x}",
                    }
                )
            command["write_data"] = writes
        else:
            command["body_hex"] = body.hex()
        commands.append(command)
        position += command_length
    if position != end:
        raise ImageError("DCD parser did not end at declared boundary")
    return {
        "offset": f"0x{offset:06x}",
        "tag": f"0x{tag:02x}",
        "length": length,
        "version": f"0x{version:02x}",
        "sha256": sha256(stock[offset:end]),
        "commands": commands,
    }


def extract_boot_config(stock: bytes, *, layout: Layout) -> dict[str, Any]:
    require_stock_dump(stock, layout)
    if stock[:4] != b"FCFB":
        raise ImageError("stock image does not begin with FCFB")
    (
        ivt_header,
        entry,
        reserved1,
        dcd_pointer,
        boot_data_pointer,
        self_pointer,
        csf_pointer,
        reserved2,
    ) = IVT_STRUCT.unpack_from(stock, IVT_OFFSET)
    boot_data_offset = pointer_to_offset(boot_data_pointer, len(stock))
    image_start, image_length, plugin = BOOT_DATA_STRUCT.unpack_from(
        stock, boot_data_offset
    )
    dcd = parse_dcd(stock, dcd_pointer)
    return {
        "format": "ncr2-stock-boot-config-v1",
        "source_dump_sha256": sha256(stock),
        "xip_base": f"0x{layout.xip_base:08x}",
        "fcfb": {
            "offset": "0x000000",
            "size": 512,
            "tag": stock[:4].decode("ascii"),
            "sha256": sha256(stock[:512]),
            "first_32_bytes": stock[:32].hex(),
        },
        "ivt": {
            "offset": f"0x{IVT_OFFSET:06x}",
            "header": f"0x{ivt_header:08x}",
            "entry": f"0x{entry:08x}",
            "reserved1": f"0x{reserved1:08x}",
            "dcd_pointer": f"0x{dcd_pointer:08x}",
            "boot_data_pointer": f"0x{boot_data_pointer:08x}",
            "self_pointer": f"0x{self_pointer:08x}",
            "csf_pointer": f"0x{csf_pointer:08x}",
            "reserved2": f"0x{reserved2:08x}",
        },
        "boot_data": {
            "offset": f"0x{boot_data_offset:06x}",
            "image_start": f"0x{image_start:08x}",
            "image_length": f"0x{image_length:08x}",
            "plugin": f"0x{plugin:08x}",
        },
        "dcd": dcd,
    }


def write_boot_fragments(
    stock: bytes, config: dict[str, Any], output_dir: Path
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    dcd_offset = int(config["dcd"]["offset"], 16)
    dcd_length = int(config["dcd"]["length"])
    fragments = {
        "stock_fcfb.bin": stock[:512],
        "stock_ivt.bin": stock[IVT_OFFSET : IVT_OFFSET + IVT_STRUCT.size],
        "stock_dcd.bin": stock[dcd_offset : dcd_offset + dcd_length],
        "stock_boot_header.bin": stock[:0x2000],
    }
    for name, data in fragments.items():
        (output_dir / name).write_bytes(data)


def inspect_full_image(image: bytes, *, layout: Layout) -> dict[str, Any]:
    if len(image) != layout.flash_size:
        raise ImageError("full image has the wrong size")
    result: dict[str, Any] = {
        "image_size": len(image),
        "image_sha256": sha256(image),
        "slots": {},
    }
    for slot_name in ("application_a", "application_b"):
        slot = layout.region(slot_name)
        sector = image[slot.offset : slot.offset + layout.manifest_size]
        if sector == bytes([layout.erased_byte]) * layout.manifest_size:
            result["slots"][slot_name] = {"state": "erased"}
            continue
        manifest = parse_manifest(sector, layout=layout)
        payload_offset = slot.offset + layout.manifest_size
        payload = image[
            payload_offset : payload_offset + manifest.image_size
        ]
        validate_vector(payload, manifest)
        result["slots"][slot_name] = {
            "state": "valid",
            "version": format_semantic_version(manifest.semantic_version),
            "build_number": manifest.build_number,
            "image_size": manifest.image_size,
            "image_sha256": manifest.image_sha256.hex(),
        }
    return result


def print_json(data: Any) -> None:
    print(json.dumps(data, indent=2, sort_keys=True))


def command_validate_layout(args: argparse.Namespace) -> None:
    layout = load_layout(args.layout)
    print_json(
        {
            "result": "valid",
            "layout": str(layout.path),
            "flash_size": layout.flash_size,
            "regions": [
                {
                    "name": region.name,
                    "offset": region.offset,
                    "end_exclusive": region.end,
                    "size": region.size,
                    "policy": region.policy,
                }
                for region in layout.regions
            ],
        }
    )


def command_extract_boot(args: argparse.Namespace) -> None:
    layout = load_layout(args.layout)
    stock = args.dump.read_bytes()
    config = extract_boot_config(stock, layout=layout)
    args.json_output.parent.mkdir(parents=True, exist_ok=True)
    args.json_output.write_text(
        json.dumps(config, indent=2, sort_keys=True) + "\n"
    )
    if args.fragments is not None:
        write_boot_fragments(stock, config, args.fragments)
    print_json(
        {
            "json": str(args.json_output),
            "dcd_commands": len(config["dcd"]["commands"]),
            "dcd_writes": sum(
                len(command.get("write_data", []))
                for command in config["dcd"]["commands"]
            ),
            "fragments": (
                str(args.fragments) if args.fragments is not None else None
            ),
        }
    )


def command_pack(args: argparse.Namespace) -> None:
    layout = load_layout(args.layout)
    stock = args.dump.read_bytes()
    bootloader = args.bootloader.read_bytes()
    application = args.application.read_bytes()
    full_image, report = build_full_image(
        stock,
        bootloader,
        application,
        layout=layout,
        semantic_version=parse_semantic_version(args.version),
        build_number=args.build_number,
        verify_stock=True,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(full_image)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n"
    )
    print_json(
        {
            "output": str(args.output),
            "report": str(args.report),
            "output_sha256": report["output_sha256"],
            "changed_range_count": len(report["changed_ranges"]),
        }
    )


def command_pack_slot(args: argparse.Namespace) -> None:
    layout = load_layout(args.layout)
    payload = args.application.read_bytes()
    image = build_slot_image(
        payload,
        layout=layout,
        semantic_version=parse_semantic_version(args.version),
        build_number=args.build_number,
    )
    manifest = parse_manifest(image[: layout.manifest_size], layout=layout)
    validate_vector(image[layout.manifest_size :], manifest)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print_json(
        {
            "output": str(args.output),
            "output_size": len(image),
            "output_sha256": sha256(image),
            "payload_size": manifest.image_size,
            "payload_sha256": manifest.image_sha256.hex(),
            "version": format_semantic_version(manifest.semantic_version),
            "build_number": manifest.build_number,
        }
    )


def command_inspect(args: argparse.Namespace) -> None:
    print_json(
        inspect_full_image(
            args.image.read_bytes(), layout=load_layout(args.layout)
        )
    )


def path(value: str) -> Path:
    return Path(value).expanduser().resolve()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--layout",
        type=path,
        default=DEFAULT_LAYOUT,
        help=f"flash layout JSON (default: {DEFAULT_LAYOUT})",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser(
        "validate-layout", help="validate the complete flash partition map"
    )
    validate_parser.set_defaults(function=command_validate_layout)

    extract_parser = subparsers.add_parser(
        "extract-boot",
        help="extract and decode FCFB/IVT/BootData/DCD from verified stock",
    )
    extract_parser.add_argument("--dump", type=path, required=True)
    extract_parser.add_argument(
        "--json-output", type=path, required=True
    )
    extract_parser.add_argument("--fragments", type=path)
    extract_parser.set_defaults(function=command_extract_boot)

    pack_parser = subparsers.add_parser(
        "pack", help="build a guarded full-chip programmer image"
    )
    pack_parser.add_argument("--dump", type=path, required=True)
    pack_parser.add_argument("--bootloader", type=path, required=True)
    pack_parser.add_argument("--application", type=path, required=True)
    pack_parser.add_argument("--version", required=True)
    pack_parser.add_argument("--build-number", type=int, required=True)
    pack_parser.add_argument("--output", type=path, required=True)
    pack_parser.add_argument("--report", type=path, required=True)
    pack_parser.set_defaults(function=command_pack)

    slot_parser = subparsers.add_parser(
        "pack-slot",
        help="build a manifest plus application payload for open recovery",
    )
    slot_parser.add_argument("--application", type=path, required=True)
    slot_parser.add_argument("--version", required=True)
    slot_parser.add_argument("--build-number", type=int, required=True)
    slot_parser.add_argument("--output", type=path, required=True)
    slot_parser.set_defaults(function=command_pack_slot)

    inspect_parser = subparsers.add_parser(
        "inspect", help="validate manifests in a full-chip image"
    )
    inspect_parser.add_argument("image", type=path)
    inspect_parser.set_defaults(function=command_inspect)

    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        args.function(args)
    except (ImageError, OSError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())

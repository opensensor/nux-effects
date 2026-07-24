#!/usr/bin/env python3
"""Build, patch, and validate the zero-wear four-engine switch image.

This produces artifacts only.  It never opens a USB device or writes flash.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from nux_dfu import build_bina, parse_bina, sha256


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "zero_wear"

EXPECTED_STOCK_SHA256 = (
    "4263ef41c0745f6e8c00be13b52391b6b04a5f51779b12d0e191abf6888e7a14"
)

FLASH_BASE = 0x60000
SLOT_SIZE = 0x20000
ENGINE_COPY_SIZE = 0x1E000
PICKER_FLASH_OFFSET = 0x80000
METAL_FLASH_OFFSET = 0xC0000
REVERB_STOCK_OFFSET = 0x80000
REVERB_RELOCATED_OFFSET = 0xE0000
IMAGE_END = 0x100000

XIP_STORAGE_OFFSET = 0x1000
ENGINE_MONITOR_OFFSET = 0x1DF00
ENGINE_MONITOR_ADDRESS = ENGINE_MONITOR_OFFSET | 1
ENGINE_MONITOR_ENABLE_OFFSET = 0x1DFA0
GPT1_VECTOR_INDEX = 16 + 100
GPT1_VECTOR_OFFSET = GPT1_VECTOR_INDEX * 4
ENGINE_LAYOUT = (
    ("delay", 0x60000, 0x60000),
    ("reverb", REVERB_STOCK_OFFSET, REVERB_RELOCATED_OFFSET),
    ("modulation", 0xA0000, 0xA0000),
    ("metal", METAL_FLASH_OFFSET, METAL_FLASH_OFFSET),
)
EXPECTED_STARTUP = {
    "delay": (0xF8CD, 0xFC09, 0x120DE),
    "reverb": (0x19095, 0x193C9, 0x1B9F1),
    "modulation": (0xC04D, 0xC369, 0xE00B),
    "metal": (0xE4B5, 0xE7D9, 0xF2F1),
}
GPT1_START_HOOKS = {
    "delay": 0x46F4,
    "reverb": 0x145B4,
    "modulation": 0x5848,
    "metal": 0x7F28,
}
EXPECTED_GPT1_START = bytes.fromhex("0d48016841f0010101607047")


class BuildError(RuntimeError):
    pass


def run(command: list[str], *, cwd: Path) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise BuildError(f"required tool is unavailable: {name}")
    return path


def build_assembly(output_dir: Path) -> tuple[bytes, bytes, bytes]:
    gcc = require_tool("arm-none-eabi-gcc")
    objcopy = require_tool("arm-none-eabi-objcopy")
    objdump = require_tool("arm-none-eabi-objdump")

    common = [
        gcc,
        "-mcpu=cortex-m7",
        "-mthumb",
        "-nostdlib",
        "-ffreestanding",
        "-fno-builtin",
        "-Wa,--fatal-warnings",
    ]

    products: list[tuple[str, str]] = [
        ("picker_itcm", "picker_itcm.ld"),
        ("xip_runtime", "xip_runtime.ld"),
        ("engine_monitor", "engine_monitor.ld"),
    ]
    binaries: dict[str, bytes] = {}
    for stem, linker_script in products:
        source = SOURCE_DIR / f"{stem}.S"
        obj = output_dir / f"{stem}.o"
        elf = output_dir / f"{stem}.elf"
        binary = output_dir / f"{stem}.bin"
        listing = output_dir / f"{stem}.disasm.txt"
        map_file = output_dir / f"{stem}.map"

        run(common + ["-c", str(source), "-o", str(obj)], cwd=ROOT)
        run(
            common
            + [
                str(obj),
                f"-Wl,-T,{SOURCE_DIR / linker_script}",
                f"-Wl,-Map,{map_file}",
                "-Wl,--build-id=none",
                "-o",
                str(elf),
            ],
            cwd=ROOT,
        )
        run([objcopy, "-O", "binary", str(elf), str(binary)], cwd=ROOT)
        listing.write_text(
            subprocess.check_output(
                [objdump, "-d", "-r", "-t", str(elf)], text=True
            )
        )
        binaries[stem] = binary.read_bytes()

    return (
        binaries["picker_itcm"],
        binaries["xip_runtime"],
        binaries["engine_monitor"],
    )


def encode_thumb_b_w(source: int, target: int) -> bytes:
    offset = target - (source + 4)
    if offset & 1 or not -(1 << 24) <= offset < (1 << 24):
        raise BuildError("Thumb B.W target is invalid or out of range")
    immediate = offset & 0x01FFFFFF
    sign = (immediate >> 24) & 1
    i1 = (immediate >> 23) & 1
    i2 = (immediate >> 22) & 1
    imm10 = (immediate >> 12) & 0x3FF
    imm11 = (immediate >> 1) & 0x7FF
    j1 = ((~i1) & 1) ^ sign
    j2 = ((~i2) & 1) ^ sign
    first = 0xF000 | (sign << 10) | imm10
    second = 0x9000 | (j1 << 13) | (j2 << 11) | imm11
    return struct.pack("<HH", first, second)


def patch_engine(name: str, engine: bytes, monitor: bytes) -> bytes:
    if len(engine) != SLOT_SIZE:
        raise BuildError("engine slot does not have the expected size")
    if not monitor or len(monitor) > 0x100:
        raise BuildError("engine monitor does not fit its code cave")
    cave = engine[ENGINE_MONITOR_OFFSET : ENGINE_MONITOR_OFFSET + 0x100]
    if cave != bytes(0x100):
        raise BuildError("full engine monitor code cave is not zero-filled")
    hook_offset = GPT1_START_HOOKS[name]
    if (
        engine[hook_offset : hook_offset + len(EXPECTED_GPT1_START)]
        != EXPECTED_GPT1_START
    ):
        raise BuildError(f"{name} GPT1 start wrapper changed from audit")
    patched = bytearray(engine)
    patched[
        ENGINE_MONITOR_OFFSET : ENGINE_MONITOR_OFFSET + len(monitor)
    ] = monitor
    struct.pack_into(
        "<I", patched, GPT1_VECTOR_OFFSET, ENGINE_MONITOR_ADDRESS
    )
    patched[hook_offset : hook_offset + 4] = encode_thumb_b_w(
        hook_offset, ENGINE_MONITOR_ENABLE_OFFSET
    )
    return bytes(patched)


def build_flash_image(
    stock: bytes,
    picker_itcm: bytes,
    xip_runtime: bytes,
    engine_monitor: bytes,
    patched_engines: frozenset[str],
) -> bytes:
    if len(stock) != 0x800000:
        raise BuildError(f"stock dump is {len(stock):#x}, expected 0x800000")
    if len(picker_itcm) > XIP_STORAGE_OFFSET:
        raise BuildError("ITCM picker overlaps XIP runtime storage")
    if XIP_STORAGE_OFFSET + len(xip_runtime) > ENGINE_COPY_SIZE:
        raise BuildError("XIP runtime exceeds the copied picker image")

    image = bytearray(stock[FLASH_BASE:IMAGE_END])

    picker = bytearray(SLOT_SIZE)
    picker[: len(picker_itcm)] = picker_itcm
    picker[
        XIP_STORAGE_OFFSET : XIP_STORAGE_OFFSET + len(xip_runtime)
    ] = xip_runtime
    picker_relative = PICKER_FLASH_OFFSET - FLASH_BASE
    image[picker_relative : picker_relative + SLOT_SIZE] = picker

    known_engines = {name for name, _source, _target in ENGINE_LAYOUT}
    unknown_engines = patched_engines - known_engines
    if unknown_engines:
        raise BuildError(
            f"unknown engine patch names: {sorted(unknown_engines)}"
        )

    for name, source_offset, target_offset in ENGINE_LAYOUT:
        engine = stock[source_offset : source_offset + SLOT_SIZE]
        if name in patched_engines:
            engine = patch_engine(name, engine, engine_monitor)
        target_relative = target_offset - FLASH_BASE
        image[target_relative : target_relative + SLOT_SIZE] = engine

    return bytes(image)


def validate_image(
    stock: bytes, image: bytes, patched_engines: frozenset[str]
) -> None:
    if len(image) != IMAGE_END - FLASH_BASE:
        raise BuildError("generated image has the wrong length")

    def generated(offset: int, length: int) -> bytes:
        relative = offset - FLASH_BASE
        return image[relative : relative + length]

    for name, stock_offset, _generated_offset in ENGINE_LAYOUT:
        original = stock[stock_offset : stock_offset + SLOT_SIZE]
        reset, runtime, expected_last_nonzero = EXPECTED_STARTUP[name]
        if struct.unpack_from("<I", original, 4)[0] != reset:
            raise BuildError(f"{name} reset vector changed from audit")
        if struct.unpack_from("<I", original, (reset & ~1) + 0x24)[0] != runtime:
            raise BuildError(f"{name} runtime entry changed from audit")
        runtime_head = original[runtime & ~1 : (runtime & ~1) + 4]
        if runtime_head != bytes.fromhex("aff30080"):
            raise BuildError(f"{name} runtime entry is not the audited __main")
        last_nonzero = max(
            index
            for index, value in enumerate(original[:ENGINE_COPY_SIZE])
            if value
        )
        if last_nonzero != expected_last_nonzero:
            raise BuildError(f"{name} executable tail changed from audit")
        if last_nonzero >= ENGINE_MONITOR_OFFSET:
            raise BuildError(f"{name} uses the monitor code cave")

    for name, _stock_offset, generated_offset in ENGINE_LAYOUT:
        vector = struct.unpack_from(
            "<I",
            generated(generated_offset, SLOT_SIZE),
            GPT1_VECTOR_OFFSET,
        )[0]
        if name in patched_engines and vector != ENGINE_MONITOR_ADDRESS:
            raise BuildError(f"{name} GPT1 vector was not patched")

    for name, stock_offset, generated_offset in ENGINE_LAYOUT:
        original = stock[stock_offset : stock_offset + SLOT_SIZE]
        restored = bytearray(generated(generated_offset, SLOT_SIZE))
        if name in patched_engines:
            struct.pack_into(
                "<I",
                restored,
                GPT1_VECTOR_OFFSET,
                struct.unpack_from("<I", original, GPT1_VECTOR_OFFSET)[0],
            )
            restored[
                ENGINE_MONITOR_OFFSET : ENGINE_MONITOR_OFFSET + 0x100
            ] = original[
                ENGINE_MONITOR_OFFSET : ENGINE_MONITOR_OFFSET + 0x100
            ]
            hook_offset = GPT1_START_HOOKS[name]
            restored[
                hook_offset : hook_offset + len(EXPECTED_GPT1_START)
            ] = original[
                hook_offset : hook_offset + len(EXPECTED_GPT1_START)
            ]
        if bytes(restored) != original:
            raise BuildError(
                f"{name} differs beyond the vector and monitor cave"
            )


def write_new(path: Path, data: bytes) -> None:
    if path.exists():
        raise FileExistsError(f"refusing to overwrite existing file: {path}")
    path.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stock", type=Path, default=ROOT / "dump1.bin"
    )
    parser.add_argument(
        "--output-dir", type=Path, default=SOURCE_DIR / "build"
    )
    parser.add_argument(
        "--allow-stock-sha256",
        help="override the known stock SHA-256 (requires an exact value)",
    )
    args = parser.parse_args()

    stock = args.stock.read_bytes()
    actual_stock_sha = hashlib.sha256(stock).hexdigest()
    expected_stock_sha = (
        args.allow_stock_sha256 or EXPECTED_STOCK_SHA256
    ).lower()
    if actual_stock_sha != expected_stock_sha:
        raise BuildError(
            f"stock SHA-256 is {actual_stock_sha}, expected "
            f"{expected_stock_sha}"
        )

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=False)
    picker_itcm, xip_runtime, engine_monitor = build_assembly(output_dir)
    variants = (
        (
            "picker-only",
            b"ZWPICK01",
            frozenset(),
        ),
        (
            "retention-test",
            b"ZWRET002",
            frozenset({"reverb"}),
        ),
        (
            "zero-wear-switch",
            b"ZW4FX002",
            frozenset(name for name, _source, _target in ENGINE_LAYOUT),
        ),
    )

    built_artifacts: list[tuple[Path, bytes]] = []
    variant_results: list[
        tuple[str, frozenset[str], Path, bytes, bytes]
    ] = []
    for stem, version, patched_engines in variants:
        image = build_flash_image(
            stock,
            picker_itcm,
            xip_runtime,
            engine_monitor,
            patched_engines,
        )
        validate_image(stock, image, patched_engines)
        bina = build_bina(image, version)
        info = parse_bina(bina)
        if info.image != image:
            raise BuildError(
                f"{stem} BINA round trip changed the flash image"
            )

        image_path = output_dir / f"{stem}-image.bin"
        bina_path = output_dir / f"{stem}.bina"
        write_new(image_path, image)
        write_new(bina_path, bina)
        built_artifacts.extend(((image_path, image), (bina_path, bina)))
        variant_results.append(
            (stem, patched_engines, bina_path, image, bina)
        )

    manifest_entries = [
        (output_dir / "picker_itcm.bin", picker_itcm),
        (output_dir / "xip_runtime.bin", xip_runtime),
        (output_dir / "engine_monitor.bin", engine_monitor),
        *built_artifacts,
    ]
    manifest = "\n".join(
        [
            *(f"{sha256(data)}  {path.name}" for path, data in manifest_entries),
            "",
        ]
    )
    (output_dir / "SHA256SUMS").write_text(manifest)

    print()
    print(f"stock SHA-256:    {actual_stock_sha}")
    print(
        f"flash mapping:    {FLASH_BASE:#x}..{IMAGE_END - 1:#x}"
    )
    print(
        f"picker:           {PICKER_FLASH_OFFSET:#x} "
        f"({len(picker_itcm):#x} ITCM bytes, "
        f"{len(xip_runtime):#x} XIP bytes)"
    )
    print(
        f"monitor:          ITCM {ENGINE_MONITOR_OFFSET:#x} "
        f"({len(engine_monitor):#x} bytes)"
    )
    print("engine cycle:     reverb -> modulation -> metal -> delay")
    print("patched engines:  delay, reverb, modulation, metal")
    print(
        f"reverb:           relocated {REVERB_STOCK_OFFSET:#x} -> "
        f"{REVERB_RELOCATED_OFFSET:#x}"
    )
    for stem, patched_engines, bina_path, _image, bina in variant_results:
        patched = ", ".join(sorted(patched_engines)) or "none"
        print(f"{stem + ':':18s} {bina_path}")
        print(f"  patched:        {patched}")
        print(f"  BINA SHA-256:   {sha256(bina)}")
    print("hardware writes:  none")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BuildError, OSError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)

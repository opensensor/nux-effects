#!/usr/bin/env python3
"""Verify an open application against the recovered factory engine-slot ABI."""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


ITCM_START = 0x00000000
ITCM_COPY_END = 0x0001E000
DTCM_START = 0x20000000
DTCM_END = 0x20020000
VECTOR_WORDS = 176
REQUIRED_SYMBOLS = {
    "DMA0_DMA16_IRQHandler",
    "Default_Handler",
    "Reset_Handler",
    "SystemInit",
    "__factory_slot_image_end",
    "__stack_top",
    "application_main",
    "g_factory_slot_vectors",
}


def run(tool: str, *arguments: str) -> str:
    return subprocess.run(
        [tool, *arguments],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout


def parse_symbols(output: str) -> dict[str, int]:
    symbols: dict[str, int] = {}
    pattern = re.compile(r"^([0-9a-fA-F]+)\s+[A-Za-z]\s+(\S+)$")
    for line in output.splitlines():
        match = pattern.match(line.strip())
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
    return symbols


def parse_load_segments(output: str) -> list[tuple[int, int, int, int]]:
    segments: list[tuple[int, int, int, int]] = []
    pattern = re.compile(
        r"^\s*LOAD\s+"
        r"0x[0-9a-fA-F]+\s+"
        r"0x([0-9a-fA-F]+)\s+"
        r"0x([0-9a-fA-F]+)\s+"
        r"0x([0-9a-fA-F]+)\s+"
        r"0x([0-9a-fA-F]+)"
    )
    for line in output.splitlines():
        match = pattern.match(line)
        if match:
            segments.append(
                tuple(int(value, 16) for value in match.groups())
            )
    return segments


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    arguments = parser.parse_args()

    nm = shutil.which("arm-none-eabi-nm")
    readelf = shutil.which("arm-none-eabi-readelf")
    objcopy = shutil.which("arm-none-eabi-objcopy")
    objdump = shutil.which("arm-none-eabi-objdump")
    if None in (nm, readelf, objcopy, objdump):
        parser.error("Arm GNU nm/readelf/objcopy/objdump are required")
    if not arguments.elf.is_file():
        parser.error(f"ELF does not exist: {arguments.elf}")

    symbols = parse_symbols(run(nm, "-n", str(arguments.elf)))
    missing = sorted(REQUIRED_SYMBOLS - symbols.keys())
    if missing:
        raise SystemExit(
            "factory-slot application is missing symbols: "
            + ", ".join(missing)
        )
    undefined = run(nm, "-u", str(arguments.elf)).strip()
    if undefined:
        raise SystemExit(
            "factory-slot application has undefined symbols:\n" + undefined
        )

    if symbols["g_factory_slot_vectors"] != ITCM_START:
        raise SystemExit("factory-slot vector table is not at ITCM address zero")
    if symbols["__stack_top"] != DTCM_END:
        raise SystemExit("factory-slot stack does not use the audited DTCM top")
    reset = symbols["Reset_Handler"] & ~1
    if not ITCM_START <= reset < ITCM_COPY_END:
        raise SystemExit("Reset_Handler is outside the factory ITCM copy")
    image_end = symbols["__factory_slot_image_end"]
    if not ITCM_START < image_end <= ITCM_COPY_END:
        raise SystemExit("factory-slot load image exceeds 0x1e000 bytes")

    header = run(readelf, "-h", str(arguments.elf))
    entry_match = re.search(
        r"Entry point address:\s+(0x[0-9a-fA-F]+)", header
    )
    if entry_match is None:
        raise SystemExit("could not read ELF entry point")
    if int(entry_match.group(1), 16) & ~1 != reset:
        raise SystemExit("ELF entry is not Reset_Handler")

    segments = parse_load_segments(
        run(readelf, "-W", "-l", str(arguments.elf))
    )
    if not segments:
        raise SystemExit("factory-slot ELF has no load segments")
    for virtual, physical, file_size, memory_size in segments:
        if file_size:
            if not (
                ITCM_START <= physical
                and physical + file_size <= ITCM_COPY_END
            ):
                raise SystemExit(
                    "file-backed segment is outside the factory ITCM copy: "
                    f"VMA 0x{virtual:08x}, LMA 0x{physical:08x}, "
                    f"size 0x{file_size:x}"
                )
        if memory_size and virtual >= DTCM_START:
            if virtual + memory_size > DTCM_END:
                raise SystemExit("factory-slot DTCM segment exceeds DTCM")

    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        vector_path = temporary / "vectors.bin"
        binary_path = temporary / "factory-slot.bin"
        subprocess.run(
            [
                objcopy,
                "--dump-section",
                f".isr_vector={vector_path}",
                str(arguments.elf),
            ],
            check=True,
        )
        subprocess.run(
            [objcopy, "-O", "binary", str(arguments.elf), str(binary_path)],
            check=True,
        )
        vector_data = vector_path.read_bytes()
        binary_size = binary_path.stat().st_size
    if len(vector_data) != VECTOR_WORDS * 4:
        raise SystemExit(f"unexpected vector-table size {len(vector_data)}")
    vectors = struct.unpack(f"<{VECTOR_WORDS}I", vector_data)
    if vectors[0] != DTCM_END:
        raise SystemExit(f"unexpected initial stack 0x{vectors[0]:08x}")
    if (vectors[1] & ~1) != reset or not vectors[1] & 1:
        raise SystemExit("reset vector does not target Thumb Reset_Handler")
    dma_handler = symbols["DMA0_DMA16_IRQHandler"] & ~1
    if (vectors[16] & ~1) != dma_handler or not vectors[16] & 1:
        raise SystemExit(
            "external IRQ0 does not target Thumb DMA0_DMA16_IRQHandler"
        )
    if binary_size != image_end or binary_size > ITCM_COPY_END:
        raise SystemExit(
            f"raw image size 0x{binary_size:x} does not match "
            f"link end 0x{image_end:x}"
        )

    disassembly = run(
        objdump,
        "-d",
        "--disassemble=Reset_Handler",
        str(arguments.elf),
    )
    for instruction in ("cpsid", "msr", "dsb", "isb"):
        if instruction not in disassembly:
            raise SystemExit(
                f"Reset_Handler is missing required {instruction}"
            )
    if not re.search(r"\bbl(?:\.w)?\s+.*<SystemInit>", disassembly):
        raise SystemExit("Reset_Handler does not call pinned SystemInit")
    if not re.search(r"\bbl(?:\.w)?\s+.*<application_main>", disassembly):
        raise SystemExit("Reset_Handler does not enter application_main")

    print(
        "Factory-slot application verified: "
        f"ITCM load 0x{binary_size:x}/0x{ITCM_COPY_END:x}, "
        "vectors at 0, DTCM stack, pinned SystemInit"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

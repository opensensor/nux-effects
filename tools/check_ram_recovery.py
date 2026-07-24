#!/usr/bin/env python3
"""Fail closed unless the whole-flash recovery ELF is RAM-resident."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


SDRAM_START = 0x80000000
SDRAM_END = 0x80200000
DTCM_START = 0x20000000
DTCM_END = 0x20020000
XIP_START = 0x60000000
XIP_END = 0x60800000


class CheckError(RuntimeError):
    """The linked updater could depend on flash while flash is unavailable."""


def run(*arguments: str) -> str:
    result = subprocess.run(
        list(arguments),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode:
        raise CheckError(result.stdout.strip())
    return result.stdout


def in_ram(address: int) -> bool:
    return (
        SDRAM_START <= address < SDRAM_END
        or DTCM_START <= address < DTCM_END
    )


def check_load_segments(readelf: str, elf: Path) -> None:
    output = run(readelf, "-W", "-l", str(elf))
    load_lines = [
        line.strip()
        for line in output.splitlines()
        if line.lstrip().startswith("LOAD")
    ]
    if not load_lines:
        raise CheckError("ELF has no loadable segments")
    for line in load_lines:
        fields = line.split()
        if len(fields) < 4:
            raise CheckError(f"cannot parse LOAD segment: {line}")
        virtual = int(fields[2], 16)
        physical = int(fields[3], 16)
        if not in_ram(virtual) or not in_ram(physical):
            raise CheckError(
                "RAM recovery has a non-RAM load segment: " + line
            )


def check_symbols(nm: str, elf: Path) -> None:
    output = run(nm, "-n", str(elf))
    required = {
        "Reset_Handler",
        "g_ram_recovery_vectors",
        "ram_recovery_main",
        "ncr2_recovery_usb_service",
        "ncr2_flexspi_nor_init_full_flash",
    }
    found: dict[str, int] = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            try:
                address = int(fields[0], 16)
            except ValueError:
                continue
            found[fields[2]] = address
    missing = sorted(required - found.keys())
    if missing:
        raise CheckError(
            "RAM recovery is missing symbols: " + ", ".join(missing)
        )
    for name in sorted(required):
        if not in_ram(found[name]):
            raise CheckError(
                f"{name} is outside RAM at 0x{found[name]:08x}"
            )


def check_direct_branches(objdump: str, elf: Path) -> None:
    output = run(objdump, "-d", str(elf))
    branch = re.compile(
        r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4}\s+)+"
        r"(?:b(?:l|lx)?(?:\.[a-z]+)?|cbz|cbnz)\s+"
        r"([0-9a-f]+)\b",
        re.IGNORECASE,
    )
    for line in output.splitlines():
        match = branch.match(line)
        if match is None:
            continue
        target = int(match.group(1), 16)
        if XIP_START <= target < XIP_END:
            raise CheckError(
                f"RAM recovery branches into XIP flash: {line.strip()}"
            )


def check_binary_size(elf: Path, binary: Path) -> None:
    if not binary.is_file():
        raise CheckError(f"RAM recovery binary is missing: {binary}")
    size = binary.stat().st_size
    if size < 8:
        raise CheckError("RAM recovery binary has no vector table")
    if size > SDRAM_END - SDRAM_START:
        raise CheckError(
            f"RAM recovery binary is {size:#x}, exceeds SDRAM budget"
        )
    vectors = binary.read_bytes()[:8]
    stack = int.from_bytes(vectors[:4], "little")
    reset = int.from_bytes(vectors[4:8], "little")
    if not DTCM_START <= stack <= DTCM_END:
        raise CheckError(f"initial stack is outside DTCM: {stack:#x}")
    if (reset & 1) == 0 or not SDRAM_START <= (reset & ~1) < SDRAM_END:
        raise CheckError(f"reset vector is outside SDRAM: {reset:#x}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--prefix", default="arm-none-eabi-")
    arguments = parser.parse_args()

    try:
        check_load_segments(arguments.prefix + "readelf", arguments.elf)
        check_symbols(arguments.prefix + "nm", arguments.elf)
        check_direct_branches(arguments.prefix + "objdump", arguments.elf)
        check_binary_size(arguments.elf, arguments.binary)
    except (OSError, CheckError) as error:
        parser.error(str(error))
    print(
        f"RAM recovery verified: {arguments.elf} "
        f"({arguments.binary.stat().st_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

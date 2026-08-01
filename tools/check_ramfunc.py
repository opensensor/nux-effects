#!/usr/bin/env python3
"""Verify that the complete FlexSPI mutation call graph is linked in ITCM."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


ITCM_START = 0x00000000
ITCM_END = 0x00020000
FLASH_START = 0x60002000
FLASH_END = 0x60020000
REQUIRED_SYMBOLS = {
    "FLEXSPI_UpdateLUT",
    "ram_erase_sector",
    "ram_program_page",
    "ram_rom_initialize",
    "ram_rom_program_page_call",
    "ram_software_reset",
    "ram_transfer",
    "ram_wait_until_flash_idle",
}
ROM_INDIRECT_CALLERS = {
    "ram_rom_initialize",
    "ram_rom_program_page_call",
}


class CheckError(RuntimeError):
    pass


def run(command: list[str]) -> str:
    try:
        return subprocess.run(
            command,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise CheckError(str(error)) from error


def section_bounds(objdump: str, elf: Path) -> tuple[int, int, int]:
    output = run([objdump, "-h", str(elf)])
    match = re.search(
        r"^\s*\d+\s+\.ramfunc\s+"
        r"([0-9a-fA-F]+)\s+"
        r"([0-9a-fA-F]+)\s+"
        r"([0-9a-fA-F]+)\s+",
        output,
        re.MULTILINE,
    )
    if match is None:
        raise CheckError(".ramfunc section is missing")
    size, virtual, load = (int(value, 16) for value in match.groups())
    if size == 0:
        raise CheckError(".ramfunc section is empty")
    if virtual < ITCM_START or virtual + size > ITCM_END:
        raise CheckError(".ramfunc is outside the NCR-2 ITCM window")
    if load < FLASH_START or load + size > FLASH_END:
        raise CheckError(".ramfunc load image is outside the bootloader")
    return virtual, virtual + size, load


def symbol_addresses(nm: str, elf: Path) -> dict[str, int]:
    output = run([nm, "-n", str(elf)])
    symbols: dict[str, int] = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 3 and re.fullmatch(
            r"[0-9a-fA-F]+", fields[0]
        ):
            symbols[fields[-1]] = int(fields[0], 16)
    return symbols


def requirement_matches(
    symbols: dict[str, int], requirement: str
) -> list[tuple[str, int]]:
    return [
        (name, address)
        for name, address in symbols.items()
        if name == requirement or name.startswith(requirement + ".")
    ]


def canonical_function(name: str | None) -> str | None:
    if name is None:
        return None
    return name.split(".", 1)[0]


def check_direct_calls(
    objdump: str,
    elf: Path,
    section_start: int,
    section_end: int,
) -> None:
    output = run(
        [objdump, "-d", "--section=.ramfunc", str(elf)]
    )
    current_function: str | None = None
    rom_callers_seen: set[str] = set()
    for line in output.splitlines():
        label = re.search(r"<([^>]+)>:$", line)
        if label is not None:
            current_function = label.group(1)
        match = re.search(
            r"\bblx?\s+([0-9a-fA-F]+)(?:\s|$)",
            line,
        )
        if match is not None:
            destination = int(match.group(1), 16)
            if not section_start <= destination < section_end:
                raise CheckError(
                    "RAM function branches outside .ramfunc: "
                    f"{line.strip()}"
                )
        elif re.search(r"\bblx?\s+r(?:1[0-5]|[0-9])\b", line):
            caller = canonical_function(current_function)
            if caller not in ROM_INDIRECT_CALLERS:
                raise CheckError(
                    "RAM function contains an unchecked indirect call: "
                    f"{line.strip()}"
                )
            rom_callers_seen.add(caller)
    missing_rom_calls = ROM_INDIRECT_CALLERS - rom_callers_seen
    if missing_rom_calls:
        raise CheckError(
            "expected checked boot-ROM calls are missing from: "
            + ", ".join(sorted(missing_rom_calls))
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument(
        "--objdump",
        default="arm-none-eabi-objdump",
    )
    parser.add_argument(
        "--nm",
        default="arm-none-eabi-nm",
    )
    args = parser.parse_args()

    elf = args.elf.expanduser().resolve()
    try:
        if not elf.is_file():
            raise CheckError(f"ELF does not exist: {elf}")
        section_start, section_end, load = section_bounds(
            args.objdump, elf
        )
        symbols = symbol_addresses(args.nm, elf)
        missing = {
            name
            for name in REQUIRED_SYMBOLS
            if not requirement_matches(symbols, name)
        }
        if missing:
            raise CheckError(
                "required RAM symbols are missing: "
                + ", ".join(sorted(missing))
            )
        for name in sorted(REQUIRED_SYMBOLS):
            for resolved_name, address in requirement_matches(
                symbols, name
            ):
                if not section_start <= address < section_end:
                    raise CheckError(
                        f"{resolved_name} is outside .ramfunc at "
                        f"0x{address:08x}"
                    )
        check_direct_calls(
            args.objdump,
            elf,
            section_start,
            section_end,
        )
    except CheckError as error:
        parser.error(str(error))
    print(
        "RAM call graph verified: "
        f"VMA 0x{section_start:08x}-0x{section_end:08x}, "
        f"LMA 0x{load:08x}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Verify that the RT1051 integration probe is complete and non-bootable."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
from pathlib import Path


REQUIRED_SYMBOLS = {
    "USB_OTG1_IRQHandler",
    "boot_handoff_prepare",
    "ncr2_board_make_recovery_request",
    "ncr2_board_recovery_input_init",
    "ncr2_board_recovery_requested",
    "ncr2_board_usb_clock_init",
    "ncr2_board_usb_irq_enable",
    "ncr2_board_warm_reset",
    "ncr2_board_watchdog_refresh",
    "ncr2_board_watchdog_reset_status",
    "ncr2_board_watchdog_start_trial",
    "ncr2_flexspi_nor_init",
    "ncr2_recovery_usb_start",
}


def run(tool: str, *arguments: str) -> str:
    completed = subprocess.run(
        [tool, *arguments],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return completed.stdout


def parse_symbols(nm_output: str) -> dict[str, int]:
    symbols: dict[str, int] = {}
    pattern = re.compile(
        r"^([0-9a-fA-F]+)\s+[A-Za-z]\s+(\S+)$"
    )
    for line in nm_output.splitlines():
        match = pattern.match(line.strip())
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
    return symbols


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    arguments = parser.parse_args()

    nm = shutil.which("arm-none-eabi-nm")
    readelf = shutil.which("arm-none-eabi-readelf")
    if nm is None or readelf is None:
        parser.error("arm-none-eabi-nm/readelf are required")
    if not arguments.elf.is_file():
        parser.error(f"ELF does not exist: {arguments.elf}")

    symbols = parse_symbols(run(nm, "-n", str(arguments.elf)))
    missing = sorted(REQUIRED_SYMBOLS - symbols.keys())
    if missing:
        raise SystemExit(
            "integration probe is missing symbols: "
            + ", ".join(missing)
        )

    header = run(readelf, "-h", str(arguments.elf))
    entry_match = re.search(
        r"Entry point address:\s+(0x[0-9a-fA-F]+)",
        header,
    )
    if entry_match is None:
        raise SystemExit("could not read ELF entry point")
    entry = int(entry_match.group(1), 16) & ~1
    expected = (
        symbols["ncr2_board_recovery_input_init"] & ~1
    )
    if entry != expected:
        raise SystemExit(
            f"unexpected probe entry 0x{entry:08x}; "
            f"expected 0x{expected:08x}"
        )

    sections = run(readelf, "-S", str(arguments.elf))
    if ".isr_vector" in sections:
        raise SystemExit(
            "integration probe unexpectedly contains a vector table"
        )
    if "Reset_Handler" in symbols:
        raise SystemExit(
            "integration probe unexpectedly contains Reset_Handler"
        )

    print(
        "Hardware integration probe verified: complete symbols, "
        "no vector table, non-reset entry"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

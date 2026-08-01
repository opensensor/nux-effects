#!/usr/bin/env python3
"""Fail closed unless the source-native NCR-2 audio app is self-contained."""

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
DMA0_DMA16_VECTOR_OFFSET = 16 * 4

# Enforced for every hardware application regardless of what it exercises.
COMMON_SYMBOLS = {
    "Reset_Handler",
    "DMA0_DMA16_IRQHandler",
    "application_main",
}

# Each profile additionally proves that the build contains the code path it
# claims to test, so a silently gutted image cannot pass the audit.
PROFILE_SYMBOLS = {
    "audio": {
        "ncr2_factory_engine_copy_to_itcm",
        "ncr2_factory_engine_launch",
        "ncr2_factory_engine_sync_and_jump",
        "ncr2_factory_return_monitor",
        "ncr2_factory_return_monitor_start",
        "ncr2_factory_return_monitor_original_led",
        "ncr2_factory_return_monitor_end",
        "ncr2_factory_audio_process_block",
        "ncr2_factory_board_release_audio",
    },
    "bringup": {
        "ncr2_factory_board_pulse_candidate",
        "ncr2_factory_board_restore_idle",
        "ncr2_factory_board_delay_ms",
    },
}


class CheckError(RuntimeError):
    """The linked hardware application violates its execution contract."""


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


def symbols(nm: str, elf: Path) -> dict[str, int]:
    found: dict[str, int] = {}
    for line in run(nm, "-n", str(elf)).splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        try:
            found[fields[2]] = int(fields[0], 16)
        except ValueError:
            continue
    return found


def check_segments(readelf: str, elf: Path) -> None:
    loads = [
        line.split()
        for line in run(readelf, "-W", "-l", str(elf)).splitlines()
        if line.lstrip().startswith("LOAD")
    ]
    if not loads:
        raise CheckError("ELF has no loadable segments")
    for fields in loads:
        if len(fields) < 4:
            raise CheckError("cannot parse LOAD segment")
        virtual = int(fields[2], 16)
        physical = int(fields[3], 16)
        if not in_ram(virtual) or not in_ram(physical):
            raise CheckError(
                "hardware app has a non-RAM load segment: " +
                " ".join(fields)
            )


def check_branches(objdump: str, elf: Path) -> None:
    branch = re.compile(
        r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4}\s+)+"
        r"(?:b(?:l|lx)?(?:\.[a-z]+)?|cbz|cbnz)\s+"
        r"([0-9a-f]+)\b",
        re.IGNORECASE,
    )
    for line in run(objdump, "-d", str(elf)).splitlines():
        match = branch.match(line)
        if match is None:
            continue
        target = int(match.group(1), 16)
        if XIP_START <= target < XIP_END:
            raise CheckError(
                f"hardware app branches into XIP flash: {line.strip()}"
            )


def check_vectors(binary: Path, found: dict[str, int]) -> None:
    image = binary.read_bytes()
    if len(image) <= DMA0_DMA16_VECTOR_OFFSET + 4:
        raise CheckError("binary does not contain the DMA vector")
    stack = int.from_bytes(image[0:4], "little")
    reset = int.from_bytes(image[4:8], "little")
    dma = int.from_bytes(
        image[
            DMA0_DMA16_VECTOR_OFFSET:
            DMA0_DMA16_VECTOR_OFFSET + 4
        ],
        "little",
    )
    if not DTCM_START <= stack <= DTCM_END:
        raise CheckError(f"initial stack is outside DTCM: {stack:#x}")
    if (
        (reset & 1) == 0
        or not SDRAM_START <= (reset & ~1) < SDRAM_END
    ):
        raise CheckError(f"reset vector is outside SDRAM: {reset:#x}")
    expected_dma = found["DMA0_DMA16_IRQHandler"] | 1
    if dma != expected_dma:
        raise CheckError(
            f"DMA vector is {dma:#x}, expected {expected_dma:#x}"
        )


def check_factory_monitor(binary: Path, found: dict[str, int]) -> None:
    start = found["ncr2_factory_return_monitor_start"]
    original_led = found["ncr2_factory_return_monitor_original_led"]
    end = found["ncr2_factory_return_monitor_end"]

    if original_led - start != 0xFC:
        raise CheckError(
            "factory LED tail-call entry is not at code-cave offset 0xfc"
        )
    if not start < end or end - start != 0x100:
        raise CheckError(
            "factory return monitor does not exactly fill its 256-byte cave"
        )
    image = binary.read_bytes()
    first = start - SDRAM_START
    last = end - SDRAM_START
    if first < 0 or last > len(image):
        raise CheckError("factory return monitor is outside the app binary")
    monitor = image[first:last]
    required_words = {
        0x401B8008,  # GPIO1 input PSR
        0x403B002C,  # ADC_ETC trigger 0, chain 2 selector result
        0x400F8038,  # SRC GPR7 hold counter
        0x400F8044,  # SRC GPR10 return latch
        0x46414330,  # factory request magic
        0xE000ED0C,  # AIRCR warm reset
        0x05FA0004,  # reset key and SYSRESETREQ
    }
    monitor_words = {
        int.from_bytes(monitor[index:index + 4], "little")
        for index in range(0, len(monitor) - 3, 4)
    }
    missing = sorted(required_words - monitor_words)
    if missing:
        raise CheckError(
            "factory return monitor is missing pinned words: "
            + ", ".join(f"{word:#010x}" for word in missing)
        )
    forbidden_words = {
        0x401B8000,  # GPIO1 DR output latch, not an input sampler
        0x400F803C,  # recovery mailbox GPR8
        0x400F8040,  # recovery mailbox GPR9
    }
    present = sorted(forbidden_words & monitor_words)
    if present:
        raise CheckError(
            "factory return monitor contains forbidden words: "
            + ", ".join(f"{word:#010x}" for word in present)
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--prefix", default="arm-none-eabi-")
    parser.add_argument(
        "--profile",
        choices=sorted(PROFILE_SYMBOLS),
        default="audio",
        help=(
            "which application contract to enforce; 'bringup' builds drive "
            "the board sweep and deliberately never start SAI or eDMA"
        ),
    )
    arguments = parser.parse_args()

    try:
        check_segments(arguments.prefix + "readelf", arguments.elf)
        found = symbols(arguments.prefix + "nm", arguments.elf)
        required = COMMON_SYMBOLS | PROFILE_SYMBOLS[arguments.profile]
        missing = sorted(required - found.keys())
        if missing:
            raise CheckError(
                "hardware app is missing symbols: " +
                ", ".join(missing)
            )
        for name in required:
            if not in_ram(found[name]):
                raise CheckError(
                    f"{name} is outside RAM at {found[name]:#x}"
                )
        check_branches(arguments.prefix + "objdump", arguments.elf)
        check_vectors(arguments.binary, found)
        if arguments.profile == "audio":
            check_factory_monitor(arguments.binary, found)
    except (OSError, CheckError) as error:
        parser.error(str(error))

    print(
        f"Hardware audio app verified: {arguments.elf} "
        f"({arguments.binary.stat().st_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

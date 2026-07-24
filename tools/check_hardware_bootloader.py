#!/usr/bin/env python3
"""Verify the opt-in RT1051 hardware bootloader's structural safety."""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


FLASH_START = 0x60002000
FLASH_END = 0x60020000
DTCM_START = 0x20000000
DTCM_TOP = 0x20020000
USB_OTG1_VECTOR_INDEX = 16 + 113

REQUIRED_SYMBOLS = {
    "Reset_Handler",
    "SystemInit",
    "USB_OTG1_IRQHandler",
    "bootloader_main",
    "bootloader_run",
    "g_boot_vectors",
    "ncr2_board_make_recovery_request",
    "ncr2_board_recovery_input_init",
    "ncr2_board_recovery_requested",
    "ncr2_board_warm_reset",
    "ncr2_board_watchdog_start_trial",
    "ncr2_flexspi_nor_init",
    "recovery_storage_init",
}

XIP_RECOVERY_SYMBOLS = {
    "ncr2_board_usb_clock_init",
    "ncr2_board_usb_irq_enable",
    "ncr2_recovery_usb_start",
    "recovery_engine_init",
}

EMBEDDED_RECOVERY_SYMBOLS = {
    "__ram_recovery_blob_start",
    "__ram_recovery_blob_end",
    "launch_embedded_ram_recovery",
}

USB_STACK_SYMBOLS = {
    "USB_DeviceClassInit",
    "USB_DeviceEhciIsrFunction",
    "USB_DeviceHidRecv",
    "USB_DeviceHidSend",
    "USB_DeviceRun",
}

USB_DMA_SYMBOL_ALIGNMENTS = {
    "g_configuration_descriptor": 4,
    "g_device_descriptor": 4,
    "g_in_packet": 4,
    "g_out_packet": 4,
    "g_report_descriptor": 4,
    "qh_buffer": 2048,
    "s_UsbDeviceEhciDtd": 32,
    "s_UsbDeviceSetupBuffer": 4,
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


def section_bounds(
    readelf_output: str, section: str
) -> tuple[int, int]:
    pattern = re.compile(
        rf"\[\s*\d+\]\s+{re.escape(section)}\s+\S+\s+"
        r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+"
        r"([0-9a-fA-F]+)"
    )
    match = pattern.search(readelf_output)
    if match is None:
        raise SystemExit(f"required section is missing: {section}")
    address = int(match.group(1), 16)
    size = int(match.group(2), 16)
    return address, address + size


def verify_flash_load_segments(readelf: str, elf: Path) -> None:
    program_headers = run(readelf, "-lW", str(elf))
    pattern = re.compile(
        r"^\s*LOAD\s+"
        r"0x[0-9a-fA-F]+\s+"
        r"0x[0-9a-fA-F]+\s+"
        r"(0x[0-9a-fA-F]+)\s+"
        r"(0x[0-9a-fA-F]+)\s+",
        re.MULTILINE,
    )
    load_ranges: list[tuple[int, int]] = []
    for match in pattern.finditer(program_headers):
        physical = int(match.group(1), 16)
        file_size = int(match.group(2), 16)
        if file_size == 0:
            continue
        end = physical + file_size
        if physical < FLASH_START or end > FLASH_END:
            raise SystemExit(
                "load segment escapes protected boot partition: "
                f"0x{physical:08x}-0x{end:08x}"
            )
        load_ranges.append((physical, end))
    if not load_ranges:
        raise SystemExit("hardware bootloader has no flash load segments")

    load_ranges.sort()
    for previous, current in zip(load_ranges, load_ranges[1:]):
        if current[0] < previous[1]:
            raise SystemExit(
                "overlapping flash load segments: "
                f"0x{previous[0]:08x}-0x{previous[1]:08x} and "
                f"0x{current[0]:08x}-0x{current[1]:08x}"
            )


def read_vectors(objcopy: str, elf: Path) -> tuple[int, ...]:
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "vectors.bin"
        subprocess.run(
            [
                objcopy,
                "--dump-section",
                f".isr_vector={output}",
                str(elf),
            ],
            check=True,
        )
        data = output.read_bytes()
    expected_size = (16 + 160) * 4
    if len(data) != expected_size:
        raise SystemExit(
            f"unexpected vector size {len(data)}; "
            f"expected {expected_size}"
        )
    return struct.unpack(f"<{len(data) // 4}I", data)


def reset_installs_vtor(
    objcopy: str,
    objdump: str,
    readelf_output: str,
    symbols: dict[str, int],
    elf: Path,
) -> None:
    disassembly = run(
        objdump,
        "-d",
        "--disassemble=Reset_Handler",
        str(elf),
    )
    if not re.search(
        r"\bstr(?:\.w)?\s+r\d+,\s*\[r\d+",
        disassembly,
    ):
        raise SystemExit("Reset_Handler has no VTOR store")
    if "dsb" not in disassembly or "isb" not in disassembly:
        raise SystemExit("Reset_Handler lacks VTOR barriers")
    if not re.search(r"\bbl(?:\.w)?\s+.*<SystemInit>", disassembly):
        raise SystemExit("Reset_Handler does not call RT1051 SystemInit")

    text_start, text_end = section_bounds(
        readelf_output, ".text"
    )
    reset = symbols["Reset_Handler"] & ~1
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "text.bin"
        subprocess.run(
            [
                objcopy,
                "--dump-section",
                f".text={output}",
                str(elf),
            ],
            check=True,
        )
        text = output.read_bytes()
    offset = reset - text_start
    window_end = min(offset + 128, text_end - text_start)
    window = text[offset:window_end]
    if struct.pack("<I", 0xE000ED08) not in window:
        raise SystemExit("Reset_Handler lacks the SCB->VTOR address")
    vector = symbols["g_boot_vectors"]
    if struct.pack("<I", vector) not in window:
        raise SystemExit("Reset_Handler lacks the vector-table address")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument(
        "--write-enabled",
        action="store_true",
        help="expect the explicitly write-capable build marker",
    )
    parser.add_argument(
        "--expect-usb-stack",
        action="store_true",
        help="require the nonzero-ID USB stack to survive link GC",
    )
    parser.add_argument(
        "--expect-embedded-ram-recovery",
        action="store_true",
        help="require the hardware bootloader to contain the RAM updater",
    )
    parser.add_argument(
        "--ram-recovery-bin",
        type=Path,
        help="require the embedded blob to equal this checked RAM image",
    )
    arguments = parser.parse_args()

    nm = shutil.which("arm-none-eabi-nm")
    readelf = shutil.which("arm-none-eabi-readelf")
    objcopy = shutil.which("arm-none-eabi-objcopy")
    objdump = shutil.which("arm-none-eabi-objdump")
    if (
        nm is None
        or readelf is None
        or objcopy is None
        or objdump is None
    ):
        parser.error("Arm GNU nm/readelf/objcopy/objdump are required")
    if not arguments.elf.is_file():
        parser.error(f"ELF does not exist: {arguments.elf}")
    if (
        arguments.expect_usb_stack
        and arguments.expect_embedded_ram_recovery
    ):
        parser.error(
            "XIP USB stack and embedded RAM recovery expectations conflict"
        )
    if (
        arguments.ram_recovery_bin is not None
        and not arguments.expect_embedded_ram_recovery
    ):
        parser.error(
            "--ram-recovery-bin requires "
            "--expect-embedded-ram-recovery"
        )

    nm_output = run(nm, "-n", str(arguments.elf))
    symbols = parse_symbols(nm_output)
    expected_symbols = set(REQUIRED_SYMBOLS)
    if arguments.expect_embedded_ram_recovery:
        expected_symbols |= EMBEDDED_RECOVERY_SYMBOLS
    else:
        expected_symbols |= XIP_RECOVERY_SYMBOLS
    missing = sorted(expected_symbols - symbols.keys())
    if missing:
        raise SystemExit(
            "hardware bootloader is missing symbols: "
            + ", ".join(missing)
        )
    undefined = run(nm, "-u", str(arguments.elf)).strip()
    if undefined:
        raise SystemExit(
            "hardware bootloader has undefined symbols:\n"
            + undefined
        )

    expected_marker = (
        "ncr2_hardware_recovery_write_enabled"
        if arguments.write_enabled
        else "ncr2_hardware_recovery_readonly"
    )
    rejected_marker = (
        "ncr2_hardware_recovery_readonly"
        if arguments.write_enabled
        else "ncr2_hardware_recovery_write_enabled"
    )
    if expected_marker not in symbols:
        raise SystemExit(
            f"missing build-capability marker: {expected_marker}"
        )
    if rejected_marker in symbols:
        raise SystemExit(
            f"conflicting build-capability marker: {rejected_marker}"
        )

    header = run(readelf, "-h", str(arguments.elf))
    entry_match = re.search(
        r"Entry point address:\s+(0x[0-9a-fA-F]+)",
        header,
    )
    if entry_match is None:
        raise SystemExit("could not read ELF entry point")
    entry = int(entry_match.group(1), 16) & ~1
    reset = symbols["Reset_Handler"] & ~1
    if entry != reset:
        raise SystemExit(
            f"entry 0x{entry:08x} is not Reset_Handler "
            f"0x{reset:08x}"
        )

    sections = run(readelf, "-SW", str(arguments.elf))
    if ".isr_vector" not in sections:
        raise SystemExit("hardware bootloader has no vector table")
    if section_bounds(sections, ".text")[1] > FLASH_END:
        raise SystemExit("hardware bootloader exceeds protected flash")
    verify_flash_load_segments(readelf, arguments.elf)

    vectors = read_vectors(objcopy, arguments.elf)
    if vectors[0] != DTCM_TOP:
        raise SystemExit(
            f"unexpected initial stack 0x{vectors[0]:08x}"
        )
    if (vectors[1] & ~1) != reset or (vectors[1] & 1) == 0:
        raise SystemExit("reset vector does not target Thumb Reset_Handler")
    reset_installs_vtor(
        objcopy,
        objdump,
        sections,
        symbols,
        arguments.elf,
    )
    usb_handler = symbols["USB_OTG1_IRQHandler"] & ~1
    usb_vector = vectors[USB_OTG1_VECTOR_INDEX]
    if (usb_vector & ~1) != usb_handler or (usb_vector & 1) == 0:
        raise SystemExit("USB OTG1 vector does not target its handler")

    if arguments.expect_usb_stack:
        missing_usb = sorted(USB_STACK_SYMBOLS - symbols.keys())
        if missing_usb:
            raise SystemExit(
                "enumerating USB build is missing stack symbols: "
                + ", ".join(missing_usb)
            )
        missing_dma = sorted(
            USB_DMA_SYMBOL_ALIGNMENTS.keys() - symbols.keys()
        )
        if missing_dma:
            raise SystemExit(
                "enumerating USB build is missing DMA objects: "
                + ", ".join(missing_dma)
            )
        for symbol, alignment in USB_DMA_SYMBOL_ALIGNMENTS.items():
            address = symbols[symbol]
            if not DTCM_START <= address < DTCM_TOP:
                raise SystemExit(
                    f"USB DMA object {symbol} is outside DTCM: "
                    f"0x{address:08x}"
                )
            if address % alignment != 0:
                raise SystemExit(
                    f"USB DMA object {symbol} is not "
                    f"{alignment}-byte aligned: 0x{address:08x}"
                )

    if arguments.expect_embedded_ram_recovery:
        blob_start = symbols["__ram_recovery_blob_start"]
        blob_end = symbols["__ram_recovery_blob_end"]
        section_start, section_end = section_bounds(
            sections, ".ram_recovery_blob"
        )
        if (
            blob_start != section_start
            or blob_end != section_end
            or blob_end <= blob_start
        ):
            raise SystemExit(
                "embedded RAM recovery symbols do not cover its section"
            )
        if arguments.ram_recovery_bin is not None:
            if not arguments.ram_recovery_bin.is_file():
                parser.error(
                    "RAM recovery binary does not exist: "
                    f"{arguments.ram_recovery_bin}"
                )
            with tempfile.TemporaryDirectory() as directory:
                extracted = Path(directory) / "ram-recovery.bin"
                subprocess.run(
                    [
                        objcopy,
                        "--dump-section",
                        f".ram_recovery_blob={extracted}",
                        str(arguments.elf),
                    ],
                    check=True,
                )
                embedded = extracted.read_bytes()
            expected = arguments.ram_recovery_bin.read_bytes()
            if embedded != expected:
                raise SystemExit(
                    "embedded RAM recovery differs from checked binary"
                )

    mode = "write-enabled" if arguments.write_enabled else "read-only"
    if arguments.expect_embedded_ram_recovery:
        usb = "embedded-RAM recovery"
    elif arguments.expect_usb_stack:
        usb = "enumerating"
    else:
        usb = "ID-guarded"
    print(
        "Hardware bootloader verified: "
        f"bootable, {mode}, {usb}, complete vectors/symbols"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

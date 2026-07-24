#!/usr/bin/env python3
"""Verify the copyright-neutral factory Metal compatibility bridge."""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


SDRAM_START = 0x80000000
SDRAM_END = 0x80200000
DTCM_TOP = 0x20020000
VECTOR_WORDS = 176
EXPECTED_ABSOLUTE_SYMBOLS = {
    "ncr2_factory_engine_source": 0x600C0000,
    "ncr2_factory_engine_copy_size": 0x0001E000,
    "ncr2_factory_engine_stack": 0x20018000,
    "ncr2_factory_engine_reset": 0x0000E4B5,
}
BOOT_TRIAL_MAILBOX = 0x400F8028
SCB_AIRCR = 0xE000ED0C
SCB_ICIALLU = 0xE000EF50
AIRCR_SYSRESETREQ = 0x05FA0004
FACTORY_COMPAT_MPU_WORDS = {
    0xC0000010,
    0x03100039,
    0x80000011,
    0x0310003B,
    0x60000012,
    0x03030039,
    0x03030021,
    0x20000015,
    0x20200016,
    0x03030023,
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
    pattern = re.compile(
        r"^([0-9a-fA-F]+)\s+[A-Za-z]\s+(\S+)$"
    )
    for line in output.splitlines():
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
    objcopy = shutil.which("arm-none-eabi-objcopy")
    objdump = shutil.which("arm-none-eabi-objdump")
    if None in (nm, readelf, objcopy, objdump):
        parser.error("Arm GNU nm/readelf/objcopy/objdump are required")
    if not arguments.elf.is_file():
        parser.error(f"ELF does not exist: {arguments.elf}")

    symbols = parse_symbols(run(nm, "-n", str(arguments.elf)))
    required = {
        "Default_Handler",
        "Reset_Handler",
        "boot_trial_arm_confirmation",
        "g_factory_bridge_vectors",
        *EXPECTED_ABSOLUTE_SYMBOLS,
    }
    missing = sorted(required - symbols.keys())
    if missing:
        raise SystemExit(
            "factory bridge is missing symbols: "
            + ", ".join(missing)
        )
    undefined = run(nm, "-u", str(arguments.elf)).strip()
    if undefined:
        raise SystemExit(
            "factory bridge has undefined symbols:\n" + undefined
        )
    for symbol, expected in EXPECTED_ABSOLUTE_SYMBOLS.items():
        if symbols[symbol] != expected:
            raise SystemExit(
                f"{symbol} is 0x{symbols[symbol]:08x}, "
                f"expected 0x{expected:08x}"
            )

    header = run(readelf, "-h", str(arguments.elf))
    entry_match = re.search(
        r"Entry point address:\s+(0x[0-9a-fA-F]+)",
        header,
    )
    if entry_match is None:
        raise SystemExit("could not read ELF entry point")
    reset = symbols["Reset_Handler"] & ~1
    if int(entry_match.group(1), 16) & ~1 != reset:
        raise SystemExit("ELF entry is not Reset_Handler")
    if not SDRAM_START <= reset < SDRAM_END:
        raise SystemExit("Reset_Handler is outside the SDRAM application")

    with tempfile.TemporaryDirectory() as directory:
        vector_path = Path(directory) / "vectors.bin"
        subprocess.run(
            [
                objcopy,
                "--dump-section",
                f".isr_vector={vector_path}",
                str(arguments.elf),
            ],
            check=True,
        )
        vector_data = vector_path.read_bytes()
    if len(vector_data) != VECTOR_WORDS * 4:
        raise SystemExit(
            f"unexpected vector-table size {len(vector_data)}"
        )
    vectors = struct.unpack(f"<{VECTOR_WORDS}I", vector_data)
    if vectors[0] != DTCM_TOP:
        raise SystemExit(
            f"unexpected initial stack 0x{vectors[0]:08x}"
        )
    if (vectors[1] & ~1) != reset or not vectors[1] & 1:
        raise SystemExit("reset vector does not target Thumb Reset_Handler")

    disassembly = run(
        objdump,
        "-d",
        "--disassemble=Reset_Handler",
        str(arguments.elf),
    )
    required_instructions = ("cpsid", "dsb", "isb", "bx")
    missing_instructions = [
        item for item in required_instructions
        if item not in disassembly
    ]
    if missing_instructions:
        raise SystemExit(
            "factory bridge is missing handoff instructions: "
            + ", ".join(missing_instructions)
        )
    copy_match = re.search(
        r"<bridge_copy>:(.*?)"
        r"ldr\.w\s+r3,\s*\[r1\],\s*#4",
        disassembly,
        re.DOTALL,
    )
    if copy_match is None:
        raise SystemExit("factory bridge copy loop was not found")
    if re.search(
        r"\bldr\s+r1,\s*\[pc,",
        copy_match.group(1),
    ) is None:
        raise SystemExit(
            "factory bridge does not reload its caller-clobbered "
            "engine source before the ITCM copy"
        )

    with tempfile.TemporaryDirectory() as directory:
        binary_path = Path(directory) / "bridge.bin"
        subprocess.run(
            [objcopy, "-O", "binary", str(arguments.elf), str(binary_path)],
            check=True,
        )
        binary = binary_path.read_bytes()
    required_words = {
        "trial mailbox": BOOT_TRIAL_MAILBOX,
        "SCB AIRCR": SCB_AIRCR,
        "SCB ICIALLU": SCB_ICIALLU,
        "AIRCR reset request": AIRCR_SYSRESETREQ,
    }
    for name, value in required_words.items():
        if struct.pack("<I", value) not in binary:
            raise SystemExit(
                f"factory bridge lacks {name} constant 0x{value:08x}"
            )

    if "ncr2_factory_compat_prepare" in symbols:
        missing_mpu_words = sorted(
            value
            for value in FACTORY_COMPAT_MPU_WORDS
            if struct.pack("<I", value) not in binary
        )
        if missing_mpu_words:
            formatted = ", ".join(
                f"0x{value:08x}" for value in missing_mpu_words
            )
            raise SystemExit(
                "factory compatibility bridge lacks MPU words: "
                + formatted
            )
        if "CLOCK_InitArmPll" not in symbols:
            raise SystemExit(
                "factory compatibility bridge lacks ARM PLL setup"
            )

    print(
        "Factory bridge verified: Metal vectors, trial confirmation, "
        "post-call source reload, 0x1e000-byte ITCM copy, "
        "I-cache invalidation, SDRAM handoff"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

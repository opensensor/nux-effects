#!/usr/bin/env python3
"""Validate a source factory-slot image against the recovered audio contract.

This offline-only tool extracts the load image and symbols from an ELF, runs
its reset path in a narrow RT1051 Unicorn model, and stops when
`application_main` records the audio initialization result. It never opens a
USB device or creates a flash container.
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

try:
    from unicorn import (
        UC_ARCH_ARM,
        UC_HOOK_CODE,
        UC_HOOK_MEM_INVALID,
        UC_HOOK_MEM_WRITE,
        UC_MODE_MCLASS,
        UC_MODE_THUMB,
        Uc,
        UcError,
    )
    from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC
except ImportError as error:  # pragma: no cover - local optional tool
    raise SystemExit(
        "unicorn is required; install it in an isolated environment first"
    ) from error


ITCM_SIZE = 0x00080000
DTCM_SIZE = 0x00020000
FACTORY_COPY_LIMIT = 0x0001E000
AUDIO_OK = 0

EXPECTED_SAI = {
    0x0C: 0x00000010,
    0x10: 0x07000001,
    0x14: 0x00010000,
    0x18: 0x00031F1B,
    0x1C: 0x1F1F1F00,
    0x8C: 0x00000010,
    0x90: 0x47000001,
    0x94: 0x00010000,
    0x98: 0x00031F1B,
    0x9C: 0x1F1F1F00,
}


def _u16(emulator: Uc, address: int) -> int:
    return int.from_bytes(bytes(emulator.mem_read(address, 2)), "little")


def _u32(emulator: Uc, address: int) -> int:
    return int.from_bytes(bytes(emulator.mem_read(address, 4)), "little")


def _write_u32(emulator: Uc, address: int, value: int) -> None:
    emulator.mem_write(address, value.to_bytes(4, "little"))


def _tool(name: str) -> str:
    found = shutil.which(name)
    if found is None:
        raise RuntimeError(f"{name} is required")
    return found


def _run(*command: str) -> str:
    return subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout


def _symbols(elf: Path) -> dict[str, int]:
    output = _run(_tool("arm-none-eabi-nm"), "-n", str(elf))
    symbols: dict[str, int] = {}
    for line in output.splitlines():
        match = re.match(
            r"^([0-9a-fA-F]+)\s+[A-Za-z]\s+(\S+)$",
            line.strip(),
        )
        if match is not None:
            symbols[match.group(2)] = int(match.group(1), 16)
    return symbols


def _extract_binary(elf: Path) -> bytes:
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "factory-slot.bin"
        subprocess.run(
            [
                _tool("arm-none-eabi-objcopy"),
                "-O",
                "binary",
                str(elf),
                str(output),
            ],
            check=True,
        )
        return output.read_bytes()


def _build_emulator(image: bytes) -> Uc:
    emulator = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for address, size in (
        (0x00000000, ITCM_SIZE),
        (0x20000000, DTCM_SIZE),
        (0x20200000, 0x00100000),
        (0x40000000, 0x00400000),
        (0x60000000, 0x00800000),
        (0x80000000, 0x02000000),
        (0xE0000000, 0x00100000),
    ):
        emulator.mem_map(address, size)
    emulator.mem_write(0, image)

    for address in range(0x400D8000, 0x400D9010, 0x10):
        _write_u32(emulator, address, 0x80000000)
    _write_u32(emulator, 0xE000ED80, 0x001FE01B)
    _write_u32(emulator, 0xE000ED88, 0x00F00000)
    return emulator


def _require(
    condition: bool,
    message: str,
    failures: list[str],
) -> None:
    if not condition:
        failures.append(message)


def _validate_state(
    emulator: Uc,
    symbols: dict[str, int],
) -> list[str]:
    failures: list[str] = []
    status = _u32(
        emulator,
        symbols["g_factory_slot_audio_status"],
    )
    _require(
        status == AUDIO_OK,
        f"audio init status is {status}, expected {AUDIO_OK}",
        failures,
    )

    for offset, expected in EXPECTED_SAI.items():
        actual = _u32(emulator, 0x40384000 + offset)
        _require(
            actual == expected,
            f"SAI1+0x{offset:02x}=0x{actual:08x}, "
            f"expected 0x{expected:08x}",
            failures,
        )
    for offset in (0x08, 0x88):
        actual = _u32(emulator, 0x40384000 + offset)
        _require(
            actual == 0x80050001,
            f"SAI1 CSR+0x{offset:02x}=0x{actual:08x}",
            failures,
        )

    _require(
        _u32(emulator, 0x400EC000) == 0x80000113,
        "RX DMAMUX channel does not match factory",
        failures,
    )
    _require(
        _u32(emulator, 0x400EC040) == 0xC0000114,
        "TX DMAMUX channel does not match factory",
        failures,
    )

    rx_buffer = symbols["g_audio_rx"]
    tx_buffer = symbols["g_audio_tx"]
    rx_tcd = symbols["g_audio_rx_tcd"]
    tx_tcd = symbols["g_audio_tx_tcd"]
    hardware_rx = 0x400E9000
    hardware_tx = 0x400E9200
    for (
        name,
        address,
        source,
        destination,
        next_tcd,
        control,
    ) in (
        (
            "RX",
            hardware_rx,
            0x403840A0,
            rx_buffer,
            rx_tcd + 0x20,
            0x0012,
        ),
        (
            "TX",
            hardware_tx,
            tx_buffer,
            0x40384020,
            tx_tcd + 0x20,
            0x0010,
        ),
    ):
        _require(
            _u32(emulator, address) == source,
            f"{name} TCD source mismatch",
            failures,
        )
        _require(
            _u16(emulator, address + 0x06) == 0x0202,
            f"{name} TCD attributes mismatch",
            failures,
        )
        _require(
            _u32(emulator, address + 0x08) == 64,
            f"{name} TCD minor byte count mismatch",
            failures,
        )
        _require(
            _u32(emulator, address + 0x10) == destination,
            f"{name} TCD destination mismatch",
            failures,
        )
        _require(
            _u16(emulator, address + 0x16) == 2,
            f"{name} TCD iteration count mismatch",
            failures,
        )
        _require(
            _u32(emulator, address + 0x18) == next_tcd,
            f"{name} TCD next pointer mismatch",
            failures,
        )
        _require(
            _u16(emulator, address + 0x1C) == control,
            f"{name} TCD control mismatch",
            failures,
        )
        _require(
            _u16(emulator, address + 0x1E) == 2,
            f"{name} TCD starting iteration mismatch",
            failures,
        )

    for mux_address in (
        0x401F8120,
        0x401F812C,
        0x401F8180,
        0x401F8184,
        0x401F8188,
    ):
        _require(
            _u32(emulator, mux_address) == 0x13,
            f"pin mux 0x{mux_address:08x} is not 0x13",
            failures,
        )
    _require(
        _u32(emulator, 0x400D8080) == 7800,
        "audio PLL numerator mismatch",
        failures,
    )
    _require(
        _u32(emulator, 0x400D8090) == 10000,
        "audio PLL denominator mismatch",
        failures,
    )
    return failures


def _validate_passthrough_isr(
    emulator: Uc,
    symbols: dict[str, int],
) -> list[str]:
    failures: list[str] = []
    rx_buffer = symbols["g_audio_rx"]
    tx_buffer = symbols["g_audio_tx"]
    rx_tcd = symbols["g_audio_rx_tcd"]
    words = 4 * 8
    pattern = [
        (0x10203040 + index * 0x01010101) & 0xFFFFFFFF
        for index in range(words)
    ]
    for index, value in enumerate(pattern):
        _write_u32(emulator, rx_buffer + index * 4, value)
        _write_u32(emulator, tx_buffer + index * 4, 0)

    # The factory ISR treats DLAST_SGA==RX TCD A as "A just completed."
    _write_u32(emulator, 0x400E8024, 1)
    _write_u32(emulator, 0x400E9018, rx_tcd)
    sentinel = 0x0007FFF0
    emulator.reg_write(UC_ARM_REG_LR, sentinel | 1)
    try:
        emulator.emu_start(
            symbols["DMA0_DMA16_IRQHandler"] | 1,
            sentinel,
            count=10_000,
        )
    except UcError as error:
        failures.append(f"passthrough ISR emulation failed: {error}")
        return failures

    for index, expected in enumerate(pattern):
        actual = _u32(emulator, tx_buffer + index * 4)
        if actual != expected:
            failures.append(
                f"passthrough word {index} is 0x{actual:08x}, "
                f"expected 0x{expected:08x}"
            )
            break
    counters = symbols["g_ncr2_factory_audio_counters"]
    _require(
        _u32(emulator, counters) == 1,
        "RX block counter did not increment",
        failures,
    )
    _require(
        _u32(emulator, counters + 4) == 1,
        "copy block counter did not increment",
        failures,
    )
    _require(
        _u32(emulator, counters + 8) == 0,
        "passthrough ISR reported an unexpected interrupt",
        failures,
    )
    return failures


def emulate(elf: Path, instruction_limit: int) -> int:
    symbols = _symbols(elf)
    required = {
        "Reset_Handler",
        "application_main",
        "g_factory_slot_audio_status",
        "g_audio_rx",
        "g_audio_tx",
        "g_audio_rx_tcd",
        "g_audio_tx_tcd",
        "g_ncr2_factory_audio_counters",
        "DMA0_DMA16_IRQHandler",
    }
    missing = sorted(required - symbols.keys())
    if missing:
        raise RuntimeError(
            "source image is missing symbols: " + ", ".join(missing)
        )
    image = _extract_binary(elf)
    if not 0 < len(image) <= FACTORY_COPY_LIMIT:
        raise RuntimeError(
            f"source image size 0x{len(image):x} is outside factory slot"
        )

    emulator = _build_emulator(image)
    recent: deque[int] = deque(maxlen=24)
    instruction_count = 0
    completed = False
    invalid: str | None = None
    status_address = symbols["g_factory_slot_audio_status"]
    application_start = symbols["application_main"] & ~1

    def on_code(
        current: Uc,
        address: int,
        _size: int,
        _user_data: object,
    ) -> None:
        nonlocal instruction_count
        instruction_count += 1
        recent.append(address)
        if instruction_count >= instruction_limit:
            current.emu_stop()

    def on_write(
        current: Uc,
        _access: int,
        address: int,
        size: int,
        value: int,
        _user_data: object,
    ) -> None:
        nonlocal completed
        pc = current.reg_read(UC_ARM_REG_PC)
        if (
            address == status_address
            and size == 4
            and value == AUDIO_OK
            and application_start <= pc < application_start + 0x40
        ):
            completed = True
            current.emu_stop()

    def on_invalid(
        _current: Uc,
        access: int,
        address: int,
        size: int,
        _value: int,
        _user_data: object,
    ) -> bool:
        nonlocal invalid
        invalid = (
            f"type={access} address=0x{address:08x} size={size}"
        )
        return False

    emulator.hook_add(UC_HOOK_CODE, on_code)
    emulator.hook_add(UC_HOOK_MEM_WRITE, on_write)
    emulator.hook_add(UC_HOOK_MEM_INVALID, on_invalid)

    reset = symbols["Reset_Handler"] | 1
    error: UcError | None = None
    try:
        emulator.emu_start(reset, 0)
    except UcError as caught:
        error = caught

    print(
        f"instructions={instruction_count} "
        f"pc=0x{emulator.reg_read(UC_ARM_REG_PC):08x} "
        f"image_size=0x{len(image):x}"
    )
    print(
        "recent_pc=" +
        " ".join(f"0x{address:x}" for address in recent)
    )
    if invalid is not None:
        print(f"invalid_memory={invalid}")
    if error is not None:
        print(f"emulator_error={error}")
    if not completed:
        print("source audio initialization did not complete")
        return 1

    # The memory-write hook runs before the store is committed.
    _write_u32(emulator, status_address, AUDIO_OK)
    failures = _validate_state(emulator, symbols)
    failures.extend(_validate_passthrough_isr(emulator, symbols))
    if failures:
        print("source audio contract mismatches:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        "source audio contract verified: SAI1, PLL, pins, DMAMUX, "
        "RX/TX TCD geometry, and passthrough ISR match factory"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument(
        "--instruction-limit",
        type=int,
        default=1_000_000,
    )
    arguments = parser.parse_args()
    if not arguments.elf.is_file():
        parser.error(f"ELF does not exist: {arguments.elf}")
    try:
        return emulate(arguments.elf, arguments.instruction_limit)
    except (
        OSError,
        RuntimeError,
        subprocess.CalledProcessError,
    ) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())

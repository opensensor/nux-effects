#!/usr/bin/env python3
"""Emulate enough of the factory Metal engine to trace audio bring-up.

This is an offline reverse-engineering aid.  It never opens a USB device and
never modifies its input files.  The model intentionally stops short of
pretending to be a complete RT1051: it supplies only the hardware completion
events needed to reach the factory application's main loop.

Unicorn has two relevant Cortex-M limitations which are worked around in the
private emulation copy:

* its Thumb IT handling loops incorrectly in the two optimized memset tails;
* its M-class backend rejects the factory double-precision PRNG initializer.

The patched bytes live only in emulator memory.  They are reported at startup
and are never written to an artifact.
"""

from __future__ import annotations

import argparse
from collections import Counter, deque
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import sys

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
    from unicorn.arm_const import (
        UC_ARM_REG_LR,
        UC_ARM_REG_PC,
        UC_ARM_REG_R0,
        UC_ARM_REG_R2,
        UC_ARM_REG_R4,
        UC_ARM_REG_R5,
        UC_ARM_REG_SP,
    )
except ImportError as error:  # pragma: no cover - depends on local tooling
    raise SystemExit(
        "unicorn is required; install it in an isolated environment first"
    ) from error


ENGINE_SIZE = 0x1E000
ENGINE_FLASH_OFFSET = 0x0C0000
ENGINE_START_PC = 0x0000E7E1
ENGINE_STACK_TOP = 0x20018000
MAX_DUMP_SIZE = 0x00800000
VERIFIED_DUMP_SHA256 = (
    "4263ef41c0745f6e8c00be13b52391b6"
    "b04a5f51779b12d0e191abf6888e7a14"
)

PATCHES = {
    # NOP the BHI in optimized reverse/forward memset.  Memory starts zero in
    # this model, so skipping the extra zero stores does not affect state.
    0x00004BA2: b"\x00\xbf",
    0x0000417E: b"\x00\xbf",
    # Return from the nonessential double-precision PRNG/table initializer.
    0x0000710C: b"\x70\x47",
}


@dataclass(frozen=True)
class WatchRange:
    start: int
    end: int
    name: str


WATCH_RANGES = (
    WatchRange(0x40384000, 0x40384100, "SAI1"),
    WatchRange(0x40388000, 0x40388100, "SAI2"),
    WatchRange(0x4038C000, 0x4038C100, "SAI3"),
    WatchRange(0x400E8000, 0x400E9000, "DMA"),
    WatchRange(0x400EC000, 0x400ED000, "DMAMUX"),
)

GPIO_WATCH_RANGES = (
    WatchRange(0x401B8000, 0x401B8100, "GPIO1"),
    WatchRange(0x401BC000, 0x401BC100, "GPIO2"),
    WatchRange(0x401C0000, 0x401C0100, "GPIO3"),
    WatchRange(0x401C4000, 0x401C4100, "GPIO4"),
    WatchRange(0x400C0000, 0x400C0100, "GPIO5"),
)

SAI_REGISTER_OFFSETS = {
    0x08: "TCSR",
    0x0C: "TCR1",
    0x10: "TCR2",
    0x14: "TCR3",
    0x18: "TCR4",
    0x1C: "TCR5",
    0x88: "RCSR",
    0x8C: "RCR1",
    0x90: "RCR2",
    0x94: "RCR3",
    0x98: "RCR4",
    0x9C: "RCR5",
}

DMA_STATE_START = 0x2000BD40
DMA_STATE_SIZE = 0x300
TCD_SIZE = 0x20

SAI1_PIN_CANDIDATES = {
    "GPIO_AD_B1_09_MCLK": (0x401F8120, 0x401F8310),
    "GPIO_AD_B1_10_RX_SYNC": (0x401F8124, 0x401F8314),
    "GPIO_AD_B1_11_RX_BCLK": (0x401F8128, 0x401F8318),
    "GPIO_AD_B1_12_RX_DATA00": (0x401F812C, 0x401F831C),
    "GPIO_AD_B1_13_TX_DATA00": (0x401F8130, 0x401F8320),
    "GPIO_AD_B1_14_TX_BCLK": (0x401F8134, 0x401F8324),
    "GPIO_AD_B1_15_TX_SYNC": (0x401F8138, 0x401F8328),
    "GPIO_B0_13_MCLK": (0x401F8170, 0x401F8360),
    "GPIO_B0_14_RX_SYNC": (0x401F8174, 0x401F8364),
    "GPIO_B0_15_RX_BCLK": (0x401F8178, 0x401F8368),
    "GPIO_B1_00_RX_DATA00": (0x401F817C, 0x401F836C),
    "GPIO_B1_01_TX_DATA00": (0x401F8180, 0x401F8370),
    "GPIO_B1_02_TX_BCLK": (0x401F8184, 0x401F8374),
    "GPIO_B1_03_TX_SYNC": (0x401F8188, 0x401F8378),
    "GPIO_SD_B1_03_MCLK": (0x401F81E0, 0x401F83D0),
    "GPIO_SD_B1_04_RX_SYNC": (0x401F81E4, 0x401F83D4),
    "GPIO_SD_B1_05_RX_BCLK": (0x401F81E8, 0x401F83D8),
    "GPIO_SD_B1_06_RX_DATA00": (0x401F81EC, 0x401F83DC),
    "GPIO_SD_B1_07_TX_DATA00": (0x401F81F0, 0x401F83E0),
    "GPIO_SD_B1_08_TX_BCLK": (0x401F81F4, 0x401F83E4),
    "GPIO_SD_B1_09_TX_SYNC": (0x401F81F8, 0x401F83E8),
}


def _u32(data: bytes) -> int:
    return int.from_bytes(data, "little")


def _write_u32(emulator: Uc, address: int, value: int) -> None:
    emulator.mem_write(address, (value & 0xFFFFFFFF).to_bytes(4, "little"))


def _read_u32(emulator: Uc, address: int) -> int:
    return _u32(bytes(emulator.mem_read(address, 4)))


def _field(value: int, shift: int, mask: int) -> int:
    return (value >> shift) & mask


def _decode_sai_registers(emulator: Uc) -> None:
    print("SAI1_decoded:")
    values: dict[str, int] = {}
    for offset, name in SAI_REGISTER_OFFSETS.items():
        value = _read_u32(emulator, 0x40384000 + offset)
        values[name] = value
        print(f"  {name}={value:#010x}")

    for direction in ("T", "R"):
        csr = values[f"{direction}CSR"]
        cr1 = values[f"{direction}CR1"]
        cr2 = values[f"{direction}CR2"]
        cr3 = values[f"{direction}CR3"]
        cr4 = values[f"{direction}CR4"]
        cr5 = values[f"{direction}CR5"]
        print(
            f"  {direction}: "
            f"enable={(csr >> 31) & 1} "
            f"bit_clock_enable={(csr >> 28) & 1} "
            f"fifo_request_dma={csr & 1} "
            f"fifo_watermark={cr1 & 0x1f} "
            f"bit_clock_divider={cr2 & 0xff} "
            f"bit_clock_master={(cr2 >> 24) & 1} "
            f"bit_clock_polarity={(cr2 >> 25) & 1} "
            f"mclk_select={(cr2 >> 26) & 0x3} "
            f"sync_mode={(cr2 >> 30) & 1} "
            f"channel_enable={(cr3 >> 16) & 0xf:#x} "
            f"frame_sync_master={cr4 & 1} "
            f"frame_sync_polarity={(cr4 >> 1) & 1} "
            f"frame_sync_early={(cr4 >> 3) & 1} "
            f"msb_first={(cr4 >> 4) & 1} "
            f"sync_width={_field(cr4, 8, 0x1f) + 1} "
            f"words_per_frame={_field(cr4, 16, 0x1f) + 1} "
            f"first_bit_index={_field(cr5, 8, 0x1f)} "
            f"first_word_bits={_field(cr5, 16, 0x1f) + 1} "
            f"word_bits={_field(cr5, 24, 0x1f) + 1}"
        )


def _decode_tcd(data: bytes) -> dict[str, int]:
    (
        source,
        source_offset,
        attributes,
        minor_bytes,
        source_last,
        destination,
        destination_offset,
        current_iterations,
        destination_last_or_next,
        control,
        starting_iterations,
    ) = struct.unpack("<IhHII I hH I HH".replace(" ", ""), data)
    return {
        "source": source,
        "source_offset": source_offset,
        "attributes": attributes,
        "minor_bytes": minor_bytes,
        "source_last": source_last,
        "destination": destination,
        "destination_offset": destination_offset,
        "current_iterations": current_iterations,
        "destination_last_or_next": destination_last_or_next,
        "control": control,
        "starting_iterations": starting_iterations,
    }


def _looks_like_tcd(fields: dict[str, int]) -> bool:
    return (
        fields["minor_bytes"] != 0
        and fields["current_iterations"] != 0
        and fields["starting_iterations"] != 0
        and (
            0x20000000 <= fields["source"] < 0x20400000
            or 0x40000000 <= fields["source"] < 0x40400000
        )
        and (
            0x20000000 <= fields["destination"] < 0x20400000
            or 0x40000000 <= fields["destination"] < 0x40400000
        )
    )


def _dump_dma_state(emulator: Uc) -> None:
    state = bytes(
        emulator.mem_read(DMA_STATE_START, DMA_STATE_SIZE)
    )
    print(
        f"dma_state[{DMA_STATE_START:#010x}:"
        f"{DMA_STATE_START + DMA_STATE_SIZE:#010x}]="
        f"{state.hex()}"
    )
    print("candidate_tcds:")
    found = 0
    for offset in range(0, len(state) - TCD_SIZE + 1, TCD_SIZE):
        fields = _decode_tcd(state[offset : offset + TCD_SIZE])
        if not _looks_like_tcd(fields):
            continue
        found += 1
        address = DMA_STATE_START + offset
        transfer_bits = (8, 16, 32, 0, 128)[
            fields["attributes"] & 0x7
        ] if (fields["attributes"] & 0x7) <= 4 else 0
        print(
            f"  {address:#010x}: "
            f"source={fields['source']:#010x} "
            f"source_offset={fields['source_offset']} "
            f"destination={fields['destination']:#010x} "
            f"destination_offset={fields['destination_offset']} "
            f"transfer_bits={transfer_bits} "
            f"attributes={fields['attributes']:#06x} "
            f"minor_bytes={fields['minor_bytes']} "
            f"iterations={fields['starting_iterations']} "
            f"next_or_adjust="
            f"{fields['destination_last_or_next']:#010x} "
            f"control={fields['control']:#06x}"
        )
    if found == 0:
        print("  (none)")


def _dump_sai1_pins(emulator: Uc) -> None:
    print("SAI1_pin_candidates_configured_for_alt3:")
    found = 0
    for name, (mux_address, pad_address) in SAI1_PIN_CANDIDATES.items():
        mux = _read_u32(emulator, mux_address)
        if (mux & 0xF) != 3:
            continue
        found += 1
        print(
            f"  {name}: mux[{mux_address:#010x}]={mux:#010x} "
            f"pad[{pad_address:#010x}]="
            f"{_read_u32(emulator, pad_address):#010x}"
        )
    if found == 0:
        print("  (none)")
    print(
        "  daisy: "
        f"MCLK={_read_u32(emulator, 0x401F858C):#x} "
        f"RX_BCLK={_read_u32(emulator, 0x401F8590):#x} "
        f"RX_DATA00={_read_u32(emulator, 0x401F8594):#x} "
        f"RX_SYNC={_read_u32(emulator, 0x401F85A4):#x} "
        f"TX_BCLK={_read_u32(emulator, 0x401F85A8):#x} "
        f"TX_SYNC={_read_u32(emulator, 0x401F85AC):#x}"
    )


def _watch_name(address: int, trace_gpio: bool) -> str | None:
    ranges = (
        WATCH_RANGES + GPIO_WATCH_RANGES
        if trace_gpio
        else WATCH_RANGES
    )
    for region in ranges:
        if region.start <= address < region.end:
            return region.name
    return None


def _load_inputs(
    dump_path: Path,
    engine_path: Path | None,
) -> tuple[bytes, bytes]:
    dump = dump_path.read_bytes()
    if len(dump) != MAX_DUMP_SIZE:
        raise ValueError(
            f"{dump_path} is {len(dump):#x} bytes, expected "
            f"{MAX_DUMP_SIZE:#x}"
        )
    digest = hashlib.sha256(dump).hexdigest()
    if digest != VERIFIED_DUMP_SHA256:
        raise ValueError(
            f"{dump_path} SHA-256 is {digest}, expected "
            f"{VERIFIED_DUMP_SHA256}"
        )
    factory_engine = dump[
        ENGINE_FLASH_OFFSET :
        ENGINE_FLASH_OFFSET + ENGINE_SIZE
    ]
    if engine_path is None:
        engine = factory_engine
    else:
        engine = engine_path.read_bytes()
    if len(engine) != ENGINE_SIZE:
        raise ValueError(
            f"factory engine is {len(engine):#x} bytes, expected "
            f"{ENGINE_SIZE:#x}"
        )
    if engine != factory_engine:
        raise ValueError(
            "factory engine does not match the verified dump's ENG3 slot"
        )
    return dump, engine


def _build_emulator(dump: bytes, engine: bytes) -> Uc:
    mutable_engine = bytearray(engine)
    for address, replacement in PATCHES.items():
        mutable_engine[address : address + len(replacement)] = replacement

    emulator = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for address, size in (
        (0x00000000, 0x00080000),  # ITCM
        (0x20000000, 0x00020000),  # DTCM
        (0x20200000, 0x00100000),  # OCRAM model
        (0x40000000, 0x00400000),  # RT1051 peripherals
        (0x60000000, 0x00800000),  # FlexSPI XIP window
        (0x80000000, 0x02000000),  # external SDRAM
        (0xE0000000, 0x00100000),  # Cortex-M system controls
    ):
        emulator.mem_map(address, size)

    emulator.mem_write(0, bytes(mutable_engine))
    emulator.mem_write(0x60000000, dump)

    # Model PLL lock indications.  The factory code preserves bit 31 while
    # programming most PLL control registers.
    for address in range(0x400D8000, 0x400D9010, 0x10):
        _write_u32(emulator, address, 0x80000000)
    _write_u32(emulator, 0x400D8270, 0x80010000)
    _write_u32(emulator, 0x400D8150, 0x80008000)
    _write_u32(emulator, 0x40080000, 0x80000000)

    # 32 KiB, four-way, 256-set, 32-byte Cortex-M7 data cache.
    _write_u32(emulator, 0xE000ED80, 0x001FE01B)
    _write_u32(emulator, 0xE000ED88, 0x00F00000)

    emulator.reg_write(UC_ARM_REG_SP, ENGINE_STACK_TOP)
    emulator.reg_write(UC_ARM_REG_LR, 0x0007FFF1)
    return emulator


def emulate(
    dump: bytes,
    engine: bytes,
    instruction_limit: int,
    trace_gpio: bool,
) -> int:
    emulator = _build_emulator(dump, engine)
    counts: Counter[int] = Counter()
    fixes: Counter[str] = Counter()
    recent: deque[int] = deque(maxlen=24)
    writes: list[tuple[int, int, str, int, int, int]] = []
    instruction_count = 0
    audio_init_complete = False
    gpio_alias_update = False

    def rmw(
        address: int,
        clear_mask: int,
        set_mask: int,
        name: str,
    ) -> None:
        fixes[name] += 1
        value = _read_u32(emulator, address)
        _write_u32(
            emulator,
            address,
            (value & ~clear_mask) | set_mask,
        )

    def on_code(
        current: Uc,
        address: int,
        _size: int,
        _user_data: object,
    ) -> None:
        nonlocal instruction_count, audio_init_complete
        instruction_count += 1
        counts[address] += 1
        recent.append(address)

        if address == 0x00005CDE:
            fixes["dwt_cycle_progress"] += 1
            _write_u32(
                current,
                0xE0001004,
                _read_u32(current, 0xE0001004) + 0x00100000,
            )
        elif address == 0x000044D2:
            rmw(
                current.reg_read(UC_ARM_REG_R4),
                0x1,
                0,
                "adc1_calibration_complete",
            )
        elif address == 0x00006CBC:
            rmw(
                current.reg_read(UC_ARM_REG_R4),
                0x1,
                0,
                "adc2_calibration_complete",
            )
        elif address == 0x00004718:
            rmw(
                current.reg_read(UC_ARM_REG_R4) + 0xE0,
                0,
                0x3,
                "adc_ready",
            )
        elif address == 0x00007F08:
            rmw(
                current.reg_read(UC_ARM_REG_R0),
                0x8000,
                0,
                "timer_reset_complete",
            )
        elif address == 0x00003B0C:
            rmw(
                current.reg_read(UC_ARM_REG_R2),
                0,
                0x80000000,
                "audio_pll_lock",
            )
        elif address == 0x00005854:
            # The main loop waits for 48 timer ticks.  Advance it without
            # emulating the GPT interrupt so post-init work can run.
            rmw(
                current.reg_read(UC_ARM_REG_R5),
                0,
                48,
                "main_tick",
            )
        elif address == 0x00008110:
            # The factory audio initializer has enabled both SAI1 directions
            # and their DMA requests.  Stop before unrelated post-init code
            # reaches hardware that this narrow model intentionally omits.
            fixes["audio_init_complete"] += 1
            audio_init_complete = True
            current.emu_stop()

        if (
            not audio_init_complete
            and instruction_count >= instruction_limit
        ):
            current.emu_stop()

    def on_write(
        current: Uc,
        _access: int,
        address: int,
        size: int,
        value: int,
        _user_data: object,
    ) -> None:
        nonlocal gpio_alias_update
        if not gpio_alias_update:
            for region in GPIO_WATCH_RANGES:
                offset = address - region.start
                if offset not in (0x84, 0x88, 0x8C):
                    continue
                gpio_alias_update = True
                data = _read_u32(current, region.start)
                if offset == 0x84:
                    data |= value
                elif offset == 0x88:
                    data &= ~value
                else:
                    data ^= value
                _write_u32(current, region.start, data)
                gpio_alias_update = False
                break

        name = _watch_name(address, trace_gpio)
        if name is None:
            return
        mask = (1 << (size * 8)) - 1
        writes.append(
            (
                instruction_count,
                current.reg_read(UC_ARM_REG_PC),
                name,
                address,
                size,
                value & mask,
            )
        )

    def on_invalid(
        current: Uc,
        access: int,
        address: int,
        size: int,
        _value: int,
        _user_data: object,
    ) -> bool:
        print(
            "invalid memory access "
            f"type={access} address={address:#010x} size={size} "
            f"pc={current.reg_read(UC_ARM_REG_PC):#010x}",
            file=sys.stderr,
        )
        return False

    emulator.hook_add(UC_HOOK_CODE, on_code)
    emulator.hook_add(UC_HOOK_MEM_WRITE, on_write)
    emulator.hook_add(UC_HOOK_MEM_INVALID, on_invalid)

    error: UcError | None = None
    try:
        emulator.emu_start(ENGINE_START_PC, 0)
    except UcError as caught:
        error = caught

    print(
        f"instructions={instruction_count} "
        f"pc={emulator.reg_read(UC_ARM_REG_PC):#010x}"
    )
    print(f"model_events={dict(fixes)}")
    print(
        "recent_pc=" +
        " ".join(f"{address:#x}" for address in recent)
    )
    print("emulator_only_patches:")
    for address, replacement in PATCHES.items():
        print(f"  {address:#010x}: {replacement.hex()}")
    print(f"peripheral_writes={len(writes)}")
    for (
        sequence,
        pc,
        name,
        address,
        size,
        value,
    ) in writes:
        print(
            f"{sequence:9d} pc={pc:08x} {name:<6} "
            f"[{address:08x}]/{size} = {value:08x}"
        )

    for name, address in (
        ("SAI1", 0x40384000),
        ("SAI2", 0x40388000),
        ("SAI3", 0x4038C000),
    ):
        print(f"{name}_registers={bytes(emulator.mem_read(address, 0x100)).hex()}")
    _decode_sai_registers(emulator)
    _dump_dma_state(emulator)
    _dump_sai1_pins(emulator)
    clock_config = bytes(
        emulator.mem_read(0x2000B91C, 0x28)
    )
    print(f"audio_clock_config={clock_config.hex()}")
    print(
        "audio_clock_registers="
        f"pll_audio={_read_u32(emulator, 0x400D8070):#010x} "
        f"pll_audio_num={_read_u32(emulator, 0x400D8080):#010x} "
        f"pll_audio_denom={_read_u32(emulator, 0x400D8090):#010x} "
        f"ccm_cscmr1={_read_u32(emulator, 0x400FC01C):#010x} "
        f"ccm_cs1cdr={_read_u32(emulator, 0x400FC028):#010x} "
        f"iomuxc_gpr_gpr1={_read_u32(emulator, 0x400AC004):#010x}"
    )
    print(
        "factory_audio_state=" +
        bytes(emulator.mem_read(0x20009524, 0x200)).hex()
    )
    print("gpio_state:")
    for region in GPIO_WATCH_RANGES:
        print(
            f"  {region.name}: "
            f"DR={_read_u32(emulator, region.start):#010x} "
            f"GDIR={_read_u32(emulator, region.start + 4):#010x}"
        )

    if error is not None:
        print(f"emulation stopped with {error}", file=sys.stderr)
        return 1
    if not audio_init_complete:
        print(
            "instruction limit reached before audio initialization completed",
            file=sys.stderr,
        )
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dump", type=Path, help="verified 8 MiB NOR dump")
    parser.add_argument(
        "--engine",
        type=Path,
        help="optional carved 0x1e000-byte Metal engine",
    )
    parser.add_argument(
        "--instruction-limit",
        type=int,
        default=15_000_000,
    )
    parser.add_argument(
        "--trace-gpio",
        action="store_true",
        help="print every GPIO write in addition to the final pin state",
    )
    arguments = parser.parse_args()

    try:
        dump, engine = _load_inputs(arguments.dump, arguments.engine)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return emulate(
        dump,
        engine,
        arguments.instruction_limit,
        arguments.trace_gpio,
    )


if __name__ == "__main__":
    raise SystemExit(main())

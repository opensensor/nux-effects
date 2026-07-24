from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

spec = importlib.util.spec_from_file_location(
    "build_zero_wear", TOOLS / "build_zero_wear.py"
)
assert spec and spec.loader
build_zero_wear = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_zero_wear)


class ZeroWearImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.stock = (ROOT / "dump1.bin").read_bytes()

    def test_patch_changes_only_vector_and_monitor_cave(self) -> None:
        start = build_zero_wear.METAL_FLASH_OFFSET
        engine = self.stock[start : start + build_zero_wear.SLOT_SIZE]
        monitor = bytes.fromhex("7047")
        patched = build_zero_wear.patch_engine("metal", engine, monitor)
        differences = [
            index
            for index, (before, after) in enumerate(zip(engine, patched))
            if before != after
        ]
        expected = set(
            range(
                build_zero_wear.GPT1_VECTOR_OFFSET,
                build_zero_wear.GPT1_VECTOR_OFFSET + 4,
            )
        )
        expected.update(
            range(
                build_zero_wear.ENGINE_MONITOR_OFFSET,
                build_zero_wear.ENGINE_MONITOR_OFFSET + len(monitor),
            )
        )
        hook_offset = build_zero_wear.GPT1_START_HOOKS["metal"]
        expected.update(range(hook_offset, hook_offset + 4))
        self.assertTrue(set(differences).issubset(expected))
        self.assertEqual(
            struct.unpack_from(
                "<I", patched, build_zero_wear.GPT1_VECTOR_OFFSET
            )[0],
            build_zero_wear.ENGINE_MONITOR_ADDRESS,
        )
        self.assertEqual(
            patched[
                build_zero_wear.ENGINE_MONITOR_OFFSET :
                build_zero_wear.ENGINE_MONITOR_OFFSET + len(monitor)
            ],
            monitor,
        )
        self.assertEqual(
            patched[hook_offset : hook_offset + 4],
            build_zero_wear.encode_thumb_b_w(
                hook_offset,
                build_zero_wear.ENGINE_MONITOR_ENABLE_OFFSET,
            ),
        )

    def test_layout_constants(self) -> None:
        self.assertEqual(build_zero_wear.GPT1_VECTOR_OFFSET, 0x1D0)
        self.assertEqual(build_zero_wear.ENGINE_MONITOR_OFFSET, 0x1DF00)
        self.assertEqual(
            build_zero_wear.ENGINE_MONITOR_ENABLE_OFFSET, 0x1DFA0
        )
        self.assertEqual(build_zero_wear.PICKER_FLASH_OFFSET, 0x80000)
        self.assertEqual(
            build_zero_wear.REVERB_RELOCATED_OFFSET, 0xE0000
        )
        self.assertEqual(build_zero_wear.IMAGE_END, 0x100000)

    def test_stock_engine_vectors_are_expected(self) -> None:
        delay = struct.unpack_from(
            "<I",
            self.stock,
            0x60000 + build_zero_wear.GPT1_VECTOR_OFFSET,
        )[0]
        reverb = struct.unpack_from(
            "<I",
            self.stock,
            build_zero_wear.REVERB_STOCK_OFFSET
            + build_zero_wear.GPT1_VECTOR_OFFSET,
        )[0]
        metal = struct.unpack_from(
            "<I",
            self.stock,
            build_zero_wear.METAL_FLASH_OFFSET
            + build_zero_wear.GPT1_VECTOR_OFFSET,
        )[0]
        modulation = struct.unpack_from(
            "<I",
            self.stock,
            0xA0000 + build_zero_wear.GPT1_VECTOR_OFFSET,
        )[0]
        self.assertEqual(delay, 0x46A5)
        self.assertEqual(reverb, 0x14565)
        self.assertEqual(modulation, 0x57F9)
        self.assertEqual(metal, 0x7ED9)


if __name__ == "__main__":
    unittest.main()

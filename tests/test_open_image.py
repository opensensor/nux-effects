import importlib.util
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "open_image", ROOT / "tools" / "open_image.py"
)
open_image = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = open_image
SPEC.loader.exec_module(open_image)


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.layout = open_image.load_layout()

    def test_layout_is_complete_and_contiguous(self):
        self.assertEqual(self.layout.flash_size, 0x800000)
        self.assertEqual(self.layout.regions[0].offset, 0)
        self.assertEqual(self.layout.regions[-1].end, 0x800000)
        for left, right in zip(
            self.layout.regions, self.layout.regions[1:]
        ):
            self.assertEqual(left.end, right.offset)

    def test_c_header_matches_json_layout(self):
        header = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "include"
            / "ncr2_flash_layout.h"
        ).read_text()

        def value(name):
            match = re.search(
                rf"#define {name} UINT32_C\((0x[0-9A-Fa-f]+)\)", header
            )
            self.assertIsNotNone(match, name)
            return int(match.group(1), 16)

        self.assertEqual(value("NCR2_FLASH_SIZE"), self.layout.flash_size)
        self.assertEqual(
            value("NCR2_BOOTLOADER_SIZE"),
            self.layout.region("bootloader").size,
        )
        self.assertEqual(
            value("NCR2_APPLICATION_A_OFFSET"),
            self.layout.region("application_a").offset,
        )
        self.assertEqual(
            value("NCR2_APPLICATION_B_OFFSET"),
            self.layout.region("application_b").offset,
        )
        self.assertEqual(
            value("NCR2_APPLICATION_SLOT_SIZE"),
            self.layout.region("application_a").size,
        )
        self.assertEqual(
            value("NCR2_APPLICATION_LOAD_ADDRESS"),
            self.layout.application_load_address,
        )


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.layout = open_image.load_layout()
        self.application = struct.pack(
            "<II", 0x20020000, 0x80000009
        ) + bytes.fromhex("00bf00bf00bf00bf")

    def test_manifest_round_trip(self):
        sector = open_image.build_manifest(
            self.application,
            layout=self.layout,
            semantic_version=open_image.parse_semantic_version("1.2.345"),
            build_number=17,
        )
        manifest = open_image.parse_manifest(sector, layout=self.layout)
        open_image.validate_vector(self.application, manifest)
        self.assertEqual(manifest.image_size, len(self.application))
        self.assertEqual(
            open_image.format_semantic_version(manifest.semantic_version),
            "1.2.345",
        )
        self.assertEqual(manifest.build_number, 17)

    def test_manifest_crc_rejects_mutation(self):
        sector = bytearray(
            open_image.build_manifest(
                self.application,
                layout=self.layout,
                semantic_version=0,
                build_number=1,
            )
        )
        sector[20] ^= 0x01
        with self.assertRaises(open_image.ImageError):
            open_image.parse_manifest(bytes(sector), layout=self.layout)


class FullImageTests(unittest.TestCase):
    def setUp(self):
        self.layout = open_image.load_layout()
        self.stock = bytes([0xFF]) * self.layout.flash_size
        self.bootloader = struct.pack(
            "<II", 0x20020000, 0x60002009
        ) + bytes.fromhex("00bf00bf00bf00bf")
        self.application = struct.pack(
            "<II", 0x20020000, 0x80000009
        ) + bytes.fromhex("00bf00bf00bf00bf")

    def test_only_approved_regions_change(self):
        image, report = open_image.build_full_image(
            self.stock,
            self.bootloader,
            self.application,
            layout=self.layout,
            semantic_version=open_image.parse_semantic_version("0.1.0"),
            build_number=1,
            verify_stock=False,
        )
        self.assertEqual(len(image), self.layout.flash_size)
        factory = self.layout.region("factory_compatibility")
        self.assertEqual(
            image[factory.offset : factory.end],
            self.stock[factory.offset : factory.end],
        )
        self.assertEqual(
            image[: self.layout.boot_header_size],
            self.stock[: self.layout.boot_header_size],
        )
        changed = {
            entry["region"] for entry in report["changed_ranges"]
        }
        self.assertLessEqual(
            changed,
            {
                "bootloader",
                "boot_metadata",
                "application_a",
                "application_b",
            },
        )
        inspected = open_image.inspect_full_image(
            image, layout=self.layout
        )
        self.assertEqual(
            inspected["slots"]["application_a"]["state"], "valid"
        )
        self.assertEqual(
            inspected["slots"]["application_b"]["state"], "erased"
        )

    def test_bad_boot_vector_is_rejected(self):
        broken = bytearray(self.bootloader)
        struct.pack_into("<I", broken, 4, 0x60003000)
        with self.assertRaises(open_image.ImageError):
            open_image.build_full_image(
                self.stock,
                bytes(broken),
                self.application,
                layout=self.layout,
                semantic_version=0,
                build_number=1,
                verify_stock=False,
            )


class StockExtractionTests(unittest.TestCase):
    def test_verified_dump_boot_config_when_available(self):
        dump = ROOT / "dump1.bin"
        if not dump.exists():
            self.skipTest("private stock dump is not present")
        layout = open_image.load_layout()
        config = open_image.extract_boot_config(
            dump.read_bytes(), layout=layout
        )
        self.assertEqual(config["fcfb"]["tag"], "FCFB")
        self.assertEqual(config["ivt"]["entry"], "0x60002000")
        self.assertEqual(config["ivt"]["dcd_pointer"], "0x60001030")
        self.assertEqual(config["dcd"]["length"], 1072)
        self.assertEqual(len(config["dcd"]["commands"]), 9)
        self.assertEqual(
            sum(
                len(command.get("write_data", []))
                for command in config["dcd"]["commands"]
            ),
            125,
        )


class EmbeddedSha256Tests(unittest.TestCase):
    def test_embedded_sha256_matches_standard_vector(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")
        source = r"""
#include <stdint.h>
#include <stdio.h>
#include "sha256.h"

int main(void)
{
    static const char message[] = "abc";
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_context_t context;

    sha256_init(&context);
    sha256_update(&context, message, 3);
    sha256_final(&context, digest);
    for (unsigned int index = 0; index < SHA256_DIGEST_SIZE; ++index) {
        printf("%02x", digest[index]);
    }
    putchar('\n');
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "sha_test.c"
            executable = directory_path / "sha_test"
            test_source.write_text(source)
            subprocess.run(
                [
                    compiler,
                    "-std=c17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "firmware" / "bootloader" / "include"),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "sha256.c"
                    ),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            result = subprocess.check_output([executable], text=True).strip()
        self.assertEqual(
            result,
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad",
        )


if __name__ == "__main__":
    unittest.main()

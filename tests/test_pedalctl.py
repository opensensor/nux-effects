import dataclasses
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

SPEC = importlib.util.spec_from_file_location(
    "pedalctl", TOOLS / "pedalctl.py"
)
pedalctl = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = pedalctl
SPEC.loader.exec_module(pedalctl)


class PacketTests(unittest.TestCase):
    def test_packet_layout_and_crc_round_trip(self):
        packet = pedalctl.Packet(
            command=pedalctl.COMMANDS["write-chunk"],
            flags=1,
            session=0x12345678,
            sequence=9,
            offset=0x1020,
            payload=b"\x01\x02\x03",
        )
        encoded = packet.encode()
        self.assertEqual(len(encoded), 64)
        self.assertEqual(encoded[:4], b"NXFX")
        self.assertEqual(encoded[4], 2)
        self.assertEqual(encoded[5], 4)
        self.assertEqual(struct.unpack_from("<H", encoded, 0x14)[0], 3)
        self.assertEqual(pedalctl.Packet.decode(encoded), packet)

        corrupted = bytearray(encoded)
        corrupted[0x20] ^= 1
        with self.assertRaises(pedalctl.RecoveryError):
            pedalctl.Packet.decode(bytes(corrupted))

    def test_rejects_oversized_payload(self):
        with self.assertRaises(pedalctl.RecoveryError):
            pedalctl.Packet(
                command=pedalctl.COMMANDS["write-chunk"],
                payload=bytes(33),
            ).encode()


class RecoveryDeviceDiscoveryTests(unittest.TestCase):
    @staticmethod
    def add_hidraw(
        sysfs_root: Path,
        name: str,
        hid_id: str,
    ) -> None:
        device = sysfs_root / name / "device"
        device.mkdir(parents=True)
        (device / "uevent").write_text(
            f"DRIVER=hid-generic\nHID_ID={hid_id}\n"
        )

    def test_discovers_single_open_recovery_node(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sysfs_root = root / "sys"
            device_root = root / "dev"
            sysfs_root.mkdir()
            device_root.mkdir()
            self.add_hidraw(
                sysfs_root,
                "hidraw3",
                "0003:00009527:0000C157",
            )
            self.add_hidraw(
                sysfs_root,
                "hidraw4",
                "0003:00003142:0000A010",
            )

            self.assertEqual(
                pedalctl.discover_recovery_device(
                    sysfs_root,
                    device_root,
                ),
                device_root / "hidraw3",
            )

    def test_discovery_rejects_zero_or_multiple_nodes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sysfs_root = root / "sys"
            device_root = root / "dev"
            sysfs_root.mkdir()
            device_root.mkdir()
            with self.assertRaises(pedalctl.RecoveryError):
                pedalctl.discover_recovery_device(
                    sysfs_root,
                    device_root,
                )

            self.add_hidraw(
                sysfs_root,
                "hidraw6",
                "0003:00009527:0000C157",
            )
            self.add_hidraw(
                sysfs_root,
                "hidraw7",
                "0003:00009527:0000C157",
            )
            with self.assertRaises(pedalctl.RecoveryError):
                pedalctl.discover_recovery_device(
                    sysfs_root,
                    device_root,
                )

    def test_restore_full_device_argument_is_optional(self):
        arguments = pedalctl.build_parser().parse_args(
            [
                "restore-full",
                "full-image.bin",
                "--confirm",
                pedalctl.FULL_FLASH_CONFIRMATION,
            ]
        )
        self.assertIsNone(arguments.device)


class SlotImageTests(unittest.TestCase):
    def test_open_slot_image_validates(self):
        layout = pedalctl.open_image.load_layout()
        payload = struct.pack(
            "<IIII", 0x20020000, 0x80000009, 0xBF00BF00, 0xBF00BF00
        )
        slot = pedalctl.open_image.build_slot_image(
            payload,
            layout=layout,
            semantic_version=pedalctl.open_image.parse_semantic_version(
                "0.2.0"
            ),
            build_number=4,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.slot"
            path.write_bytes(slot)
            validated, manifest = pedalctl.validate_slot_image(path)
        self.assertEqual(validated, slot)
        self.assertEqual(manifest.image_size, len(payload))

    def test_slot_image_rejects_trailing_data(self):
        layout = pedalctl.open_image.load_layout()
        payload = struct.pack("<II", 0x20020000, 0x80000009)
        slot = pedalctl.open_image.build_slot_image(
            payload,
            layout=layout,
            semantic_version=0,
            build_number=1,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.slot"
            path.write_bytes(slot + b"\xff")
            with self.assertRaises(pedalctl.RecoveryError):
                pedalctl.validate_slot_image(path)


class FakeTransport:
    def __init__(self):
        self.session = 0xA1B2C3D4
        self.expected_sequence = 1
        self.requests: list[pedalctl.Packet] = []

    def exchange(self, report: bytes) -> bytes:
        request = pedalctl.Packet.decode(report)
        self.requests.append(request)
        if request.command == pedalctl.COMMANDS["get-info"]:
            payload = pedalctl.INFO_STRUCT.pack(
                0x800000,
                0x200000,
                0x400000,
                0x600000,
                0x1000,
                0,
                0xFF,
                0,
                0,
                0xF,
                32,
            )
            return pedalctl.Packet(
                command=request.command,
                payload=payload,
            ).encode()
        if request.command == pedalctl.COMMANDS["get-log"]:
            payload = pedalctl.NOR_DIAGNOSTIC_STRUCT.pack(
                pedalctl.NOR_DIAGNOSTIC_MAGIC,
                1,
                2,
                3,
                0x60600000,
                0x200000,
                4,
                -11,
                6,
                0,
                0x60604000,
            )
            return pedalctl.Packet(
                command=request.command,
                payload=payload,
            ).encode()
        if request.command == pedalctl.COMMANDS["begin-image"]:
            return pedalctl.Packet(
                command=request.command,
                flags=request.flags,
                session=self.session,
            ).encode()
        if request.command == pedalctl.COMMANDS["begin-full-flash"]:
            if request.session != pedalctl.FULL_FLASH_UNLOCK:
                raise AssertionError("missing full-flash unlock")
            return pedalctl.Packet(
                command=request.command,
                flags=request.flags,
                session=self.session,
            ).encode()
        if request.session != self.session:
            raise AssertionError("client sent the wrong session")
        if request.sequence != self.expected_sequence:
            raise AssertionError("client sent the wrong sequence")
        self.expected_sequence += 1
        payload = request.payload if request.command == 5 else b""
        offset = request.offset
        if request.command == pedalctl.COMMANDS["erase-full-flash"]:
            offset += pedalctl.FULL_FLASH_ERASE_CHUNK_SIZE
        return pedalctl.Packet(
            command=request.command,
            flags=request.flags,
            session=request.session,
            sequence=request.sequence,
            offset=offset,
            payload=payload,
        ).encode()


class ClientTests(unittest.TestCase):
    def test_full_restore_requires_physically_verified_programming(self):
        info = pedalctl.RecoveryInfo(
            flash_size=0x800000,
            slot_size=0x200000,
            slot_a_offset=0x400000,
            slot_b_offset=0x600000,
            manifest_size=0x1000,
            confirmed_slot=0,
            pending_slot=0xFF,
            selected_slot=0,
            update_phase=0,
            capabilities=(
                pedalctl.CAPABILITY_FULL_FLASH_RAM |
                pedalctl.CAPABILITY_PROGRESSIVE_FULL_ERASE
            ),
            max_chunk_size=32,
        )

        with self.assertRaisesRegex(
            pedalctl.RecoveryError,
            "not physically verified",
        ):
            pedalctl.validate_full_restore_target(info, 0x800000)

        proven = dataclasses.replace(
            info,
            capabilities=(
                info.capabilities |
                pedalctl.CAPABILITY_VERIFIED_FULL_PROGRAM
            ),
        )
        pedalctl.validate_full_restore_target(proven, 0x800000)

    def test_client_sequences_an_update(self):
        transport = FakeTransport()
        client = pedalctl.RecoveryClient(transport)
        info = client.get_info()
        self.assertEqual(info.confirmed_slot, 0)
        diagnostics = client.get_log()
        self.assertEqual(diagnostics.backend_status, -11)
        self.assertEqual(diagnostics.detail, 0x60604000)
        self.assertEqual(client.begin(1, 0x1028), transport.session)
        client.erase()
        client.write(bytes(range(40)))
        self.assertEqual(client.read(4, 8), bytes(8))
        client.finalize()
        client.set_pending()
        client.reboot()

        self.assertEqual(
            [request.command for request in transport.requests],
            [1, 9, 2, 3, 4, 4, 5, 6, 7, 8],
        )
        begin_request = next(
            request
            for request in transport.requests
            if request.command == pedalctl.COMMANDS["begin-image"]
        )
        self.assertEqual(begin_request.offset, 0x1028)
        write_requests = [
            request
            for request in transport.requests
            if request.command == pedalctl.COMMANDS["write-chunk"]
        ]
        self.assertEqual([request.offset for request in write_requests], [0, 32])
        self.assertEqual([len(request.payload) for request in write_requests], [32, 8])

    def test_client_sequences_full_flash_commands(self):
        transport = FakeTransport()
        client = pedalctl.RecoveryClient(transport)
        digest = bytes(range(32))

        self.assertEqual(
            client.begin_full_flash(0x800000, digest),
            transport.session,
        )
        client.erase_full_flash()
        client.write(bytes(range(40)))
        client.finalize_full_flash()
        client.reboot()

        self.assertEqual(
            [request.command for request in transport.requests],
            [10] +
            [11] * (
                0x800000 //
                pedalctl.FULL_FLASH_ERASE_CHUNK_SIZE
            ) +
            [4, 4, 12, 8],
        )
        self.assertTrue(
            all(
                request.flags == pedalctl.FLAG_FULL_FLASH
                for request in transport.requests
            )
        )

        write_requests = [
            request
            for request in transport.requests
            if request.command == pedalctl.COMMANDS["write-chunk"]
        ]
        self.assertEqual(
            [request.offset for request in write_requests],
            [0, 32],
        )

    def test_client_rejects_invalid_write_window(self):
        client = pedalctl.RecoveryClient(FakeTransport())

        with self.assertRaises(pedalctl.RecoveryError):
            client.write(b"data", chunk_size=0)
        with self.assertRaises(pedalctl.RecoveryError):
            client.write(b"data", start_offset=5)


class BoundedEraseTests(unittest.TestCase):
    def erase_offsets(self, **kwargs):
        transport = FakeTransport()
        client = pedalctl.RecoveryClient(transport)
        client.begin_full_flash(0x800000, b"\x00" * 32)
        client.erase_full_flash(**kwargs)
        return [
            request.offset
            for request in transport.requests
            if request.command == pedalctl.COMMANDS["erase-full-flash"]
        ]

    def test_boot_region_handoff_erases_one_chunk_only(self):
        offsets = self.erase_offsets(limit=pedalctl.BOOT_REGION_SIZE)
        self.assertEqual(offsets, [0])

    def test_unbounded_erase_still_covers_the_whole_chip(self):
        offsets = self.erase_offsets()
        self.assertEqual(len(offsets), 128)
        self.assertEqual(offsets[0], 0)
        self.assertEqual(offsets[-1], 0x800000 - 0x10000)

    def test_limit_must_be_a_whole_erase_chunk_inside_the_chip(self):
        for limit in (0, 0x8000, 0x900000, -0x10000):
            with self.assertRaises(pedalctl.RecoveryError):
                self.erase_offsets(limit=limit)


class KnobTransport:
    """Serves READ_KNOBS captures from a scripted list of selector values."""

    def __init__(self, selectors, capabilities=pedalctl.CAPABILITY_KNOB_SAMPLE):
        self.selectors = list(selectors)
        self.capabilities = capabilities
        self.sample_index = 0

    def exchange(self, report: bytes) -> bytes:
        request = pedalctl.Packet.decode(report)
        if request.command == pedalctl.COMMANDS["get-info"]:
            payload = pedalctl.INFO_STRUCT.pack(
                0x800000,
                0x200000,
                0x400000,
                0x600000,
                0x1000,
                0,
                0xFF,
                0,
                0,
                self.capabilities,
                32,
            )
            return pedalctl.Packet(
                command=request.command, payload=payload
            ).encode()
        if request.command != pedalctl.COMMANDS["read-knobs"]:
            raise AssertionError(f"unexpected command {request.command}")
        self.sample_index += 1
        selector = self.selectors[
            min(self.sample_index - 1, len(self.selectors) - 1)
        ]
        payload = pedalctl.KNOB_SAMPLE_STRUCT.pack(
            pedalctl.KNOB_SAMPLE_MAGIC,
            100,
            200,
            selector,
            300,
            selector - 3,
            selector + 3,
            5,
            8,
            9,
            11,
            16,
            1,
            12,
            0,
            self.sample_index,
            0,
        )
        return pedalctl.Packet(
            command=request.command, payload=payload
        ).encode()


class KnobSampleTests(unittest.TestCase):
    def test_decodes_a_capture(self):
        transport = KnobTransport([2103])
        sample = pedalctl.RecoveryClient(transport).read_knobs()
        self.assertEqual(sample.selector, 2103)
        self.assertEqual(sample.values[0], 100)
        self.assertEqual(sample.channels, (5, 8, 9, 11))
        self.assertEqual(sample.selector_spread, 6)
        self.assertEqual(sample.sample_index, 1)
        self.assertTrue(sample.valid)

    def test_rejects_a_foreign_payload(self):
        payload = pedalctl.KNOB_SAMPLE_STRUCT.pack(
            0xDEADBEEF, *([0] * 4), 0, 0, *([0] * 4), 0, 0, 0, 0, 0, 0
        )
        with self.assertRaises(pedalctl.RecoveryError):
            pedalctl.KnobSample.decode(payload)
        with self.assertRaises(pedalctl.RecoveryError):
            pedalctl.KnobSample.decode(payload[:16])

    def test_each_read_is_a_fresh_capture(self):
        transport = KnobTransport([2103, 2110, 2820])
        client = pedalctl.RecoveryClient(transport)
        indices = [client.read_knobs().sample_index for _ in range(3)]
        self.assertEqual(indices, [1, 2, 3])

    def test_capability_guard_rejects_older_firmware(self):
        client = pedalctl.RecoveryClient(KnobTransport([0], capabilities=0xF))
        with self.assertRaises(pedalctl.RecoveryError):
            pedalctl.require_knob_capability(client)

    def test_capture_detent_reports_median_and_envelope(self):
        transport = KnobTransport([2100, 2104, 2102])
        client = pedalctl.RecoveryClient(transport)
        value, low, high = pedalctl.capture_detent(client, 3)
        self.assertEqual(value, 2102)
        self.assertEqual(low, 2097)
        self.assertEqual(high, 2107)


class SelectorCalibrationTests(unittest.TestCase):
    def test_clean_ladder_has_no_warnings(self):
        detents = [40, 620, 1200, 1780, 2360, 2940, 3520, 4090]
        report = pedalctl.selector_calibration_report(detents, [6] * 8)
        self.assertEqual(report["minimum_gap"], 570)
        self.assertEqual(report["warnings"], [])

    def test_duplicate_detents_are_flagged(self):
        detents = [40, 620, 620, 1780, 2360, 2940, 3520, 4090]
        report = pedalctl.selector_calibration_report(detents, [6] * 8)
        self.assertIn("duplicate-detents", report["warnings"])

    def test_gap_swamped_by_noise_is_flagged(self):
        detents = [40, 60, 1200, 1780, 2360, 2940, 3520, 4090]
        report = pedalctl.selector_calibration_report(detents, [40] * 8)
        self.assertIn("gap-not-clear-of-noise", report["warnings"])

    def test_every_warning_has_operator_text(self):
        for key in ("duplicate-detents", "non-monotonic",
                    "gap-not-clear-of-noise"):
            self.assertIn(key, pedalctl.CALIBRATION_WARNINGS)

    def test_rendered_table_is_valid_c(self):
        table = pedalctl.render_selector_table([40, 620, 1200])
        self.assertIn("NCR2_EFFECT_COUNT", table)
        self.assertIn("UINT16_C(620)", table)
        self.assertTrue(table.rstrip().endswith("};"))


if __name__ == "__main__":
    unittest.main()

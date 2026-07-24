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
        self.assertEqual(encoded[4], 1)
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
        if request.command == pedalctl.COMMANDS["begin-image"]:
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
        return pedalctl.Packet(
            command=request.command,
            flags=request.flags,
            session=request.session,
            sequence=request.sequence,
            offset=request.offset,
            payload=payload,
        ).encode()


class ClientTests(unittest.TestCase):
    def test_client_sequences_an_update(self):
        transport = FakeTransport()
        client = pedalctl.RecoveryClient(transport)
        info = client.get_info()
        self.assertEqual(info.confirmed_slot, 0)
        self.assertEqual(client.begin(1), transport.session)
        client.erase()
        client.write(bytes(range(40)))
        self.assertEqual(client.read(4, 8), bytes(8))
        client.finalize()
        client.set_pending()
        client.reboot()

        self.assertEqual(
            [request.command for request in transport.requests],
            [1, 2, 3, 4, 4, 5, 6, 7, 8],
        )
        write_requests = [
            request
            for request in transport.requests
            if request.command == pedalctl.COMMANDS["write-chunk"]
        ]
        self.assertEqual([request.offset for request in write_requests], [0, 32])
        self.assertEqual([len(request.payload) for request in write_requests], [32, 8])


if __name__ == "__main__":
    unittest.main()

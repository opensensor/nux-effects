import ctypes
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
RING_SOURCE = FIRMWARE / "hardware_app" / "src" / "usb_audio_ring.c"
RING_INCLUDE = FIRMWARE / "hardware_app" / "include"


class UsbAudioRing(ctypes.Structure):
    _fields_ = [
        ("write_index", ctypes.c_uint32),
        ("read_index", ctypes.c_uint32),
        ("overrun_frames", ctypes.c_uint32),
        ("underrun_packets", ctypes.c_uint32),
        ("captured_frames", ctypes.c_uint32),
        ("streamed_frames", ctypes.c_uint32),
        ("samples", ctypes.c_int32 * 1024),
    ]


@unittest.skipIf(shutil.which("cc") is None, "host C compiler unavailable")
class UsbAudioRingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        library = Path(cls.directory.name) / "libusb_audio_ring.so"
        subprocess.run(
            [
                "cc", "-std=c17", "-Wall", "-Wextra", "-Werror",
                "-shared", "-fPIC", f"-I{RING_INCLUDE}",
                str(RING_SOURCE), "-o", str(library),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        cls.library = ctypes.CDLL(str(library))
        cls.library.ncr2_usb_audio_ring_reset.argtypes = [
            ctypes.POINTER(UsbAudioRing)
        ]
        cls.library.ncr2_usb_audio_ring_push.argtypes = [
            ctypes.POINTER(UsbAudioRing), ctypes.c_int32
        ]
        cls.library.ncr2_usb_audio_ring_available.argtypes = [
            ctypes.POINTER(UsbAudioRing)
        ]
        cls.library.ncr2_usb_audio_ring_available.restype = ctypes.c_size_t
        cls.library.ncr2_usb_audio_ring_packet.argtypes = [
            ctypes.POINTER(UsbAudioRing),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
        cls.library.ncr2_usb_audio_ring_packet.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.ring = UsbAudioRing()
        self.packet = (ctypes.c_uint8 * 147)()
        self.library.ncr2_usb_audio_ring_reset(ctypes.byref(self.ring))

    def push(self, sample):
        self.library.ncr2_usb_audio_ring_push(
            ctypes.byref(self.ring), sample
        )

    def packet_bytes(self):
        length = self.library.ncr2_usb_audio_ring_packet(
            ctypes.byref(self.ring), self.packet, len(self.packet)
        )
        return bytes(self.packet[:length])

    def test_empty_ring_emits_bounded_silence_without_blocking(self):
        packet = self.packet_bytes()
        self.assertEqual(len(packet), 47 * 3)
        self.assertEqual(packet, bytes(len(packet)))
        self.assertEqual(self.ring.underrun_packets, 1)

    def test_nominal_packet_is_signed_24_bit_little_endian(self):
        self.push(0x12345600)
        self.push(-256)
        for _ in range(46):
            self.push(0)
        packet = self.packet_bytes()
        self.assertEqual(len(packet), 48 * 3)
        self.assertEqual(packet[:6], bytes.fromhex("563412ffffff"))
        self.assertEqual(self.ring.captured_frames, 48)
        self.assertEqual(self.ring.streamed_frames, 48)

    def test_elastic_packet_drains_a_backlog(self):
        for value in range(145):
            self.push(value << 8)
        packet = self.packet_bytes()
        self.assertEqual(len(packet), 49 * 3)
        self.assertEqual(
            self.library.ncr2_usb_audio_ring_available(
                ctypes.byref(self.ring)
            ),
            96,
        )

    def test_full_ring_drops_the_new_usb_copy(self):
        for value in range(1024):
            self.push(value)
        self.assertEqual(self.ring.captured_frames, 1023)
        self.assertEqual(self.ring.overrun_frames, 1)


class UsbAudioIntegrationTests(unittest.TestCase):
    def test_normal_app_owns_a_distinct_guarded_audio_identity(self):
        cmake = (FIRMWARE / "CMakeLists.txt").read_text()
        self.assertIn("NCR2_HARDWARE_APP_USB_AUDIO", cmake)
        self.assertIn("NCR2_OPEN_AUDIO_USB_PID", cmake)
        self.assertIn(
            "normal-app USB audio and Open Recover must use different PIDs",
            cmake,
        )
        self.assertIn(
            "borrowed NUX recovery VID/PID is not permitted for USB audio",
            cmake,
        )

    def test_audio_capture_is_a_nonblocking_tap_of_the_dry_frame(self):
        main = (FIRMWARE / "hardware_app" / "src" / "main.c").read_text()
        dry = "const int32_t dry = capture_frame(&input[base]);"
        tap = "ncr2_usb_audio_capture_push(dry);"
        self.assertIn(dry, main)
        self.assertIn(tap, main)
        self.assertLess(main.index(dry), main.index(tap))
        self.assertIn("ncr2_usb_audio_capture_start();", main)

    def test_descriptor_is_mono_24_bit_48khz_async_capture(self):
        descriptor = (
            FIRMWARE / "hardware_app" / "src" / "usb_audio_descriptor.c"
        ).read_text()
        self.assertIn("NCR2_USB_AUDIO_BYTES_PER_SAMPLE, 24U, 1U", descriptor)
        self.assertIn("0x80U, 0xBBU, 0x00U", descriptor)
        self.assertIn("0x05U", descriptor)
        self.assertIn("NCR2_USB_AUDIO_MAX_PACKET_BYTES", descriptor)
        self.assertIn("static uint8_t g_string_product[]", descriptor)

    def test_usb_interrupt_has_its_own_vector(self):
        startup = (
            FIRMWARE / "hardware_app" / "src" / "startup.S"
        ).read_text()
        self.assertIn("external interrupt 113: USB OTG1", startup)
        self.assertIn(".word USB_OTG1_IRQHandler", startup)

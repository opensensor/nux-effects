import importlib.util
import hashlib
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
EDITOR = ROOT / "host" / "editor"
if str(EDITOR) not in sys.path:
    sys.path.insert(0, str(EDITOR))


def _load(name: str):
    spec = importlib.util.spec_from_file_location(
        name, EDITOR / f"{name}.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


codegen = _load("codegen")
builder = _load("builder")
rt_rules = _load("rt_rules")
hardware_app = _load("hardware_app")
visual_effect = _load("visual_effect")
pedal_audio = _load("pedal_audio")
server = _load("server")


GAIN = codegen.ProgramNode(codegen.VENDOR_OPEN, 1, ((1, 2.0),))
CLIP = codegen.ProgramNode(codegen.VENDOR_OPEN, 2, ((1, 6.0), (2, 0.9)))


def ramp(frames: int, channels: int = 2) -> bytes:
    payload = bytearray()
    for index in range(frames):
        value = (index / (frames - 1)) * 2.0 - 1.0
        payload += struct.pack("<f", value) * channels
    return bytes(payload)


def samples(payload: bytes) -> tuple[float, ...]:
    return struct.unpack(f"<{len(payload) // 4}f", payload)


class CodegenTests(unittest.TestCase):
    def test_float_and_identifier_literals_are_valid_c(self):
        self.assertEqual(codegen.format_float(1.0), "1.0F")
        self.assertEqual(codegen.format_float(0.5), "0.5F")
        self.assertEqual(
            codegen.format_u32(codegen.VENDOR_OPEN), "UINT32_C(0x4F50454E)"
        )
        with self.assertRaises(codegen.CodegenError):
            codegen.format_float(float("nan"))
        with self.assertRaises(codegen.CodegenError):
            codegen.format_u32(-1)
        with self.assertRaises(codegen.CodegenError):
            codegen.validate_identifier("not an identifier")

    def test_only_external_descriptors_can_be_registered(self):
        source = (
            "static const effect_descriptor_t hidden = {0};\n"
            "const effect_descriptor_t visible = {0};\n"
        )
        self.assertEqual(
            codegen.parse_descriptor_symbols(source), ["visible"]
        )
        self.assertEqual(
            codegen.parse_static_descriptor_symbols(source), ["hidden"]
        )

    def test_unbaked_configuration_omits_parameter_values(self):
        program = codegen.ProgramSpec("Demo", 1, (GAIN, CLIP))
        baked = codegen.generate_config(program, bake_parameters=True)
        live = codegen.generate_config(program, bake_parameters=False)

        self.assertIn("2.0F", baked)
        self.assertNotIn("2.0F", live)
        # A control change must not invalidate the cached binary.
        moved = codegen.ProgramSpec(
            "Demo",
            1,
            (
                codegen.ProgramNode(codegen.VENDOR_OPEN, 1, ((1, 3.5),)),
                CLIP,
            ),
        )
        self.assertEqual(
            live, codegen.generate_config(moved, bake_parameters=False)
        )

    def test_exported_program_quotes_shipped_parameter_macros(self):
        program = codegen.ProgramSpec("Boosted Crunch", 5, (GAIN, CLIP))
        catalog = [
            {
                "name": "Basic Gain",
                "vendor_id": codegen.VENDOR_OPEN,
                "effect_id": 1,
                "parameters": [{"parameter_id": 1, "name": "Gain"}],
            },
            {
                "name": "Basic Soft Clip",
                "vendor_id": codegen.VENDOR_OPEN,
                "effect_id": 2,
                "parameters": [
                    {"parameter_id": 1, "name": "Drive"},
                    {"parameter_id": 2, "name": "Level"},
                ],
            },
        ]
        source = codegen.generate_program_source(program, catalog)

        self.assertIn('#include "effects_basic.h"', source)
        self.assertIn("EFFECT_GAIN_PARAMETER_GAIN", source)
        self.assertIn("EFFECT_SOFT_CLIP_PARAMETER_DRIVE", source)
        self.assertIn("EFFECT_OPEN_BASIC_SOFT_CLIP_ID", source)
        self.assertIn("PROGRAM_OPEN_BOOSTED_CRUNCH_ID", source)

    def test_authored_effects_use_derived_macro_names(self):
        effect = {
            "name": "Tape Sat",
            "vendor_id": codegen.VENDOR_OPEN,
            "effect_id": 0x1005,
            "authored": True,
            "parameters": [{"parameter_id": 1, "name": "Amount"}],
        }
        program = codegen.ProgramSpec(
            "Tape",
            2,
            (codegen.ProgramNode(codegen.VENDOR_OPEN, 0x1005, ((1, 0.5),)),),
        )
        source = codegen.generate_program_source(program, [effect])
        header = codegen.generate_effect_header(
            effect, "ncr2_effect_tape_sat", "tape_sat"
        )

        self.assertIn("EFFECT_TAPE_SAT_PARAMETER_AMOUNT", source)
        self.assertIn('#include "effects_tape_sat.h"', source)
        self.assertIn("EFFECT_TAPE_SAT_PARAMETER_AMOUNT", header)
        self.assertIn(
            "extern const effect_descriptor_t ncr2_effect_tape_sat;", header
        )

    def test_export_requires_at_least_one_node(self):
        with self.assertRaises(codegen.CodegenError):
            codegen.generate_program_source(
                codegen.ProgramSpec("Empty", 1, ()), []
            )

    def test_instrument_exports_use_the_shipped_public_identity(self):
        effect = {
            "name": "Instrument · Tonewheel Organ",
            "vendor_id": codegen.VENDOR_OPEN,
            "effect_id": 0x103,
            "parameters": [
                {"parameter_id": 1, "name": "Transformation"},
                {"parameter_id": 3, "name": "Instrument mix"},
            ],
        }
        program = codegen.ProgramSpec(
            "Organ",
            11,
            (
                codegen.ProgramNode(
                    codegen.VENDOR_OPEN,
                    0x103,
                    ((1, 0.4), (3, 0.8)),
                ),
            ),
        )
        source = codegen.generate_program_source(program, [effect])
        self.assertIn('#include "effects_instrument.h"', source)
        self.assertIn("EFFECT_OPEN_TONEWHEEL_ORGAN_ID", source)
        self.assertIn("EFFECT_INSTRUMENT_PARAMETER_TRANSFORMATION", source)


class RealTimeRuleTests(unittest.TestCase):
    def test_allocation_and_formatting_are_flagged(self):
        findings = rt_rules.scan_source(
            "effects_bad.c",
            "void f(void) {\n"
            "    float *b = malloc(64);\n"
            "    printf(\"%f\", *b);\n"
            "}\n",
        )
        self.assertEqual(
            {finding.rule for finding in findings},
            {"allocation", "formatting"},
        )

    def test_const_tables_and_functions_are_not_mutable_state(self):
        clean = (
            "static const effect_parameter_descriptor_t table[] = {0};\n"
            "static uint16_t process(void *c, effect_audio_block_t *b)\n"
            "{\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(rt_rules.scan_source("effects_ok.c", clean), [])

        dirty = "static float history[128];\n"
        self.assertEqual(
            [finding.rule for finding in
             rt_rules.scan_source("effects_bad.c", dirty)],
            ["mutable-static"],
        )

    def test_comments_do_not_raise_findings(self):
        source = "/* malloc is forbidden here */\n// printf too\n"
        self.assertEqual(rt_rules.scan_source("effects_ok.c", source), [])


class ServerHelperTests(unittest.TestCase):
    def test_status_names_are_read_from_the_firmware_headers(self):
        names = server.status_names(
            builder.APP_INCLUDE / "effect_runtime.h", "EFFECT_RUNTIME_"
        )
        self.assertEqual(names[0], "EFFECT_RUNTIME_OK")
        self.assertEqual(names[9], "EFFECT_RUNTIME_PARAMETER_OUT_OF_RANGE")

    def test_source_names_cannot_escape_the_build_directory(self):
        builder.validate_source_name("effects_tape.c")
        for name in ("../escape.c", "effects.h", "a/b.c", ""):
            with self.assertRaises(builder.BuildError):
                builder.validate_source_name(name)

    def test_only_loopback_addresses_are_accepted(self):
        with self.assertRaises(SystemExit):
            server.build_server("0.0.0.0", 8765, Path("/tmp"))


class PedalAudioTests(unittest.TestCase):
    def test_discovers_the_pedal_by_usb_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            card = Path(directory) / "card4"
            card.mkdir()
            (card / "usbid").write_text("cafe:4e58\n")
            (card / "id").write_text("NCR2\n")
            with mock.patch.object(
                pedal_audio.shutil, "which", return_value="/usr/bin/arecord"
            ):
                descriptor = pedal_audio.discover(Path(directory))

        self.assertTrue(descriptor["available"])
        self.assertEqual(descriptor["device"], "hw:4,0")
        self.assertEqual(descriptor["card_id"], "NCR2")
        self.assertEqual(descriptor["sample_rate"], 48000)
        self.assertEqual(pedal_audio.CAPTURE_PERIOD_FRAMES, 256)
        self.assertEqual(pedal_audio.CAPTURE_BUFFER_FRAMES, 1024)

    def test_packed_pcm24_is_converted_and_duplicated(self):
        packed = bytes.fromhex("000080 000000 ffff7f")
        converted = pedal_audio.pcm24_to_float32(packed, 2)
        values = struct.unpack("<6f", converted)

        self.assertEqual(values[0:2], (-1.0, -1.0))
        self.assertEqual(values[2:4], (0.0, 0.0))
        self.assertAlmostEqual(values[4], 1.0, places=6)
        self.assertEqual(values[4], values[5])

    def test_rejects_partial_or_invalid_capture_formats(self):
        with self.assertRaisesRegex(
            pedal_audio.PedalAudioError, "partial PCM24"
        ):
            pedal_audio.pcm24_to_float32(b"\x00", 1)
        with self.assertRaisesRegex(
            pedal_audio.PedalAudioError, "mono or stereo"
        ):
            pedal_audio.pcm24_to_float32(b"\x00\x00\x00", 3)

    def test_program_json_round_trips_into_a_specification(self):
        program = server.program_from_json(
            {
                "name": "Demo",
                "program_id": 3,
                "nodes": [
                    {
                        "vendor_id": codegen.VENDOR_OPEN,
                        "effect_id": 2,
                        "parameters": [
                            {"parameter_id": 1, "value": 4.0},
                        ],
                    }
                ],
            }
        )
        self.assertEqual(program.name, "Demo")
        self.assertEqual(program.nodes[0].parameters, ((1, 4.0),))


class VisualEffectTests(unittest.TestCase):
    def test_palette_and_recipes_expose_valid_bounded_specs(self):
        catalog = visual_effect.describe()
        self.assertGreaterEqual(len(catalog["blocks"]), 10)
        self.assertGreaterEqual(len(catalog["recipes"]), 6)
        self.assertEqual(catalog["limits"]["maximum_blocks"], 10)
        self.assertIn(
            "Envelope swell",
            [block["name"] for block in catalog["blocks"]],
        )
        self.assertNotIn(
            "Bit crusher",
            [block["name"] for block in catalog["blocks"]],
        )
        self.assertIn(
            "Bowed string pad",
            [recipe["name"] for recipe in catalog["recipes"]],
        )
        for recipe in catalog["recipes"]:
            generated = visual_effect.generate_source(
                {
                    "name": recipe["name"],
                    "effect_id": 0x2100,
                    "blocks": recipe["blocks"],
                }
            )
            self.assertIn("const effect_descriptor_t", generated["text"])
            self.assertEqual(
                rt_rules.scan_source(generated["file_name"], generated["text"]),
                [],
            )

    def test_invalid_blocks_and_values_are_rejected(self):
        base = {"name": "Bad", "effect_id": 0x2101}
        with self.assertRaisesRegex(ValueError, "unknown type"):
            visual_effect.generate_source(
                {**base, "blocks": [{"kind": "teleporter", "values": {}}]}
            )
        with self.assertRaisesRegex(ValueError, "outside"):
            visual_effect.generate_source(
                {**base, "blocks": [{"kind": "gain", "values": {"gain": 99}}]}
            )
        with self.assertRaisesRegex(ValueError, "only once"):
            visual_effect.generate_source(
                {
                    **base,
                    "blocks": [
                        {"kind": "short_delay", "values": {}},
                        {"kind": "short_delay", "values": {}},
                    ],
                }
            )


class BrowserBankAndDfuTests(unittest.TestCase):
    def test_page_exposes_four_open_engines_and_bounded_dfu(self):
        page = (EDITOR / "static" / "index.html").read_text()
        script = (EDITOR / "static" / "editor.js").read_text()
        dfu = (EDITOR / "static" / "open_recovery_dfu.mjs").read_text()

        self.assertEqual(page.count("data-open-engine="), 4)
        self.assertIn('id="effect-positions"', page)
        self.assertIn('id="review-notes"', page)
        self.assertIn('id="import-bank"', page)
        self.assertIn('id="level-match"', page)
        self.assertNotIn('id="level-match" checked', page)
        self.assertIn('id="live-input"', page)
        self.assertIn('id="audio-input-device"', page)
        self.assertIn('id="refresh-audio-inputs"', page)
        self.assertIn('id="record-input"', page)
        self.assertIn('id="record-seconds"', page)
        self.assertIn('<option value="60">60 s</option>', page)
        self.assertIn("Recorded guitar / audio input", page)
        self.assertIn('value="guitar"', page)
        self.assertIn('id="dfu-connect"', page)
        self.assertIn('id="dfu-install"', page)
        self.assertIn("OPEN_ENGINE_LAYOUT", script)
        self.assertIn("OPTIONAL_ENGINE_PACKS", script)
        self.assertIn('id="engine-pack-select"', page)
        self.assertIn('id="load-engine-pack"', page)
        self.assertIn('schema: "ncr2-open-engine-bank"', script)
        self.assertIn("version: 3", script)
        self.assertIn("WORKSPACE_STORAGE_KEY", script)
        self.assertIn("clean-guitar-di.wav", script)
        self.assertIn("/api/live/start", script)
        self.assertIn("/api/live/chunk", script)
        self.assertIn("/api/live/pedal-chunk", script)
        self.assertIn("/api/pedal/record", script)
        self.assertIn('"pedal-alsa"', script)
        self.assertIn("direct (recommended)", script)
        self.assertIn("PEDAL_CAPTURE_GAIN_DB = 18", script)
        self.assertIn("pedal +${PEDAL_CAPTURE_GAIN_DB} dB calibrated", script)
        self.assertIn("autoGainControl: false", script)
        self.assertIn("NCR-2 Open Pedal Audio", script)
        self.assertIn("enumerateDevices()", script)
        self.assertIn("deviceId: { exact: deviceId }", script)
        self.assertIn('id="visual-designer"', page)
        self.assertIn('id="visual-palette"', page)
        self.assertIn('id="build-visual-effect"', page)
        self.assertIn("/api/visual-source", script)
        self.assertIn("parameter.default_value", script)
        self.assertIn("canvas.dataset.cssHeight", script)
        self.assertNotIn('Number(canvas.getAttribute("height"));', script)
        self.assertIn("BEGIN_IMAGE: 2", dfu)
        self.assertIn("ERASE_SLOT: 3", dfu)
        self.assertIn("SET_PENDING: 7", dfu)
        self.assertNotIn("BEGIN_FULL_FLASH", dfu)
        self.assertNotIn("ERASE_FULL_FLASH", dfu)

    def test_tailwind_is_local_and_real_guitar_is_bundled(self):
        package = json.loads((ROOT / "package.json").read_text())
        generated = (EDITOR / "static" / "tailwind.generated.css").read_text()
        recording = EDITOR / "static" / "audio" / "clean-guitar-di.wav"
        audio_notes = (recording.parent / "README.md").read_text()

        self.assertEqual(package["devDependencies"]["tailwindcss"], "4.3.3")
        self.assertIn("tailwindcss v4.3.3", generated)
        self.assertIn(".shadow-sm", generated)
        self.assertIn("CC0 1.0", audio_notes)
        with wave.open(str(recording), "rb") as source:
            self.assertEqual(source.getframerate(), 48000)
            self.assertEqual(source.getnchannels(), 1)
            self.assertEqual(source.getsampwidth(), 2)
            self.assertEqual(source.getnframes(), 288000)

    def test_static_server_allows_bounded_audio_subdirectories(self):
        self.assertIsNotNone(
            server.STATIC_NAME_PATTERN.fullmatch("audio/clean-guitar-di.wav")
        )
        self.assertEqual(server.CONTENT_TYPES[".wav"], "audio/wav")
        source = (EDITOR / "server.py").read_text()
        self.assertIn('part in {".", ".."}', source)

    @unittest.skipIf(
        shutil.which("node") is None,
        "Node.js is unavailable for the browser protocol check",
    )
    def test_web_dfu_packet_matches_the_host_protocol(self):
        module = (EDITOR / "static" / "open_recovery_dfu.mjs").as_uri()
        script = (
            f'import {{ encodePacket }} from "{module}";'
            "const packet = encodePacket({"
            "command:4,flags:1,session:0x12345678,sequence:9,offset:64,"
            "payload:new TextEncoder().encode('abc')});"
            "process.stdout.write(Buffer.from(packet).toString('hex'));"
        )
        actual = subprocess.run(
            ["node", "--input-type=module", "-e", script],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        expected = (
            "4e5846580204010078563412090000004000000003000000c2412435"
            "36ad4c626162630000000000000000000000000000000000000000000000"
            "000000000000"
        )
        self.assertEqual(actual, expected)


@unittest.skipIf(
    shutil.which("cc") is None, "host C compiler is unavailable"
)
class HostRenderTests(unittest.TestCase):
    """The editor must preview through the firmware runtime itself."""

    @classmethod
    def setUpClass(cls):
        cls._directory = tempfile.TemporaryDirectory()
        cls.builder = builder.EffectBuilder(Path(cls._directory.name))

    @classmethod
    def tearDownClass(cls):
        cls._directory.cleanup()

    def build(self, sources, program, bake=True):
        symbols = [
            symbol
            for source in sources
            for symbol in codegen.parse_descriptor_symbols(source.text)
        ]
        config = codegen.generate_config(
            program, extra_descriptors=symbols, bake_parameters=bake
        )
        result = self.builder.build(sources, config)
        self.assertTrue(result.ok, result.log)
        return result

    def test_gain_program_matches_the_descriptor_arithmetic(self):
        program = codegen.ProgramSpec("Gain", 1, (GAIN,))
        result = self.build([], program)
        payload = struct.pack("<ff", 0.25, -0.125)
        rendered = self.builder.render(result.binary, payload, 48000, 64, 2)

        self.assertTrue(rendered.ok, rendered.error)
        self.assertEqual(rendered.report["prepare_status"], 0)
        self.assertEqual(
            [round(value, 6) for value in samples(rendered.audio)],
            [0.5, -0.25],
        )

    def test_visual_slapback_compiles_registers_and_renders(self):
        recipe = next(
            item for item in visual_effect.describe()["recipes"]
            if item["id"] == "slapback"
        )
        generated = visual_effect.generate_source(
            {
                "name": "Visual Slapback",
                "effect_id": 0x2201,
                "blocks": recipe["blocks"],
            }
        )
        source = builder.SourceFile(generated["file_name"], generated["text"])
        node = codegen.ProgramNode(codegen.VENDOR_OPEN, 0x2201)
        result = self.build([source], codegen.ProgramSpec("Visual", 9, (node,)))
        catalog = self.builder.catalog(result.binary)
        self.assertTrue(catalog.ok, catalog.error)
        self.assertIn(
            "Visual Slapback",
            [effect["name"] for effect in catalog.report["effects"]],
        )
        rendered = self.builder.render(result.binary, ramp(4096), 48000, 64, 2)
        self.assertTrue(rendered.ok, rendered.error)
        self.assertEqual(rendered.report["nonfinite_samples"], 0)

    def test_runtime_overrides_equal_baked_program_values(self):
        program = codegen.ProgramSpec("Chain", 2, (GAIN, CLIP))
        payload = ramp(512)

        baked = self.builder.render(
            self.build([], program, bake=True).binary,
            payload,
            48000,
            64,
            2,
        )
        live = self.builder.render(
            self.build([], program, bake=False).binary,
            payload,
            48000,
            64,
            2,
            overrides=[(0, 1, 2.0), (1, 1, 6.0), (1, 2, 0.9)],
        )
        self.assertEqual(samples(baked.audio), samples(live.audio))

    def test_stateful_live_process_accepts_sequential_native_chunks(self):
        result = self.build(
            [], codegen.ProgramSpec("Live gain", 12, (GAIN,)), bake=False
        )
        live = self.builder.start_live(
            result.binary,
            48000,
            64,
            1,
            overrides=[(0, 1, 2.0)],
        )
        try:
            first = live.process_audio(struct.pack("<ff", 0.2, -0.1))
            second = live.process_audio(struct.pack("<ff", 0.05, -0.25))
        finally:
            live.stop()
        self.assertEqual(
            [round(value, 6) for value in samples(first + second)],
            [0.4, -0.2, 0.1, -0.5],
        )

    def test_out_of_range_parameters_are_rejected_by_the_runtime(self):
        program = codegen.ProgramSpec("Gain", 1, (GAIN,))
        result = self.build([], program, bake=False)
        rendered = self.builder.render(
            result.binary,
            ramp(64),
            48000,
            64,
            2,
            overrides=[(0, 1, 99.0)],
        )
        self.assertEqual(rendered.report["parameter_status"], 9)
        self.assertEqual(
            rendered.report["parameter_results"][0]["status"], 9
        )

    def test_out_of_range_program_values_never_reach_an_effect(self):
        program = codegen.ProgramSpec(
            "Bad",
            3,
            (codegen.ProgramNode(codegen.VENDOR_OPEN, 1, ((1, 99.0),)),),
        )
        result = self.build([], program)
        rendered = self.builder.render(result.binary, ramp(64), 48000, 64, 2)

        self.assertEqual(rendered.report["prepare_status"], 2)
        self.assertEqual(rendered.report["instance_count"], 0)
        self.assertEqual(rendered.audio, b"")

    def test_authored_effect_registers_and_renders(self):
        text = codegen.effect_template("Tape Sat", 0x1005)
        sources = [builder.SourceFile("effects_tape_sat.c", text)]
        program = codegen.ProgramSpec(
            "Tape",
            4,
            (codegen.ProgramNode(codegen.VENDOR_OPEN, 0x1005, ((1, 0.5),)),),
        )
        result = self.build(sources, program)

        catalog = self.builder.catalog(result.binary)
        self.assertTrue(catalog.ok, catalog.error)
        names = [effect["name"] for effect in catalog.report["effects"]]
        self.assertEqual(names[:2], ["Basic Gain", "Basic Soft Clip"])
        self.assertIn("Instrument · Bowed Ensemble", names)
        self.assertIn("Instrument · Bell / Marimba", names)
        self.assertEqual(names[-1], "Tape Sat")

        verified = self.builder.verify(result.binary, 48000, 64, 2)
        self.assertEqual(verified.report["catalog_status"], 0)
        self.assertEqual(verified.report["library_status"], 0)
        self.assertEqual(verified.report["prepare_status"], 0)

        rendered = self.builder.render(
            result.binary, ramp(256), 48000, 64, 2
        )
        self.assertEqual(rendered.report["nonfinite_samples"], 0)
        self.assertEqual(rendered.report["instance_count"], 1)

    def test_duplicate_effect_identity_is_reported_not_silently_used(self):
        text = codegen.effect_template("Clone", 1)
        sources = [builder.SourceFile("effects_clone.c", text)]
        program = codegen.ProgramSpec("Clone", 5, ())
        result = self.build(sources, program)
        catalog = self.builder.catalog(result.binary)

        self.assertEqual(
            catalog.report["registry_status"], 3
        )  # EFFECT_RUNTIME_DUPLICATE_EFFECT

    def test_broken_source_fails_with_located_diagnostics(self):
        sources = [
            builder.SourceFile(
                "effects_broken.c",
                'const effect_descriptor_t broken = { .missing = 1 };\n',
            )
        ]
        config = codegen.generate_config(
            codegen.ProgramSpec("Broken", 6, ()),
            extra_descriptors=["broken"],
        )
        result = self.builder.build(sources, config)

        self.assertFalse(result.ok)
        self.assertTrue(
            any(
                diagnostic.level == "error"
                and diagnostic.file == "effects_broken.c"
                for diagnostic in result.diagnostics
            ),
            result.log,
        )

    def test_exported_sources_compile_against_the_firmware_tree(self):
        text = codegen.effect_template("Tape Sat", 0x1005)
        catalog = [
            {
                "name": "Basic Soft Clip",
                "vendor_id": codegen.VENDOR_OPEN,
                "effect_id": 2,
                "parameters": [{"parameter_id": 1, "name": "Drive"}],
            },
            {
                "name": "Tape Sat",
                "vendor_id": codegen.VENDOR_OPEN,
                "effect_id": 0x1005,
                "authored": True,
                "parameters": [
                    {"parameter_id": 1, "name": "Amount"},
                    {"parameter_id": 2, "name": "Mix"},
                ],
            },
        ]
        program = codegen.ProgramSpec(
            "Tape Crunch",
            7,
            (
                codegen.ProgramNode(codegen.VENDOR_OPEN, 2, ((1, 6.0),)),
                codegen.ProgramNode(
                    codegen.VENDOR_OPEN, 0x1005, ((1, 0.3),)
                ),
            ),
        )
        files = {
            "src/effects_tape_sat.c": text,
            "include/effects_tape_sat.h": codegen.generate_effect_header(
                catalog[1], "ncr2_effect_tape_sat", "tape_sat"
            ),
            "src/programs_tape_crunch.c": codegen.generate_program_source(
                program, catalog
            ),
        }

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "src").mkdir()
            (base / "include").mkdir()
            for name, content in files.items():
                (base / name).write_text(content)
            for name in (
                "src/effects_tape_sat.c",
                "src/programs_tape_crunch.c",
            ):
                completed = subprocess.run(
                    [
                        shutil.which("cc"),
                        "-std=c17",
                        "-Wall",
                        "-Wextra",
                        "-Wno-unused-const-variable",
                        "-c",
                        "-o",
                        str(base / "out.o"),
                        f"-I{builder.APP_INCLUDE}",
                        f"-I{base / 'include'}",
                        str(base / name),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    completed.returncode, 0, completed.stderr
                )

    def test_catalog_reports_the_shipped_parameter_schema(self):
        result = self.build([], codegen.ProgramSpec("Empty", 8, ()))
        catalog = self.builder.catalog(result.binary)
        clip = next(
            effect
            for effect in catalog.report["effects"]
            if effect["name"] == "Basic Soft Clip"
        )
        drive = clip["parameters"][0]

        self.assertEqual(drive["name"], "Drive")
        self.assertEqual(drive["minimum"], 1.0)
        self.assertEqual(drive["maximum"], 32.0)
        self.assertEqual(json.loads(json.dumps(drive))["default_value"], 1.0)

    def test_instrument_tracker_follows_expressive_input_pitch(self):
        node = codegen.ProgramNode(
            codegen.VENDOR_OPEN,
            0x103,
            ((1, 0.1), (2, 0.55), (3, 1.0), (4, 0.003)),
        )
        result = self.build(
            [],
            codegen.ProgramSpec("Tonewheel", 10, (node,)),
        )
        for expected in (110.0, 220.0, 440.0, 880.0):
            frames = 48000
            payload = struct.pack(
                f"<{frames}f",
                *(
                    0.2 * math.sin(2.0 * math.pi * expected * frame / 48000.0)
                    for frame in range(frames)
                ),
            )
            rendered = self.builder.render(
                result.binary,
                payload,
                48000,
                64,
                1,
            )
            self.assertTrue(rendered.ok, rendered.error)
            tail = samples(rendered.audio)[-8192:]
            best_frequency = 0.0
            best_power = -1.0
            lower = max(65, int(expected * 0.8))
            upper = min(1400, int(expected * 1.2))
            for frequency in range(lower, upper + 1, 2):
                real = 0.0
                imaginary = 0.0
                for index, value in enumerate(tail):
                    angle = 2.0 * math.pi * frequency * index / 48000.0
                    real += value * math.cos(angle)
                    imaginary -= value * math.sin(angle)
                power = real * real + imaginary * imaginary
                if power > best_power:
                    best_power = power
                    best_frequency = float(frequency)
            self.assertLess(
                abs(best_frequency - expected) / expected,
                0.04,
                (expected, best_frequency),
            )

    def test_instrument_defaults_are_level_safe_on_real_clean_guitar(self):
        recording = EDITOR / "static" / "audio" / "clean-guitar-di.wav"
        with wave.open(str(recording), "rb") as source:
            pcm = source.readframes(source.getnframes())
        integer = struct.unpack(f"<{len(pcm) // 2}h", pcm)
        payload = struct.pack(
            f"<{len(integer)}f", *(sample / 32768.0 for sample in integer)
        )
        source_samples = [sample / 32768.0 for sample in integer]
        fingerprints = set()
        outputs = {}
        for effect_id in range(0x100, 0x108):
            node = codegen.ProgramNode(codegen.VENDOR_OPEN, effect_id)
            result = self.build(
                [], codegen.ProgramSpec("Instrument", effect_id, (node,))
            )
            rendered = self.builder.render(
                result.binary, payload, 48000, 64, 1
            )
            self.assertTrue(rendered.ok, rendered.error)
            report = rendered.report
            self.assertEqual(report["nonfinite_samples"], 0)
            self.assertLessEqual(report["peak_output"], 0.7)
            change = 20.0 * math.log10(
                report["rms_output"] / report["rms_input"]
            )
            self.assertGreaterEqual(change, -9.0, effect_id)
            self.assertLessEqual(change, 3.0, effect_id)
            fingerprints.add(hashlib.sha256(rendered.audio).digest())
            output = samples(rendered.audio)
            outputs[effect_id] = output

            # A nominally wet instrument voice must not merely be a scaled
            # copy of the pickup.  Remove the best-fit dry component and
            # require the resynthesized residual to dominate the result.
            dry = source_samples[24000:]
            wet = output[24000:]
            dry_energy = sum(value * value for value in dry)
            wet_energy = sum(value * value for value in wet)
            dry_scale = sum(
                original * processed
                for original, processed in zip(dry, wet)
            ) / dry_energy
            residual_energy = sum(
                (processed - dry_scale * original) ** 2
                for original, processed in zip(dry, wet)
            )
            self.assertGreater(
                math.sqrt(residual_energy / wet_energy),
                0.90,
                effect_id,
            )
        self.assertEqual(len(fingerprints), 8)

        # Different labels also need different spectra.  This specifically
        # guards the old Organ/Brass pair, which differed numerically but was
        # effectively the same harmonic oscillator by ear.
        for first_id, first in outputs.items():
            first_tail = first[24000:]
            for second_id, second in outputs.items():
                if second_id <= first_id:
                    continue
                second_tail = second[24000:]
                cross = sum(
                    a * b for a, b in zip(first_tail, second_tail)
                )
                normalization = math.sqrt(
                    sum(a * a for a in first_tail) *
                    sum(b * b for b in second_tail)
                )
                self.assertLess(
                    abs(cross / normalization),
                    0.97,
                    (first_id, second_id),
                )

    def test_transformation_macro_has_an_unmistakable_dry_to_voice_sweep(self):
        frames = 48000
        payload = struct.pack(
            f"<{frames}f",
            *(
                0.16 * math.sin(2.0 * math.pi * 220.0 * frame / 48000.0)
                for frame in range(frames)
            ),
        )
        program = codegen.ProgramSpec(
            "Organ macro",
            13,
            (codegen.ProgramNode(codegen.VENDOR_OPEN, 0x103),),
        )
        result = self.build([], program, bake=False)
        dryish = self.builder.render(
            result.binary,
            payload,
            48000,
            64,
            1,
            overrides=[(0, 1, 0.0), (0, 3, 1.0), (0, 4, 0.003)],
        )
        transformed = self.builder.render(
            result.binary,
            payload,
            48000,
            64,
            1,
            overrides=[(0, 1, 1.0), (0, 3, 1.0), (0, 4, 0.003)],
        )
        dark = self.builder.render(
            result.binary,
            payload,
            48000,
            64,
            1,
            overrides=[
                (0, 1, 1.0), (0, 2, 0.0), (0, 3, 1.0), (0, 4, 0.003)
            ],
        )
        bright = self.builder.render(
            result.binary,
            payload,
            48000,
            64,
            1,
            overrides=[
                (0, 1, 1.0), (0, 2, 1.0), (0, 3, 1.0), (0, 4, 0.003)
            ],
        )
        source_samples = samples(payload)

        def difference(rendered):
            return sum(
                abs(output - original)
                for output, original in zip(
                    samples(rendered.audio)[-12000:],
                    source_samples[-12000:],
                )
            )

        self.assertGreater(
            difference(transformed), difference(dryish) * 5.0
        )
        character_difference = math.sqrt(sum(
            (low - high) ** 2
            for low, high in zip(
                samples(dark.audio)[-12000:],
                samples(bright.audio)[-12000:],
            )
        ) / 12000)
        self.assertGreater(character_difference, 0.02)


@unittest.skipUnless(
    hardware_app.available(),
    "firmware/hardware_app is not present",
)
class HardwareAppExtractionTests(unittest.TestCase):
    """The preset adapter must track main.c or fail loudly."""

    def test_all_four_by_eight_presets_are_described(self):
        described = hardware_app.describe()
        self.assertTrue(described["available"])
        self.assertEqual(described["sample_rate"], 48000)
        self.assertEqual(
            [preset["name"] for preset in described["presets"]],
            list(hardware_app.PRESET_NAMES),
        )
        self.assertIn("String Ensemble", hardware_app.PRESET_NAMES)
        self.assertNotIn("Bit Crush", hardware_app.PRESET_NAMES)
        self.assertEqual(len(set(hardware_app.preset_keys())), 32)

    def test_extraction_keeps_the_dsp_and_drops_the_hardware(self):
        extracted = hardware_app.extract_dsp()

        for name in hardware_app.REQUIRED_FUNCTIONS:
            self.assertIn(name, extracted)
        for name in hardware_app.PRESET_ENUM_NAMES:
            self.assertIn(name, extracted)
        # Nothing that would drag in the SDK or a linker script.
        for forbidden in (
            "IOMUXC",
            "CLOCK_",
            "WDOG1",
            "codec_probe",
            'section(".sdram_bss")',
        ):
            self.assertNotIn(forbidden, extracted)

    def test_a_renamed_dsp_function_fails_loudly(self):
        text = hardware_app.MAIN_SOURCE.read_text().replace(
            "process_selected_effect", "process_renamed_effect"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "main.c"
            path.write_text(text)
            with self.assertRaises(hardware_app.ExtractionError):
                hardware_app.extract_dsp(path)

    def test_a_missing_preset_fails_loudly(self):
        text = hardware_app.MAIN_SOURCE.read_text().replace(
            "NCR2_EFFECT_WHAMMY_FUZZ", "NCR2_EFFECT_RETIRED"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "main.c"
            path.write_text(text)
            with self.assertRaises(hardware_app.ExtractionError):
                hardware_app.extract_dsp(path)

    def test_presets_are_not_exportable_as_effect_chains(self):
        program = codegen.ProgramSpec(
            "Wah",
            1,
            (
                codegen.ProgramNode(
                    hardware_app.VENDOR_OPEN,
                    hardware_app.EFFECT_ID_BASE + 5,
                ),
            ),
        )
        self.assertTrue(server.program_uses_hardware_app(program))
        self.assertFalse(
            server.program_uses_hardware_app(
                codegen.ProgramSpec("Gain", 1, (GAIN,))
            )
        )


@unittest.skipIf(
    shutil.which("cc") is None, "host C compiler is unavailable"
)
@unittest.skipUnless(
    hardware_app.available(),
    "firmware/hardware_app is not present",
)
class HardwareAppRenderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._directory = tempfile.TemporaryDirectory()
        cls.builder = builder.EffectBuilder(Path(cls._directory.name))
        cls.adapter = builder.SourceFile(
            hardware_app.ADAPTER_SOURCE_NAME, hardware_app.adapter_source()
        )

    @classmethod
    def tearDownClass(cls):
        cls._directory.cleanup()

    def build(self, nodes):
        program = codegen.ProgramSpec("Preset", 1, tuple(nodes))
        config = codegen.generate_config(
            program,
            extra_descriptors=hardware_app.descriptor_symbols(),
            bake_parameters=False,
        )
        return self.builder.build([self.adapter], config)

    def node(self, index):
        return codegen.ProgramNode(
            hardware_app.VENDOR_OPEN, hardware_app.EFFECT_ID_BASE + index
        )

    def test_every_preset_registers_with_the_firmware_names(self):
        result = self.build([])
        self.assertTrue(result.ok, result.log)
        catalog = self.builder.catalog(result.binary)

        self.assertEqual(catalog.report["registry_status"], 0)
        names = [effect["name"] for effect in catalog.report["effects"]]
        self.assertEqual(names[10:], list(hardware_app.PRESET_NAMES))
        self.assertEqual(
            names[2:10],
            [
                "Instrument · Bowed Ensemble",
                "Instrument · Cello",
                "Instrument · Violin",
                "Instrument · Tonewheel Organ",
                "Instrument · Clarinet",
                "Instrument · Synth Brass",
                "Instrument · Synth Bass",
                "Instrument · Bell / Marimba",
            ],
        )
        preset = catalog.report["effects"][10]
        self.assertEqual(
            [parameter["name"] for parameter in preset["parameters"]],
            ["Amount", "Character", "Level"],
        )
        self.assertEqual(preset["parameters"][0]["maximum"], 4095.0)

    def test_each_preset_produces_distinct_finite_audio(self):
        payload = ramp(4096)
        rendered = {}
        for index, name in enumerate(hardware_app.PRESET_NAMES):
            result = self.build([self.node(index)])
            self.assertTrue(result.ok, result.log)
            run = self.builder.render(
                result.binary,
                payload,
                48000,
                64,
                2,
                overrides=[(0, 1, 3000.0), (0, 2, 2048.0), (0, 3, 4095.0)],
            )
            self.assertTrue(run.ok, run.error)
            self.assertEqual(run.report["prepare_status"], 0, name)
            self.assertEqual(run.report["nonfinite_samples"], 0, name)
            self.assertLessEqual(run.report["peak_output"], 1.0, name)
            rendered[name] = samples(run.audio)

        for name, values in rendered.items():
            self.assertTrue(any(value != 0.0 for value in values), name)
        self.assertEqual(len(set(rendered.values())), len(rendered))

    def test_a_second_preset_in_one_chain_is_refused(self):
        result = self.build([self.node(0), self.node(1)])
        self.assertTrue(result.ok, result.log)
        run = self.builder.render(result.binary, ramp(256), 48000, 64, 2)

        # File-scope DSP state cannot be shared by two instances.
        self.assertEqual(
            run.report["prepare_status"], 9
        )  # PROGRAM_RUNTIME_CHAIN_ERROR
        self.assertEqual(run.report["instance_count"], 0)

    def test_a_non_device_sample_rate_is_refused(self):
        result = self.build([self.node(0)])
        self.assertTrue(result.ok, result.log)
        run = self.builder.render(result.binary, ramp(256), 44100, 64, 2)

        self.assertEqual(run.report["prepare_status"], 9)
        self.assertEqual(run.audio, b"")


if __name__ == "__main__":
    unittest.main()

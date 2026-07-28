#!/usr/bin/env python3
"""Local design and preview server for open NCR-2 effects.

The page this serves composes programs from the real effect registry,
lets an effect be written in C, and previews the result by compiling and
running `firmware/app`'s own runtime on this machine. It never opens a USB
device, never writes to the repository, and never flashes anything.

It does compile and execute C that the browser page submits, which is the
point of the tool and also its risk: run it on loopback only, on a machine
you trust, and treat it as a development tool rather than a service.

    python3 host/editor/server.py --port 8765
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import struct
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Sequence
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))

import builder as builder_module  # noqa: E402
import codegen  # noqa: E402
import hardware_app  # noqa: E402
import rt_rules  # noqa: E402


ROOT = Path(__file__).resolve().parents[2]
STATIC = Path(__file__).resolve().parent / "static"
DEFAULT_CACHE = ROOT / "build" / "effect-editor"

LOOPBACK_HOSTS = frozenset({"127.0.0.1", "::1", "localhost"})
MAX_REQUEST_BYTES = 64 * 1024 * 1024
MAX_INPUT_CACHE_BYTES = 96 * 1024 * 1024
FRAME_HEADER = struct.Struct("<I")
STATIC_NAME_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+$")
ENUM_PATTERN = re.compile(
    r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*(\d+)\s*,?\s*$",
    re.MULTILINE,
)

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".svg": "image/svg+xml",
}


def status_names(header: Path, prefix: str) -> dict[int, str]:
    """Read status enum names from a firmware header.

    Reading them beats copying them: the page shows the same names the
    firmware uses even after the enums grow.
    """
    text = header.read_text()
    names: dict[int, str] = {}
    for match in ENUM_PATTERN.finditer(text):
        name, value = match.group(1), int(match.group(2))
        if name.startswith(prefix):
            names[value] = name
    return names


def program_from_json(payload: dict[str, Any]) -> codegen.ProgramSpec:
    nodes = []
    for entry in payload.get("nodes", ()):
        parameters = tuple(
            (int(item["parameter_id"]), float(item["value"]))
            for item in entry.get("parameters", ())
        )
        nodes.append(
            codegen.ProgramNode(
                vendor_id=int(entry.get("vendor_id", codegen.VENDOR_OPEN)),
                effect_id=int(entry["effect_id"]),
                parameters=parameters,
            )
        )
    return codegen.ProgramSpec(
        name=str(payload.get("name") or "Untitled"),
        program_id=int(payload.get("program_id", 1)),
        nodes=tuple(nodes),
        vendor_id=int(payload.get("vendor_id", codegen.VENDOR_OPEN)),
        bank_name=str(payload.get("bank_name") or "Editor Bank"),
        bank_id=int(payload.get("bank_id", 1)),
    )


def program_uses_hardware_app(program: codegen.ProgramSpec) -> bool:
    """True when a program contains a hardware-application preset.

    Those presets are previewable but not exportable: they are not
    registry effects on the device.
    """
    presets = hardware_app.preset_keys()
    return any(
        (node.vendor_id, node.effect_id) in presets
        for node in program.nodes
    )


def sources_from_json(payload: Any) -> list[builder_module.SourceFile]:
    sources = []
    for entry in payload or ():
        sources.append(
            builder_module.SourceFile(
                name=builder_module.validate_source_name(
                    str(entry["name"])
                ),
                text=str(entry["text"]),
            )
        )
    return sources


class EditorSession:
    """Serialized access to the compiler and the rendered-input cache."""

    def __init__(self, builder: builder_module.EffectBuilder) -> None:
        self.builder = builder
        self.lock = threading.Lock()
        self._inputs: dict[str, bytes] = {}
        self._order: list[str] = []

    def remember_input(self, payload: bytes) -> str:
        digest = hashlib.sha256(payload).hexdigest()
        with self.lock:
            if digest not in self._inputs:
                self._inputs[digest] = payload
                self._order.append(digest)
                total = sum(len(item) for item in self._inputs.values())
                while total > MAX_INPUT_CACHE_BYTES and len(self._order) > 1:
                    oldest = self._order.pop(0)
                    total -= len(self._inputs.pop(oldest, b""))
        return digest

    def recall_input(self, digest: str) -> bytes | None:
        with self.lock:
            return self._inputs.get(digest)

    def describe_effects(
        self,
        catalog: dict[str, Any],
        authored: set[tuple[int, int]],
    ) -> list[dict[str, Any]]:
        presets = hardware_app.preset_keys()
        effects = []
        for effect in catalog.get("effects", ()):
            entry = dict(effect)
            key = (int(entry["vendor_id"]), int(entry["effect_id"]))
            entry["hardware_app"] = key in presets
            entry["authored"] = key in authored and key not in presets
            effects.append(entry)
        return effects

    def build_session(
        self,
        sources: list[builder_module.SourceFile],
        program: codegen.ProgramSpec,
        bake_parameters: bool,
        extra_sources: list[builder_module.SourceFile] | None = None,
        extra_symbols: Sequence[str] = (),
    ) -> dict[str, Any]:
        """Compile authored sources plus any generated adapters.

        `extra_sources` is machine-generated (today, the hardware
        application's preset adapter). It registers effects and it
        compiles, but it is never treated as the user's own code. Its
        descriptors are named by `extra_symbols` rather than parsed:
        generated units may define them through macros.
        """
        generated = list(extra_sources or ())
        symbols: list[str] = []
        static_only: list[dict[str, Any]] = []
        for source in sources:
            found = codegen.parse_descriptor_symbols(source.text)
            symbols.extend(found)
            if not found:
                for symbol in codegen.parse_static_descriptor_symbols(
                    source.text
                ):
                    static_only.append(
                        {"file": source.name, "symbol": symbol}
                    )

        symbols.extend(extra_symbols)

        config = codegen.generate_config(
            program,
            extra_descriptors=symbols,
            bake_parameters=bake_parameters,
        )
        with self.lock:
            build = self.builder.build(sources + generated, config)
        return {
            "build": build,
            "config": config,
            "symbols": symbols,
            "static_only": static_only,
        }


class EditorHandler(BaseHTTPRequestHandler):
    server_version = "OpenEffectEditor/1.0"
    protocol_version = "HTTP/1.1"

    session: EditorSession
    expected_host: str
    effect_status_names: dict[int, str]
    program_status_names: dict[int, str]

    def log_message(self, format: str, *args: Any) -> None:
        sys.stderr.write(
            f"{self.address_string()} {format % args}\n"
        )

    # Request plumbing -----------------------------------------------

    def _origin_allowed(self) -> bool:
        host = (self.headers.get("Host") or "").strip()
        if host and host != self.expected_host:
            return False
        origin = self.headers.get("Origin")
        if origin is None:
            return True
        return origin in (
            f"http://{self.expected_host}",
            f"http://localhost:{self.expected_host.rsplit(':', 1)[-1]}",
        )

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        if length < 0 or length > MAX_REQUEST_BYTES:
            raise ValueError("request too large")
        return self.rfile.read(length) if length else b""

    def _send(
        self,
        payload: bytes,
        content_type: str,
        status: int = 200,
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(payload)

    def _send_json(self, payload: Any, status: int = 200) -> None:
        self._send(
            json.dumps(payload).encode(),
            "application/json",
            status,
        )

    def _send_frame(self, header: Any, payload: bytes) -> None:
        encoded = json.dumps(header).encode()
        body = io.BytesIO()
        body.write(FRAME_HEADER.pack(len(encoded)))
        body.write(encoded)
        body.write(payload)
        self._send(body.getvalue(), "application/octet-stream")

    # Routing --------------------------------------------------------

    def do_GET(self) -> None:  # noqa: N802 - http.server API
        if not self._origin_allowed():
            self._send_json({"error": "cross-origin request denied"}, 403)
            return
        path = self.path.split("?", 1)[0]
        try:
            if path == "/":
                self._send_static("index.html")
            elif path.startswith("/static/"):
                self._send_static(path[len("/static/"):])
            elif path == "/api/session":
                self._handle_session()
            elif path.startswith("/api/template"):
                self._handle_template()
            else:
                self._send_json({"error": "not found"}, 404)
        except (codegen.CodegenError, ValueError) as error:
            self._send_json({"error": str(error)}, 400)

    def do_POST(self) -> None:  # noqa: N802 - http.server API
        if not self._origin_allowed():
            self._send_json({"error": "cross-origin request denied"}, 403)
            return
        path = self.path.split("?", 1)[0]
        try:
            if path == "/api/build":
                self._handle_build()
            elif path == "/api/verify":
                self._handle_verify()
            elif path == "/api/render":
                self._handle_render()
            elif path == "/api/export":
                self._handle_export()
            else:
                self._send_json({"error": "not found"}, 404)
        except (
            builder_module.BuildError,
            codegen.CodegenError,
            hardware_app.ExtractionError,
            KeyError,
            TypeError,
            ValueError,
        ) as error:
            self._send_json({"error": str(error)}, 400)

    def _send_static(self, name: str) -> None:
        if STATIC_NAME_PATTERN.match(name) is None:
            self._send_json({"error": "not found"}, 404)
            return
        path = STATIC / name
        if not path.is_file():
            self._send_json({"error": "not found"}, 404)
            return
        self._send(
            path.read_bytes(),
            CONTENT_TYPES.get(path.suffix, "application/octet-stream"),
        )

    # Handlers -------------------------------------------------------

    def _handle_session(self) -> None:
        self._send_json(
            {
                "compiler": self.session.builder.compiler,
                "compiler_available": self.session.builder.available,
                "vendor_open": codegen.VENDOR_OPEN,
                "effect_status_names": {
                    str(value): name
                    for value, name in self.effect_status_names.items()
                },
                "program_status_names": {
                    str(value): name
                    for value, name in self.program_status_names.items()
                },
                "sample_rates": [44100, 48000, 96000],
                "block_frames": [16, 32, 64, 128, 256],
                "template": codegen.effect_template("My Effect", 0x1001),
                "rules": [
                    {"id": rule.identifier, "message": rule.message}
                    for rule in rt_rules.RULES
                ],
                "hardware_app": hardware_app.describe(),
            }
        )

    def _handle_template(self) -> None:
        query = parse_qs(urlparse(self.path).query)
        name = (query.get("name") or ["My Effect"])[0].strip()
        raw_id = (query.get("effect_id") or ["4097"])[0]
        try:
            effect_id = int(raw_id, 0)
        except ValueError:
            effect_id = 0x1001
        if not name or len(name) > 48:
            raise ValueError("effect names are 1 to 48 characters")
        identifier = codegen.slug(name)
        self._send_json(
            {
                "name": name,
                "effect_id": effect_id,
                "file_name": f"effects_{identifier}.c",
                "symbol": f"ncr2_effect_{identifier}",
                "text": codegen.effect_template(name, effect_id),
            }
        )

    def _session_payload(
        self,
        request: dict[str, Any],
        bake_parameters: bool,
    ) -> tuple[dict[str, Any], list[builder_module.SourceFile]]:
        sources = sources_from_json(request.get("sources"))
        program = program_from_json(request.get("program") or {})
        generated: list[builder_module.SourceFile] = []
        generated_symbols: list[str] = []
        if request.get("include_hardware_app") and hardware_app.available():
            generated.append(
                builder_module.SourceFile(
                    hardware_app.ADAPTER_SOURCE_NAME,
                    hardware_app.adapter_source(),
                )
            )
            generated_symbols.extend(hardware_app.descriptor_symbols())
        result = self.session.build_session(
            sources, program, bake_parameters, generated, generated_symbols
        )
        result["program"] = program
        return result, sources

    def _handle_build(self) -> None:
        request = json.loads(self._read_body() or b"{}")
        result, sources = self._session_payload(request, False)
        build: builder_module.BuildResult = result["build"]
        payload: dict[str, Any] = {
            "build": build.as_dict(),
            "lint": rt_rules.scan_sources(sources),
            "symbols": result["symbols"],
            "static_only": result["static_only"],
            "config": result["config"],
        }
        if build.ok and build.binary is not None:
            with self.session.lock:
                catalog = self.session.builder.catalog(build.binary)
            if catalog.ok:
                authored = self._authored_keys(catalog.report, sources)
                payload["catalog"] = self.session.describe_effects(
                    catalog.report, authored
                )
                payload["registry_status"] = catalog.report.get(
                    "registry_status"
                )
            else:
                payload["error"] = catalog.error
        self._send_json(payload)

    def _authored_keys(
        self,
        catalog: dict[str, Any],
        sources: list[builder_module.SourceFile],
    ) -> set[tuple[int, int]]:
        """Effect keys that came from this session's own sources."""
        known = set(codegen.BUILTIN_EFFECT_MACROS) | set(
            hardware_app.preset_keys()
        )
        keys = set()
        for effect in catalog.get("effects", ()):
            key = (int(effect["vendor_id"]), int(effect["effect_id"]))
            if key not in known and sources:
                keys.add(key)
        return keys

    def _handle_verify(self) -> None:
        request = json.loads(self._read_body() or b"{}")
        result, sources = self._session_payload(request, True)
        build: builder_module.BuildResult = result["build"]
        payload: dict[str, Any] = {
            "build": build.as_dict(),
            "lint": rt_rules.scan_sources(sources),
            "config": result["config"],
        }
        if build.ok and build.binary is not None:
            with self.session.lock:
                verified = self.session.builder.verify(
                    build.binary,
                    int(request.get("sample_rate", 48000)),
                    int(request.get("block_frames", 64)),
                    int(request.get("channels", 2)),
                )
            payload["verify"] = verified.as_dict()
        self._send_json(payload)

    def _handle_render(self) -> None:
        body = self._read_body()
        if len(body) < FRAME_HEADER.size:
            raise ValueError("truncated render request")
        (header_length,) = FRAME_HEADER.unpack_from(body, 0)
        start = FRAME_HEADER.size
        request = json.loads(body[start:start + header_length].decode())
        audio = body[start + header_length:]

        digest = request.get("input_sha256")
        if audio:
            digest = self.session.remember_input(audio)
        elif digest:
            cached = self.session.recall_input(str(digest))
            if cached is None:
                self._send_frame({"status": "unknown-input"}, b"")
                return
            audio = cached
        else:
            raise ValueError("render request has no input audio")

        result, sources = self._session_payload(request, False)
        build: builder_module.BuildResult = result["build"]
        if not build.ok or build.binary is None:
            self._send_frame(
                {
                    "status": "build-failed",
                    "build": build.as_dict(),
                    "lint": rt_rules.scan_sources(sources),
                },
                b"",
            )
            return

        overrides = [
            (int(item[0]), int(item[1]), float(item[2]))
            for item in request.get("overrides", ())
        ]
        with self.session.lock:
            rendered = self.session.builder.render(
                build.binary,
                audio,
                int(request.get("sample_rate", 48000)),
                int(request.get("block_frames", 64)),
                int(request.get("channels", 2)),
                overrides,
            )
        self._send_frame(
            {
                "status": "ok" if rendered.ok else "render-failed",
                "input_sha256": digest,
                "report": rendered.report,
                "error": rendered.error,
                "build": build.as_dict(),
                "lint": rt_rules.scan_sources(sources),
            },
            rendered.audio,
        )

    def _handle_export(self) -> None:
        request = json.loads(self._read_body() or b"{}")
        sources = sources_from_json(request.get("sources"))
        program = program_from_json(request.get("program") or {})
        catalog = request.get("catalog") or []
        files: list[dict[str, str]] = []

        if program_uses_hardware_app(program):
            raise ValueError(
                "the hardware application's presets are fixed-point code "
                "inside firmware/hardware_app/src/main.c, not registry "
                "effects; a program using them cannot be expressed as an "
                "effect chain. Preview them here, then change main.c."
            )

        for source in sources:
            symbols = codegen.parse_descriptor_symbols(source.text)
            if not symbols:
                continue
            name = codegen.slug(Path(source.name).stem)
            if name.startswith("effects_"):
                name = name[len("effects_"):]
            effect = next(
                (
                    entry
                    for entry in catalog
                    if entry.get("authored")
                    and codegen.slug(entry["name"]) == name
                ),
                None,
            )
            if effect is None:
                effect = next(
                    (
                        entry
                        for entry in catalog
                        if entry.get("authored")
                    ),
                    None,
                )
            files.append(
                {
                    "path": f"firmware/app/src/effects_{name}.c",
                    "text": source.text,
                }
            )
            if effect is not None:
                files.append(
                    {
                        "path": f"firmware/app/include/effects_{name}.h",
                        "text": codegen.generate_effect_header(
                            effect, symbols[0], name
                        ),
                    }
                )

        files.append(
            {
                "path": "firmware/app/src/programs_"
                f"{codegen.slug(program.name)}.c",
                "text": codegen.generate_program_source(program, catalog),
            }
        )
        self._send_json({"files": files})


def build_server(
    host: str,
    port: int,
    cache_directory: Path,
    compiler: str | None = None,
) -> ThreadingHTTPServer:
    if host not in LOOPBACK_HOSTS:
        raise SystemExit(
            "the editor compiles and runs C submitted by the page; it "
            "binds loopback addresses only"
        )
    builder = builder_module.EffectBuilder(cache_directory, compiler)
    session = EditorSession(builder)

    class BoundHandler(EditorHandler):
        pass

    BoundHandler.session = session
    BoundHandler.expected_host = f"{host}:{port}"
    BoundHandler.effect_status_names = status_names(
        builder_module.APP_INCLUDE / "effect_runtime.h",
        "EFFECT_RUNTIME_",
    )
    BoundHandler.program_status_names = status_names(
        builder_module.APP_INCLUDE / "program_runtime.h",
        "PROGRAM_RUNTIME_",
    )
    return ThreadingHTTPServer((host, port), BoundHandler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument(
        "--cache-directory",
        type=Path,
        default=DEFAULT_CACHE,
        help="where compiled preview binaries are cached",
    )
    parser.add_argument("--compiler", default=None)
    parser.add_argument(
        "--open",
        action="store_true",
        help="open the page in a browser once the server is listening",
    )
    arguments = parser.parse_args(argv)

    server = build_server(
        arguments.host,
        arguments.port,
        arguments.cache_directory,
        arguments.compiler,
    )
    address = f"http://{arguments.host}:{arguments.port}/"
    handler_class: Any = server.RequestHandlerClass
    if not handler_class.session.builder.available:
        print(
            "warning: no host C compiler found; previews are unavailable",
            file=sys.stderr,
        )
    print(f"open effect editor on {address}")
    print(
        "this server compiles and runs C from the page on this machine; "
        "keep it local",
        file=sys.stderr,
    )
    if arguments.open:
        webbrowser.open(address)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

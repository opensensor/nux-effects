"""Compile and run the real firmware effect runtime on the host.

The editor never re-implements an effect in another language. Every
preview links `firmware/app/src/effect_runtime.c`,
`firmware/app/src/program_runtime.c`, shipped effects, and authored sources
with the session's authored sources and a generated configuration unit, so
what the page plays is what the chain computes.

The compiled program executes locally with a wall-clock timeout and CPU,
address-space, and file-size limits. It is host tooling: it proves DSP
behaviour and catches descriptor mistakes early. It says nothing about
on-target timing, and it is not part of any firmware image.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import resource
import select
import shutil
import subprocess
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


ROOT = Path(__file__).resolve().parents[2]
APP_INCLUDE = ROOT / "firmware" / "app" / "include"
APP_SOURCE = ROOT / "firmware" / "app" / "src"
# The hardware-application preset adapter needs the executed factory
# audio contract (sample rate and slot count) to match the device.
FACTORY_INCLUDE = ROOT / "firmware" / "factory_slot" / "include"
HARNESS_SOURCE = Path(__file__).resolve().parent / "native" / "editor_host.c"

FIRMWARE_SOURCES = (
    APP_SOURCE / "effect_runtime.c",
    APP_SOURCE / "program_runtime.c",
    APP_SOURCE / "effects_basic.c",
    APP_SOURCE / "effects_instrument.c",
)

COMPILE_FLAGS = (
    "-std=c17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-fno-common",
    "-fdiagnostics-color=never",
    "-fmax-errors=25",
)

SOURCE_NAME_PATTERN = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]*\.c$")
DIAGNOSTIC_PATTERN = re.compile(
    r"^(?P<file>[^:\n]+):(?P<line>\d+):(?:(?P<column>\d+):)?\s+"
    r"(?P<level>error|warning|note):\s+(?P<message>.*)$"
)

DEFAULT_COMPILE_TIMEOUT = 30.0
DEFAULT_RUN_TIMEOUT = 20.0
DEFAULT_ADDRESS_SPACE_BYTES = 1024 * 1024 * 1024
DEFAULT_CPU_SECONDS = 15
STREAM_MAGIC = 0x45564C31
STREAM_READY = struct.Struct("<IIII")
STREAM_LENGTH = struct.Struct("<I")
EFFECT_RUNTIME_OK_VALUE = 0


class BuildError(RuntimeError):
    """Raised when the host toolchain cannot be used at all."""


@dataclass(frozen=True)
class SourceFile:
    name: str
    text: str


@dataclass(frozen=True)
class Diagnostic:
    file: str
    line: int | None
    column: int | None
    level: str
    message: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "file": self.file,
            "line": self.line,
            "column": self.column,
            "level": self.level,
            "message": self.message,
        }


@dataclass(frozen=True)
class BuildResult:
    ok: bool
    key: str
    binary: Path | None
    diagnostics: tuple[Diagnostic, ...]
    log: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "key": self.key,
            "diagnostics": [
                diagnostic.as_dict() for diagnostic in self.diagnostics
            ],
            "log": self.log,
        }


@dataclass(frozen=True)
class RunResult:
    ok: bool
    report: dict[str, Any]
    audio: bytes
    error: str | None = None

    def as_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "report": self.report,
            "error": self.error,
        }


class LiveProcess:
    """Persistent native preview retaining firmware DSP state per chunk."""

    def __init__(
        self,
        process: subprocess.Popen[bytes],
        channels: int,
        block_frames: int,
    ) -> None:
        self.process = process
        self.channels = channels
        self.block_frames = block_frames

    def _read_exact(self, size: int, timeout: float) -> bytes:
        if self.process.stdout is None:
            raise BuildError("live preview has no output pipe")
        deadline = time.monotonic() + timeout
        chunks: list[bytes] = []
        completed = 0
        while completed < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BuildError("live preview chunk timed out")
            ready, _, _ = select.select(
                [self.process.stdout.fileno()], [], [], remaining
            )
            if not ready:
                raise BuildError("live preview chunk timed out")
            chunk = os.read(
                self.process.stdout.fileno(), size - completed
            )
            if not chunk:
                raise BuildError("live preview process stopped")
            chunks.append(chunk)
            completed += len(chunk)
        return b"".join(chunks)

    def process_audio(self, audio: bytes, timeout: float = 2.0) -> bytes:
        frame_bytes = 4 * self.channels
        if not audio or len(audio) % frame_bytes != 0:
            raise BuildError("live audio is not complete float32 frames")
        if self.process.poll() is not None or self.process.stdin is None:
            raise BuildError("live preview process is not running")
        try:
            self.process.stdin.write(STREAM_LENGTH.pack(len(audio)))
            self.process.stdin.write(audio)
            self.process.stdin.flush()
            (length,) = STREAM_LENGTH.unpack(
                self._read_exact(STREAM_LENGTH.size, timeout)
            )
            if length != len(audio):
                raise BuildError("live preview returned the wrong chunk size")
            return self._read_exact(length, timeout)
        except (BrokenPipeError, OSError) as error:
            raise BuildError(f"live preview transport failed: {error}") from error

    def stop(self) -> None:
        if self.process.poll() is None and self.process.stdin is not None:
            try:
                self.process.stdin.write(STREAM_LENGTH.pack(0))
                self.process.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
        try:
            self.process.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=1.0)
        for pipe in (
            self.process.stdin,
            self.process.stdout,
            self.process.stderr,
        ):
            if pipe is not None:
                pipe.close()


def parse_diagnostics(log: str) -> tuple[Diagnostic, ...]:
    found: list[Diagnostic] = []
    for line in log.splitlines():
        match = DIAGNOSTIC_PATTERN.match(line.strip())
        if match is None:
            continue
        column = match.group("column")
        found.append(
            Diagnostic(
                file=Path(match.group("file")).name,
                line=int(match.group("line")),
                column=int(column) if column is not None else None,
                level=match.group("level"),
                message=match.group("message"),
            )
        )
    return tuple(found)


def validate_source_name(name: str) -> str:
    if SOURCE_NAME_PATTERN.match(name) is None or "/" in name:
        raise BuildError(f"invalid source file name: {name!r}")
    return name


def _apply_limits() -> None:  # pragma: no cover - runs in the child
    resource.setrlimit(
        resource.RLIMIT_CPU,
        (DEFAULT_CPU_SECONDS, DEFAULT_CPU_SECONDS),
    )
    resource.setrlimit(
        resource.RLIMIT_AS,
        (DEFAULT_ADDRESS_SPACE_BYTES, DEFAULT_ADDRESS_SPACE_BYTES),
    )
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))


class EffectBuilder:
    """Compile editor sessions and run the resulting host binary."""

    def __init__(
        self,
        cache_directory: Path,
        compiler: str | None = None,
    ) -> None:
        self.cache_directory = Path(cache_directory)
        self.compiler = compiler or shutil.which("cc") or ""

    @property
    def available(self) -> bool:
        return bool(self.compiler)

    def _require_compiler(self) -> str:
        if not self.available:
            raise BuildError(
                "no host C compiler found; install cc to preview effects"
            )
        return self.compiler

    def _key(
        self,
        sources: Sequence[SourceFile],
        config: str,
    ) -> str:
        digest = hashlib.sha256()
        digest.update(self.compiler.encode())
        digest.update("\0".join(COMPILE_FLAGS).encode())
        for path in (*FIRMWARE_SOURCES, HARNESS_SOURCE):
            digest.update(path.name.encode())
            digest.update(path.read_bytes())
        for include in sorted(APP_INCLUDE.glob("*.h")):
            digest.update(include.name.encode())
            digest.update(include.read_bytes())
        for source in sources:
            digest.update(source.name.encode())
            digest.update(source.text.encode())
        digest.update(config.encode())
        return digest.hexdigest()

    def build(
        self,
        sources: Sequence[SourceFile],
        config: str,
        timeout: float = DEFAULT_COMPILE_TIMEOUT,
    ) -> BuildResult:
        compiler = self._require_compiler()
        key = self._key(sources, config)
        directory = self.cache_directory / key
        binary = directory / "editor_host"
        record = directory / "build.json"

        if binary.exists() and record.exists():
            stored = json.loads(record.read_text())
            return BuildResult(
                ok=True,
                key=key,
                binary=binary,
                diagnostics=tuple(
                    Diagnostic(**entry) for entry in stored["diagnostics"]
                ),
                log=stored.get("log", ""),
            )

        directory.mkdir(parents=True, exist_ok=True)
        written: list[Path] = []
        for source in sources:
            path = directory / validate_source_name(source.name)
            path.write_text(source.text)
            written.append(path)
        config_path = directory / "editor_config.c"
        config_path.write_text(config)
        written.append(config_path)

        command = [
            compiler,
            *COMPILE_FLAGS,
            f"-I{APP_INCLUDE}",
            f"-I{FACTORY_INCLUDE}",
            *(str(path) for path in FIRMWARE_SOURCES),
            str(HARNESS_SOURCE),
            *(str(path) for path in written),
            "-o",
            str(binary),
            "-lm",
        ]
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return BuildResult(
                ok=False,
                key=key,
                binary=None,
                diagnostics=(),
                log=f"compilation exceeded {timeout:g}s",
            )

        log = (completed.stderr or "") + (completed.stdout or "")
        diagnostics = parse_diagnostics(log)
        ok = completed.returncode == 0 and binary.exists()
        if ok:
            record.write_text(
                json.dumps(
                    {
                        "diagnostics": [
                            diagnostic.as_dict()
                            for diagnostic in diagnostics
                        ],
                        "log": log,
                    }
                )
            )
        else:
            binary.unlink(missing_ok=True)
        return BuildResult(
            ok=ok,
            key=key,
            binary=binary if ok else None,
            diagnostics=diagnostics,
            log=log,
        )

    def _run(
        self,
        binary: Path,
        arguments: Sequence[str],
        stdin_bytes: bytes = b"",
        timeout: float = DEFAULT_RUN_TIMEOUT,
    ) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [str(binary), *arguments],
            input=stdin_bytes,
            capture_output=True,
            timeout=timeout,
            check=False,
            preexec_fn=_apply_limits,
        )

    def catalog(self, binary: Path) -> RunResult:
        try:
            completed = self._run(binary, ["--catalog"])
        except subprocess.TimeoutExpired:
            return RunResult(False, {}, b"", "catalog run timed out")
        if completed.returncode != 0:
            return RunResult(
                False,
                {},
                b"",
                completed.stderr.decode(errors="replace").strip()
                or f"catalog run failed ({completed.returncode})",
            )
        return RunResult(
            True,
            json.loads(completed.stdout.decode()),
            b"",
        )

    def verify(
        self,
        binary: Path,
        sample_rate: int,
        block_frames: int,
        channels: int,
    ) -> RunResult:
        arguments = [
            "--verify",
            "--sample-rate",
            str(int(sample_rate)),
            "--block-frames",
            str(int(block_frames)),
            "--channels",
            str(int(channels)),
        ]
        try:
            completed = self._run(binary, arguments)
        except subprocess.TimeoutExpired:
            return RunResult(False, {}, b"", "verification timed out")
        if completed.returncode != 0 or not completed.stdout:
            return RunResult(
                False,
                {},
                b"",
                completed.stderr.decode(errors="replace").strip()
                or f"verification failed ({completed.returncode})",
            )
        return RunResult(True, json.loads(completed.stdout.decode()), b"")

    def render(
        self,
        binary: Path,
        audio: bytes,
        sample_rate: int,
        block_frames: int,
        channels: int,
        overrides: Sequence[tuple[int, int, float]] = (),
        timeout: float = DEFAULT_RUN_TIMEOUT,
    ) -> RunResult:
        arguments = [
            "--sample-rate",
            str(int(sample_rate)),
            "--block-frames",
            str(int(block_frames)),
            "--channels",
            str(int(channels)),
        ]
        for node, parameter_id, value in overrides:
            arguments.extend(
                ["--set", f"{int(node)}:{int(parameter_id)}:{float(value)!r}"]
            )
        try:
            completed = self._run(
                binary,
                arguments,
                stdin_bytes=audio,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return RunResult(
                False,
                {},
                b"",
                f"render exceeded {timeout:g}s and was stopped",
            )

        stderr = completed.stderr.decode(errors="replace").strip()
        if completed.returncode != 0:
            return RunResult(
                False,
                {},
                b"",
                stderr or f"render failed (exit {completed.returncode})",
            )
        report: dict[str, Any] = {}
        error: str | None = None
        if stderr:
            try:
                report = json.loads(stderr.splitlines()[-1])
            except json.JSONDecodeError:
                error = stderr
        return RunResult(
            ok=error is None,
            report=report,
            audio=completed.stdout,
            error=error,
        )

    def start_live(
        self,
        binary: Path,
        sample_rate: int,
        block_frames: int,
        channels: int,
        overrides: Sequence[tuple[int, int, float]] = (),
        timeout: float = 3.0,
    ) -> LiveProcess:
        arguments = [
            "--stream",
            "--sample-rate",
            str(int(sample_rate)),
            "--block-frames",
            str(int(block_frames)),
            "--channels",
            str(int(channels)),
        ]
        for node, parameter_id, value in overrides:
            arguments.extend(
                ["--set", f"{int(node)}:{int(parameter_id)}:{float(value)!r}"]
            )
        process = subprocess.Popen(
            [str(binary), *arguments],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            preexec_fn=_apply_limits,
        )
        live = LiveProcess(process, int(channels), int(block_frames))
        try:
            magic, status, ready_channels, ready_frames = STREAM_READY.unpack(
                live._read_exact(STREAM_READY.size, timeout)
            )
            if magic != STREAM_MAGIC:
                raise BuildError("live preview returned an invalid handshake")
            if status != EFFECT_RUNTIME_OK_VALUE:
                raise BuildError(
                    f"firmware runtime rejected live preview ({status})"
                )
            if ready_channels != int(channels) or ready_frames != int(block_frames):
                raise BuildError("live preview format handshake did not match")
            return live
        except Exception:
            live.stop()
            raise

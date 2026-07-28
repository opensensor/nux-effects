"""Compile and run the real firmware effect runtime on the host.

The editor never re-implements an effect in another language. Every
preview links `firmware/app/src/effect_runtime.c`,
`firmware/app/src/program_runtime.c`, and `firmware/app/src/effects_basic.c`
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
import re
import resource
import shutil
import subprocess
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

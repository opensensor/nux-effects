"""Heuristic real-time rule scan for authored effect sources.

`docs/app/EFFECT_RUNTIME.md` states what an effect callback may not do:
allocate, touch flash, use USB or logging, format text, or lock. A text
scan cannot prove any of that. It can catch the mistakes people actually
make when moving desktop DSP into an audio callback, early enough to
matter, which is all this module claims to do.

Findings are advisory. They never block a build, and a clean scan is not
evidence that an effect is real-time safe.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any, Callable, Iterable


STATIC_PATTERN = re.compile(r"^[ \t]*static[ \t]+(?P<rest>.*)$")


def _is_mutable_static(line: str) -> bool:
    """Flag file-scope `static` objects that are not read-only.

    Function definitions and `static const` tables are the two shapes an
    effect legitimately uses, so both are ignored.
    """
    match = STATIC_PATTERN.match(line)
    if match is None:
        return False
    rest = match.group("rest").strip()
    head = rest.split("=", 1)[0]
    if re.match(r"^const\b", head) or re.search(r"\bconst\b", head):
        return False
    if "(" in head:
        return False
    return head.rstrip().endswith(";") or "=" in rest


@dataclass(frozen=True)
class Rule:
    identifier: str
    pattern: re.Pattern[str] | None
    message: str
    predicate: Callable[[str], bool] | None = None

    def matches(self, line: str) -> bool:
        if self.predicate is not None:
            return self.predicate(line)
        return (
            self.pattern is not None
            and self.pattern.search(line) is not None
        )


@dataclass(frozen=True)
class Finding:
    rule: str
    file: str
    line: int
    text: str
    message: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "rule": self.rule,
            "file": self.file,
            "line": self.line,
            "text": self.text,
            "message": self.message,
        }


RULES: tuple[Rule, ...] = (
    Rule(
        "allocation",
        re.compile(
            r"\b(malloc|calloc|realloc|free|aligned_alloc|strdup)\s*\("
        ),
        "the runtime performs no dynamic allocation; keep state in the "
        "effect context",
    ),
    Rule(
        "formatting",
        re.compile(
            r"\b(printf|fprintf|sprintf|snprintf|puts|fputs|perror)\s*\("
        ),
        "no logging or formatting in the audio path",
    ),
    Rule(
        "file-access",
        re.compile(r"\b(fopen|fread|fwrite|fclose|open|read|write)\s*\("),
        "no file, flash, or device access from an effect callback",
    ),
    Rule(
        "locking",
        re.compile(r"\b(pthread_\w+|sem_\w+|std::mutex)\b"),
        "no locking in the audio path",
    ),
    Rule(
        "blocking",
        re.compile(r"\b(sleep|usleep|nanosleep|select|poll)\s*\("),
        "no blocking calls in the audio path",
    ),
    Rule(
        "host-header",
        re.compile(r"#\s*include\s*<(stdio|stdlib|string|time)\.h>"),
        "host headers are unavailable on the device; use only "
        "stdint.h, stddef.h, and math.h",
    ),
    Rule(
        "mutable-static",
        None,
        "mutable file-scope state is shared by every instance; move it "
        "into the effect context",
        predicate=lambda line: _is_mutable_static(line),
    ),
    Rule(
        "double-precision",
        re.compile(r"\b(sin|cos|tan|exp|log|pow|sqrt|tanh|fabs)\s*\("),
        "double-precision maths has no hardware unit on the Cortex-M7 "
        "single-precision FPU; prefer the f-suffixed calls",
    ),
)

COMMENT_PATTERN = re.compile(r"//.*$|/\*.*?\*/", re.DOTALL)


def _strip_comments(text: str) -> list[str]:
    """Blank out comments while preserving line numbering."""
    def replace(match: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    return COMMENT_PATTERN.sub(replace, text).splitlines()


def scan_source(name: str, text: str) -> list[Finding]:
    findings: list[Finding] = []
    for number, line in enumerate(_strip_comments(text), start=1):
        for rule in RULES:
            if rule.matches(line):
                findings.append(
                    Finding(
                        rule=rule.identifier,
                        file=name,
                        line=number,
                        text=line.strip()[:160],
                        message=rule.message,
                    )
                )
    return findings


def scan_sources(sources: Iterable[Any]) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    for source in sources:
        for finding in scan_source(source.name, source.text):
            findings.append(finding.as_dict())
    return findings

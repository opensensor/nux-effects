#!/usr/bin/env python3
"""Fetch the exact MCUXpresso components pinned by firmware/sdk-lock.json."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = ROOT / "firmware" / "sdk-lock.json"
DEFAULT_DESTINATION = ROOT / "third_party" / "mcux-sdk-workspace"


class FetchError(RuntimeError):
    pass


def run(command: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return result.stdout


def load_lock(path: Path) -> list[dict[str, str]]:
    raw = json.loads(path.read_text())
    if raw.get("schema_version") != 1:
        raise FetchError("unsupported SDK lock schema")
    components = raw.get("components")
    if not isinstance(components, list) or not components:
        raise FetchError("SDK lock has no components")
    seen_names: set[str] = set()
    seen_paths: set[str] = set()
    for component in components:
        required = {"name", "path", "url", "revision"}
        if set(component) != required:
            raise FetchError(
                f"component fields differ for {component.get('name')!r}"
            )
        if component["name"] in seen_names:
            raise FetchError(f"duplicate component {component['name']!r}")
        if component["path"] in seen_paths:
            raise FetchError(f"duplicate component path {component['path']!r}")
        revision = component["revision"]
        if len(revision) != 40 or any(
            character not in "0123456789abcdef" for character in revision
        ):
            raise FetchError(
                f"component {component['name']} is not pinned to a SHA"
            )
        target = Path(component["path"])
        if target.is_absolute() or ".." in target.parts:
            raise FetchError(f"unsafe component path {target}")
        seen_names.add(component["name"])
        seen_paths.add(component["path"])
    return components


def clone_component(component: dict[str, str], destination: Path) -> None:
    target = destination / component["path"]
    if target.exists():
        raise FetchError(f"refusing to replace existing path: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    print(f"fetching {component['name']} -> {target}")
    run(
        [
            "git",
            "clone",
            "--filter=blob:none",
            "--no-checkout",
            component["url"],
            str(target),
        ]
    )
    run(["git", "checkout", "--detach", component["revision"]], cwd=target)
    actual = run(["git", "rev-parse", "HEAD"], cwd=target).strip()
    if actual != component["revision"]:
        raise FetchError(
            f"{component['name']} checked out {actual}, "
            f"expected {component['revision']}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--lock",
        type=Path,
        default=DEFAULT_LOCK,
    )
    parser.add_argument(
        "--destination",
        type=Path,
        default=DEFAULT_DESTINATION,
    )
    args = parser.parse_args()

    lock_path = args.lock.expanduser().resolve()
    destination = args.destination.expanduser().resolve()
    try:
        components = load_lock(lock_path)
        if destination.exists() and any(destination.iterdir()):
            raise FetchError(
                f"destination is not empty; refusing to modify it: "
                f"{destination}"
            )
        destination.mkdir(parents=True, exist_ok=True)
        for component in components:
            clone_component(component, destination)
    except (FetchError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    print(f"SDK workspace ready: {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())


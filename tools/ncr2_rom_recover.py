#!/usr/bin/env python3
"""Recover NCR-2 NOR through the immutable i.MX RT1051 ROM downloader.

The RT1051 ROM (USB 1fc9:0130) cannot program FlexSPI NOR directly. This
tool loads NXP's RT1052 flashloader into OCRAM, waits for its 15a2:0073 USB
personality, configures the W25Q64, and can restore a complete 8 MiB image.

No flash mutation is possible without the ``flash`` command and ``--execute``.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
VENDOR_ROOT = ROOT / "tools" / "vendor" / "nxp-mcubootutility"
SDPHOST = VENDOR_ROOT / "linux-amd64" / "sdphost"
BLHOST = VENDOR_ROOT / "linux-amd64" / "blhost"
FLASHLOADER = VENDOR_ROOT / "MIMXRT1052" / "ivt_flashloader.bin"

ROM_USB = (0x1FC9, 0x0130)
FLASHLOADER_USB = (0x15A2, 0x0073)
FLASHLOADER_ADDRESS = 0x20208200
CONFIG_ADDRESS = 0x20202000
FLEXSPI_NOR_MEMORY_ID = 9
WINBOND_W25Q_CONFIG_OPTION = 0xC0000007
FLASH_BASE = 0x60000000
FLASH_SIZE = 0x00800000
IVT_OFFSET = 0x1000

VENDOR_SHA256 = {
    SDPHOST: "671621fad603cf593e4776d6a1a8a33a2146abbc5c2ae3c1646048b19d6f2263",
    BLHOST: "4f3cb30dc6727626c3118e5c378a4ca345185196cf2876ddff2fb740c2d40d6e",
    FLASHLOADER: "c0af776dc30f7312c99eb39c8458c7ba3b28c89f964b9e809d68d232209019a3",
}


class RecoveryError(RuntimeError):
    """A recovery precondition or command failed."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_vendor_assets() -> None:
    for path, expected in VENDOR_SHA256.items():
        if not path.is_file():
            raise RecoveryError(f"required recovery asset is missing: {path}")
        actual = sha256_file(path)
        if actual != expected:
            raise RecoveryError(
                f"recovery asset hash mismatch for {path}: {actual} != {expected}"
            )


def validate_flash_image(path: Path, expected_sha256: str | None = None) -> str:
    if not path.is_file():
        raise RecoveryError(f"flash image does not exist: {path}")
    if path.stat().st_size != FLASH_SIZE:
        raise RecoveryError(
            f"flash image must be exactly {FLASH_SIZE} bytes, "
            f"got {path.stat().st_size}"
        )
    with path.open("rb") as stream:
        header = stream.read(4)
        stream.seek(IVT_OFFSET)
        ivt = stream.read(4)
    if header != b"FCFB":
        raise RecoveryError("flash image does not begin with an NXP FCFB")
    if len(ivt) != 4 or ivt[0] != 0xD1 or ivt[3] not in range(0x40, 0x44):
        raise RecoveryError("flash image has no plausible i.MX RT IVT at 0x1000")

    actual = sha256_file(path)
    if expected_sha256 is not None and actual.lower() != expected_sha256.lower():
        raise RecoveryError(
            f"flash image hash mismatch: {actual} != {expected_sha256.lower()}"
        )
    return actual


def usb_device_path(vid: int, pid: int) -> Path | None:
    sysfs = Path("/sys/bus/usb/devices")
    if not sysfs.is_dir():
        return None
    for device in sysfs.iterdir():
        try:
            device_vid = int((device / "idVendor").read_text().strip(), 16)
            device_pid = int((device / "idProduct").read_text().strip(), 16)
            bus = int((device / "busnum").read_text().strip())
            number = int((device / "devnum").read_text().strip())
        except (FileNotFoundError, PermissionError, ValueError):
            continue
        if (device_vid, device_pid) == (vid, pid):
            return Path(f"/dev/bus/usb/{bus:03d}/{number:03d}")
    return None


def require_usb_access(vid: int, pid: int, label: str) -> Path:
    path = usb_device_path(vid, pid)
    if path is None:
        raise RecoveryError(f"{label} {vid:04x}:{pid:04x} is not connected")
    if not os.access(path, os.R_OK | os.W_OK):
        raise RecoveryError(
            f"no read/write access to {path}; run:\n  sudo chmod a+rw {path}"
        )
    return path


def run(command: list[str], *, capture: bool = False) -> str:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    output = result.stdout or ""
    if capture and output:
        print(output, end="" if output.endswith("\n") else "\n")
    if result.returncode:
        raise RecoveryError(
            f"command failed with exit status {result.returncode}: {command[0]}"
        )
    return output


def sdp_command(*arguments: str, capture: bool = False) -> str:
    vid, pid = ROM_USB
    return run(
        [str(SDPHOST), "-u", f"0x{vid:04x},0x{pid:04x}", "-V", "--"]
        + list(arguments),
        capture=capture,
    )


def blhost_command(
    *arguments: str, capture: bool = False, timeout_ms: int = 10_000
) -> str:
    vid, pid = FLASHLOADER_USB
    return run(
        [
            str(BLHOST),
            "-u",
            f"0x{vid:04x},0x{pid:04x}",
            "-t",
            str(timeout_ms),
            "--",
        ]
        + list(arguments),
        capture=capture,
    )


def rom_status() -> None:
    path = require_usb_access(*ROM_USB, "RT1051 ROM downloader")
    print(f"ROM downloader: {path}")
    sdp_command("error-status")


def load_flashloader(wait_seconds: float) -> Path:
    rom_status()
    sdp_command("write-file", hex(FLASHLOADER_ADDRESS), str(FLASHLOADER))
    sdp_command("jump-address", hex(FLASHLOADER_ADDRESS))

    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        path = usb_device_path(*FLASHLOADER_USB)
        if path is not None:
            print(f"RAM flashloader: {path}")
            if not os.access(path, os.R_OK | os.W_OK):
                raise RecoveryError(
                    "RAM flashloader enumerated, but its new USB node needs "
                    f"permission; run:\n  sudo chmod a+rw {path}\n"
                    "Then use the flash subcommand without power-cycling."
                )
            return path
        time.sleep(0.2)
    raise RecoveryError("RAM flashloader did not enumerate before timeout")


def configure_nor() -> None:
    path = require_usb_access(*FLASHLOADER_USB, "RT1052 RAM flashloader")
    print(f"RAM flashloader: {path}")
    blhost_command(
        "fill-memory",
        hex(CONFIG_ADDRESS),
        "4",
        hex(WINBOND_W25Q_CONFIG_OPTION),
        "word",
    )
    blhost_command(
        "configure-memory", str(FLEXSPI_NOR_MEMORY_ID), hex(CONFIG_ADDRESS)
    )
    attributes = blhost_command(
        "get-property", "25", str(FLEXSPI_NOR_MEMORY_ID), capture=True
    )
    required = (
        "Start Address = 0x60000000",
        "Total Size = 8 MB",
        "Page Size = 256 bytes",
        "Sector Size = 4 KB",
    )
    missing = [field for field in required if field not in attributes]
    if missing:
        raise RecoveryError(
            "unexpected FlexSPI NOR geometry; missing: " + ", ".join(missing)
        )


def flash_image(
    image: Path,
    *,
    execute: bool,
    verify: bool,
    expected_sha256: str | None,
) -> None:
    image_sha256 = validate_flash_image(image, expected_sha256)
    print(f"image:  {image}")
    print(f"size:   {FLASH_SIZE} bytes")
    print(f"sha256: {image_sha256}")
    print(
        f"plan:   erase and write {FLASH_BASE:#010x}.."
        f"{FLASH_BASE + FLASH_SIZE - 1:#010x}"
    )
    if not execute:
        print("dry run only; pass --execute to mutate NOR")
        return

    configure_nor()
    print(
        "Erasing the complete NOR. USB may stop answering for several "
        "minutes; do not interrupt it.",
        flush=True,
    )
    blhost_command(
        "flash-erase-region",
        hex(FLASH_BASE),
        hex(FLASH_SIZE),
        str(FLEXSPI_NOR_MEMORY_ID),
        timeout_ms=600_000,
    )
    print("Erase complete. Writing the complete image.", flush=True)
    blhost_command(
        "write-memory",
        hex(FLASH_BASE),
        str(image),
        str(FLEXSPI_NOR_MEMORY_ID),
        timeout_ms=600_000,
    )
    print("Write complete.", flush=True)

    if not verify:
        print("Readback verification skipped by explicit --no-verify.")
        return

    print("Reading the complete NOR back for SHA-256 verification.", flush=True)
    with tempfile.TemporaryDirectory(prefix="ncr2-rom-recover-") as directory:
        readback = Path(directory) / "readback.bin"
        blhost_command(
            "read-memory",
            hex(FLASH_BASE),
            hex(FLASH_SIZE),
            str(readback),
            timeout_ms=600_000,
        )
        readback_sha256 = sha256_file(readback)
    if readback_sha256 != image_sha256:
        raise RecoveryError(
            f"readback hash mismatch: {readback_sha256} != {image_sha256}"
        )
    print(f"Verified SHA-256: {readback_sha256}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("status", help="query ROM without modifying flash")

    load_parser = subparsers.add_parser(
        "load", help="load and start the flashloader in RAM"
    )
    load_parser.add_argument("--wait-seconds", type=float, default=10.0)

    flash_parser = subparsers.add_parser(
        "flash", help="configure, erase, and restore the complete NOR"
    )
    flash_parser.add_argument("--image", required=True, type=Path)
    flash_parser.add_argument("--expected-sha256")
    flash_parser.add_argument(
        "--execute",
        action="store_true",
        help="required acknowledgement that this command mutates NOR",
    )
    verify_group = flash_parser.add_mutually_exclusive_group()
    verify_group.add_argument(
        "--verify",
        dest="verify",
        action="store_true",
        default=True,
        help="read back all 8 MiB and compare SHA-256 (default)",
    )
    verify_group.add_argument(
        "--no-verify",
        dest="verify",
        action="store_false",
        help="skip full readback",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    arguments = parser.parse_args(argv)
    try:
        validate_vendor_assets()
        if arguments.command == "status":
            rom_status()
        elif arguments.command == "load":
            load_flashloader(arguments.wait_seconds)
        elif arguments.command == "flash":
            flash_image(
                arguments.image.resolve(),
                execute=arguments.execute,
                verify=arguments.verify,
                expected_sha256=arguments.expected_sha256,
            )
    except (OSError, RecoveryError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

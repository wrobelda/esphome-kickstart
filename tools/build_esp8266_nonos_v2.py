#!/usr/bin/env python3
"""Build and verify an ESP8266 non-OS SDK V2 Kickstart image."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.esp8266_nonos_v2 import parse_v2_image


FLASH_MODES = {"qio": 0x00, "qout": 0x01, "dio": 0x02, "dout": 0x03}
FLASH_LAYOUTS = {
    "256KB": 0x10,
    "512KB": 0x00,
    "1MB": 0x20,
    "2MB": 0x30,
    "4MB": 0x40,
    "2MB-c1": 0x50,
    "4MB-c1": 0x60,
    "8MB": 0x80,
    "16MB": 0x90,
}
FLASH_FREQUENCIES = {"40m": 0x00, "26m": 0x01, "20m": 0x02, "80m": 0x0F}
RAM_RANGES = (
    (0x3FFE8000, 0x40000000, "DRAM"),
    (0x40100000, 0x40108000, "IRAM"),
)


def elf_irom_vma(elf: bytes) -> int:
    """Return the lowest flash-mapped PT_LOAD VMA from an ELF32 LE image."""
    if elf[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("input is not a little-endian ELF32 image")
    program_offset = struct.unpack_from("<I", elf, 28)[0]
    entry_size, entry_count = struct.unpack_from("<HH", elf, 42)
    addresses: list[int] = []
    for index in range(entry_count):
        offset = program_offset + index * entry_size
        kind, _, vma, _, file_size = struct.unpack_from("<IIIII", elf, offset)
        if kind == 1 and file_size and 0x40200000 <= vma < 0x40300000:
            addresses.append(vma)
    if not addresses:
        raise ValueError("ELF has no ESP8266 flash-mapped PT_LOAD segment")
    return min(addresses)


def elf_entrypoint(elf: bytes) -> int:
    """Return e_entry from a little-endian ELF32 image."""
    if elf[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("input is not a little-endian ELF32 image")
    return struct.unpack_from("<I", elf, 24)[0]


def elf_symbol_value(elf: bytes, wanted: str) -> int:
    """Return a named symbol value from a little-endian ELF32 symbol table."""
    if elf[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("input is not a little-endian ELF32 image")
    section_offset = struct.unpack_from("<I", elf, 32)[0]
    section_size, section_count = struct.unpack_from("<HH", elf, 46)
    sections = []
    for index in range(section_count):
        offset = section_offset + index * section_size
        if offset + 40 > len(elf):
            raise ValueError("truncated ELF section table")
        kind = struct.unpack_from("<I", elf, offset + 4)[0]
        file_offset, size, link = struct.unpack_from("<III", elf, offset + 16)
        entry_size = struct.unpack_from("<I", elf, offset + 36)[0]
        sections.append((kind, file_offset, size, link, entry_size))

    for kind, file_offset, size, link, entry_size in sections:
        if kind not in (2, 11) or entry_size < 16 or link >= len(sections):
            continue
        _, strings_offset, strings_size, _, _ = sections[link]
        strings = elf[strings_offset : strings_offset + strings_size]
        for offset in range(file_offset, file_offset + size, entry_size):
            if offset + 16 > len(elf):
                raise ValueError("truncated ELF symbol table")
            name_offset, value = struct.unpack_from("<II", elf, offset)
            if name_offset >= len(strings):
                continue
            name_end = strings.find(b"\0", name_offset)
            if name_end < 0:
                continue
            if strings[name_offset:name_end].decode("utf-8", "replace") == wanted:
                return value
    raise ValueError(f"ELF has no symbol named {wanted!r}")


def validate_user_bin(
    image: bytes,
    *,
    flash_mode: str,
    flash_layout: str,
    flash_frequency: str,
    max_size: int,
    expected_entrypoint: int | None = None,
) -> dict[str, object]:
    if len(image) < 4 or image[:2] != b"\xea\x04":
        raise ValueError("output is not an ESP8266 V2 user-bin")
    expected_mode = FLASH_MODES[flash_mode]
    expected_size_frequency = FLASH_LAYOUTS[flash_layout] | FLASH_FREQUENCIES[flash_frequency]
    if image[2] != expected_mode:
        raise ValueError(
            f"V2 image flash mode is {image[2]:#x}, expected {flash_mode} ({expected_mode:#x})"
        )
    if image[3] != expected_size_frequency:
        raise ValueError(
            f"V2 image size/frequency byte is {image[3]:#x}, expected "
            f"{flash_layout}/{flash_frequency} ({expected_size_frequency:#x})"
        )
    result = parse_v2_image(image, 0)
    if result["irom_address"] != 0:
        raise ValueError(
            f"V2 irom address field is {int(result['irom_address']):#x}, expected 0"
        )
    if expected_entrypoint is not None and result["entrypoint"] != expected_entrypoint:
        raise ValueError(
            f"V2 image entry point is {int(result['entrypoint']):#x}; expected "
            f"{expected_entrypoint:#x}"
        )
    ram_spans: list[tuple[int, int]] = []
    for segment in result["segments"]:
        address = int(segment["address"])
        size = int(segment["size"])
        end = address + size
        if size == 0 or end < address or not any(
            address >= lower and end <= upper for lower, upper, _ in RAM_RANGES
        ):
            raise ValueError(
                f"V2 RAM segment {address:#x}..{end:#x} is outside ESP8266 DRAM/IRAM"
            )
        ram_spans.append((address, end))
    ram_spans.sort()
    for (_, previous_end), (address, _) in zip(ram_spans, ram_spans[1:]):
        if address < previous_end:
            raise ValueError("V2 RAM segments overlap")
    if not result["checksum_valid"]:
        raise ValueError("V2 image segment checksum is invalid")
    if not result["crc32_valid"]:
        raise ValueError("V2 image SDK CRC32 is invalid")
    if result["length"] != len(image):
        raise ValueError("V2 image contains trailing data")
    if len(image) > max_size:
        raise ValueError(
            f"V2 image is {len(image)} bytes, larger than the configured slot limit {max_size}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="ESPHome bridge firmware.elf")
    parser.add_argument("output", type=Path, help="output V2 user-bin")
    parser.add_argument("--irom-vma", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--entry-symbol", required=True)
    parser.add_argument("--max-size", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--flash-mode", choices=FLASH_MODES, required=True)
    parser.add_argument("--flash-layout", choices=FLASH_LAYOUTS, required=True)
    parser.add_argument("--flash-frequency", choices=FLASH_FREQUENCIES, required=True)
    args = parser.parse_args()

    elf = args.elf.read_bytes()
    irom_vma = elf_irom_vma(elf)
    if irom_vma != args.irom_vma:
        raise ValueError(
            f"ELF irom starts at {irom_vma:#x}; the configured bootloader mapping expects {args.irom_vma:#x}"
        )
    entrypoint = elf_entrypoint(elf)
    entry_symbol_value = elf_symbol_value(elf, args.entry_symbol)
    if entrypoint != entry_symbol_value:
        raise ValueError(
            f"ELF entry point is {entrypoint:#x}; symbol {args.entry_symbol!r} is at "
            f"{entry_symbol_value:#x}"
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            "esp8266",
            "elf2image",
            "--version",
            "2",
            "--flash-mode",
            args.flash_mode,
            "--flash-freq",
            args.flash_frequency,
            "--flash-size",
            args.flash_layout,
            "--output",
            str(args.output),
            str(args.elf),
        ],
        check=True,
    )

    image = args.output.read_bytes()
    result = validate_user_bin(
        image,
        flash_mode=args.flash_mode,
        flash_layout=args.flash_layout,
        flash_frequency=args.flash_frequency,
        max_size=args.max_size,
        expected_entrypoint=entrypoint,
    )
    summary = {
        "output": str(args.output),
        "bytes": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "elf_irom_vma": f"0x{irom_vma:08x}",
        "entrypoint": f"0x{entrypoint:08x}",
        "entry_symbol": args.entry_symbol,
        "segment_checksum_valid": result["checksum_valid"],
        "sdk_crc32_valid": result["crc32_valid"],
        "flash_mode": args.flash_mode,
        "flash_frequency": args.flash_frequency,
        "flash_layout": args.flash_layout,
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

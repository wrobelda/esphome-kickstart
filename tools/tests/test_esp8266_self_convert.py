from __future__ import annotations

import os
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.esp8266_self_convert import (  # noqa: E402
    PATCH_SIZE,
    SECTOR_SIZE,
    Segment,
    eboot_crc32,
    e9_checksum,
    factory_patch,
    parse_e9,
    patch_factory_fields,
    reconstruct_v1_app,
)
from tools.esp8266_nonos_v2 import parse_v2_image, sdk_crc32  # noqa: E402

IROM_VMA = 0x40201010
IRAM_VMA = 0x40100000
DRAM_VMA = 0x3FFE8000
REAL_IROM = bytes(range(8))  # eight bytes of "compiled code" after the patch
RAM_IRAM = b"IRAM"
RAM_DRAM = b"DRAM"


def make_v1_app(*, patch: bytes = b"\x00" * PATCH_SIZE) -> bytes:
    segments = [Segment(IROM_VMA, PATCH_SIZE + len(REAL_IROM)), Segment(IRAM_VMA, 4), Segment(DRAM_VMA, 4)]
    payloads = [patch + REAL_IROM, RAM_IRAM, RAM_DRAM]
    image = bytearray(struct.pack("<BBBBI", 0xE9, len(segments), 0x00, 0x50, 0x401000C0))
    for segment, payload in zip(segments, payloads):
        assert len(payload) == segment.size
        image += struct.pack("<II", segment.vma, segment.size) + payload
    while len(image) % 16 != 15:
        image.append(0x00)
    image.append(e9_checksum(segments, payloads, IROM_VMA))
    return bytes(image)


def make_v2_image() -> bytes:
    """Build a V2 user-bin carrying the same application as make_v1_app.

    The V2 IROM payload is eight zero bytes, the real IROM payload, then eight
    trailing padding bytes; each RAM segment is padded with trailing zeros.
    """
    irom = b"\x00" * PATCH_SIZE + REAL_IROM + b"\x00" * PATCH_SIZE
    outer = struct.pack("<BBBBI", 0xEA, 0x04, 0x00, 0x50, 0x401000C0)
    descriptor = struct.pack("<II", 0, len(irom))
    inner_header = struct.pack("<BBBBI", 0xE9, 2, 0x00, 0x50, 0x401000C0)
    inner_segs = struct.pack("<II", IRAM_VMA, 8) + RAM_IRAM + b"\x00" * 4
    inner_segs += struct.pack("<II", DRAM_VMA, 8) + RAM_DRAM + b"\x00" * 4
    image = bytearray(outer + descriptor + irom + inner_header + inner_segs)
    image += b"\x00" * ((len(image) | 0x0F) - len(image))
    image.append(0xEF ^ (sum(RAM_IRAM) ^ sum(RAM_DRAM)))
    return bytes(image) + struct.pack("<I", sdk_crc32(image))


class SelfConvertTest(unittest.TestCase):
    def _segments_and_payloads(self):
        app = make_v1_app()
        parsed = parse_e9(app, 0)
        return parsed

    def test_parse_v2_round_trip(self) -> None:
        parsed = parse_v2_image(make_v2_image(), 0)
        self.assertTrue(parsed["checksum_valid"])
        self.assertTrue(parsed["crc32_valid"])
        self.assertEqual(parsed["irom_size"], 2 * PATCH_SIZE + len(REAL_IROM))

    def test_reconstruct_matches_v1_zero_patch(self) -> None:
        v1 = make_v1_app()
        parsed = parse_e9(v1, 0)
        rebuilt = reconstruct_v1_app(
            make_v2_image(),
            irom_vma=IROM_VMA,
            entry=parsed.entry,
            mode=parsed.mode,
            size_freq=parsed.size_freq,
            segments=parsed.segments,
            patch=b"\x00" * PATCH_SIZE,
        )
        self.assertEqual(rebuilt, v1)

    def test_reconstruct_applies_direct_patch(self) -> None:
        v1 = make_v1_app(patch=factory_patch(0x1234, 0xABCD))
        parsed = parse_e9(v1, 0)
        rebuilt = reconstruct_v1_app(
            make_v2_image(),
            irom_vma=IROM_VMA,
            entry=parsed.entry,
            mode=parsed.mode,
            size_freq=parsed.size_freq,
            segments=parsed.segments,
            patch=parsed.segments_data[0][:PATCH_SIZE],
        )
        self.assertEqual(rebuilt, v1)

    def test_patch_factory_fields_recomputes_size_and_crc(self) -> None:
        parsed = parse_e9(make_v1_app(), 0)
        body = reconstruct_v1_app(
            make_v2_image(),
            irom_vma=IROM_VMA,
            entry=parsed.entry,
            mode=parsed.mode,
            size_freq=parsed.size_freq,
            segments=parsed.segments,
            patch=b"\x00" * PATCH_SIZE,
        )
        factory = bytearray(b"\xff" * SECTOR_SIZE) + bytearray(body)
        patch_factory_fields(factory)
        self.assertEqual(struct.unpack_from("<I", factory, SECTOR_SIZE + 16)[0], len(factory))
        self.assertNotEqual(struct.unpack_from("<I", factory, SECTOR_SIZE + 20)[0], 0)
        once = bytes(factory)
        patch_factory_fields(factory)
        self.assertEqual(bytes(factory), once)

    def test_patch_size_mismatch_rejected(self) -> None:
        parsed = parse_e9(make_v1_app(), 0)
        with self.assertRaisesRegex(ValueError, "patch must be"):
            reconstruct_v1_app(
                make_v2_image(),
                irom_vma=IROM_VMA,
                entry=parsed.entry,
                mode=parsed.mode,
                size_freq=parsed.size_freq,
                segments=parsed.segments,
                patch=b"\x00",
            )

    def test_missing_ram_segment_rejected(self) -> None:
        parsed = parse_e9(make_v1_app(), 0)
        bogus = list(parsed.segments) + [Segment(0x40190000, 4)]
        with self.assertRaisesRegex(ValueError, "no RAM segment"):
            reconstruct_v1_app(
                make_v2_image(),
                irom_vma=IROM_VMA,
                entry=parsed.entry,
                mode=parsed.mode,
                size_freq=parsed.size_freq,
                segments=bogus,
                patch=b"\x00" * PATCH_SIZE,
            )

    def test_eboot_crc32_reference_value(self) -> None:
        # Polynomial 0x04C11DB7, most-significant-bit first.
        self.assertEqual(eboot_crc32(b"123456789"), 0x0376E6E7)


FIXTURES = Path(os.environ.get("ESPHOME_SELF_CONVERT_FIXTURES", "/nonexistent"))


@unittest.skipUnless(
    (FIXTURES / "firmware.bin").is_file() and (FIXTURES / "kickstart-v2.bin").is_file(),
    "set ESPHOME_SELF_CONVERT_FIXTURES to a directory with firmware.bin (elf2bin "
    "V1 factory) and kickstart-v2.bin (esptool V2 user-bin) built from the same ELF",
)
class RealFirmwareTest(unittest.TestCase):
    """Byte-exact reconstruction against a real same-ELF V1/V2 pair."""

    def test_reconstruct_real_bridge(self) -> None:
        v1 = (FIXTURES / "firmware.bin").read_bytes()
        v2 = (FIXTURES / "kickstart-v2.bin").read_bytes()
        parsed = parse_e9(v1, SECTOR_SIZE)
        rebuilt = reconstruct_v1_app(
            v2,
            irom_vma=IROM_VMA,
            entry=parsed.entry,
            mode=parsed.mode,
            size_freq=parsed.size_freq,
            segments=parsed.segments,
            patch=parsed.segments_data[0][:PATCH_SIZE],
        )
        self.assertEqual(rebuilt, v1[SECTOR_SIZE:])

    def test_runtime_path_matches_elf2bin(self) -> None:
        v1 = (FIXTURES / "firmware.bin").read_bytes()
        v2 = (FIXTURES / "kickstart-v2.bin").read_bytes()
        parsed = parse_e9(v1, SECTOR_SIZE)
        body = reconstruct_v1_app(
            v2,
            irom_vma=IROM_VMA,
            entry=parsed.entry,
            mode=parsed.mode,
            size_freq=parsed.size_freq,
            segments=parsed.segments,
            patch=b"\x00" * PATCH_SIZE,
        )
        factory = bytearray(v1[:SECTOR_SIZE]) + bytearray(body)
        patch_factory_fields(factory)
        self.assertEqual(bytes(factory), v1)


if __name__ == "__main__":
    unittest.main()

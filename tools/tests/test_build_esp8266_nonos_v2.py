from __future__ import annotations

import binascii
import importlib.util
import struct
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location(
    "build_esp8266_nonos_v2",
    ROOT / "tools" / "build_esp8266_nonos_v2.py",
)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def make_v2_image(
    *, mode: int = 0x00, size_frequency: int = 0x50, entrypoint: int = 0x40100004
) -> bytes:
    image = bytearray(struct.pack("<BBBBI", 0xEA, 4, mode, size_frequency, entrypoint))
    image += struct.pack("<II", 0, 0)
    image += struct.pack("<BBBBI", 0xE9, 1, mode, size_frequency, entrypoint)
    segment = b"\x01\x02\x03\x04"
    image += struct.pack("<II", 0x40100000, len(segment)) + segment
    checksum = 0xEF
    for byte in segment:
        checksum ^= byte
    image += b"\x00" * ((len(image) | 0x0F) - len(image))
    image.append(checksum)
    crc = binascii.crc32(image) & 0xFFFFFFFF
    crc = crc ^ 0xFFFFFFFF if crc & 0x80000000 else crc + 1
    image += struct.pack("<I", crc)
    return bytes(image)


class KickstartOtaTest(unittest.TestCase):
    def validate(self, image: bytes):
        return MODULE.validate_user_bin(
            image,
            flash_mode="qio",
            flash_layout="2MB-c1",
            flash_frequency="40m",
            max_size=0xFA000,
        )

    def test_accepts_valid_v2_image(self) -> None:
        result = self.validate(make_v2_image())
        self.assertTrue(result["checksum_valid"])
        self.assertTrue(result["crc32_valid"])

    def test_rejects_normal_2mb_header(self) -> None:
        image = bytearray(make_v2_image())
        image[3] = 0x30
        with self.assertRaisesRegex(ValueError, "2MB-c1"):
            self.validate(bytes(image))

    def test_rejects_dout_header(self) -> None:
        image = bytearray(make_v2_image())
        image[2] = 0x03
        with self.assertRaisesRegex(ValueError, "expected qio"):
            self.validate(bytes(image))

    def test_reads_elf_entrypoint(self) -> None:
        elf = bytearray(52)
        elf[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<I", elf, 24, 0x401000C0)
        self.assertEqual(MODULE.elf_entrypoint(bytes(elf)), 0x401000C0)

    def test_reads_named_elf_symbol(self) -> None:
        elf = bytearray(52 + 3 * 40 + 2 * 16 + 11)
        elf[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<I", elf, 32, 52)
        struct.pack_into("<HH", elf, 46, 40, 3)
        symbol_offset = 52 + 3 * 40
        strings_offset = symbol_offset + 2 * 16
        # Section 1: SHT_SYMTAB linked to section 2, with 16-byte entries.
        struct.pack_into("<I", elf, 52 + 40 + 4, 2)
        struct.pack_into("<III", elf, 52 + 40 + 16, symbol_offset, 32, 2)
        struct.pack_into("<I", elf, 52 + 40 + 36, 16)
        # Section 2: SHT_STRTAB.
        struct.pack_into("<I", elf, 52 + 80 + 4, 3)
        struct.pack_into("<II", elf, 52 + 80 + 16, strings_offset, 11)
        # Second symbol names app_entry at 0x40100094.
        struct.pack_into("<II", elf, symbol_offset + 16, 1, 0x40100094)
        elf[strings_offset:] = b"\0app_entry\0"
        self.assertEqual(MODULE.elf_symbol_value(bytes(elf), "app_entry"), 0x40100094)

    def test_rejects_wrong_expected_entrypoint(self) -> None:
        image = make_v2_image()
        with self.assertRaisesRegex(ValueError, "entry point"):
            MODULE.validate_user_bin(
                image,
                flash_mode="qio",
                flash_layout="2MB-c1",
                flash_frequency="40m",
                max_size=0xFA000,
                expected_entrypoint=0x401000C0,
            )

    def test_rejects_image_larger_than_target_slot(self) -> None:
        image = make_v2_image()
        with self.assertRaisesRegex(ValueError, "slot limit"):
            MODULE.validate_user_bin(
                image,
                flash_mode="qio",
                flash_layout="2MB-c1",
                flash_frequency="40m",
                max_size=len(image) - 1,
            )

    def test_rejects_nonzero_v2_irom_address_field(self) -> None:
        image = bytearray(make_v2_image())
        struct.pack_into("<I", image, 8, 1)
        with self.assertRaisesRegex(ValueError, "irom address field"):
            self.validate(bytes(image))

    def test_rejects_ram_segment_outside_esp8266_memory(self) -> None:
        image = bytearray(make_v2_image())
        first_segment_header = 8 + 8 + 8
        struct.pack_into("<I", image, first_segment_header, 0x40000000)
        with self.assertRaisesRegex(ValueError, "outside ESP8266"):
            self.validate(bytes(image))


if __name__ == "__main__":
    unittest.main()

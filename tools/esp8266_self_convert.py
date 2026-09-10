"""Reconstruct an ESP8266 eboot V1 application image from a running non-OS V2 image.

A Kickstart bridge runs as an Espressif non-OS SDK V2 user-bin under the vendor
bootloader. To convert itself to the eboot V1 layout it must rewrite its own
application bytes at flash offset 0x1000 in the eboot format and then replace the
vendor bootloader with eboot. The application code is unchanged: esptool's V2
packaging and Arduino's ``elf2bin`` V1 packaging contain the same segment data,
differing only in layout.

The V2 user-bin stores:

* an 8-byte outer header (``0xEA 0x04 mode size_freq entry``);
* an 8-byte IROM descriptor (``irom_address``, which is always 0, ``irom_size``);
* the IROM payload starting at file offset 16;
* an inner E9 image holding the RAM (IRAM/DRAM) segments.

The eboot V1 application image is a single E9 image holding the IROM segment
first, then the RAM segments. Given the V1 segment table, the V1 image is
reconstructed from the V2 payload as follows (verified byte-for-byte against
``elf2bin`` output for the same ELF):

* IROM: ``patch(8) + v2_irom[8 : 8 + (irom_size - 8)]``. The first eight bytes of
  the V1 IROM segment are the whole-image size and CRC that ``elf2bin`` patches
  in; esptool zeroes them and pads the V2 IROM by eight trailing bytes.
* Each RAM segment: the V2 segment with the same VMA, truncated to the V1 size.
  esptool pads segments with trailing zeros; the V1 lengths are the exact ELF
  section sizes.

The E9 segment checksum excludes the eight patch bytes, matching the firmware's
``validate_e9_``. The whole-image size/CRC that ``validate_factory_crc_`` checks
are computed over the reassembled V1 factory image.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from tools.esp8266_nonos_v2 import parse_v2_image

E9_MAGIC = 0xE9
V2_MAGIC = 0xEA
V2_MARKER = 0x04
V2_HEADER_SIZE = 8
IROM_DESCRIPTOR_SIZE = 8
IROM_PAYLOAD_OFFSET = V2_HEADER_SIZE + IROM_DESCRIPTOR_SIZE
PATCH_SIZE = 8
SECTOR_SIZE = 0x1000
CHECKSUM_SEED = 0xEF


@dataclass(frozen=True)
class Segment:
    vma: int
    size: int


@dataclass(frozen=True)
class E9Image:
    mode: int
    size_freq: int
    entry: int
    segments: list[Segment]
    checksum: int
    checksum_offset: int
    segments_data: list[bytes]


def parse_e9(image: bytes, start: int = 0) -> E9Image:
    """Parse an Espressif E9 image at *start*.

    The eight bytes at ``start`` are the header; segment headers follow, each an
    ``address, size`` pair followed by ``size`` bytes of data. The checksum lies
    at the next 16-byte boundary after the final segment.
    """
    magic, count, mode, size_freq, entry = struct.unpack_from("<BBBBI", image, start)
    if magic != E9_MAGIC:
        raise ValueError(f"no E9 image at {start:#x} (magic {magic:#x})")
    cursor = start + 8
    segments: list[Segment] = []
    segments_data: list[bytes] = []
    for _ in range(count):
        address, size = struct.unpack_from("<II", image, cursor)
        cursor += 8
        data = image[cursor : cursor + size]
        if len(data) != size:
            raise ValueError("truncated E9 segment")
        segments.append(Segment(address, size))
        segments_data.append(data)
        cursor += size
    checksum_offset = cursor | 0x0F
    return E9Image(
        mode=mode,
        size_freq=size_freq,
        entry=entry,
        segments=segments,
        checksum=image[checksum_offset],
        checksum_offset=checksum_offset - start,
        segments_data=segments_data,
    )


def e9_checksum(segments: list[Segment], segments_data: list[bytes], irom_vma: int) -> int:
    """CRC-free E9 segment checksum, excluding the IROM size/CRC patch bytes."""
    checksum = CHECKSUM_SEED
    for segment, data in zip(segments, segments_data):
        start = PATCH_SIZE if segment.vma == irom_vma else 0
        for byte in data[start:]:
            checksum ^= byte
    return checksum


def _v1_segment_payloads(
    v2_image: bytes,
    *,
    irom_vma: int,
    segments: list[Segment],
    patch: bytes,
) -> list[bytes]:
    """Return the V1 segment payloads recovered from a V2 user-bin."""
    v2 = parse_v2_image(v2_image, 0)
    irom_size = int(v2["irom_size"])
    inner = parse_e9(v2_image, IROM_PAYLOAD_OFFSET + irom_size)
    v2_irom = v2_image[IROM_PAYLOAD_OFFSET : IROM_PAYLOAD_OFFSET + irom_size]
    ram_by_vma = {segment.vma: data for segment, data in zip(inner.segments, inner.segments_data)}

    payloads: list[bytes] = []
    for segment in segments:
        if segment.vma == irom_vma:
            payload = patch + v2_irom[PATCH_SIZE : PATCH_SIZE + (segment.size - PATCH_SIZE)]
            if len(payload) != segment.size:
                raise ValueError("V2 IROM payload too short for the V1 IROM segment")
        else:
            source = ram_by_vma.get(segment.vma)
            if source is None or len(source) < segment.size:
                raise ValueError(f"V2 image has no RAM segment at {segment.vma:#x}")
            payload = source[: segment.size]
        payloads.append(payload)
    return payloads


def reconstruct_v1_app(
    v2_image: bytes,
    *,
    irom_vma: int,
    entry: int,
    mode: int,
    size_freq: int,
    segments: list[Segment],
    patch: bytes,
) -> bytes:
    """Return the eboot V1 E9 application image for *v2_image*.

    *segments* is the V1 segment table (IROM first, then IRAM/DRAM), *patch* is
    the eight size/CRC bytes ``elf2bin`` writes at the start of the IROM segment.
    """
    if len(patch) != PATCH_SIZE:
        raise ValueError(f"patch must be {PATCH_SIZE} bytes")
    if not segments or segments[0].vma != irom_vma:
        raise ValueError("first V1 segment must be the IROM segment")

    payloads = _v1_segment_payloads(v2_image, irom_vma=irom_vma, segments=segments, patch=patch)
    checksum = e9_checksum(segments, payloads, irom_vma)

    image = bytearray(struct.pack("<BBBBI", E9_MAGIC, len(segments), mode, size_freq, entry))
    for segment, payload in zip(segments, payloads):
        image += struct.pack("<II", segment.vma, segment.size) + payload
    # The checksum occupies the byte at (end | 0x0F), so the image ends on the
    # next 16-byte boundary.
    while len(image) % 16 != 15:
        image.append(0x00)
    image.append(checksum)
    return bytes(image)


def factory_patch(size: int, crc: int) -> bytes:
    """Return the little-endian whole-image size/CRC patch."""
    return struct.pack("<II", size, crc)


def eboot_crc32(data: bytes) -> int:
    """CRC32 as computed by the firmware's ``validate_factory_crc_``."""
    crc = 0xFFFFFFFF
    for byte in data:
        for mask in (0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01):
            invert = ((crc & 0x80000000) != 0) != ((byte & mask) != 0)
            crc = (crc << 1) & 0xFFFFFFFF
            if invert:
                crc ^= 0x04C11DB7
    return crc


def patch_factory_fields(factory_image: bytearray) -> None:
    """Write the whole-image size and CRC patch into a V1 factory image in place.

    ``factory_image`` starts with the 0x1000-byte eboot sector followed by the
    application; the size and CRC live at ``0x1010`` and ``0x1014``.
    """
    size_offset = SECTOR_SIZE + 16
    value_offset = SECTOR_SIZE + 20
    image_size = len(factory_image)
    # The CRC covers the whole image with both size and CRC fields zeroed.
    struct.pack_into("<I", factory_image, size_offset, 0)
    struct.pack_into("<I", factory_image, value_offset, 0)
    crc = eboot_crc32(bytes(factory_image))
    struct.pack_into("<I", factory_image, size_offset, image_size)
    struct.pack_into("<I", factory_image, value_offset, crc)

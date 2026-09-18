#!/usr/bin/env python3
"""Exercise the build-time PNG boundary without Pillow or external tools."""

import importlib.util
from pathlib import Path
import struct
import unittest
import zlib

spec = importlib.util.spec_from_file_location(
    "embed_wallpaper", Path(__file__).resolve().parents[1] / "tools/embed_wallpaper.py")
wallpaper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wallpaper)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(raw, width=2, height=2, color=2, depth=8, interlace=0):
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, interlace)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class WallpaperPngTests(unittest.TestCase):
    def test_pixels_and_all_scanline_filters(self):
        first = bytes((10, 20, 30, 40, 50, 60))
        second = bytes((50, 60, 70, 55, 65, 75))
        filtered = (
            second,
            bytes((50, 60, 70, 5, 5, 5)),
            bytes((40, 40, 40, 15, 15, 15)),
            bytes((45, 50, 55, 10, 10, 10)),
            bytes((40, 40, 40, 5, 5, 5)),
        )
        expected = struct.pack("<IIII", wallpaper.MAGIC, 2, 2, 0)
        for row in (first, second):
            for offset in (0, 3):
                expected += bytes((row[offset + 2], row[offset + 1], row[offset], 0))
        for filter_type, encoded in enumerate(filtered):
            with self.subTest(filter=filter_type):
                self.assertEqual(wallpaper.decode_png(png(b"\0" + first + bytes((filter_type,)) + encoded)), expected)

    def test_rgba_is_opaque_and_deterministic(self):
        image = png(b"\0\x12\x34\x56\xff", width=1, height=1, color=6)
        result = wallpaper.decode_png(image)
        self.assertEqual(result[-4:], b"\x56\x34\x12\0")
        self.assertEqual(wallpaper.decode_png(image), result)
        with self.assertRaises(ValueError):
            wallpaper.decode_png(png(b"\0\x12\x34\x56\xfe", width=1, height=1, color=6))

    def test_malformed_and_unsupported_images(self):
        valid = png(b"\0\x12\x34\x56", width=1, height=1)
        bad_crc = bytearray(valid)
        bad_crc[29] ^= 1
        malformed = (
            valid[:-1], bytes(bad_crc), valid + b"junk", b"not-png",
            png(b"\0\x12\x34", width=1, height=1),
            png(b"\0\x12\x34\x56\0", width=1, height=1),
            png(b"\5\x12\x34\x56", width=1, height=1),
            png(b"", width=0), png(b"", width=16385),
            png(b"", depth=16), png(b"", color=3), png(b"", interlace=1),
        )
        for image in malformed:
            with self.subTest(size=len(image)), self.assertRaises(ValueError):
                wallpaper.decode_png(image)


if __name__ == "__main__":
    unittest.main()

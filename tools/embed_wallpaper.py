#!/usr/bin/env python3
"""Convert an RGB/RGBA 8-bit, noninterlaced PNG to Anunix XRGB8888 wallpaper."""

import argparse
import struct
import zlib
from pathlib import Path

MAGIC = 0x414E5750
MAX_DIM = 16384
MAX_PIXELS = 16 * 1024 * 1024


def paeth(left, up, diagonal):
    estimate = left + up - diagonal
    distances = (abs(estimate - left), abs(estimate - up), abs(estimate - diagonal))
    return (left, up, diagonal)[distances.index(min(distances))]


def decode_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, header, compressed, ended = 8, None, bytearray(), False
    while pos + 12 <= len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        end = pos + 8 + length
        if end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        chunk = data[pos + 8:end]
        if zlib.crc32(kind + chunk) != struct.unpack_from(">I", data, end)[0]:
            raise ValueError("PNG chunk CRC mismatch")
        if kind == b"IHDR":
            if header is not None or pos != 8 or length != 13:
                raise ValueError("invalid PNG header")
            header = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            if header is None:
                raise ValueError("PNG data precedes header")
            compressed.extend(chunk)
        elif kind == b"IEND":
            if length:
                raise ValueError("invalid PNG end")
            ended = True
            pos = end + 4
            break
        elif kind in (b"acTL", b"tRNS") or not kind[0] & 0x20 and kind != b"PLTE":
            raise ValueError("unsupported PNG animation, transparency, or critical chunk")
        pos = end + 4
    if not header or not ended or pos != len(data):
        raise ValueError("incomplete PNG")
    width, height, depth, color, compression, filtering, interlace = header
    if (not 0 < width <= MAX_DIM or not 0 < height <= MAX_DIM or
            width * height > MAX_PIXELS):
        raise ValueError("wallpaper exceeds image bounds")
    if depth != 8 or color not in (2, 6) or compression or filtering or interlace:
        raise ValueError("wallpaper PNG must be RGB/RGBA 8-bit and noninterlaced")
    channels = 3 if color == 2 else 4
    stride = width * channels
    expected = (stride + 1) * height
    decoder = zlib.decompressobj()
    raw = decoder.decompress(compressed, expected + 1)
    if len(raw) != expected or not decoder.eof or decoder.unused_data:
        raise ValueError("invalid PNG decompressed size")
    output = bytearray(struct.pack("<IIII", MAGIC, width, height, 0))
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_type = raw[start]
        row = bytearray(raw[start + 1:start + stride + 1])
        if filter_type > 4:
            raise ValueError("invalid PNG row filter")
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            diagonal = previous[x - channels] if x >= channels else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 255
            elif filter_type == 2:
                row[x] = (row[x] + up) & 255
            elif filter_type == 3:
                row[x] = (row[x] + (left + up) // 2) & 255
            elif filter_type == 4:
                row[x] = (row[x] + paeth(left, up, diagonal)) & 255
        for x in range(0, stride, channels):
            if channels == 4 and row[x + 3] != 255:
                raise ValueError("wallpaper must be opaque")
            output.extend((row[x + 2], row[x + 1], row[x], 0))
        previous = row
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        output = decode_png(args.source.read_bytes())
    except (ValueError, zlib.error, OSError) as error:
        parser.exit(1, f"wallpaper: {error}\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)


if __name__ == "__main__":
    main()

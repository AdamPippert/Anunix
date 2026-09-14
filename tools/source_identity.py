#!/usr/bin/env python3
"""Fingerprint the kernel sources, headers, linkers, Makefile, and this generator."""

import argparse
import hashlib
import mmap
from pathlib import Path
import re

PREFIX = "ANUNIX_SOURCE_PROFILE_V1 sha256="
SOURCE_MARKER = re.compile(rb"ANUNIX_SOURCE_PROFILE_[^\r\n\x00]{1,128}\r?\n")


def source_fingerprint(root):
    paths = [root / "Makefile", root / "tools/source_identity.py"]
    paths += [p for p in (root / "kernel").rglob("*") if p.suffix in (".c", ".h", ".S", ".ld", ".inc")]
    if len(paths) <= 2:
        raise ValueError("kernel source set is empty")
    result = hashlib.sha256(b"Anunix kernel source fingerprint v1\x00")
    for path in sorted(paths, key=lambda p: p.relative_to(root).as_posix()):
        if path.is_symlink() or not path.is_file():
            raise ValueError("source input must be a regular file: " + str(path))
        name = path.relative_to(root).as_posix().encode("utf-8")
        content = path.read_bytes()
        for field in (name, content):
            result.update(len(field).to_bytes(8, "big"))
            result.update(field)
    return result.hexdigest()


def parse_marker(marker):
    match = re.fullmatch(re.escape(PREFIX) + r"([0-9a-f]{64})", marker)
    if not match or match[1] == "0" * 64:
        raise ValueError("malformed source fingerprint")
    return match[1]


def artifact_source(artifact):
    if not artifact.is_file() or not artifact.stat().st_size:
        raise ValueError("source identity requires a nonempty artifact")
    with artifact.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        markers = {match[0].decode("ascii").strip() for match in SOURCE_MARKER.finditer(data)}
    if len(markers) != 1:
        raise ValueError("artifact must contain one consistent source fingerprint")
    return parse_marker(markers.pop())


def validate_source_guest(expected, output):
    markers = [line.strip() for line in output.splitlines() if line.startswith("ANUNIX_SOURCE_PROFILE_")]
    if len(markers) != 1 or parse_marker(markers[0]) != expected:
        raise ValueError("guest source fingerprint mismatch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--header", type=Path)
    parser.add_argument("--label", choices=("ANUNIX_HOST_SOURCE_V1", "ANUNIX_CONFORMANCE_SOURCE_V1"))
    args = parser.parse_args()
    fingerprint = source_fingerprint(args.root)
    if args.header:
        content = '#define ANX_SOURCE_SHA256 "' + fingerprint + '"\n'
        args.header.parent.mkdir(parents=True, exist_ok=True)
        if not args.header.exists() or args.header.read_text() != content:
            temporary = args.header.with_suffix(".tmp")
            temporary.write_text(content)
            temporary.replace(args.header)
    else:
        print((args.label + " sha256=" if args.label else "") + fingerprint)


if __name__ == "__main__":
    main()

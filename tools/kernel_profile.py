#!/usr/bin/env python3
"""Bind a compiled configuration to one kernel or disk-image artifact."""

import argparse
import hashlib
import json
import mmap
from pathlib import Path
import re


ARCHITECTURES = ("x86_64", "arm64", "heteris")
MARKER = re.compile(rb"ANUNIX_KERNEL_PROFILE_[^\r\n\x00]{1,128}\r?\n")


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def configuration_from_marker(marker):
    match = re.fullmatch(
        r"ANUNIX_KERNEL_PROFILE_V1 arch=(x86_64|arm64|heteris) research_test=([01])", marker)
    if not match:
        raise ValueError("unsupported or malformed kernel configuration marker")
    return {"architecture": match[1], "research_test": int(match[2])}


def artifact_configuration(artifact):
    if not artifact.is_file() or artifact.stat().st_size == 0:
        raise ValueError("artifact must be a nonempty regular file")
    with artifact.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        markers = {match[0].decode("ascii").strip() for match in MARKER.finditer(data)}
    if len(markers) != 1:
        raise ValueError("artifact must contain one consistent kernel configuration")
    return configuration_from_marker(markers.pop())


def create_profile(artifact, revision):
    return {"schema": 1, "declared_revision": revision,
            "configuration": artifact_configuration(artifact),
            "artifact_sha256": digest(artifact), "artifact_bytes": artifact.stat().st_size}


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate profile key: " + key)
        result[key] = value
    return result


def load_profile(path):
    return json.loads(path.read_text(), object_pairs_hook=unique_keys)


def validate_profile(profile, artifact, revision, architecture, research_test):
    fields = {"schema", "declared_revision", "configuration", "artifact_sha256", "artifact_bytes"}
    if type(profile) is not dict or set(profile) != fields:
        raise ValueError("unexpected kernel profile fields")
    if type(profile["schema"]) is not int or profile["schema"] != 1:
        raise ValueError("unsupported kernel profile schema")
    if (type(profile["declared_revision"]) is not str or
            not re.fullmatch(r"[0-9a-f]{7,40}", profile["declared_revision"]) or
            profile["declared_revision"] != revision):
        raise ValueError("kernel profile revision mismatch")
    config = profile["configuration"]
    if (type(config) is not dict or set(config) != {"architecture", "research_test"} or
            type(config["architecture"]) is not str or config["architecture"] not in ARCHITECTURES or
            type(config["research_test"]) is not int or config["research_test"] not in (0, 1)):
        raise ValueError("malformed compiled configuration")
    if (architecture not in ARCHITECTURES or type(research_test) is not int or
            research_test not in (0, 1) or
            config != {"architecture": architecture, "research_test": research_test}):
        raise ValueError("incompatible required configuration")
    if (type(profile["artifact_bytes"]) is not int or profile["artifact_bytes"] <= 0 or
            type(profile["artifact_sha256"]) is not str or
            not re.fullmatch(r"[0-9a-f]{64}", profile["artifact_sha256"])):
        raise ValueError("malformed artifact identity")
    if artifact_configuration(artifact) != config:
        raise ValueError("profile disagrees with compiled configuration")
    if artifact.stat().st_size != profile["artifact_bytes"] or digest(artifact) != profile["artifact_sha256"]:
        raise ValueError("profile artifact digest or size mismatch")


def validate_guest(profile, output):
    markers = [line.strip() for line in output.splitlines() if line.startswith("ANUNIX_KERNEL_PROFILE_")]
    if len(markers) != 1 or configuration_from_marker(markers[0]) != profile["configuration"]:
        raise ValueError("running guest disagrees with kernel profile")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "check"))
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--arch", choices=ARCHITECTURES, default="x86_64")
    parser.add_argument("--research-test", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    try:
        profile = (create_profile(args.artifact, args.revision) if args.action == "create"
                   else load_profile(args.profile))
        validate_profile(profile, args.artifact, args.revision, args.arch, args.research_test)
        if args.action == "create":
            args.profile.write_text(json.dumps(profile, indent=2) + "\n")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps({"compatible": True, "profile": str(args.profile), "sha256": profile["artifact_sha256"]}))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Record a source candidate only after matching kernel and UEFI validation."""

import argparse
import json
from pathlib import Path
import re

from kernel_profile import digest, unique_keys, validate_guest, validate_profile
from source_identity import artifact_source, source_fingerprint, validate_source_guest


def read_report(path):
    value = json.loads(path.read_text(), object_pairs_hook=unique_keys)
    if type(value) is not dict:
        raise ValueError("report must be an object")
    return value


def required_tests(day):
    if type(day) is not int or not 1 <= day <= 999:
        raise ValueError("required day must be between 1 and 999")
    return [f"day-{i:03d}" for i in range(1, day + 1)] + [f"day-{day:03d}"]


def check_boot(report, artifact, revision, source, mode, tests):
    if (report.get("passed") is not True or report.get("expected_result_observed") is not True or
            report.get("expected_failure") is not False or report.get("profile_verified") is not True or
            report.get("source_verified") is not True or "error" in report):
        raise ValueError("boot validation is absent or failed")
    if report.get("revision") != revision or report.get("boot_mode") != mode:
        raise ValueError("boot revision or mode mismatch")
    profile = report.get("kernel_profile")
    validate_profile(profile, artifact, revision, "x86_64", 1)
    if report.get("sha256") != profile["artifact_sha256"]:
        raise ValueError("boot artifact identity mismatch")
    if report.get("source_sha256") != source or artifact_source(artifact) != source:
        raise ValueError("source candidate does not match booted artifact")
    sysinfo = report.get("sysinfo")
    if type(sysinfo) is not dict or sysinfo.get("status") != "ok" or type(sysinfo.get("output")) is not str:
        raise ValueError("missing guest identity output")
    validate_guest(profile, sysinfo["output"])
    validate_source_guest(source, sysinfo["output"])
    results = report.get("tests")
    if type(results) is not list or len(results) != len(tests):
        raise ValueError("missing required regressions")
    for result, name in zip(results, tests):
        if type(result) is not dict or result.get("name") != name or result.get("passed") is not True:
            raise ValueError("regression order or result mismatch")
        response = result.get("response")
        if type(response) is not dict or response.get("status") != "ok" or type(response.get("output")) is not str:
            raise ValueError("missing regression output")
        markers = [line for line in response["output"].splitlines() if line.startswith("RESEARCH ")]
        if markers != [f"RESEARCH {name} PASS rc=0"]:
            raise ValueError("regression output contradicts passing result")


def validate_candidate(root, revision, day, kernel, image, kernel_report, image_report, host_log, conformance_log):
    if type(revision) is not str or not re.fullmatch(r"[0-9a-f]{7,40}", revision):
        raise ValueError("malformed declared revision")
    tests = required_tests(day)
    source = source_fingerprint(root)
    evidence = {}
    artifacts = {}
    for mode, artifact, report_path in (("kernel", kernel, kernel_report), ("uefi", image, image_report)):
        check_boot(read_report(report_path), artifact, revision, source, mode, tests)
        evidence[mode] = digest(report_path)
        artifacts[mode] = {"sha256": digest(artifact), "bytes": artifact.stat().st_size}
    host = host_log.read_text()
    if (not re.search(r"(?m)^.*59 passed, 0 failed.*$", host) or
            not re.search(r"(?m)^Ran 9 tests in ", host) or "\nOK\n" not in host or
            not re.search(r"(?m)^Ran 12 tests in ", host) or
            re.search(r"(?m)^(FAILED|FAIL:|ERROR:)", host)):
        raise ValueError("host validation is absent or failed")
    conformance = conformance_log.read_text()
    if "threshold gate: PASS" not in conformance or "threshold gate: FAIL" in conformance:
        raise ValueError("conformance validation is absent or failed")
    for label, log in (("ANUNIX_HOST_SOURCE_V1", host), ("ANUNIX_CONFORMANCE_SOURCE_V1", conformance)):
        markers = [line for line in log.splitlines() if line.startswith(label)]
        if markers != [label + " sha256=" + source]:
            raise ValueError("host or conformance source fingerprint mismatch")
    evidence["host"] = digest(host_log)
    evidence["conformance"] = digest(conformance_log)
    # A source change during validation invalidates the candidate too.
    if source_fingerprint(root) != source:
        raise ValueError("source candidate changed during validation")
    return {"schema": 1, "validated": True, "declared_revision": revision,
            "source_sha256": source, "required_tests": tests,
            "artifacts": artifacts, "evidence_sha256": evidence}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "kernel", "image", "kernel-report", "image-report", "host-log", "conformance-log", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--through-day", type=int, required=True)
    args = parser.parse_args()
    try:
        result = validate_candidate(args.source, args.revision, args.through_day, args.kernel, args.image,
                                    args.kernel_report, args.image_report, args.host_log, args.conformance_log)
        with args.output.open("x") as stream:
            stream.write(json.dumps(result, indent=2) + "\n")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps({"validated": True, "record": str(args.output), "source_sha256": result["source_sha256"]}))


if __name__ == "__main__":
    main()

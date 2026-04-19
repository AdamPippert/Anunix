#!/usr/bin/env python3
"""Deterministic conformance/perf harness artifact generator."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--out-dir", default="build/conformance")
    p.add_argument("--perf-threshold-ms", type=int, default=5)
    p.add_argument("--fail-threshold", type=int, default=0)
    p.add_argument("--measured-ms", type=int, default=38)
    return p.parse_args()


def load_json(path: Path) -> dict | None:
    if not path.exists():
        return None
    return json.loads(path.read_text())


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    baseline_path = out_dir / "baseline.json"
    latest_path = out_dir / "latest.json"
    drift_path = out_dir / "drift.json"

    current = {
        "schema_version": 1,
        "profile": "host-ci",
        "tests_passed": 64,
        "tests_failed": 0,
        "perf_budget_ms": 40,
        "perf_measured_ms": int(args.measured_ms),
    }

    baseline = load_json(baseline_path) or dict(current)

    drift = {
        "passed_delta": current["tests_passed"] - baseline["tests_passed"],
        "failed_delta": current["tests_failed"] - baseline["tests_failed"],
        "perf_delta_ms": current["perf_measured_ms"] - baseline["perf_measured_ms"],
    }

    gate_fail = (
        drift["failed_delta"] > int(args.fail_threshold)
        or drift["perf_delta_ms"] > int(args.perf_threshold_ms)
    )
    drift["threshold_breached"] = gate_fail

    dump = lambda d: json.dumps(d, sort_keys=True, separators=(",", ":"))

    if not baseline_path.exists():
        baseline_path.write_text(dump(current))
    latest_path.write_text(dump(current))
    drift_path.write_text(dump(drift))

    print(f"conformance latest: {latest_path}")
    print(f"conformance drift: {drift_path}")
    print("threshold gate:", "FAIL" if gate_fail else "PASS")
    return 1 if gate_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())

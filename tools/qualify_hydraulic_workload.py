#!/usr/bin/env python3
"""Repeat native canonical hydraulic work on fresh repositories and verify each in a new process.

All repositories are disposable, created under a uniquely named result directory.
The verifier's elapsed time includes process startup, reopening and full verification;
Linux page cache is NOT flushed. This is not a cold-cache storage benchmark.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

REQUIRED = (
    "engine", "fixture", "pipes", "scalar_edits", "topology_edits",
    "import_ms", "indexed_scalar_mean_ms", "indexed_topology_mean_ms",
    "targeted_read_samples", "targeted_read_mean_ms", "full_network_read_ms",
    "divergent_diff_ms", "divergent_diff_entities", "canonical_merge_ms",
    "merge_parents", "pack_bytes_after_import", "pack_bytes_final",
    "pack_growth_bytes", "index_bytes_after_import", "index_bytes_final",
    "index_growth_bytes", "physical_objects_final", "peak_rss_kib",
)
INTEGER = {
    "pipes", "scalar_edits", "topology_edits", "targeted_read_samples",
    "divergent_diff_entities", "merge_parents", "pack_bytes_after_import",
    "pack_bytes_final", "pack_growth_bytes", "index_bytes_after_import",
    "index_bytes_final", "index_growth_bytes", "physical_objects_final", "peak_rss_kib",
}


def checksum(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_metrics(output, pipes, edits, topology):
    values = {}
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in values:
            raise ValueError("duplicate benchmark metric: " + key)
        values[key] = value
    if any(key not in values for key in REQUIRED):
        raise ValueError("missing benchmark metrics: " + ", ".join(
            key for key in REQUIRED if key not in values))
    if values["engine"] != "pack" or values["fixture"] != "canonical-hydraulic-distribution-network":
        raise ValueError("benchmark did not use the canonical native pack fixture")
    for key in REQUIRED:
        if key in {"engine", "fixture"}:
            continue
        try:
            value = int(values[key]) if key in INTEGER else float(values[key])
        except ValueError as error:
            raise ValueError("non-numeric benchmark metric: " + key) from error
        if not (0 <= value < float("inf")):
            raise ValueError("invalid benchmark metric: " + key)
        values[key] = value
    if (values["pipes"], values["scalar_edits"], values["topology_edits"]) != (pipes, edits, topology):
        raise ValueError("benchmark echoed incorrect fixture parameters")
    if values["merge_parents"] != 2 or values["divergent_diff_entities"] < 1:
        raise ValueError("canonical merge or divergent diff was not exercised")
    if values["targeted_read_samples"] < 1 or values["physical_objects_final"] < pipes:
        raise ValueError("targeted read or physical object count is invalid")
    for category in ("pack", "index"):
        if (values[f"{category}_bytes_final"] - values[f"{category}_bytes_after_import"]
                != values[f"{category}_growth_bytes"]):
            raise ValueError(category + " growth metrics do not reconcile")
    return values


def execute(args, log_path, timeout):
    start = time.monotonic()
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("command timed out: " + str(args[0])) from error
    log_path.write_text("COMMAND: " + " ".join(str(arg) for arg in args) + "\n"
                        + "STDOUT:\n" + result.stdout + "\nSTDERR:\n" + result.stderr)
    if result.returncode:
        # Keep the full diagnostic in the retained log, but expose the cause
        # directly through CTest instead of requiring a second manual lookup.
        details = (result.stderr.strip() or result.stdout.strip()).replace("\n", " | ")
        if details:
            details = "; output: " + details[:1200]
        raise RuntimeError(f"command failed ({result.returncode}): {args[0]}{details}; see {log_path}")
    return result.stdout, (time.monotonic() - start) * 1000.0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build-linux-tests-release/aowis-dvmdb-hydraulic-edit-benchmark"))
    parser.add_argument("--verifier", type=Path, default=Path("build-linux-tests-release/aowis-dvmdb-verify"))
    parser.add_argument("--pipes", type=int, default=10000)
    parser.add_argument("--edits", type=int, default=20)
    parser.add_argument("--topology-edits", type=int, default=3)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--results-root", type=Path, default=Path("build-qualification"))
    parser.add_argument("--timeout", type=int, default=3600, help="seconds per executable")
    parser.add_argument("--keep-fixtures", action="store_true")
    args = parser.parse_args()
    if not (2 <= args.pipes <= 1000000 and 1 <= args.edits <= 100000
            and 1 <= args.topology_edits <= 100000 and 1 <= args.repeats <= 20
            and 10 <= args.timeout <= 86400):
        parser.error("invalid work size, repetitions or timeout")
    binary = args.binary.resolve(strict=True)
    verifier = args.verifier.resolve(strict=True)
    for executable in (binary, verifier):
        if not executable.is_file() or not os.access(executable, os.X_OK):
            parser.error("not an executable: " + str(executable))
    root = args.results_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="hydraulic-workload-", dir=root))
    summary = {
        "kind": "aowis-dvmdb-hydraulic-workload-qualification",
        "version": 1,
        "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Canonical model operations and full verification in fresh processes; warm/uncontrolled OS page cache",
        "binary": {"path": str(binary), "sha256": checksum(binary)},
        "verifier": {"path": str(verifier), "sha256": checksum(verifier)},
        "parameters": {"pipes": args.pipes, "scalar_edits": args.edits,
                       "topology_edits": args.topology_edits, "repeats": args.repeats},
        "runs": [],
    }
    try:
        for index in range(1, args.repeats + 1):
            fixture = folder / f"repository-{index:02d}"
            benchmark_log = folder / f"benchmark-{index:02d}.log"
            stdout, total_ms = execute(
                [str(binary), str(fixture), str(args.pipes), str(args.edits), str(args.topology_edits)],
                benchmark_log, args.timeout)
            measurements = parse_metrics(stdout, args.pipes, args.edits, args.topology_edits)
            pack = fixture / "objects.pack"
            # The current engine uses objects.pack as the initial pack generation.
            index_path = fixture / "objects.idx"
            if not index_path.is_file() or not pack.is_file():
                raise ValueError("benchmark did not create a pack and derived index")
            if index_path.stat().st_size != measurements["index_bytes_final"] or pack.stat().st_size != measurements["pack_bytes_final"]:
                raise ValueError("on-disk sizes disagree with benchmark metrics")
            verification_log = folder / f"verify-{index:02d}.log"
            verified_output, reopen_full_verify_ms = execute(
                [str(verifier), "--full", str(fixture)], verification_log, args.timeout)
            if "Full native storage integrity verified:" not in verified_output:
                raise ValueError("independent full repository verification is missing")
            summary["runs"].append({
                "iteration": index,
                "metrics": measurements,
                "benchmark_wall_ms": round(total_ms, 3),
                "fresh_process_reopen_full_verify_ms": round(reopen_full_verify_ms, 3),
                "pack_sha256": checksum(pack),
                "index_sha256": checksum(index_path),
                "benchmark_log": benchmark_log.name,
                "verification_log": verification_log.name,
            })
            print(f"Hydraulic run {index}/{args.repeats}: pipes={args.pipes} "
                  f"pack_mib={measurements['pack_bytes_final'] / 1048576:.2f} "
                  f"read_ms={measurements['full_network_read_ms']:.2f} "
                  f"merge_ms={measurements['canonical_merge_ms']:.2f} "
                  f"reopen_full_verify_ms={reopen_full_verify_ms:.2f}", flush=True)
            if not args.keep_fixtures:
                shutil.rmtree(fixture)
            temporary_report = folder / "report.json.tmp"
            temporary_report.write_text(json.dumps(summary, indent=2) + "\n")
            os.replace(temporary_report, folder / "report.json")
        print("Hydraulic workload qualification passed: " + str(folder / "report.json"))
    except Exception as error:
        print("FAILED: retained qualification evidence in " + str(folder), file=sys.stderr)
        print(str(error), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

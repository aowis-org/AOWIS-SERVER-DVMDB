#!/usr/bin/env python3
"""Offline canonical hydraulic report comparison / evidence regressions."""

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from analyze_hydraulic_workloads import analyze

SCRIPT = ROOT / "tools" / "analyze_hydraulic_workloads.py"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def create_report(root, pipes, benchmark_sha="a" * 64, verifier_sha="b" * 64):
    folder = root / f"hydraulic-workload-{pipes}"
    folder.mkdir()
    runs = []
    for iteration in (1, 2, 3):
        metrics = {
            "engine": "pack", "fixture": "canonical-hydraulic-distribution-network", "pipes": pipes,
            "scalar_edits": 20, "topology_edits": 3, "import_ms": float(pipes) / 10 + iteration,
            "indexed_scalar_mean_ms": 1.0, "indexed_topology_mean_ms": 2.0,
            "targeted_read_samples": 32, "targeted_read_mean_ms": 0.5,
            "full_network_read_ms": float(pipes) / 10 + iteration,
            "divergent_diff_ms": float(pipes) / 100, "divergent_diff_entities": 2,
            "canonical_merge_ms": float(pipes) / 5 + iteration, "merge_parents": 2,
            "pack_bytes_after_import": pipes * 1000,
            "pack_bytes_final": pipes * 1350,
            "pack_growth_bytes": pipes * 350,
            "index_bytes_after_import": pipes * 100,
            "index_bytes_final": pipes * 110,
            "index_growth_bytes": pipes * 10,
            "physical_objects_final": pipes * 3, "peak_rss_kib": pipes * 2,
        }
        benchname = f"benchmark-{iteration:02d}.log"
        verifyname = f"verify-{iteration:02d}.log"
        stdout = "\n".join(f"{key}={value}" for key, value in metrics.items()) + "\n"
        (folder / benchname).write_text("COMMAND: bench\nSTDOUT:\n" + stdout + "\nSTDERR:\n")
        (folder / verifyname).write_text("COMMAND: verify\nSTDOUT:\nFull native storage integrity verified: 10 physical objects\n\nSTDERR:\n")
        runs.append({"iteration": iteration, "metrics": metrics,
                     "benchmark_wall_ms": 500.0,
                     "fresh_process_reopen_full_verify_ms": 10.0 + float(pipes) / 9 + iteration,
                     "pack_sha256": "c" * 64, "index_sha256": "d" * 64,
                     "benchmark_log": benchname, "verification_log": verifyname})
    report = {
        "kind": "aowis-dvmdb-hydraulic-workload-qualification", "version": 1,
        "parameters": {"pipes": pipes, "scalar_edits": 20, "topology_edits": 3, "repeats": 3},
        "binary": {"path": "/tmp/bench", "sha256": benchmark_sha},
        "verifier": {"path": "/tmp/verify", "sha256": verifier_sha},
        "runs": runs,
    }
    (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return folder


def cli(*arguments):
    return subprocess.run([sys.executable, str(SCRIPT), *(str(arg) for arg in arguments)],
                          capture_output=True, text=True, timeout=15)


def run_test():
    with tempfile.TemporaryDirectory(prefix="aowis-hydraulic-analysis-") as temporary:
        root = Path(temporary)
        small = create_report(root, 1000)
        large = create_report(root, 5000)
        rows = analyze([large, small / "report.json"])
        require([row["pipes"] for row in rows] == [1000, 5000], "sort by pipes failed")
        require(rows[0]["repeats"] == 3 and rows[1]["pipe_ratio"] == 5,
                "repeat count or relative pipe count incorrect")
        require(rows[0]["network_read_ms"] == 102 and rows[1]["network_read_ms"] == 502,
                "median read time incorrect")
        require(rows[0]["pack_bytes_per_pipe"] == 1350,
                "pack bytes per pipe incorrect")
        require(rows[1]["reopen_verify_ratio"] > 4.0 and
                rows[1]["pack_mib_ratio"] == 5.0 and
                rows[1]["network_read_ms_per_1000_pipes"] == 100.4 and
                rows[0]["targeted_read_ms_ratio"] == 1.0,
                "extended size-scaling ratios are incorrect")
        csv_path = root / "summary.csv"
        result = cli(large, small, "--csv", csv_path)
        require(result.returncode == 0, "successful CLI comparison failed: " + result.stderr)
        require("Indexed operation medians" in result.stdout and
                "Growth relative to 1000 pipes" in result.stdout and
                "targeted_read" in result.stdout,
                "indexed-work and normalized-growth summaries missing from CLI")
        with csv_path.open(newline="") as stream:
            data = list(csv.DictReader(stream))
        require(len(data) == 2 and data[0]["pipes"] == "1000" and
                data[1]["merge_ratio"] != "" and
                data[1]["reopen_verify_ratio"] != "" and
                data[1]["pack_mib_ratio"] != "", "missing or incorrect CSV output")
        different = create_report(root, 9000, benchmark_sha="e" * 64)
        result = cli(small, different)
        require(result.returncode != 0 and "different benchmark/verifier" in result.stderr,
                "mixed benchmark binary comparisons incorrectly accepted")
        result = cli(small, small)
        require(result.returncode != 0 and "duplicate report" in result.stderr,
                "duplicate input report accepted")
        logs = large / "benchmark-02.log"
        logs.write_text(logs.read_text().replace("merge_parents=2", "merge_parents=1"))
        result = cli(small, large)
        require(result.returncode != 0 and "canonical merge" in result.stderr,
                "invalid merge log accepted")
        logs.write_text(logs.read_text().replace("merge_parents=1", "merge_parents=2"))
        verifier = small / "verify-01.log"
        verifier.write_text("COMMAND: verify\nSTDOUT:\nVerification failed\nSTDERR:\n")
        result = cli(small)
        require(result.returncode != 0 and "verification evidence" in result.stderr,
                "missing independent verification accepted")
    print("Hydraulic workload analysis tests passed")


if __name__ == "__main__":
    run_test()

#!/usr/bin/env python3
"""Offline checks for the canonical hydraulic benchmark qualification driver."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "tools" / "qualify_hydraulic_workload.py"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def fake_executable(path, content):
    path.write_text("#!/usr/bin/env python3\n" + content)
    path.chmod(0o755)


def run_driver(binary, verifier, root, *extra):
    return subprocess.run([sys.executable, str(DRIVER), "--binary", str(binary),
                           "--verifier", str(verifier), "--pipes", "2", "--edits", "1",
                           "--topology-edits", "1", "--repeats", "2", "--results-root",
                           str(root), *extra], capture_output=True, text=True, timeout=30)


BENCHMARK = '''
import pathlib
import sys
folder = pathlib.Path(sys.argv[1])
folder.mkdir()
(folder / "objects.pack").write_bytes(b"0123456789")
(folder / "objects.idx").write_bytes(b"abcdef")
print("engine=pack\\nfixture=canonical-hydraulic-distribution-network\\npipes=2\\nscalar_edits=1")
print("topology_edits=1\\nimport_ms=100\\nindexed_scalar_mean_ms=2")
print("indexed_topology_mean_ms=3\\ntargeted_read_samples=100")
print("targeted_read_mean_ms=0.5\\nfull_network_read_ms=4")
print("divergent_diff_ms=2\\ndivergent_diff_entities=2")
print("canonical_merge_ms=5\\nmerge_parents=2")
print("pack_bytes_after_import=5\\npack_bytes_final=10\\npack_growth_bytes=5")
print("index_bytes_after_import=3\\nindex_bytes_final=6\\nindex_growth_bytes=3")
print("physical_objects_final=10\\npeak_rss_kib=10000")
'''
VERIFIER = '''
import pathlib
import sys
assert sys.argv[1] == "--full"
assert (pathlib.Path(sys.argv[2]) / "objects.idx").is_file()
print("Full native storage integrity verified: 10 physical objects")
'''


def run_test():
    with tempfile.TemporaryDirectory(prefix="aowis-hydraulic-qualification-") as temporary:
        root = Path(temporary)
        binpath = root / "benchmark"
        verify = root / "verifier"
        fake_executable(binpath, BENCHMARK)
        fake_executable(verify, VERIFIER)
        results = root / "results"
        output = run_driver(binpath, verify, results)
        require(output.returncode == 0, "valid fixture failed: " + output.stderr)
        reports = list(results.glob("hydraulic-workload-*/report.json"))
        require(len(reports) == 1, "missing or multiple result files")
        report = json.loads(reports[0].read_text())
        require(len(report["runs"]) == 2 and report["parameters"]["pipes"] == 2,
                "missing independent repetitions")
        require(all(entry["metrics"]["merge_parents"] == 2 and
                    entry["metrics"]["pack_growth_bytes"] == 5 and
                    entry["fresh_process_reopen_full_verify_ms"] >= 0
                    for entry in report["runs"]), "missing validated measurements")
        require(len(list(reports[0].parent.glob("verify-*.log"))) == 2,
                "independent verifier logs not preserved")
        require(not list(reports[0].parent.glob("repository-*")),
                "default run failed to delete disposable repositories")
        require(report["binary"]["sha256"] and report["verifier"]["sha256"],
                "binary identities missing")
        keep = run_driver(binpath, verify, results, "--keep-fixtures")
        require(keep.returncode == 0, "--keep-fixtures failed: " + keep.stderr)
        require(len(list(results.glob("hydraulic-workload-*/report.json"))) == 2,
                "previous immutable result directory overwritten")
        require(len(list((results / next(name for name in [p.name for p in results.iterdir()]
                                          if name != reports[0].parent.name)).glob("repository-*"))) == 2,
                "--keep-fixtures deleted the repositories")
        fake_executable(binpath, BENCHMARK.replace("merge_parents=2", "merge_parents=1"))
        bad = run_driver(binpath, verify, results)
        require(bad.returncode != 0 and "canonical merge" in bad.stderr,
                "non-two-parent merge was incorrectly accepted")
        fake_executable(binpath, "import sys\nprint('fixture edit rejected', file=sys.stderr)\nsys.exit(1)\n")
        failed_benchmark = run_driver(binpath, verify, results)
        require(failed_benchmark.returncode != 0 and
                "fixture edit rejected" in failed_benchmark.stderr and
                "benchmark-01.log" in failed_benchmark.stderr,
                "benchmark failure did not include retained log and underlying reason")
        fake_executable(binpath, BENCHMARK)
        fake_executable(verify, "import sys\nprint('not verified')\n")
        invalid = run_driver(binpath, verify, results)
        require(invalid.returncode != 0 and "full repository verification" in invalid.stderr,
                "missing independent verification was incorrectly accepted")
        malformed = run_driver(binpath, verify, results, "--pipes", "0")
        require(malformed.returncode != 0 and "invalid" in malformed.stderr,
                "unsafe fixture size accepted")
    print("Hydraulic workload driver offline tests passed")


if __name__ == "__main__":
    run_test()

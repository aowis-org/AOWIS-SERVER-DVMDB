#!/usr/bin/env python3
"""Offline smoke for the production-store worker; no QEMU or privileges required."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import select
import signal
import time


def launch(binary, action, repository, stdin=None):
    return subprocess.run([str(binary), action, str(repository)], input=stdin,
                          text=True, capture_output=True, timeout=45)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="aowis-vm-powercut-worker-") as location:
        repo = Path(location) / "repository"
        assert launch(binary, "verify", repo).returncode != 0, "missing repository accepted"
        initialized = launch(binary, "init", repo)
        assert initialized.returncode == 0, initialized.stderr
        assert re.search(r"^STATE 0 [0-9a-f]{64}$", initialized.stdout, re.MULTILINE)
        first = launch(binary, "writer", repo, stdin="")
        assert first.returncode == 0, first.stderr
        assert re.search(r"^ACK 1 [0-9a-f]{64}$", first.stdout, re.MULTILINE)
        second = launch(binary, "writer", repo, stdin="GO\n")
        assert second.returncode == 0, second.stderr
        assert re.search(r"^ACK 2 [0-9a-f]{64}$", second.stdout, re.MULTILINE)
        assert re.search(r"^ACK 3 [0-9a-f]{64}$", second.stdout, re.MULTILINE)
        verified = launch(binary, "verify", repo)
        assert verified.returncode == 0, verified.stderr
        assert re.search(r"^STATE 3 [0-9a-f]{64}$", verified.stdout, re.MULTILINE)
        assert launch(binary, "unknown", repo).returncode != 0
        for scenario in ("branch", "truncate"):
            root = Path(location) / ("delete-" + scenario)
            initialized = subprocess.run([str(binary), "delete-init", str(root), scenario],
                                         capture_output=True, text=True, timeout=30)
            assert initialized.returncode == 0, initialized.stderr
            result = launch(binary, "delete-writer", root)
            assert result.returncode == 0, result.stderr
            verified = subprocess.run([str(binary), "delete-verify", str(root), "deleted"],
                                      capture_output=True, text=True, timeout=30)
            assert verified.returncode == 0, verified.stderr
        stress = Path(location) / "deletion-stress"
        init = subprocess.run([str(binary), "stress-init", str(stress)],
                              capture_output=True, text=True, timeout=30)
        assert init.returncode == 0, init.stderr
        writer = subprocess.Popen([str(binary), "stress-writer", str(stress)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert writer.stdout is not None
            readable, _, _ = select.select([writer.stdout], [], [], 15)
            assert readable and writer.stdout.readline().strip() == b"STRESS-RUNNING"
            time.sleep(0.1)
            assert writer.poll() is None, "stress writer ended unexpectedly"
            writer.kill()
            assert writer.wait(timeout=5) == -signal.SIGKILL
        finally:
            if writer.poll() is None:
                writer.kill()
            writer.communicate(timeout=5)
        verified = subprocess.run([str(binary), "stress-verify", str(stress)],
                                  capture_output=True, text=True, timeout=30)
        assert verified.returncode == 0, verified.stderr
        print("QEMU durability worker smoke passed: commits and both deletion modes and continuous deletion recovery")


def run_barriers(binary):
    # Local deterministic process-crash smoke. The separate QEMU qualification
    # kills the VM itself at the identical instrumented checkpoints.
    phases = ("before-intent", "after-intent", "after-ref", "after-drafts", "after-finish")
    with tempfile.TemporaryDirectory(prefix="aowis-vm-deletion-barriers-") as location:
        for scenario in ("branch", "truncate"):
            for phase in phases:
                root = Path(location) / f"{scenario}-{phase}"
                prepared = subprocess.run([str(binary), "delete-init", str(root), scenario],
                                          text=True, capture_output=True, timeout=30)
                assert prepared.returncode == 0, prepared.stderr
                env = dict(os.environ, AOWIS_VM_DELETE_POINT=phase)
                worker = subprocess.Popen([str(binary), "delete-writer", str(root)],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
                try:
                    assert worker.stdout is not None
                    readable, _, _ = select.select([worker.stdout], [], [], 15)
                    assert readable, f"worker did not reach {phase}: {scenario}"
                    marker = worker.stdout.readline().decode("ascii").strip()
                    assert marker == "BARRIER " + phase, marker
                    worker.kill()
                    assert worker.wait(timeout=5) == -signal.SIGKILL
                finally:
                    if worker.poll() is None:
                        worker.kill()
                    worker.communicate(timeout=5)
                result = subprocess.run([str(binary), "delete-verify", str(root),
                                         "original" if phase == "before-intent" else "deleted"],
                                        capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, (scenario, phase, result.stderr)
    print("Deletion crash barriers passed: 10 deterministic SIGKILL recovery cases")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--barriers", action="store_true")
    args = parser.parse_args()
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error("compiled worker missing or not executable")
    run(args.binary)
    if args.barriers:
        run_barriers(args.binary)


if __name__ == "__main__":
    main()

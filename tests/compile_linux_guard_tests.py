#!/usr/bin/env python3
"""Build only the DVMDB server and never launch it after a failed build."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_scenario(source: Path, root: Path, scenario: str, arguments: list[str],
                 expected_status: int, should_launch: bool, expected_calls: int) -> None:
    project = root / scenario
    project.mkdir()
    script = project / "compile_linux.sh"
    shutil.copy2(source, script)
    script.chmod(0o755)
    binary = project / "build-linux/aowis-server-dvmdb"
    binary.parent.mkdir()
    # A stale but executable copy exists before configuring/building.
    binary.write_text('#!/bin/sh\nprintf "started:%s\\n" "$PWD" >> "$GUARD_LAUNCH_LOG"\n',
                      encoding="utf-8")
    binary.chmod(0o755)
    if scenario == "missing_binary":
        binary.unlink()

    fakebin = project / "fakebin"
    fakebin.mkdir()
    fake_cmake = fakebin / "cmake"
    fake_cmake.write_text('''#!/bin/sh
printf '%s\\n' "$*" >> "$GUARD_CMAKE_LOG"
if [ "$1" = "-S" ] && [ "$GUARD_SCENARIO" = "configure_failure" ]; then
    exit 31
fi
if [ "$1" = "--build" ]; then
    case " $* " in
        *" --target aowis-server-dvmdb "*)
            if [ "$GUARD_SCENARIO" = "build_failure" ]; then exit 32; fi
            if [ "$GUARD_SCENARIO" = "target_failure" ]; then exit 33; fi ;;
        *) exit 34 ;;
    esac
fi
exit 0
''', encoding="utf-8")
    fake_cmake.chmod(0o755)
    launches = project / "launches.log"
    cmake_calls = project / "cmake-calls.log"
    environment = os.environ.copy()
    environment.update({
        "PATH": str(fakebin) + os.pathsep + environment.get("PATH", ""),
        "GUARD_SCENARIO": scenario,
        "GUARD_LAUNCH_LOG": str(launches),
        "GUARD_CMAKE_LOG": str(cmake_calls),
    })
    # Run from another directory to ensure the script launches its *own* binary.
    result = subprocess.run([str(script), *arguments], cwd=root, env=environment,
                            capture_output=True, text=True, timeout=15, check=False)
    output = result.stdout + result.stderr
    require(result.returncode == expected_status,
            f"{scenario}: expected exit {expected_status}, got {result.returncode}: {output}")
    require(launches.exists() == should_launch,
            f"{scenario}: stale server launch check failed: {output}")
    if should_launch:
        require(launches.read_text(encoding="utf-8") == f"started:{project}\n",
                f"{scenario}: incorrect launch count")
    calls = cmake_calls.read_text(encoding="utf-8").splitlines() if cmake_calls.exists() else []
    require(len(calls) == expected_calls,
            f"{scenario}: expected {expected_calls} CMake calls, got {calls}")
    if expected_calls == 2:
        require("--target aowis-server-dvmdb" in calls[1],
                f"{scenario}: expected only the HTTP server target to be built: {calls}")
        require("-DBUILD_TESTING=OFF" in calls[0],
                f"{scenario}: Release configuration should not enable tests: {calls}")
    if scenario in {"configure_failure", "build_failure", "target_failure"}:
        require("server NOT started" in output,
                f"{scenario}: failure did not clearly prevent launch: {output}")
    if scenario == "missing_binary":
        require("missing or not executable" in output,
                f"{scenario}: missing binary was not detected")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="aowis-dvmdb-build-guard-") as temporary:
        root = Path(temporary)
        for scenario, arguments, exit_code, should_launch, calls in [
            ("configure_failure", [], 31, False, 1),
            ("build_failure", [], 32, False, 2),
            ("target_failure", [], 33, False, 2),
            ("missing_binary", [], 1, False, 2),
            ("successful_build", [], 0, True, 2),
            ("build_only", ["--build-only"], 0, False, 2),
            ("bad_option", ["--unknown"], 2, False, 0),
        ]:
            run_scenario(args.script.resolve(), root, scenario, arguments,
                         exit_code, should_launch, calls)
    print("DVMDB build/launch guards: passed")


if __name__ == "__main__":
    main()

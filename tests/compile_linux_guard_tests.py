#!/usr/bin/env python3
"""Build only the DVMDB server, never launch it after a failed build, and pass
the listen address, port and data root options through to the freshly built
server."""

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
                 expected_status: int, should_launch: bool, expected_calls: int,
                 server_arguments: str = "", message: str = "") -> None:
    project = root / scenario
    project.mkdir()
    script = project / "compile_linux.sh"
    shutil.copy2(source, script)
    script.chmod(0o755)
    binary = project / "build-linux/aowis-server-dvmdb"
    binary.parent.mkdir()
    # A stale but executable copy exists before configuring/building.
    binary.write_text('#!/bin/sh\nprintf "started:%s args:%s\\n" "$PWD" "$*" >> "$GUARD_LAUNCH_LOG"\n',
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
        # A known home for "~/" in --data-root.
        "HOME": str(root / "home"),
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
        launched = launches.read_text(encoding="utf-8")
        require(launched == f"started:{project} args:{server_arguments}\n",
                f"{scenario}: incorrect launch or server arguments: {launched!r}")
    if message:
        require(message in output, f"{scenario}: expected {message!r} in: {output}")
    if expected_status == 2:
        require("Usage:" in output, f"{scenario}: an invalid command line must print usage: {output}")
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
        # Listen address, port and data root: passed to the freshly built
        # server as its own --bind/--port/--data-root options; the ini and
        # defaults apply otherwise.
        for scenario, arguments, server_arguments in [
            ("listen_address", ["--listen-address", "0.0.0.0"], "--bind 0.0.0.0"),
            ("listen_address_port", ["--listen-address", "0.0.0.0:9000"],
             "--bind 0.0.0.0 --port 9000"),
            ("listen_address_equals", ["--listen-address=127.0.0.1:8756"],
             "--bind 127.0.0.1 --port 8756"),
            ("listen_port", ["--listen-port", "9001"], "--port 9001"),
            ("listen_port_equals", ["--listen-port=9004"], "--port 9004"),
            ("listen_address_and_port", ["--listen-port", "9002", "--listen-address", "10.0.0.5"],
             "--bind 10.0.0.5 --port 9002"),
            ("listen_ipv6_any_port", ["--listen-address", "[::]:9003"], "--bind :: --port 9003"),
            ("listen_ipv6_bracketed", ["--listen-address", "[::1]"], "--bind ::1"),
            ("listen_ipv6_plain", ["--listen-address", "::1"], "--bind ::1"),
            ("listen_ipv6_full", ["--listen-address", "fe80::1:2"], "--bind fe80::1:2"),
            ("listen_port_leading_zero", ["--listen-port", "08080"], "--port 8080"),
            ("listen_port_bounds", ["--listen-address", "0.0.0.0:65535"],
             "--bind 0.0.0.0 --port 65535"),
            # Storage root: absolute as given; relative to the CALLER's
            # directory (the script runs from root, the server in the
            # project); "~" is the home directory even unexpanded by the shell.
            ("data_root_absolute", ["--data-root", "/srv/aowis data"],
             "--data-root /srv/aowis data"),
            ("data_root_equals", ["--data-root=/srv/dvmdb"], "--data-root /srv/dvmdb"),
            ("data_root_relative", ["--data-root", "projects/db"],
             f"--data-root {root}/projects/db"),
            ("data_root_tilde", ["--data-root=~/dvmdb"], f"--data-root {root}/home/dvmdb"),
            ("data_root_tilde_alone", ["--data-root", "~"], f"--data-root {root}/home"),
            ("data_root_with_listen", ["--data-root", "/srv/dvmdb", "--listen-address",
                                       "0.0.0.0:9000"],
             "--bind 0.0.0.0 --port 9000 --data-root /srv/dvmdb"),
        ]:
            run_scenario(args.script.resolve(), root, scenario, arguments, 0, True, 2,
                         server_arguments)
        # Invalid listen options: usage, exit 2, nothing configured, built or
        # started.
        for scenario, arguments, message in [
            ("port_zero", ["--listen-port", "0"], "Invalid listen port '0'"),
            ("port_too_large", ["--listen-port", "65536"], "Invalid listen port '65536'"),
            ("port_not_number", ["--listen-port", "http"], "Invalid listen port 'http'"),
            ("port_negative", ["--listen-port", "-1"], "Invalid listen port '-1'"),
            ("address_port_invalid", ["--listen-address", "0.0.0.0:99999"],
             "Invalid listen port '99999'"),
            ("address_port_empty", ["--listen-address", "0.0.0.0:"], "Missing port"),
            ("ipv6_port_empty", ["--listen-address", "[::]:"], "Missing port"),
            ("address_empty", ["--listen-address", ":9000"], "Missing address"),
            ("address_empty_equals", ["--listen-address="], "Missing address"),
            ("ipv6_brackets_empty", ["--listen-address", "[]:9000"], "Missing address"),
            ("address_host_name", ["--listen-address", "localhost"],
             "Invalid listen address 'localhost'"),
            ("address_missing_value", ["--listen-address"], "Missing value for --listen-address"),
            ("port_missing_value", ["--listen-port"], "Missing value for --listen-port"),
            ("port_twice", ["--listen-address", "0.0.0.0:9000", "--listen-port", "9001"],
             "The listen port is given twice"),
            ("port_option_twice", ["--listen-port", "9000", "--listen-port", "9001"],
             "The listen port is given twice"),
            ("address_twice", ["--listen-address", "0.0.0.0", "--listen-address", "::"],
             "--listen-address given twice"),
            ("build_only_with_address", ["--build-only", "--listen-address", "0.0.0.0"],
             "--build-only starts no server"),
            ("build_only_with_port", ["--listen-port", "9000", "--build-only"],
             "--build-only starts no server"),
            ("build_only_twice", ["--build-only", "--build-only"], "--build-only given twice"),
            ("unknown_after_valid", ["--listen-port", "9000", "--bind", "0.0.0.0"],
             "Unknown option: --bind"),
            ("data_root_missing_value", ["--data-root"], "Missing value for --data-root"),
            ("data_root_empty", ["--data-root="], "Missing directory for --data-root"),
            ("data_root_twice", ["--data-root", "/a", "--data-root", "/b"],
             "--data-root given twice"),
            ("build_only_with_data_root", ["--build-only", "--data-root", "/srv/dvmdb"],
             "--build-only starts no server: it takes no data root"),
        ]:
            run_scenario(args.script.resolve(), root, scenario, arguments, 2, False, 0,
                         message=message)
    print("DVMDB build/launch guards: passed")


if __name__ == "__main__":
    main()

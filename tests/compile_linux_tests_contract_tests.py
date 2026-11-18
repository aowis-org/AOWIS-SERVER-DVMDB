#!/usr/bin/env python3
"""The Linux tests helper must build and run nested submodule tests, too."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def scenario(source: Path, root: Path, name: str, args: list[str],
             fail_stage: str = "", total: int = 83) -> None:
    project = root / name
    project.mkdir()
    script = project / "compile_linux_tests.sh"
    shutil.copy2(source, script)
    script.chmod(0o755)
    fakebin = project / "fakebin"
    fakebin.mkdir()
    (fakebin / "cmake").write_text("""#!/bin/sh
printf 'cmake %s\n' "$*" >> "$CALL_LOG"
if [ "$1" = "--build" ]; then
    case " $* " in *" --target "*) exit 38;; esac
    if [ "$FAIL_STAGE" = "build" ]; then exit 37; fi
else
    if [ "$FAIL_STAGE" = "configure" ]; then exit 36; fi
fi
exit 0
""", encoding="utf-8")
    (fakebin / "ctest").write_text("""#!/bin/sh
printf 'ctest %s\n' "$*" >> "$CALL_LOG"
case " $* " in *" -L "*) exit 39;; esac
case " $* " in
    *" -N "*) printf 'Total Tests: %s\n' "$MOCK_TOTAL";;
    *) if [ "$FAIL_STAGE" = "test" ]; then exit 40; fi;;
esac
exit 0
""", encoding="utf-8")
    (fakebin / "cmake").chmod(0o755)
    (fakebin / "ctest").chmod(0o755)
    log = project / "calls.log"
    env = os.environ.copy()
    env.update({"PATH": str(fakebin) + os.pathsep + env.get("PATH", ""),
                "CALL_LOG": str(log), "MOCK_TOTAL": str(total),
                "FAIL_STAGE": fail_stage})
    result = subprocess.run([str(script), *args], cwd=root, env=env,
                            text=True, capture_output=True, timeout=20, check=False)
    calls = log.read_text(encoding="utf-8").splitlines() if log.exists() else []
    if name in {"default", "both"}:
        expected = 4 if name == "default" else 8
        require(result.returncode == 0, f"{name}: failed: {result.stderr}")
        require(len(calls) == expected, f"{name}: unexpected calls: {calls}")
        for offset in range(0, expected, 4):
            configure, build, list_tests, run_tests = calls[offset:offset + 4]
            require(configure.startswith("cmake -S ") and "-DBUILD_TESTING=ON" in configure,
                    f"{name}: tests not enabled: {configure}")
            require(build.startswith("cmake --build ") and " --parallel" in build
                    and "--target" not in build,
                    f"{name}: not building ALL targets: {build}")
            require(list_tests.startswith("ctest --test-dir ") and list_tests.endswith(" -N"),
                    f"{name}: not enumerating all tests: {list_tests}")
            require(run_tests.startswith("ctest --test-dir ")
                    and run_tests.endswith(" --output-on-failure") and " -L " not in run_tests,
                    f"{name}: not running complete suite: {run_tests}")
    else:
        require(result.returncode != 0, f"{name}: failure accepted")
        if name == "no_tests":
            require(len(calls) == 3, f"{name}: CTest executed with no tests: {calls}")
        if name == "configure_error":
            require(len(calls) == 1, f"{name}: build ran after configure failed: {calls}")
        if name == "build_error":
            require(len(calls) == 2, f"{name}: tests ran after build failed: {calls}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path, required=True)
    source = parser.parse_args().script.resolve()
    with tempfile.TemporaryDirectory(prefix="aowis-dvmdb-full-tests-") as temporary:
        root = Path(temporary)
        scenario(source, root, "default", [])
        scenario(source, root, "both", ["--both"])
        scenario(source, root, "no_tests", [], total=0)
        scenario(source, root, "configure_error", [], fail_stage="configure")
        scenario(source, root, "build_error", [], fail_stage="build")
        scenario(source, root, "test_error", [], fail_stage="test")
        scenario(source, root, "bad_argument", ["--unknown"])
    print("Linux test script builds ALL and runs every CTest test: passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
usage() {
    printf 'Usage: %s [--debug | --release | --both]\n' "${0##*/}"
}
case "${1:-}" in
    --help|-h) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; usage; exit 0 ;;
    ''|--debug|--release|--both) [[ $# -le 1 ]] || { usage >&2; exit 2; } ;;
    *) usage >&2; exit 2 ;;
esac
run_suite() {
    local build_dir="$1" build_type="$2"
    local arguments=(-S "$project_dir" -B "$build_dir" -G Ninja -DBUILD_TESTING=ON)
    if [[ -n "$build_type" ]]; then
        arguments+=("-DCMAKE_BUILD_TYPE=$build_type")
    fi
    cmake "${arguments[@]}"
    # Build the default ALL target: aowis-dvmdb-tests only aggregates DVMDB
    # targets, not the test executables registered by nested MODEL submodules.
    # ALL also covers future submodule test executables without maintaining a
    # fragile list of test target names here.
    cmake --build "$build_dir" --parallel
    local registered
    registered="$(ctest --test-dir "$build_dir" -N)"
    if ! grep -Eq 'Total Tests: [1-9][0-9]*' <<< "$registered"; then
        printf 'ERROR: no tests registered\n%s\n' "$registered" >&2
        exit 1
    fi
    # Do not filter on the DVMDB-specific 'aowis' label: MODEL tests belong
    # to the same configured CTest suite but have no such label.
    ctest --test-dir "$build_dir" --output-on-failure
}
case "${1:-}" in
    '') run_suite "$project_dir/build-linux-tests" '' ;;
    --debug) run_suite "$project_dir/build-linux-tests-debug" Debug ;;
    --release) run_suite "$project_dir/build-linux-tests-release" Release ;;
    --both) run_suite "$project_dir/build-linux-tests-debug" Debug; run_suite "$project_dir/build-linux-tests-release" Release ;;
esac

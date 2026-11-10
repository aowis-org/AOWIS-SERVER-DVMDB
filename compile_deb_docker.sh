#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
dist_directory="$project_root/dist"
docker_image="aowis-server-dvmdb-debian-builder:trixie"
revision="${DEB_REVISION:-1}"
run_tests=false
build_network="host"
test_network="host"

usage()
{
    cat <<'HELP'
Usage: ./compile_deb_docker.sh [--test] [--revision NUMBER]
                               [--build-network bridge|host]
                               [--test-network bridge|host]

Build the AOWIS DVMDB Debian package in a Debian 13 (trixie) Docker
container without installing Debian build dependencies on the host.

  --test              Also build the next Debian revision and qualify package
                      installation, upgrade, runtime HTTP, removal and purge in
                      a disposable, fresh Debian container.
  --revision NUMBER   Initial Debian revision (positive integer, default: 1).
  --build-network MODE
                      Docker network for the toolchain image and package builds
                      (bridge or host; default host).
  --test-network MODE Docker network for package-installation qualification
                      (bridge or host; default host). With host networking,
                      the test HTTP listener uses the HOST loopback port 18755.
  -h, --help          Show help.

Output: dist/aowis-server-dvmdb_<version>+deb<revision>_<architecture>.deb
HELP
}

while (( $# > 0 )); do
    case "$1" in
        --test)
            run_tests=true
            shift
            ;;
        --revision)
            if (( $# < 2 )); then
                echo 'Error: --revision needs a number.' >&2
                exit 2
            fi
            revision="$2"
            shift 2
            ;;
        --build-network|--test-network)
            if (( $# < 2 )); then
                printf 'Error: %s requires bridge or host.\n' "$1" >&2
                exit 2
            fi
            if [[ "$2" != "bridge" && "$2" != "host" ]]; then
                printf 'Error: %s must be bridge or host, not %s.\n' "$1" "$2" >&2
                exit 2
            fi
            if [[ "$1" == "--build-network" ]]; then
                build_network="$2"
            else
                test_network="$2"
            fi
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Error: unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! "$revision" =~ ^[1-9][0-9]*$ ]]; then
    echo 'Error: --revision must be a positive integer.' >&2
    exit 2
fi

if [[ ! -f "$project_root/external/AOWIS-SERVER-MODEL/CMakeLists.txt" ]]; then
    echo 'Error: missing MODEL submodule; run ./git_submodule_init.sh first.' >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo 'Error: Docker is required. Install Docker and ensure the daemon is accessible.' >&2
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    echo 'Error: Cannot access the Docker daemon.' >&2
    exit 1
fi

mkdir -p "$dist_directory"

printf 'Building Debian 13 package toolchain image (network: %s)...\n' "$build_network"
if [[ "$build_network" == "host" ]]; then
    printf '%s\n' 'Host networking is enabled for the toolchain image and package builds.'
fi
docker build \
    --network "$build_network" \
    --file "$project_root/tools/debian/Dockerfile" \
    --tag "$docker_image" \
    "$project_root/tools/debian"

build_package()
{
    local package_revision="$1"
    local old_packages=()
    local new_packages=()
    shopt -s nullglob
    old_packages=("$dist_directory"/aowis-server-dvmdb_*"+deb${package_revision}"_*.deb)
    shopt -u nullglob
    # Do not accept a stale package as the result of a failed or skipped build.
    if (( ${#old_packages[@]} > 0 )); then
        rm -f -- "${old_packages[@]}"
    fi

    printf '\nBuilding Debian package revision %s...\n' "$package_revision"
    docker run --rm --init \
        --network "$build_network" \
        --user "$(id -u):$(id -g)" \
        --volume "$project_root:/source:ro" \
        --volume "$dist_directory:/packages" \
        --env "DEB_REVISION=$package_revision" \
        "$docker_image" \
        bash /source/tools/debian/build_in_container.sh

    shopt -s nullglob
    new_packages=("$dist_directory"/aowis-server-dvmdb_*"+deb${package_revision}"_*.deb)
    shopt -u nullglob
    if (( ${#new_packages[@]} != 1 )); then
        printf 'Error: Docker build produced %d packages for revision %s (expected 1).\n' \
            "${#new_packages[@]}" "$package_revision" >&2
        exit 1
    fi
    printf 'Package ready: %s\n' "${new_packages[0]}"
}

build_package "$revision"

if [[ "$run_tests" == true ]]; then
    next_revision="$((revision + 1))"
    build_package "$next_revision"
    printf '\nQualifying installed packages in a clean Debian 13 container (network: %s)...\n' "$test_network"
    if [[ "$test_network" == "host" ]]; then
        printf '%s\n' 'WARNING: qualification binds 127.0.0.1:18755 in the host network namespace.'
        printf '%s\n' 'Ensure that port 18755 is available on the host; host services are not modified.'
    fi
    docker run --rm --init \
        --network "$test_network" \
        --volume "$dist_directory:/packages:ro" \
        --volume "$project_root/tools/debian:/qualification:ro" \
        --env "BASE_REVISION=$revision" \
        --env "UPGRADE_REVISION=$next_revision" \
        debian:trixie-slim \
        bash /qualification/test_installed_package.sh
fi

printf '\nDebian package build%s passed. Packages: %s\n' \
    "$(if [[ "$run_tests" == true ]]; then printf ' and installation qualification'; fi)" \
    "$dist_directory"

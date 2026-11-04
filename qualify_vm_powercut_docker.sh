#!/usr/bin/env bash
# Self-contained QEMU ext4/XFS power-cut qualification via Docker.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
    cat <<'USAGE'
Usage: ./qualify_vm_powercut_docker.sh [--prepare] [qualification options]

Builds/reuses an AOWIS Debian 12 QEMU Docker image. Without --prepare it
builds a matching Release worker, downloads and verifies a Debian cloud image,
provisions a disposable guest, and runs the real QEMU power-cut qualification.

Examples:
  ./qualify_vm_powercut_docker.sh --rounds 4
  ./qualify_vm_powercut_docker.sh --rounds 24 --filesystems ext4 xfs
  ./qualify_vm_powercut_docker.sh --workload deletions --rounds 10 --filesystems ext4 xfs
  ./qualify_vm_powercut_docker.sh --workload deletion-stress --rounds 32 --filesystems ext4 xfs
  ./tools/prepare_vm_powercut_docker.sh

Docker is the only required host application. /dev/kvm is passed to Docker
only when available; otherwise QEMU uses much slower software emulation.
No host disks are attached. All guest data disks are freshly created files.

Environment:
  AOWIS_VM_DOCKER_IMAGE   Docker image tag (default: aowis-dvmdb-powercut:debian12)
  AOWIS_VM_CACHE          Cached cloud image and compiled worker
  AOWIS_VM_RESULTS        Logs and disposable VM workspaces
  AOWIS_DOCKER            Docker executable (default: docker)
  AOWIS_VM_DOCKER_NETWORK Docker network (host, default; or bridge)
USAGE
}

case "${1:-}" in
    --help|-h) usage; exit 0 ;;
esac

image="${AOWIS_VM_DOCKER_IMAGE:-aowis-dvmdb-powercut:debian12}"
# Host networking avoids Docker bridge DNS failures during apt and during
# Debian guest-image downloads and guest cloud-init package installation.
# It affects Docker's networking only: the workload still runs inside QEMU.
network="${AOWIS_VM_DOCKER_NETWORK:-host}"
case "$network" in
    host|bridge) ;;
    *) printf 'ERROR: AOWIS_VM_DOCKER_NETWORK must be host or bridge.\n' >&2; exit 2 ;;
esac
docker_cmd="${AOWIS_DOCKER:-docker}"
if ! command -v "$docker_cmd" >/dev/null 2>&1; then
    printf 'ERROR: Docker is required (not installed or not on PATH).\n' >&2
    exit 1
fi

# Sending only the Dockerfile directory prevents copying large repositories,
# test fixtures, build products, or submodules to the Docker daemon.
# Match the host's numeric identity inside Debian's account database. Docker
# --user alone sets the UID/GID but does not create a passwd entry; OpenSSH
# ssh-keygen requires getpwuid() to succeed for the running UID.
"$docker_cmd" build --network "$network" --tag "$image" \
    --build-arg "AOWIS_HOST_UID=$(id -u)" \
    --build-arg "AOWIS_HOST_GID=$(id -g)" \
    --file "$project_dir/tools/vm-powercut/Dockerfile" \
    "$project_dir/tools/vm-powercut"

if [[ "${1:-}" == --prepare ]]; then
    [[ $# -eq 1 ]] || { usage >&2; exit 2; }
    printf 'QEMU qualification Docker image ready: %s\n' "$image"
    exit 0
fi

cache_dir="${AOWIS_VM_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aowis-dvmdb-powercut}"
result_dir="${AOWIS_VM_RESULTS:-$project_dir/build-vm-powercut}"
mkdir -p -- "$cache_dir" "$result_dir"
cache_dir="$(cd -- "$cache_dir" && pwd -P)"
result_dir="$(cd -- "$result_dir" && pwd -P)"

kvm_args=()
if [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]; then
    kvm_args=(--device /dev/kvm:/dev/kvm --group-add "$(stat -c %g /dev/kvm)")
    printf 'QEMU: /dev/kvm available; using hardware acceleration.\n'
else
    printf 'QEMU: /dev/kvm unavailable; software emulation (slow).\n'
fi

"$docker_cmd" run --rm --init --network "$network" \
    --user "$(id -u):$(id -g)" \
    --env HOME=/results \
    --volume "$project_dir:/src:ro" \
    --volume "$cache_dir:/cache" \
    --volume "$result_dir:/results" \
    "${kvm_args[@]}" \
    "$image" "$@"

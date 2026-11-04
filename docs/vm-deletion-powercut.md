# Deletion durability qualification (QEMU / ext4 / XFS)

The existing commit-powercut qualification is unchanged and remains the default.
Two additional **opt-in** workloads use the same disposable QEMU guest/data disk
and reboot verifier. Docker provisions QEMU; the Docker container itself is not
the storage-failure simulation.

## Deterministic deletion cut points

```bash
./qualify_vm_powercut_docker.sh --workload deletions --rounds 10 --filesystems ext4 xfs
```

Each round initializes a fresh project/branch fixture on the disposable data
disk and launches a deletion or revision truncation. A **qualification-only**
C++ hook signals the host over SSH when it reaches the requested persistence
boundary and blocks indefinitely. The host kills QEMU with SIGKILL, not the
DVMDB process. The reboot verifier reopens DVMDB, replays durable intents and
checks the deleted branch, surviving branch, affected working copy, unrelated
working copy, HEAD and absence of pending intents. On subsequent boots every
previous fixture is checked again. The fixture manifest itself is synced
before the destructive operation begins.

The pair of workloads (`branch`, `truncate`) rotates through five barriers:

1. `before-intent`: branch and saved draft must survive the unconfirmed operation.
2. `after-intent`: recovery must finish deletion despite lost acknowledgment.
3. `after-ref`: branch/ref state has changed; recovery must remove drafts.
4. `after-drafts`: recovery must finish the persisted deletion intent.
5. `after-finish`: completed deletion must remain deleted on restart.

**Use at least 10 rounds per filesystem to exercise every scenario/barrier pair.**
Fewer rounds perform only the initial subset. The native barrier tests exercise
all ten combinations even when a VM is unavailable.

## Continuous deletion stress

```bash
./qualify_vm_powercut_docker.sh --workload deletion-stress --rounds 32 --filesystems ext4 xfs
```

One DVMDB service repeatedly creates UUIDv7 branches, saves working copies,
then confirms branch deletion. QEMU is terminated during that continuing stream
at varied delays. After reboot the verifier checks repository integrity,
permanent survivor branch, all published working-copy references, and removal
of pending deletion intents. This is statistical coverage; it **does not**
guarantee that the host killed the VM during a particular write. The
instrumented workload above covers deterministic application persistence
boundaries. The continuous workload deliberately tests branch deletion; revision
truncation is exercised in the deterministic workload.

## Local smoke tests

The opt-in Docker build configures `AOWIS_DVMDB_VM_DELETION_BARRIERS=ON` and
links the worker with the native ProjectService. Ordinary builds default to
`OFF`; no blocking hook can activate in those binaries. With an instrumented
build, run:

```bash
cmake --build build-linux-tests --target aowis-dvmdb-vm-powercut-worker -j 8
ctest --test-dir build-linux-tests --output-on-failure \
  -R '^aowis-dvmdb-vm-(powercut-runner-tests|powercut-worker-smoke|deletion-barrier-tests)$'
```

The optional `deletion-barrier-tests` CTest entry exists only when the CMake
option is enabled. It checks all ten barrier phases using local process
SIGKILL, **not** QEMU, and cannot substitute for the VM runs.

## Boundaries of the evidence

Killing QEMU removes guest RAM and guest page cache while preserving the raw
virtual disk. The QEMU data disk uses `cache=none`. This is stronger than
killing a container or DVMDB process but is **not** a simulated physical
power cut to the host's SSD, does not guarantee torn sectors, and does not
inject failed flushes or I/O errors. These require separate storage-layer
fault injection and/or physical-device qualification.

No production data disk is attached or formatted. Tests allocate and format
disposable raw disk-image files only; failures retain the test workspace for
diagnostics. An operation whose deletion intent was durably published may
complete on recovery even if its final response was never acknowledged.

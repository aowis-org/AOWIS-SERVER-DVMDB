# Durability qualification

How DVMDB's crash, disk-full and load behaviour is tested, what each test
proves and what it does not. The first group runs in every
`./compile_linux_tests.sh --both`; the second group is opt-in because it needs
QEMU or takes long. Measured results are in
[engineering findings](engineering-findings.md); operator procedures are in
[operations](operations.md).

Every drill works on disposable directories or disks it creates itself.
Never point these tools at a real storage root or a host block device.

## In CTest

| Test | What it does | What it does not prove |
|---|---|---|
| `aowis-dvmdb-recovery-tests` | Links a separate **test-only** build of the engine with deterministic fault points (process exit, simulated ENOSPC, partial writes) across object append, sync, ref publication, incremental import, compaction and stage-file cleanup; checks integrity, references, index rebuild and later writes | behaviour of the production binary under a real kernel failure |
| `aowis-dvmdb-physical-enospc-tests` | Production library on a private **32 MiB tmpfs** in a new user/mount namespace: fills it until the kernel returns ENOSPC during a commit, a named export and an incremental import, then frees space, reopens, verifies and retries | ext4/XFS journaling, device caches, power loss. **Skips** (not passes) where unprivileged namespaces or tmpfs mounts are forbidden |
| `aowis-dvmdb-sigkill-restart-tests` | A child process commits and reports each commit through a pipe; the parent `SIGKILL`s it at varying points (after the first acknowledgement), reopens, fully verifies and checks every acknowledged commit is there (at most one more, unacknowledged one may be); a second fixture kills bundle exports and imports every completed bundle | page-cache loss or device flush failure: the kernel keeps running |
| `aowis-dvmdb-http-transfer-stress-tests` | The real server on a loopback port: staging admission with a one-transfer budget, an aborted upload and download, then six concurrent clients importing, exporting and reading HEADs across three projects; repositories are verified after shutdown | throughput, latency or memory envelopes |
| `aowis-dvmdb-upgrade-restore-tests` | Four projects with divergent branches and >1 MiB of model data: verified backup, independent restore, checks of IDs, ancestry and payloads, a commit on the restored root, and a second backup/restore of that state | that an older binary reads newer data |
| `aowis-dvmdb-format-compatibility-tests` | Refuses a frozen pre-release pack and its bundles (`tests/fixtures/pack-20261024`) without modifying the destination, and round-trips the current format | — |
| `aowis-dvmdb-working-copy-tests`, `aowis-dvmdb-local-commit-tests` | Draft and ledger crash remnants, torn journal tails, damaged files, concurrent readers and writers, failed and interrupted Commits | — |

Run one of them verbosely:

```bash
ctest --test-dir build-linux-tests-release -R '^aowis-dvmdb-sigkill-restart-tests$' --output-on-failure -V
```

A failed test keeps its private directory and logs under `/tmp` for
inspection.

## QEMU power-cut drill (opt-in)

Kills a whole virtual machine while the production library commits, on
**ext4** and **XFS** data disks, and checks that every acknowledged commit
survives.

```bash
./qualify_vm_powercut_docker.sh --rounds 4                        # both filesystems
./qualify_vm_powercut_docker.sh --rounds 24 --filesystems ext4 xfs
./tools/prepare_vm_powercut_docker.sh                             # optional: build the image only
```

Only Docker is needed on the Linux x86_64 host. The wrapper builds a Debian 12
image (cached), compiles the worker inside it, downloads and SHA-512-checks
the official Debian 12 cloud image, creates a throwaway SSH identity and runs
`tools/qualify_vm_powercut.py`. `/dev/kvm` is used when accessible; otherwise
QEMU emulates (much slower). The first run downloads several hundred MiB;
images are cached in `~/.cache/aowis-dvmdb-powercut/` (`AOWIS_VM_CACHE`),
results go to `build-vm-powercut/run-*/` (`AOWIS_VM_RESULTS`).

Each round:

1. boot the guest, mount a fresh sparse data disk (`/dev/vdb`) and stage the
   worker (the guest OS disk may lose unsynced writes too, so the worker is
   staged on every boot; that never touches the repository);
2. on later boots, reopen the repository in a new process and run full
   verification;
3. the worker commits and prints `ACK <sequence> <record-hash>` after each
   synchronous commit, then waits for `GO`;
4. the host confirms the ACK, sends `GO` (allowing one more, possibly
   in-flight commit) and kills **QEMU** with `SIGKILL`: no sync, no unmount;
5. reboot on the same disk, let the filesystem replay its journal, verify, and
   check that the recovered sequence is the last acknowledged one or exactly
   one more.

Docker uses host networking by default, because bridge DNS often fails while
building the image (`Temporary failure resolving deb.debian.org` means the
build container has no DNS; no test ran). `AOWIS_VM_DOCKER_NETWORK=bridge`
selects bridge networking. The VM's SSH forward binds to 127.0.0.1 only. The
container runs with your UID/GID and creates a matching passwd entry, which
OpenSSH needs.

On success the overlays, keys and cloud-init data are deleted and
`report.json` is kept. On failure everything is kept for inspection, including
a throwaway private key; delete it afterwards.

**What it proves:** consistency after loss of the guest's RAM and page cache on
the tested guest, filesystem, virtio disk (`cache=none`) and cut timings.
**Not:** physical SSD or controller power loss, torn sectors or lying flushes.

### Without Docker

`tools/qualify_vm_powercut.py` runs the same drill with your own guest image:
a bootable x86_64 qcow2/raw image with an SSH server, util-linux, `mkfs.ext4`,
`mkfs.xfs` and a compatible C++ runtime, booting from virtio `/dev/vda`, and an
SSH account that is `root` or has passwordless sudo.

```bash
cmake --build build-linux-tests-release --target aowis-dvmdb-vm-powercut-worker
python3 tools/qualify_vm_powercut.py \
  --guest-image ~/vm-images/test-base.qcow2 --ssh-key ~/.ssh/aowis-vm-test \
  --guest-user aowis-test --cloud-init-seed /tmp/aowis-vm-seed.img \
  --worker ./build-linux-tests-release/aowis-dvmdb-vm-powercut-worker \
  --filesystems ext4 xfs --rounds 24 --kvm \
  --work-root "$(mktemp -d /tmp/aowis-vm.XXXXXXXX)" \
  --report-json build-qualification/powercut.json
```

The guest boots from a new overlay of the image with a new sparse data disk;
the tool refuses unexpected disk layouts before formatting. An optional
cloud-init seed (`cloud-localds`) can create the test user and install
`e2fsprogs`/`xfsprogs`; it is attached read-only and `cloud-init status --wait`
is checked. Use a throwaway SSH key. `--boot-timeout` helps slow guests.

The CTest `aowis-dvmdb-vm-powercut-worker-smoke` only checks the worker's
protocol; it does not run QEMU.

## Branch/revision deletion VM qualification (completed)

Deletion has a separate opt-in VM workload; see
[deletion power-cut qualification](vm-deletion-powercut.md) for crash boundaries,
fixtures, recovery checks, commands and evidence limitations. This does not
replace the commit-acknowledgment drill described above.

The developer's November 5, 2026 Docker/QEMU runs **passed all 84 rounds**:
20/20 deterministic branch-delete/revision-truncate boundaries (10 ext4,
10 XFS), and 64/64 continuous deletion-stress rounds (32 per filesystem).
Each round was verified after reboot. The stress crashes used varied delays
of 37–229 ms; they are not guaranteed to coincide with a physical write.
The tests establish recovery from the tested guest-cache-loss interruptions,
not host-device failure, torn sectors or lost/failed flushes.

## HTTP soak (opt-in)

One server process under six concurrent clients that import multi-MiB bundles
into new branches, export and byte-compare them and read HEADs across three
projects, while `/health` is probed continuously. Admission 503s are retried
and counted. At the end all repositories are reopened, verified and checked
against the import count.

```bash
./build-linux-tests-release/aowis-dvmdb-http-transfer-stress-tests \
  --soak-seconds 3600 ./build-linux-tests-release/aowis-server-dvmdb
```

The log has a progress line per minute (elapsed seconds, imports, exports,
503s). The counts are measurements of that run, not service-level objectives.
This workload creates many branches from repeated transfers; it is not a
hydraulic edit or merge workload ([benchmarks](benchmark-plan.md)).

### Resource sampling

`tools/qualify_http_resources.py` runs the same soak and samples the server and
the client harness from `/proc` (RSS and its high-water mark, file descriptors,
threads, CPU time). Each run gets a new `build-qualification/http-resources-*`
directory with the soak log, `samples.csv` and a JSON report including host and
kernel details and the executables' SHA-256.

```bash
python3 tools/qualify_http_resources.py --seconds 300
python3 tools/qualify_http_resources.py --seconds 3600 --interval-ms 1000 --max-server-rss-mib 256
python3 tools/analyze_http_resource_runs.py build-qualification/http-resources-* \
  --output-json build-qualification/http-resource-analysis.json
```

`--max-server-rss-mib` is an optional ceiling you choose for that run. The
analyzer checks each report against its samples and log hash, and reports
transfers per second (per minute and per quarter of the run), peak RSS and
descriptors and CPU in core equivalents; it flags runs that are not comparable
(duration, host, kernel, binaries). RSS grows with retained history and
caches; growth alone is not a leak.

## Keeping evidence

`tools/qualification_evidence.py` checks saved results; it runs nothing. It
requires at least `--min-rounds` (default 4) consecutive successful power-cut
rounds on both ext4 and XFS with every acknowledgement preserved, and can check
a combined CTest log (`--min-tests`) and a soak log (`--min-soak-seconds`),
recording each file's SHA-256.

```bash
mkdir -p build-qualification
set -o pipefail
./compile_linux_tests.sh --both 2>&1 | tee build-qualification/ctest.log
./build-linux-tests-release/aowis-dvmdb-http-transfer-stress-tests --soak-seconds 300 \
  ./build-linux-tests-release/aowis-server-dvmdb 2>&1 | tee build-qualification/http-soak.log
./qualify_vm_powercut_docker.sh --rounds 4 --filesystems ext4 xfs
python3 tools/qualification_evidence.py \
  --qemu-report build-vm-powercut/run-<id>/report.json \
  --ctest-log build-qualification/ctest.log --soak-log build-qualification/http-soak.log \
  --require-all --output build-qualification/evidence.json
```

Keep the original logs and reports with the summary. Hashes detect later
changes; they are not signed provenance, and a validated summary is not a
release or hardware certificate.

## Still open

Repeated power-cut and soak runs on the final first-release build, physical
power-loss tests on production hardware, degraded devices, cold-cache
workloads, larger divergent replicas and fuzzing of transfers.

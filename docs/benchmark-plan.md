# Native database benchmarks

Benchmark the current append-only pack engine, not alternate physical
backends. Always use a fresh randomly named parent directory; tests must
never reuse an existing repository path.

## Canonical hydraulic models

Use the Release build of the real AOWIS hydraulic persistence benchmark:

```bash
run_root="$(mktemp -d /tmp/aowis-hydraulic-bench.XXXXXXXX)"
./build-linux-tests-release/aowis-dvmdb-hydraulic-edit-benchmark \
  "$run_root/network" 100000 20 3
```

Record initial import time, indexed entity edits, topology changes,
whole-network operations, peak RSS, branch/merge behavior, and disk usage.
Use representative networks and several repetitions. Compiler build mode,
cache state and hardware must be included with results.

## Replication

`tools/benchmark_replication_scaling.py` produces deterministic raw and model
histories for the pack format. Split source preparation from import runs to
avoid mixing fixture construction with importer latency:

```bash
fixture_root="$(mktemp -d /tmp/aowis-replication-fixture.XXXXXXXX)"
rmdir "$fixture_root"
python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux-tests-release/aowis-dvmdb-replication-scaling-benchmark \
  --objects 100000 --commits 20 --payload-bytes 96 \
  --kinds model --backends pack --mode prepare \
  --fixture-dir "$fixture_root" --progress

python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux-tests-release/aowis-dvmdb-replication-scaling-benchmark \
  --objects 100000 --commits 20 --payload-bytes 96 \
  --kinds model --backends pack --repeats 3 --mode imports \
  --fixture-dir "$fixture_root" --output "$fixture_root/results.csv"
```

The CSV includes full and incremental import timings, native verification
phases, independent full-integrity audit time, import peak RSS, transferred
object counts, pack size, and sampled temporary-file allocations. Results
are checked against the expected immutable revision heads. Each import
uses a new disposable repository; the fixture can be retained for reruns.

## Recovery, concurrency and maintenance

`aowis-dvmdb-pack-operational-stress-tests` covers multiple simultaneous
repository handles, readers/writers, stale edits, model merges, incremental
sync, compaction exclusivity and reopening. `aowis-dvmdb-recovery-tests`
adds deterministic crash and ENOSPC injection. Before public release also
run real power-failure and filesystem-specific tests where practical.

Report full repository verification and offline compaction separately.
Compaction is not supported while other handles are open. Measurements from
individual storage benchmarks are workload-specific and should not be used as
production guarantees.

## Physical object index

`aowis-dvmdb-index-scaling-benchmark` and `tools/benchmark_native_index.py`
measure the **existing** SHA-256-to-pack-location index. They do not
silently swap in a second storage engine. Use the optimized Release binary.
The driver **automatically rebuilds the exact Release target supplied in
`--binary`** before generating fixtures; it rejects Debug or foreign CMake
build trees. `./compile_linux.sh --build-only` builds `build-linux`, **not**
`build-linux-tests-release`. To do an explicit clean Release test build first,
run `./compile_linux_tests.sh --release`. The example below uses `build-linux`
so it also works after `./compile_linux.sh --build-only`:

```bash
run_root="$(mktemp -d /tmp/aowis-index-capacity.XXXXXXXX)"
python3 tools/benchmark_native_index.py \
  --binary ./build-linux/aowis-dvmdb-index-scaling-benchmark \
  --sizes 100000,1000000,10000000 \
  --keys 4096 --batch 1024 --reads 1000 --write-rounds 5 \
  --multi-projects 2 --work-root "$run_root" \
  --output "$run_root/index-capacity.csv"
```

For deliberately comparing already-built historical binaries, `--no-build`
skips the automatic rebuild; use it only when that is intentional. Smoke tests
run as part of CTest and do not invoke a nested CMake build.

For a quick validation of the benchmark itself, run
`ctest --test-dir build-linux-tests-release -R aowis-dvmdb-index-scaling-smoke
--output-on-failure`. The benchmark driver creates randomized disposable
subdirectories, refuses existing fixture paths, and removes fixtures after
sampling. **It never modifies an existing user repository.** Rebuild,
compaction and commit phases run on independent *copies* of the fixture.
Large runs require free space for these copies and temporary compacted packs.
Do not run benchmarks on a volume with production repositories or tight disk
budgets. Use `--skip-rebuild` and `--skip-compact` to measure indexing at 10M
objects first without paying for full-pack hashing or maintenance. Use
`--repeats 3` for independent repetitions. The CSV is atomically updated after each completed target/repetition;
partial results from completed targets survive later failures. An interrupted
target does not produce a misleading partial phase row. To assess sparsity, repeat with different `--keys` values,
for example 256 and 65536 at the same physical object targets.

Each row reports a separate process and phase:

- `prepare`: deterministic fixture creation until at least the requested
  **physical** object count. `physical_objects` includes old revision/tree
  objects, not just live model records. It reports durable pack and sidecar
  sizes, creation time, and current/peak RSS.
- `reopen`: fresh-process startup from the intact sidecar, subsequent checksummed
  reads of present UUIDs and misses, current memory before/after construction,
  and process peak RSS. Average read latency is
  `lookup_hit_ms / samples` or `lookup_miss_ms / samples`.
- `rebuild`: identical copy with **only the derived index sidecar removed**;
  measures full verified pack scanning and sidecar recreation. Asserts the
  object count and branch head are unchanged.
- `compact`, `post_compaction_reopen`, `writes`: separately copied fixtures;
  compaction preserves the complete published revision history. Writes report
  mean and p95 latency per fixed-size atomic commit. Phase peaks are not
  polluted by earlier phases.
- `multi_project_open`: multiple physically independent copies held open
  simultaneously in one process. The dynamic `rss_project_N_bytes` columns
  show resident growth as each additional repository is opened.

These measurements are **warm or uncontrolled OS-page-cache** timings,
not guaranteed cold-cache disk latency. For a meaningful 100k/1m/10m
comparison, record CPU, memory capacity, filesystem, storage media,
Linux kernel, build flags, available disk space, run repetitions and whether
cache was cold. `VmRSS` and `VmHWM` come from `/proc/self/status` and are
subject to page granularity, allocator retention and process overhead;
`rss_after_open_bytes - rss_before_bytes` estimates map-related resident
memory but is not an isolated allocator measurement. Record the actual
`physical_objects` value because batches can overshoot the requested target.

**Retain the compact sidecar-backed index** unless reproducible benchmarks establish a regression or stronger alternative. The before/after evidence and assumptions are recorded in [engineering findings](engineering-findings.md). Any future changes must preserve pack integrity and publication guarantees. This benchmark does not yet qualify cold-cache reads, sustained
production mixed workloads, or network-service contention.

## Sustained HTTP resource envelope

Use `tools/qualify_http_resources.py`, which runs the real six-client,
three-project HTTP bundle soak and samples the server from `/proc`, and
`tools/analyze_http_resource_runs.py` to compare saved runs. Commands and
interpretation are in [durability qualification](durability-qualification.md#resource-sampling);
the measured runs (including the branch-lookup fix that removed a throughput
decline as branches accumulated) are in
[engineering findings](engineering-findings.md#measured-sustained-linux-http-resource-envelope).
Keep the original run directories: the analyzer needs their CSV, JSON and log.

This workload transfers bundles and creates branches; it is not representative
hydraulic edit traffic, disk-use or cold-reopen measurement.

## Representative canonical hydraulic workload qualification

The `aowis-dvmdb-hydraulic-edit-benchmark` uses **real serialized
NetworkHydraulic objects** on one project-scoped UUIDv7 revision DAG, not a
synthetic model-record topology.
Every measured edit passes the precise expected UUIDv7 HEAD; branch diff and
hydraulic merge operate on immutable project-qualified revision IDs. The
`engine=pack` metric remains unchanged for existing qualification tooling; it
identifies the pack storage engine, **not** the revision-ID type.
The benchmark also measures indexed pipe reads, complete network reconstruction,
divergent-branch diff, a validated two-parent hydraulic field merge, and
pack/index growth from initial import to completion. It verifies that
nonconflicting fields survive merging and that an immutable pre-merge revision
can still be read. See [hydraulic merging](native-hydraulic-merging.md) for
conflict semantics.

Build the actual **Release** targets and run repeated independent fixtures:

```bash
cmake --build build-linux-tests-release --target \
  aowis-dvmdb-hydraulic-edit-benchmark aowis-dvmdb-verify --parallel
python3 tools/qualify_hydraulic_workload.py \
  --binary ./build-linux-tests-release/aowis-dvmdb-hydraulic-edit-benchmark \
  --verifier ./build-linux-tests-release/aowis-dvmdb-verify \
  --pipes 10000 --edits 20 --topology-edits 3 --repeats 3
```

To compare multiple DVMDB-native Release runs, use the offline analyzer:

```bash
python3 tools/qualify_hydraulic_workload.py \
  --pipes 10000 --edits 20 --topology-edits 3 --repeats 3
python3 tools/qualify_hydraulic_workload.py \
  --pipes 50000 --edits 20 --topology-edits 3 --repeats 3
python3 tools/qualify_hydraulic_workload.py \
  --pipes 100000 --edits 20 --topology-edits 3 --repeats 3
python3 tools/analyze_hydraulic_workloads.py \
  build-qualification/hydraulic-workload-<10k-run> \
  build-qualification/hydraulic-workload-<50k-run> \
  build-qualification/hydraulic-workload-<100k-run> \
  --csv build-qualification/hydraulic-size-comparison.csv
```

For the successful operator-reported October 26, 2026 series, the saved
local directories can be compared without re-running the benchmarks:

```bash
python3 tools/analyze_hydraulic_workloads.py \
  build-qualification/hydraulic-workload-f18bsyo0 \
  build-qualification/hydraulic-workload-7kem6w99 \
  build-qualification/hydraulic-workload-nvqj2qod \
  --csv build-qualification/hydraulic-size-comparison.csv
```

This also prints the **indexed scalar/topology edit**, **targeted-read** and
**divergent-diff** medians, plus ratios and milliseconds per 1,000 pipes for
whole-network operations. These distinctions matter: a 100K-pipe full
reconstruction or merge is not a proxy for the latency of an indexed edit.
The comparison will refuse different benchmark or verifier hashes, inconsistent
edit settings and missing raw evidence; without the saved per-run files, only
the operator-supplied aggregate timings can be assessed.

Replace the placeholders with the actual output directory names. The comparison
requires matching benchmark/verifier SHA-256 hashes and edit counts; because
its executable identity changed during the DVMDB API rename, do **not** mix
pre-rename 10K evidence with fresh DVMDB runs. Each benchmark run has a fresh
disposable repository and removes its own fixtures on success.

The runner saves a unique `build-qualification/hydraulic-workload-*/report.json`
and the original benchmark and verifier logs. Every repetition uses a brand-new
repository, verifies its **complete native repository integrity from a new
process**, and checks that reported physical pack/index sizes match on-disk
bytes. Each executable's SHA-256 is recorded. By default it removes
**only its freshly created disposable repository directories** on success;
use `--keep-fixtures` when investigating a particular case. A failed run
retains the fixture and diagnostics. Do not run against actual project data.

The result field `fresh_process_reopen_full_verify_ms` measures **process
startup + repository reopening + full logical/physical verification** together,
not pure open latency. The OS page cache is not cleared, so it must **not** be
called a cold-cache disk benchmark. Internal `full_network_read_ms` is a
separate warm in-process canonical reconstruction. Pack and index growth
includes all tested operations, including revision/branch overhead. Every
iteration's raw metrics remain available in `report.json`; no absolute latency
or memory threshold is implied by this fixture. It is a linear chain of pipes
with representative entity types and deterministic edits—not a field-derived
real water network or a concurrent GUI session. Run with several network sizes
and actual project fixtures before claiming deployment representativeness.

The normal Qt-enabled CTest suite includes a small end-to-end hydraulic
workload smoke and an additional Qt-free, offline runner-contract regression.
The smoke only verifies semantic correctness, never a performance SLA.

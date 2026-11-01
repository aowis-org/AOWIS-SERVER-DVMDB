# Engineering evidence and architectural decisions

This document preserves **measured results and reusable engineering conclusions**.
It is not a development diary. The current implementation is described in
[architecture](architecture.md); reproducible measurement commands belong in
[benchmark-plan](benchmark-plan.md), and unfinished qualification in
[roadmap](roadmap.md).

## Storage backend choice

The database uses **one native, append-only, content-addressed pack** per
repository, with immutable revision objects and persistent Merkle model and
reverse-reference trees. An `objects.idx` sidecar is derived from the pack and
can be rebuilt. Branch publication occurs after synchronizing the objects
that a new immutable revision references. The public C++ API, versioning,
merging and replication are not SQL adapters.

Earlier investigations considered SQL-backed storage (SQLite/PostgreSQL) and
an experimental native block layout. SQL's transaction mechanisms alone do
not prove that a whole AOWIS multi-entity operation was implemented as one
atomic SQL transaction; conversely, the native engine's one-ref publication
provides atomic visibility for one branch update without undoing physical
appends. The native engine still needs independently tested recovery, index
integrity, backups and replica semantics. Those properties, rather than the
label of a storage backend, are the useful comparison criteria.

The older experimental block format is intentionally **not supported**: a
current build refuses to open it, and bundles written by older builds are
refused as well, so pre-release data is recreated rather than migrated. The
pack is the only physical format.

**Benchmark evidence boundary:** No adequately controlled numerical
pack-versus-block or SQLite/PostgreSQL-versus-native performance comparison
is preserved in this repository snapshot. No speedup, durability advantage,
or resource figure for those historical alternatives should be quoted as a
verified result. The reproducible numbers below compare *two lookup structures
on the same native pack and format*, not two different storage engines.

## Physical object lookup: measured index improvement

Release measurements from the developer's Linux machine on October 23–24,
2026, with deterministic workloads, shared fixture parameters and an
uncontrolled/warm OS page cache. Physical object count includes historical
revision and Merkle objects, not just currently visible entities.

| Physical objects | Old map: extra RSS | Compact lookup: extra RSS | Old reopen | Compact reopen |
|---:|---:|---:|---:|---:|
| 101,387 | 8.35 MiB | 2.73 MiB | 49.5 ms | 38.9 ms |
| 1,001,959 | 72.8 MiB | 16.7 MiB | 690 ms | 290 ms |
| 10,001,723 | 703.5 MiB | 128.8 MiB | 8.61 s | 3.66 s |

At 10 million objects, **two concurrently open repositories** reached about
1.48 GB total process RSS with the original `unordered_map` lookup, versus
about **278 MB** with the compact implementation. A 1,000-hit read probe on
the 10M fixture took ~47 ms for the old index and ~31.8 ms for the compact
index; cache conditions were not independently controlled. Write latency
remained of similar order, but the test is not a cold-I/O or production SLA.

**Implementation:** 8-byte open-addressed fingerprint/ordinal slots; a
candidate must be verified against the full 32-byte SHA-256 in the checksummed
sidecar before using its physical location. A 4,096-entry bounded hot cache
reduces repeated sidecar reads. Pack bytes, sidecar encoding, revision IDs
and bundle format are **unchanged**. The accelerator presently has a
`2^32-1` sidecar-entry limit per generation.

**Benchmark lesson:** A first comparison incorrectly used an older executable
from `build-linux-tests-release` after rebuilding `build-linux`. The benchmark
driver now rebuilds the **exact selected Release target** and reports its
storage-implementation identity. Always record the executable/build tree and
do not interpret an unchanged result as evidence until binary freshness is
established.

## Incremental replication and Merkle pruning

A measured one-record incremental update on a 2,000-record model originally
re-read **9,442 local objects at 50 commits** (~3 MB) and **202,398 objects
at 800 commits** (~75 MB) in an earlier ASan profile. The approach visited
historical data that the recipient already had. This showed that a small
transfer can still be expensive if validation or delta generation traverses
the complete local ancestry.

The current design uses trusted, already-verified local objects as import
boundaries and aligns old/new compressed Merkle tries to enumerate relevant
changes. In a focused 1-record delta regression, only **106 local object
reads at both 50 and 400 commits** were observed (previously roughly 9,400
and 100,000 respectively), with a **17-object delta bundle**. This is a
specific fixture/result, not a claim of constant-time synchronization in
all histories. Exported deltas may contain duplicate-existing content when
older revisions restore a previously deleted subtree, but imports do not
store identical content twice. More complex revision DAGs and large deltas
still need representative profiling.

Full-history export uses disk-backed deduplication and external sorting to
avoid an in-memory set of all reachable hashes. HTTP transfer staging and
internal native scratch have separate budgets; see [operations](operations.md).

## Durability and correctness evidence

- **Atomic branch visibility:** immutable objects are synchronized before
  branch-reference publication. An unacknowledged operation can nevertheless
  have published; on retry, reread the branch head first.
- **Pack corruption policy:** unpublished torn tails can be saved as evidence
  and truncated if safe; damage to published objects or in the middle of a
  pack fails closed. A lost/damaged *derived* sidecar is rebuildable.
- **Semantic safety:** model reference integrity, hydraulic domain validation
  and recursive criss-cross merge bases are separate responsibilities. Native
  generic merge correctness does not replace hydraulic-network validation.
- **Physical ENOSPC:** a disposable 32 MiB Linux tmpfs test forces real
  `ENOSPC` during commit, export and incremental import, then verifies recovery
  and retry. It is not an ext4/XFS power-loss test.
- **Process termination:** production-library SIGKILL/reopen tests cover
  acknowledged commit recovery, potential unacknowledged publication and
  complete named-bundle visibility. The filesystem/kernel itself remains live.
- **Backups:** an offline whole-service backup utility copies a locked
  multi-project root, independently verifies its repositories and publishes
  a new backup directory atomically. Online backup is not implemented.

**QEMU guest-cache-loss evidence (October 24, 2026):** A Debian 12 Linux
cloud guest running under QEMU/KVM with disposable virtio data disks completed
**four abrupt whole-VM kills on ext4 and four on XFS**. After each cut, the
same disk was remounted and the production native repository was fully
verified in a new process. Every acknowledged branch revision survived. In
three of four rounds on each filesystem, one additional complete but
unacknowledged revision was recovered; this is permitted because publication
can precede the client's acknowledgement. Both filesystems finished at
sequence **7**, and each independently reported `result: passed`. The operator's
JSON report was preserved outside the ephemeral VM and contains per-round
revision hashes. The run used guest-cache loss (`qemu-system-x86_64` killed by
`SIGKILL`, virtual disks with `cache=none`), **not power loss to the host SSD or
controller**. Results are specific to the observed guest, filesystem and write
schedule; a four-round run is not exhaustive crash-interleaving coverage.

Merely passing the QEMU-worker CTest does **not** execute the VM drill. The
optional offline evidence checker verifies the report's consistency and can
record hashes of supplied CTest and HTTP-soak logs; it does not execute any
workload or authenticate the operator's evidence. See [durability
qualification](durability-qualification.md).

## Service concurrency: observed load

The server uses per-project operation coordination and a bounded worker pool,
with independent projects rather than a global operation mutex. A developer
Linux Release test on October 24, 2026 completed a five-minute soak over
**three projects**, with **7,516 successful imports and 7,516 exports**, zero
admission-limit 503 responses, and **12 ms worst health request latency**
reported by the harness. These figures describe the tested workload, not guaranteed throughput/latency
under arbitrary contention. Longer soaks and realistic hardware/slow-client
workloads remain necessary.

## Pre-release format rejection

An October 24, 2026 pack and its bundles (`tests/fixtures/pack-20261024`) are
kept as immutable bytes from an earlier build. They predate UUIDv7 revisions
and UUID-keyed branch records. Since the SHA-256 revision
engine was removed (October 31, 2026), `aowis-dvmdb-format-compatibility-tests`
uses them to prove the opposite of compatibility: the current build must
refuse the old repository and every old bundle with an explicit error, without
modifying the destination. The same test exercises a full current-format
round trip (two branches, incremental delta, idempotent re-import, alias
rejection, reopen). See [format policy](format-compatibility.md).

## Service-root restore qualification

`aowis-dvmdb-upgrade-restore-tests` builds four projects (divergent branches,
one with more than 1 MiB of model data), takes a verified offline backup,
restores it independently, checks project IDs, branch UUIDs, ancestry and
payloads, commits into the restored root, then backs up and restores that new
state again. It guards the backup tooling against cross-project omissions; it
is **not** evidence that an older executable can read new writes.

## What these results do not prove

The work does not yet establish durability after physical power loss, formal
linearizability, production latency SLOs, large-scale cold-cache throughput,
all-node corruption repair, supported long-term on-disk format compatibility,
or any authentication/security boundary for the HTTP service. The complete
list of qualification gates is in the [roadmap](roadmap.md).

## Measured sustained Linux HTTP resource envelope

On October 25, 2026, the operator ran the **same Release server and workload
binaries twice** on an x86_64, eight-logical-CPU Linux host (kernel
`7.2.6-arch2-1`). Each run exercised six clients and three projects for
300 seconds. The exact binary SHA-256 values and complete run reports are
retained in the operator's `build-qualification` directories; report values
were supplied to this qualification record:

| Observation | Run A | Run B |
|---|---:|---:|
| Successful bundle imports | 7,505 | 7,459 |
| Successful bundle exports | 7,505 | 7,459 |
| Combined transfers/s | 50.03 | 49.73 |
| Admission 503 | 0 | 0 |
| Worst reported health latency | 4 ms | 3 ms |
| Server initial RSS | 27,500 KiB | 27,688 KiB |
| Server final RSS | 28,188 KiB | 28,384 KiB |
| Peak sampled server RSS | 28,752 KiB | 28,736 KiB |
| Peak server file descriptors | 37 | 37 |
| Peak server threads | 10 | 10 |
| Server CPU time (user + system) | 914.96 s | 909.89 s |

The two runs' combined transfer rates differ by about **0.6%**. First-to-last
server RSS changed by just **688 KiB** and **696 KiB**, respectively; peak
sampled RSS was approximately **28.1 MiB** in both. CPU consumption is about
**3.0 logical cores on average** during each 300-second run, not 915 seconds
of elapsed time and not a host-wide utilization measurement. The sampling
interval was 500 ms; the server was observed for approximately 299.8 seconds.
The unchanged descriptor peak, small short-run RSS change, zero admission
503s and reproducible transfer rate are positive **short-duration** evidence.

These two reports **do not establish absence of memory leaks** over hours,
quantify retained revision-history growth, capture host contention, or set a
production memory/CPU/latency guarantee. Five-minute CPU load, page cache,
disk and background workloads were not independently controlled. A separate
read-only report analyzer (`tools/analyze_http_resource_runs.py`) checks source
reports against their per-process CSV and saved log, and compares central
10–25% versus 75–90% server-RSS medians. It reports changes descriptively,
without inventing a leakage threshold.

**One-hour run (October 25–26, 2026):** The same hashed Release server and
stress executables, the same eight-logical-CPU x86_64 host, and Linux kernel
`7.2.6-arch2-1` completed a **3,600-second** continuous run with **67,747
imports and 67,747 exports** (135,494 successful transfers, **37.64/s**),
**zero admission 503 responses**, and **10 ms** worst health response. The
server accumulated **10,837.03 CPU seconds**, equivalent to approximately
**3.01 logical cores** during the 3,598.789-second observed interval. The
server's first and last sampled RSS were **27,648 KiB** and **28,292 KiB**
(a **644 KiB** first-to-last increase); its sampled peak was **36,220 KiB**
(**35.4 MiB**), with 10 peak threads and 36 peak file descriptors. A higher
intermediate peak followed by lower final RSS is not proof of a leak or of a
fixed maximum memory footprint. The saved operator evidence is
`build-qualification/http-resources-qfk67i39/`, with executable and log
SHA-256 hashes in `report.json`; the samples were taken every 1,000 ms.

Compared with the mean of the two five-minute runs (~49.88 transfers/s),
the hour-long run averaged **~24.5% fewer transfers/s**. This is a real
**between-run difference**, but *not* evidence that throughput declined
within that hour: the earlier stress harness did not record interval
completion counts. The workload creates an increasing number of uniquely
named branches throughout its run; its working set differs between hour-long
and five-minute tests. Host I/O, background work and caching were not
independently recorded. Future runs print optional one-minute progress
checkpoints, and the offline report analyzer computes per-interval rates.
Old run reports remain valid, but cannot reconstruct missing historical
progress data. The operator also reported playing a YouTube video in Firefox
throughout the hour-long run, unlike the five-minute baselines. CPU scheduling,
thermal/power limits and video-decoding activity are plausible confounders;
none were separately logged. Investigate falling within-run rates, if observed,
before attributing a cause or changing the storage engine.

**Fifteen-minute run (October 26, 2026):** An independently reported
**900-second** workload on the same Release server and stress binary completed
**25,493 imports and 25,493 exports** (50,986 combined, **56.65 transfers/s**),
with **zero 503 responses** and **5 ms** worst health-request latency. Peak
server RSS was **30,532 KiB** (~29.8 MiB), versus **27,736 KiB** initially and
**29,680 KiB** at the last sample. The maximum was **36 file descriptors** and
**10 threads**; server CPU consumption was **2,750.54 CPU seconds** (~3.06
logical CPUs averaged over the full run). Evidence was recorded at
`build-qualification/http-resources-y9bg_t6y/`.

The 15-minute result is higher, not lower, than the five-minute baselines,
but this does not isolate history-growth effects from host conditions. The
saved log contains no time-segmented counters, so the rate within the run is
unknown. Its stress executable SHA-256 equals the earlier runs: the Release
binary was not rebuilt after a Debug CTest failure interrupted the two-build
workflow. The current qualification harness computes progress from the whole
soak's start time, and a real-server smoke test requires checkpoints. Reports
saved before those counters existed cannot be reconstructed retrospectively.

## Growing-branch HTTP soak: measured decline and targeted lookup fix

**900-second instrumented run (October 26, 2026):** After both Debug and
Release passed **46/46 CTest tests**, the operator ran the six-client/three-project
HTTP soak on an Intel i5-8365U Linux host. It completed **24,363 imports and
24,363 exports** (48,726 transfers, **54.14/s** overall) with **zero admission
503s** and **11 ms** worst health latency. The Release server's SHA-256 was
`04155f8e46ce16721515f76df75fbce28e9d8316bbc37058bd0447bccbba6536`;
its stress-client SHA-256 was
`86b32795e5c276f8d875349d7b291c7c7663e68a4ba34a16237fb4c8037586b8`.
Server RSS sampled at 28,424 KiB initially, 28,080 KiB finally, and a
29,580 KiB peak (~28.9 MiB); peak file descriptors were 37, peak threads 10,
and CPU consumed 2,725.56 seconds. Evidence:
`build-qualification/http-resources-ha2jc118/` (operator-local path).

The first 60-second interval completed **3,797 transfers** (**63.28/s**), but
the last completed 60-second interval (840–900 s) completed **2,505**
(**41.75/s**): approximately **34% lower throughput** within the same run.
Intermediate checkpoints decreased broadly with branch count. This is stronger
evidence than comparing runs with different desktop activity, but the root
cause cannot be established by counts alone.

**Code-level scaling issue discovered:** branch-specific HTTP reads previously
called `Repository::listBranches()` and searched the resulting vector merely
to establish whether one branch existed. `ProjectService::head()`,
`ProjectService::history()`, `ProjectService::createBranch()` and the HTTP
bundle-export/branch-scope guard all used this pattern. As the soak creates a
new ref for every successful import, each such lookup enumerated and sorted an
increasing number of ref files. The targeted replacement checks a single
regular ref with `lstat` under the existing repository lock; explicit branch
listing still performs the full enumeration. Symlinks/non-regular refs remain
unrecognized. A 4,096-ref regression exercises branch reads, history, forks,
missing references and unsafe refs. This change does not alter the pack,
index, bundle or durability protocol.

**Measured post-change comparison (October 26, 2026):** The operator ran the
same 900-second, six-client/three-project workload after rebuilding the Release
server and stress executable, on the same reported x86_64 Linux host/kernel.
The following outcomes are taken from saved operator-reported `report.json`
and one-minute `soak.log` contents, not independent execution by CI:

| Observation | Before direct branch lookup | After direct branch lookup |
|---|---:|---:|
| Successful imports | 24,363 | 28,675 |
| Successful exports | 24,363 | 28,675 |
| Combined transfers per second | 54.14 | 63.72 |
| First 60-second interval | 63.28/s | 64.82/s |
| Last 60-second interval | 41.75/s | 63.10/s |
| First-to-last interval change | -34.03% | -2.65% |
| Peak sampled server RSS | 28.89 MiB | 28.10 MiB |
| Peak server file descriptors | 37 | 37 |
| Worst health latency | 11 ms | 4 ms |
| Admission HTTP 503 | 0 | 0 |
| Server CPU seconds | 2,725.56 | 2,717.12 |

Overall throughput increased **17.7%**, and the late-run decline was much
smaller. Rates vary from minute to minute; the first/last statistic does not
mean each intermediate minute remained flat. The post-change run began at
28,492 KiB RSS, ended at 27,912 KiB, peaked at 28,772 KiB, and retained a
maximum of 10 threads. Its original evidence directory is
`build-qualification/http-resources-m5po2jr0/`, with SHA-256 of the server
`ffd42d37edb64fd5ead567d50480159bf7f89902b93f8cba5de973f317208261`
and of the workload
`6a756d8230f25c01578f5a94089ba0ea2ab5b1c2f60ccbe9bf59fbb942176c83`.
The previous server and workload hashes differ: this is a **cross-build**
comparison. Although it corroborates the identified directory-scan bottleneck,
background activity, temperature, page cache and fixture timing were not
controlled. Do not report 17.7% as a portable causal speedup or throughput
SLA. The branch-lookup improvement should be retained unless a regression
provides contrary evidence; longer same-build runs and representative model
workloads remain to be qualified.

## One-hour growing-branch qualification after direct lookups (October 26, 2026)

The operator completed the requested **3,600-second** six-client,
three-project HTTP stress campaign on the optimized Release server (same
executables as the post-change 900-second run). It reported **110,733
successful imports and 110,733 exports**: **221,466 total transfers**,
**61.52/s** average, **zero admission HTTP 503s**, and **10 ms** worst health
response. The native HTTP soak checks the published heads as it runs and
reopens the repository after server shutdown to verify branch heads and that
successful imports match the retained branch count. The run therefore also
exercised a growing branch catalogue with more than 110,000 imported heads
across three projects, not only HTTP request counters.

The Release executable SHA-256 values are
`ffd42d37edb64fd5ead567d50480159bf7f89902b93f8cba5de973f317208261`
(server) and
`6a756d8230f25c01578f5a94089ba0ea2ab5b1c2f60ccbe9bf59fbb942176c83`
(stress). Environment: x86_64 Linux `7.2.6-arch2-1`, Intel i5-8365U,
8 logical CPUs. Evidence retained locally as
`build-qualification/http-resources-lqle9235/`; the original JSON report
records the workload SHA-256, server identity and the soak-log SHA-256.
These are operator-supplied measurements, not measurements recreated from
raw CSV within CI.

| Observation | Earlier 1-hour run | Direct-lookup 1-hour run |
|---|---:|---:|
| Imports and exports (each) | 67,747 | 110,733 |
| Combined transfers/s | 37.64 | 61.52 |
| Admission HTTP 503 | 0 | 0 |
| Worst health latency | 10 ms | 10 ms |
| Peak sampled server RSS | 36,220 KiB (35.4 MiB) | 28,628 KiB (28.0 MiB) |
| Initial / final sampled server RSS | 27,648 / 28,292 KiB | 28,068 / 27,944 KiB |
| Peak server file descriptors | 36 | 37 |
| Peak server threads | 10 | 10 |
| Server CPU time | 10,837.03 s | 10,830.11 s |

The optimized run achieved **63.5% more transfers** in the same duration,
while the sampled RSS ended **124 KiB below** its first observed sample.
The peak is a sampled operating-system RSS value, **not** a maximum memory
allocation or deployment SLA. CPU consumption was ~3.01 logical-core
seconds per elapsed second, which is not a measurement of systemwide CPU
usage.

Unlike the earlier one-hour run, this run recorded **60 throughput phases**.
The first minute completed **3,428 transfers** (57.13/s), and the final
minute **3,482** (58.03/s). Quarter-hour aggregate rates were:

| Elapsed time | Combined transfers/s |
|---|---:|
| 0–15 minutes | 62.01 |
| 15–30 minutes | 58.70 |
| 30–45 minutes | 61.56 |
| 45–60 minutes | 63.81 |

There is **no sustained within-run downward trend** in this hour-long
branch-growth fixture. The quarter-level view is more representative than
isolated first/last minute samples, and the analyzer now derives this view
from saved progress checkpoints (time-weighting boundaries if necessary).
The earlier one-hour run had no progress checkpoints, so no historical
first/last-quarter comparison can be reconstructed.

The two one-hour reports have different server and stress executable hashes;
moreover, the operator previously reported Firefox/YouTube activity during
the earlier run, and host load was not independently measured. This is
strong supporting evidence that the direct single-branch lookup removed an
important scaling cost, **not** a controlled causal 63.5% speedup guarantee.
This workload continuously creates branches from repeated transfers; it
is not a representative hydraulic-model edit/merge workload, a storage
capacity study, or a test of authentication on untrusted networks. Those
are separate release qualification tasks.

## Current test baseline

On October 31, 2026, after removing the SHA-256 revision engine and the
review fixes (O(1) revision lookup, linear reachability, draft locking and
storage rework), the operator reported **66/66 CTest tests passing** in the
Qt-enabled build; the Qt-free build runs 57 suites, which also pass under
ASan/UBSan. The eight-cut QEMU/KVM ext4/XFS drill is run separately from
CTest and has not been repeated since October 24.

## Canonical hydraulic workload qualification boundary

The existing hydraulic edit benchmark models canonical AOWIS network
serialization and entity validation. A repeated qualification runner now
measures targeted/complete reads, divergent-branch diff, validated hydraulic
merge and physical pack/sidecar growth; it also independently reopens and
fully verifies each disposable repository. See [benchmark plan](benchmark-plan.md).

The operator completed three independent **10,000-pipe** Release runs
(20 scalar edits, 3 topology edits) before the public DVMDB API rename.
The **median full-network read** was **1,085.58 ms** (range 953.38–1,297.88 ms),
**median canonical merge** **1,738.38 ms** (1,714.26–2,083.17 ms), and
**median fresh-process full verification** **1,132.82 ms**
(1,099.12–1,158.29 ms). Every run reported **12.86 MiB** final pack size
and independent full verification passed. These are operator-reported
warm/uncontrolled-OS-cache measurements, not pure open times or cold-cache
results. The renamed executables have new hashes and report identifiers; the
operator subsequently repeated the 10K baseline and ran the 50K/100K series
with DVMDB-native Release executables, as recorded below.

**DVMDB hydraulic network size series (operator, October 26, 2026):**
With the renamed Release benchmark, the operator then completed **three
independent successful runs at each of 10K, 50K and 100K pipes**, using
20 scalar edits and three topology edits per repository:

| Pipes | Median full-network reconstruction | Median validated merge | Median new-process full verification | Final pack per run |
| ---: | ---: | ---: | ---: | ---: |
| 10,000 | 1,033.85 ms | 1,877.05 ms | 1,357.37 ms | 12.86 MiB |
| 50,000 | 5,843.52 ms | 11,492.34 ms | 7,065.88 ms | 62.88 MiB |
| 100,000 | 11,251.48 ms | 20,336.20 ms | 12,729.78 ms | 125.34 MiB |

Relative to the 10K run, a 10-fold larger network required **10.88x** the
reconstruction time, **10.83x** the merge time and **9.38x** the separate-process
full-verification time; final pack size was **9.75x** as large. These results
are broadly proportional to pipe count in this synthetic workload. They do
**not** prove a complexity bound or exclude other workload-specific scaling
issues. Timings are three-run medians; OS cache, host contention and filesystem
I/O were not controlled. The operator supplied summary timings rather than the
complete per-run JSON/CSV and executable hashes in this exchange, so the
cross-size analyzer's executable-identity and detailed evidence checks remain
to be run on the saved result directories.

These series were measured **before** the October 31 changes, when every
revision lookup still re-verified the whole ancestry; they should be repeated
before being quoted for the current build.

The full-network read (~11.25 seconds) and merged-revision operation
(~20.34 seconds) at 100K pipes are **whole-network operations**, not indexed
single-entity read or scalar edit latencies. Before changing the storage
engine, examine the separately recorded targeted-read, indexed-edit, diff,
index-growth and peak-RSS metrics and establish an application latency
budget. The small CTest smoke is correctness coverage, not production-scale
evidence. Unlike the branch-heavy HTTP bundle soak, this exercises real
canonical model payloads, but its chain topology, deterministic edit pattern
and lack of concurrent clients still limit production representativeness.

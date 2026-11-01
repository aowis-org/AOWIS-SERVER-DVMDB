# Architecture

DVMDB is a C++20 library with an HTTP service on top. The library stores
versioned engineering models as immutable, content-addressed objects; the
service hosts many projects in one storage root. Everything below is
implemented; planned work is in the [roadmap](roadmap.md). Byte layouts are in
[storage formats](storage-formats.md), product rules in the
[version-control contract](version-control-semantics.md).

## Layers

| Layer | Code | Responsibility |
|---|---|---|
| Model definitions and codec | `external/AOWIS-SERVER-MODEL` (`AOWIS::Model`) | Typed models, canonical `ModelCodec<T>`, validation, value rules, field merge, MODEL edit transactions |
| Hydraulic persistence | `aowis::dvmdb::hydraulic` (`AOWIS::DvmdbHydraulic`) | Networks as one root record + 256-member membership chunks + one record per entity; indexed edits; hydraulic-aware merges; draft membership materialization |
| Native store | `aowis::dvmdb::Repository` (`aowis-dvmdb-store`) | Object pack, Merkle tries, reverse-reference index, revisions, branches, merges, diffs, bundles, verification, compaction |
| Project service | `aowis::dvmdb::ProjectService` (`AOWIS::DvmdbProjectService`, Qt-free) | Catalogue of projects, repository identity, role and tracking, private drafts, local Commit ledger |
| HTTP service | `aowis-server-dvmdb` (cpp-httplib, `src/dvmdb_http_routes.cpp`) | JSON/binary REST adapter over the project service; no persistence of its own |
| Tools | `aowis-dvmdb-verify`, `-compact`, `-sync`, `-access`, `tools/offline_backup.py` | Offline verification, compaction, bundle transfer, backup/restore |

Standalone AOWIS builds can use the library directly, without HTTP. Do not
point an embedded instance and the HTTP service at the same live root: the
catalogue lock lets only one of them open it.

## Identity and history

* Projects, branches, revisions and drafts have **UUIDv7** identities. Names
  are display data and never storage keys.
* A revision is an immutable record binding its UUID, project, the model trie
  root, the reverse-reference trie root and zero, one or two ordered parents
  together with each parent's record SHA-256. The record's SHA-256 therefore
  commits to the whole ancestry; SHA-256 is integrity, UUIDv7 is identity.
* A revision UUID is resolved through a write-once binding to its record hash,
  so a lookup is O(1): one binding read, one record read, parent bindings
  checked. Whole-history verification is done by `aowis-dvmdb-verify` and by
  bundle import, which stops at revisions already trusted locally.
* A branch record holds a name, a nullable HEAD (null = unborn) and, if
  created from another branch, the immutable origin branch and fork revision.
  Provenance is metadata, not an ancestry edge.
* The model is a path-compressed 16-way Merkle trie from entity UUID to record
  hash. Unchanged records and subtrees are shared between revisions. A second
  trie maps each entity to the entities that reference it.

## Catalogue and repositories

One storage root holds `catalogue.lock`, `repository.id`, the optional
`repository.tracking`, `projects/<uuid>/` (manifest + native repository),
`working-copies/` and `local-commits/` ([layout](storage-formats.md#catalogue-root)).
Each project's repository is an append-only object pack with a rebuildable
index, UUID-named branch records and the revision bindings
([layout](storage-formats.md#native-repository)).

## Writing and durability

* **Commits.** Objects are appended and fsynced before anything refers to
  them; then the revision binding is written once, then the branch record is
  replaced atomically (stage, fsync, rename, fsync directory). A crash leaves
  at most unreachable objects. Every HEAD change is a compare-and-swap on the
  expected HEAD; an explicit null expectation pins an unborn branch.
* **Torn pack tail.** On open, if the first invalid record is followed by
  nothing valid up to EOF and every published HEAD lies before it, the region
  was never published: it is copied to `recovered-pack-tails/`, fsynced and
  truncated (`Repository::recoveredPackTailBytes()`). An invalid record followed
  by a valid one, or damage reaching a published HEAD, is mid-pack corruption:
  the repository refuses to open and nothing is modified.
* **Index.** `objects.idx` is derived. A missing or damaged index, or one that
  is behind the pack, is rebuilt by scanning the pack. Normal open checks the
  index structure; payload SHA-256 is checked on every read.
* **Compaction** (offline, exclusive) writes a new pack generation containing
  everything reachable from any branch or any bound revision, verifies it,
  then publishes it via `objects.current`. If it fails after that switch, the
  handle is invalidated and must be reopened.
* **Catalogue records** (project manifests, repository identity, tracking,
  drafts, ledger records) are staged and published with no-replace link or
  atomic rename plus directory fsync. Any publication whose outcome is
  uncertain (for example a failed directory fsync) marks the service
  unhealthy: further writes are refused until restart. Crash remnants of all
  of these are removed when the root or repository is opened.
* **Scratch space** for import/verification tables and HTTP bundle spools
  lives in each repository's `tmp/` (nameless `O_TMPFILE` files where
  available), on the pack's filesystem rather than a RAM-backed `TMPDIR`.
  C++ callers can use `Repository::createScratchFile()`.

What has and has not been qualified (SIGKILL, real ENOSPC, QEMU power cuts) is
in [durability qualification](durability-qualification.md) and
[engineering findings](engineering-findings.md).

## Concurrency

| Lock | Scope | Held for |
|---|---|---|
| `catalogue.lock` (flock, exclusive) | storage root | the lifetime of the owning `ProjectService` |
| `maintenance.lock` (flock) | repository | shared by every open handle; exclusive for compaction |
| `repo.lock` (flock) + handle mutex | repository | every repository operation |
| project operation lock | project | one repository operation at a time per project, with a bounded queue (`max_pending_per_project`; full → 503 `busy`) |
| per-draft lock | draft | every draft write and local Commit |
| catalogue mutex | service | short catalogue/health checks only, never across I/O |

Lock order: project operation → draft → catalogue mutex. Projects never block
each other, and a slow draft write or Commit holds up only its own draft and
project. Draft reads take no lock at all. Multi-call hydraulic reads stay
consistent by naming one immutable revision.

## Replication

A revision bundle carries one branch's history: everything reachable for a
full bundle, or for an incremental bundle only the revisions not reachable from
the recipient's base and, per revision, only the trie objects that changed.
Import verifies hashes, ancestry, trie structure and the exact reverse-reference
index before writing, and then only creates a branch or fast-forwards it;
divergent histories are refused. Branch and revision UUIDs are preserved.
The same code backs the CLI and the bounded HTTP transfer routes
([synchronization](offline-repository-synchronization.md)).

## Errors

Every engine failure is an `aowis::dvmdb::StorageError` with a
`StorageErrorKind` (`include/aowis/dvmdb/storage_error.h`); service-level
failures are `ProjectError` with a `ProjectErrorKind`. Classify by kind, never
by message text. Load functions return `std::nullopt` only for "not found".
The HTTP mapping is in the [service guide](native-database-service.md#errors).

## Limits of the design today

* Linux/POSIX only (`flock`, `fsync`, `O_TMPFILE`, `renameat2`).
* One writer per project at a time; no fine-grained multi-writer
  transactions.
* No TLS, authentication or authorization
  ([security backlog](security-permissions-design.md)).
* The on-disk format is not frozen for a public release
  ([format policy](format-compatibility.md)).

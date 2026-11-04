# AOWIS-SERVER-DVMDB

**DVMDB is AOWIS's native, versioned model database**: a C++20 library and an
HTTP service that store AOWIS engineering models (in particular hydraulic
networks) as immutable, content-addressed revisions with branches, merges and
replication between independent installations.

Repository: https://github.com/aowis-org/AOWIS-SERVER-DVMDB

```text
AOWIS GUI, other AOWIS services, remote clients
                    │  REST over HTTP (/dvmdb/v1)
             aowis-server-dvmdb ── many projects in one storage root
                    │
    aowis::dvmdb::ProjectService  ── catalogue, drafts, local Commit
                    │
    aowis::dvmdb::Repository      ── one per project: object pack, Merkle
                                     tries, revisions, branches, bundles
```

Standalone AOWIS embeds the native DVMDB service in the GUI process, then
accesses its normal HTTP routes through a dynamic localhost endpoint. No
separate server executable is required for Standalone.

## What it does

* **Versioned models.** Every project, branch, revision and draft has a UUIDv7.
  A revision is an immutable whole-model snapshot with zero, one or two
  parents, an author, a message and its commit time; SHA-256 binds its
  content, ancestry and metadata. Branches are movable refs
  with a nullable **committed** HEAD (a new project starts with an unborn
  `main` and no revision) and immutable fork provenance. Projects may also
  contain multiple independent unborn branches.
* **Storage.** One append-only object pack per project with a rebuildable
  index, path-compressed Merkle tries keyed by entity UUID, a reverse-reference
  index, crash-safe ref publication, full verification and offline compaction.
* **Hydraulic models.** Complete `NetworkHydraulic` persistence (all 18 entity
  collections, chunked membership, reference and physical-value validation),
  indexed reads and edits, and hydraulic-aware three-way merges with explicit
  conflict choices.
* **Private drafts.** Durable working copies in a private repository: leased,
  fenced, generation-checked autosave of MODEL edit events, and an idempotent
  local Commit with crash recovery.
* **Replication.** Full and incremental revision bundles, verified before
  anything is written, fast-forward only, over a CLI or HTTP.
* **Service.** A multi-project HTTP server with bounded per-project queues,
  transfer staging budgets and explicit 409/413/503 semantics.

DVMDB is **pre-release**. There is no TLS, authentication or authorization;
keep the server on localhost or a trusted network. HTTP local Commit, saved-draft network summaries/import, permanent local-Commit
MODEL action archives and read-only first-parent History routes are implemented.
Native C++ can verify and atomically select certain archived hydraulic action
boundaries with a durable V8 draft cursor. The MODEL-aware server supports
fenced HTTP selection of active single-network actions and archived Commit
actions. A zero-position draft journal can enter committed history without
losing its Redo tail; the UI is still read-only pending Qt end-to-end
acceptance and GUI wiring. **Unborn-base draft action replay remains unfinished.**
Hydraulic solver feasibility is not a Save/Commit precondition. Real external
GUI and embedded Standalone process-restart acceptance, whole-project
Clone/Fetch and recoverable Publish also remain open. See the
[integration roadmap](docs/roadmap.md), [current action history](docs/commit-action-archive.md)
and [remaining History work](docs/history-roadmap.md).

## Quick start

```bash
./git_submodule_init.sh        # once: initialize the AOWIS-SERVER-MODEL submodule
./compile_linux.sh             # build Release and start the server in the foreground
```

In a second terminal:

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready      # {"status":"ready"}
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects   # {"projects":[]}
```

The server prints its storage root at startup. Without `--data-root` or an INI
setting it uses `./aowis-dvmdb-data` if that exists, otherwise
`~/.local/share/aowis-dvmdb/`. Configuration, limits, deployment and errors:
[running the service](docs/native-database-service.md).

## Build and test

Linux only. Requirements: a C++20 compiler, CMake, Ninja, OpenSSL, and Qt 6
Core for the model codec, hydraulic persistence and the HTTP server. The store
and project service also build without Qt
(`-DAOWIS_DVMDB_WITH_MODEL_CODEC=OFF`).

```bash
./compile_linux_tests.sh          # current test build and CTest suite
./compile_linux_tests.sh --both   # optional: independent Debug and Release suites
./compile_linux.sh --build-only   # build only the Release server and dependencies
```

`compile_linux.sh` builds **only** `aowis-server-dvmdb` and its transitive
dependencies, then starts that freshly built server by default. `--build-only`
skips launching it. Tests, benchmarks and unrelated CLI tools are not built by
the normal server script. A failed configuration/build never launches an old
binary.

| Directory | Contents |
|---|---|
| `include/aowis/dvmdb/`, `src/` | library (`aowis-dvmdb-store`, `AOWIS::DvmdbProjectService`, `AOWIS::DvmdbHydraulic`) and HTTP routes |
| `tools/` | `aowis-dvmdb-verify`, `-compact`, `-sync`, `-access` sources; `offline_backup.py`; qualification and benchmark drivers; `zip_dvmdb.sh`, `apply_patch.sh` |
| `tests/` | CTest suites, fault-injection and durability drills |
| `benchmarks/` | storage, hydraulic and replication benchmarks |
| `deploy/` | production INI, systemd unit, optional nginx TLS proxy |
| `external/` | `AOWIS-SERVER-MODEL` (model types and canonical codec), vendored cpp-httplib |

## Using the library

```cpp
#include <aowis/dvmdb/native_store.h>

using namespace aowis::dvmdb;

// One native project repository. The ProjectService also owns the catalogue.
Repository repository("/path/to/project/repository");
const ProjectId project = ProjectId::generate();

const BranchId main = repository.createUnbornBranch(project, "main");

BranchMetadata scenario_metadata;
scenario_metadata.tags = {"hydraulics", "experiment"};
scenario_metadata.description = "Alternative network model";
const BranchId scenario = repository.createUnbornBranch(
    project, "scenario", std::nullopt, scenario_metadata);

// Both branches have independent UUIDv7 identities and no committed revision.
// An editor works in a private draft; committing that draft creates a revision.
```


Engine failures are `StorageError` with a `StorageErrorKind`; service failures
are `ProjectError` with a `ProjectErrorKind`. Classify by kind, never by
message text. See [architecture](docs/architecture.md) and the
[documentation index](docs/README.md).

## Command-line tools

The normal `compile_linux.sh` does not build these utilities. Build the
individual CMake target or use a dedicated tools build before invoking one.

```bash
./build-linux/aowis-dvmdb-verify REPOSITORY            # full verification (default)
./build-linux/aowis-dvmdb-compact REPOSITORY           # offline, exclusive
./build-linux/aowis-dvmdb-sync export REPOSITORY PROJECT main /tmp/main.aowbundle
python3 tools/offline_backup.py backup ROOT NEW_BACKUP --verify-binary ./build-linux/aowis-dvmdb-verify
```

`REPOSITORY` is one project's `<root>/projects/<uuid>/repository`. Stop the
server first. See [verification](docs/repository-verification.md),
[synchronization](docs/offline-repository-synchronization.md) and
[operations](docs/operations.md).

## Conventions for this repository

* C++ namespace `aowis::dvmdb`, headers under `include/aowis/dvmdb/`, REST
  prefix `/dvmdb/v1`, server executable `aowis-server-dvmdb`.
* Persisted markers and magics keep their AOWIS names (`AOWIS-BRANCH-*`,
  `AOWREV2x`, `AOMD`, `revision-v2/`); they are part of the current format.
* One current format, no compatibility readers or migrations before the first
  release: unsupported data is rejected ([format policy](docs/format-compatibility.md)).
* Source snapshots: `./tools/zip_dvmdb.sh` writes `../dvmdb.zip` (this project
  and its initialized submodules, no Git metadata or data roots). Patches are
  ZIPs named `aowis-dvmdb-*.zip` with entries rooted at `AOWIS-SERVER-DVMDB/`,
  applied with `./tools/apply_patch.sh ~/Downloads/<patch>.zip`; files to delete
  are listed in `.aowis-patch-delete.txt` inside the ZIP.

## License

See [LICENSE](LICENSE).

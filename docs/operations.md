# Operating DVMDB

Procedures for the people who run a DVMDB storage root: finding it, backing it
up, restoring and upgrading it, and reacting to full disks and failures.
Server configuration is in [running the service](native-database-service.md);
the tests behind these procedures are in
[durability qualification](durability-qualification.md).

A storage root holds `catalogue.lock`, `repository.id`, the optional
`repository.tracking`, `projects/<uuid>/` (manifest and repository),
`working-copies/` (drafts), `local-commits/` (Commit ledger), and
`local-commit-revisions/` (checksummed revision-to-action index)
([layout](storage-formats.md#catalogue-root)). **It is one unit.** Never copy
or restore only a pack, only one project, or only the drafts: a repository
needs its branch records, revision bindings and generation markers, and drafts
and ledger records refer to projects.

## Find the active root

The server prints `DVMDB storage root: <path>` at startup. Without
`--data-root` or `[storage] root=` it uses `./aowis-dvmdb-data` (relative to
its working directory) if that exists, otherwise `$XDG_DATA_HOME/aowis-dvmdb/`;
the `deploy/` INI uses `/var/lib/aowis-dvmdb`. Roots are never moved or merged
automatically.

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects
```

The project list shows the root the running process opened. An empty list
usually means the wrong root, not lost data.

Only one process can own a root: `catalogue.lock` stays inside it and is held
for the owner's lifetime. Never delete a live lock, point two processes (for
example the server and a Standalone application) at one root, or combine the
directories of two roots. Different roots are different databases.

## Backup and restore

Backups are **cold**: stop the server and every direct library user first.
There is no online backup.

```bash
./compile_linux.sh --build-only
python3 tools/offline_backup.py backup  /path/to/root      /backup/new-snapshot \
  --verify-binary ./build-linux/aowis-dvmdb-verify
python3 tools/offline_backup.py restore /backup/new-snapshot /path/to/new-root \
  --verify-binary ./build-linux/aowis-dvmdb-verify
python3 tools/offline_backup.py verify  /path/to/new-root \
  --verify-binary ./build-linux/aowis-dvmdb-verify
```

`backup` and `restore` do the same thing (copy, verify, publish) and differ
only in intent. They:

1. take the catalogue lock and every repository's lifetime lock, so a running
   server or library user makes them fail instead of copying a live root;
2. refuse symbolic links and special files in the source;
3. validate the repository identity, tracking file, every project manifest,
   every branch record and every draft (meta, candidate and journal with their
   digests);
4. copy the whole root to a hidden sibling directory, compare the copy with
   the source, and run `aowis-dvmdb-verify --full` on every copied repository;
5. fsync everything and publish the copy under the destination name with no
   replace. The destination must not exist.

`verify` checks a root the same way, on a private copy, without publishing
anything. An interrupted run can leave a hidden `.aowis-offline-copy-*`
directory; only a published destination is a backup.

Restore into a **new** root, point a server at it, check `/ready`, the project
list, the expected branch HEADs and a few model reads, and keep the original
until the new root has proved itself. Verification checks storage, not
hydraulic semantics. Run a full restore drill now and then; a verified backup
is necessary but not sufficient.

A filesystem snapshot of a stopped root is also a valid backup. Verify it the
same way (`offline_backup.py verify` on the snapshot) before relying on it.

## Upgrading or relocating a root

An engine upgrade, a new host or a new filesystem is a **cold, whole-root**
move:

1. Stop everything using the root. Record the binary, the projects, all branch
   HEADs and a few historical reads. Take a verified backup and store it
   elsewhere.
2. Restore the backup into a new directory **with the new build's**
   `aowis-dvmdb-verify`. This proves the new build reads the data.
3. Start a disposable server of the new build on the restored root and check
   projects, branches, ancestry and model reads, not just that it starts.
4. Make a test commit, reopen, read history again, and back up and restore
   that state once more.
5. Switch over. Keep the original root until the rollback window has passed.
   An older binary is not guaranteed to read data written by a newer one; to
   roll back, restore the original backup.

Before the first release a new build may reject data written by an older
pre-release build outright ([format policy](format-compatibility.md)); there
is no migration.

## Full disk and busy budgets

| Signal | Meaning | Action |
|---|---|---|
| `507 insufficient_storage`, `StorageErrorKind::ResourceExhausted` | the filesystem reported ENOSPC or EDQUOT | stop writes and transfers, check free bytes, inodes and quotas; never delete pack generations, `objects.current` or ref files by hand |
| `503 staging_busy` | the server's HTTP transfer staging budget is fully reserved | retry later; not a full disk |
| `503 scratch_busy` | the import/export/verification scratch cap (`max_internal_scratch_mib`) is reached | retry later; raise the cap only with disk headroom |
| `503 busy` | the project's operation queue is full | retry later |

Scratch files live in each repository's `tmp/`, are normally nameless and
disappear when their operation ends; named leftovers are removed when the
repository is opened again. The scratch cap counts logical file extents, which
is not a disk-block guarantee; keep filesystem headroom or an OS quota.
The library has no scratch cap by default (`setInternalScratchLimit()`).

**Bundle exports to a path** write a private file in the destination
directory, fsync it and link it under the requested name without replacing
anything. A crash before that leaves no file under the name; a crash after it
leaves a complete bundle. If an export failed during publication, inspect the
destination and verify or import it before retrying under a new name. Where
`O_TMPFILE` is unavailable a killed exporter can leave a hidden
`.aowis-export-*` file; remove it once no exporter is running. Exports to a
caller-owned descriptor get no such publication or cleanup.

## Failures and recovery

* **A write failed with an I/O error after it may have taken effect** (a ref
  rename, a Commit): reopen and read the branch HEAD, the draft generation or
  the Commit status before retrying. A blind retry can duplicate work; use the
  caller-chosen IDs and operation IDs the API offers.
* **`503 unavailable` after a draft, Commit or tracking write**: the service
  could not tell whether a catalogue publication took effect (typically a
  failed directory fsync) and refuses further writes. Check the disk, then
  restart. On restart unpublished staging entries are removed, every draft and
  ledger record is read from disk, and a pending local Commit is resolved on
  its next replay or status lookup.
* **A repository must be reopened** (compaction failed after switching
  generations): restart the service, or reopen the handle in the library.
* **Torn pack tail**: opening a repository after an interrupted append may set
  an unpublished tail aside in `recovered-pack-tails/` and log it. Published
  state is untouched. Keep the copy until it has been looked at.
* **A damaged draft** does not stop the service from opening; reading it
  answers 500 with details in the log. If `discard` cannot read it either,
  stop the service, move the draft's directory out of `working-copies/` for
  inspection, and create a new draft.
* **Unexplained corruption**: stop writers, keep an untouched copy of the whole
  root, run verification on another copy and keep the logs. Never repair an
  object by hand, reset a HEAD or delete a published ref.

## Compaction

`aowis-dvmdb-compact REPOSITORY` is offline and exclusive. Stop all users,
take a verified backup, compact, then run `aowis-dvmdb-verify --full` on the
repository. Compaction keeps everything reachable from any branch or any bound
revision ([details](repository-verification.md#compaction)).

## Not covered yet

Online backup, automated snapshots, multi-process scratch enforcement,
qualification on specific production filesystems and physical power-loss
guarantees ([durability qualification](durability-qualification.md)).

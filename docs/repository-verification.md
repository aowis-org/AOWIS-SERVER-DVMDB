# Repository verification

`aowis-dvmdb-verify` checks one native repository
(`<root>/projects/<uuid>/repository`). The full check uses the **same logical
validator as bundle import**, so a history that passes verification is one an
import would accept, and the other way round.

```bash
./build-linux/aowis-dvmdb-verify /path/to/repository             # full (default)
./build-linux/aowis-dvmdb-verify --full /path/to/repository
./build-linux/aowis-dvmdb-verify --physical /path/to/repository  # objects only
```

Exit code 0 means verified, 1 a failed check or unreadable repository, 2 bad
arguments. A full check prints the number of physical and reachable objects,
branches, revisions, verified trie nodes and distinct model snapshots, and
states what it did not check. `--physical` says explicitly that logical checks
were skipped. The checker holds the repository lock throughout, so HEADs
cannot move during a check.

## What the full check covers

1. **Physical objects.** Every pack record's framing and SHA-256 and its index
   entry — including objects nothing refers to.
2. **Branch records.** Every entry of `refs/heads/` is a regular file named by
   a canonical UUIDv7 with a valid, unique name and a current `AOWIS-BRANCH-*`
   record ([format](storage-formats.md#branch-records-refsheads)). A non-null
   HEAD must resolve to a revision whose record hash matches the one stored in
   the branch record. Interrupted `.stage-*` files are not branches. (That a
   fork revision is an ancestor of the HEAD is checked when a bundle is
   imported, not by this command.)
3. **Revisions.** Every revision binding in `revision-v2/ids/` — reachable from
   a branch or not — must resolve to the canonical record with that SHA-256,
   belong to the repository's project, and bind each parent UUID to the
   parent's record hash. All ancestry, including both parents of every merge,
   must exist and be acyclic.
4. **Merkle tries.** Every trie reachable from a revision has canonical nodes,
   legal compressed prefixes and depth ordering, correct UUID-to-slot paths
   and non-zero leaf hashes; leaf objects have the expected type.
5. **Model graph.** Every model-indexed revision has canonical `AOMD` records
   whose dependencies are non-zero, not self-references and point at existing
   entities, and a reverse-reference index that matches them exactly. Each
   distinct snapshot is checked once.

## What it does not do

* **No hydraulic validation.** It checks generic records, references, tries
  and history, not whether a `NetworkHydraulic` obeys the domain rules. The
  output says so.
* **No repair.** Branch heads, revisions and indexes are never changed. If a
  check fails, keep an untouched copy of the repository and investigate; do
  not reset heads or regenerate objects, which would change published
  identities.
* **Opening may tidy.** Opening a repository can rebuild the derived index,
  set aside a torn unpublished pack tail (printed as `Recovered: …`, copy kept
  in `recovered-pack-tails/`) and remove interrupted stage files. Published
  state is never changed. Run verification on a copy when you need the
  original untouched as evidence.
* **Unreferenced objects** are checked physically but cannot be interpreted;
  they are normal after interrupted writes.
* **Memory.** Dependency edges are sorted with a bounded buffer and file-backed
  runs, but revision, trie and snapshot tracking are in memory, so a very large
  history needs correspondingly more RAM and time.

Drafts, local Commit records and catalogue metadata are outside a repository;
`tools/offline_backup.py` validates those ([operations](operations.md)).

## Compaction

`aowis-dvmdb-compact PATH` (offline, exclusive) keeps everything reachable
from any branch and from **every bound revision**, even one no branch points
at, so a revision UUID that was ever published stays readable. It verifies the
bindings and branch hashes first and fails rather than discard anything it
cannot account for. Run a full verification afterwards.

## When to run it

After an unexpected shutdown, before and after a backup or restore, and when
investigating a replication problem. `tools/offline_backup.py` runs the full
check on every repository of the copy it is about to publish.

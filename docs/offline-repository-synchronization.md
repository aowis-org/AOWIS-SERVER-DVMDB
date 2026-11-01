# Synchronization with revision bundles

A **revision bundle** carries one branch's committed history between
independent repositories: by file with the `aowis-dvmdb-sync` CLI, or over
HTTP. Import verifies everything before writing and then only creates the
branch or fast-forwards it; it never merges or overwrites divergent work.
Branch and revision UUIDv7s, nullable HEADs and fork provenance arrive
unchanged. Drafts are never part of a bundle. Wire format:
[storage formats](storage-formats.md#revision-bundles-aowrev21aowrev24).

## What a bundle contains

* the project UUIDv7, the branch UUIDv7, its HEAD revision and record hash;
* for a **full** bundle, every object reachable from the HEAD; for an
  **incremental** bundle, the revisions not reachable from a recipient base
  revision and, per revision, only the trie objects that differ from a parent
  (the rest must already be in the recipient);
* the branch's origin branch and fork revision, if it has them
  (`AOWREV23`/`AOWREV24`; without provenance `AOWREV21`/`AOWREV22`).

An unborn branch has no revision and cannot be bundled; copy it with
`POST /projects/{id}/unborn-checkout` instead
([private repositories](private-working-repositories.md#checkout-of-an-unborn-branch)).

## Import rules

Before anything is written the importer checks:

* the bundle's project equals the expected project (the CLI argument or the
  URL's project);
* every object's SHA-256, every revision record, parent binding and the full
  ancestry down to revisions the recipient already trusts;
* trie structure and, for every model-indexed revision (including locally
  borrowed ancestors of a delta), the exact reverse-reference index;
* provenance: a fork revision needs an origin branch and must be an ancestor
  of the HEAD; importing into an existing branch must not change its recorded
  provenance;
* the configured byte, object and per-object limits.

Then it creates the local branch with the bundle's branch UUIDv7, fast-forwards
it, or reports that it is already up to date. A local HEAD that is not an
ancestor of the bundle's HEAD is a conflict, and so is a branch UUID that
already exists under another local name. Objects are fsynced and revision
bindings written before the branch record moves. Scratch tables live in the
repository's `tmp/`, not in memory.

## CLI

`REPOSITORY` is one project's repository directory
(`<root>/projects/<uuid>/repository`). Stop the server or other users of that
root first, or work on a copy.

```bash
project="<project-uuidv7>"
./build-linux/aowis-dvmdb-sync head      /src "$project" main        # HEAD revision UUIDv7
./build-linux/aowis-dvmdb-sync branch-id /src "$project" main        # branch UUIDv7

./build-linux/aowis-dvmdb-sync export /src "$project" main /tmp/main.aowbundle
./build-linux/aowis-dvmdb-sync import /dst "$project" /tmp/main.aowbundle main

base="$(./build-linux/aowis-dvmdb-sync head /dst "$project" main)"
./build-linux/aowis-dvmdb-sync export-incremental /src "$project" main "$base" /tmp/update.aowbundle
./build-linux/aowis-dvmdb-sync import /dst "$project" /tmp/update.aowbundle main
```

Exports never replace an existing file or follow a symlink; the bundle appears
under its name only when complete and fsynced. The import prints `created`,
`fast-forward` or `up-to-date`, the HEAD and the number of objects imported.

## Native API

```cpp
repository.exportRevisionBundle(project, "main", "/tmp/main.aowbundle");
repository.exportIncrementalRevisionBundle(project, "main", base, "/tmp/update.aowbundle");
const RevisionBundleImport result =
    repository.importRevisionBundle(project, fd, "main", limits);
```

Path forms publish atomically with no replace; descriptor forms write to an
empty caller-owned regular file and leave it empty on failure. `BundleLimits`
caps total bytes, objects and object size; hard ceilings are 64 GiB,
1,000,000 objects and 64 MiB per object.

## HTTP

* `GET /dvmdb/v1/projects/{id}/branches/{branch}/bundle` — full bundle;
  `?base={revision}` for an incremental one. Streamed from a scratch file.
* `POST` to the same path — import into `{branch}`; `201` when the branch was
  created, `200` for fast-forward or up to date, with `status`, `head` and
  `objectsImported`.

Every transfer reserves `max_transfer_mib` of the server-wide staging budget
for its duration (`503 staging_busy` when none is left); a bundle over the
limit is `413 transfer_too_large`. Use the CLI for larger transfers. Bundle
hashes prove integrity, not authorship or permission to change the project.

## Collaboration today

Bundles move history one way and only fast-forward. Because a branch keeps its
UUIDv7, the same branch cannot be imported a second time under another local
name. When two repositories have both committed to the same branch, the import
is refused, and reconciling them needs the Fetch/merge workflow below, which is
not built yet. Different branches (say a `feature` made in another repository)
import side by side and can then be merged with the hydraulic merge. Compaction keeps every
revision that was ever bound, so imported revision UUIDs stay readable even
when no branch points at them.

The personal/shared repository workflow (Fetch updates remote-tracking state,
local Commit, conditional Publish with an expected remote HEAD, provisioning a
new branch on the remote with its UUIDv7) is specified in
[private working repositories](private-working-repositories.md) and is not
built yet. Its rules already constrain bundles: a logical branch keeps its
UUIDv7 in every repository, while each repository moves its own HEAD; an absent
remote branch is not the same as an unborn one; nothing invents a fork point or
a synthetic first revision.

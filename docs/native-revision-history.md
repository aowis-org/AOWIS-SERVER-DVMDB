# Revision history and branches

Revisions, branches, HEADs and the history and graph reads, natively and over
HTTP. The rules are in the [version-control contract](version-control-semantics.md).

## Revisions and identity

A revision is a finalized, immutable whole-model snapshot. Each committed
revision has a UUIDv7, project UUIDv7, zero/one/two ordered parent revision
UUIDv7s bound to their record hashes, snapshot Merkle roots, its author
(UUIDv7 and display name), message and commit time, and a SHA-256 digest of
its canonical record ([layout](storage-formats.md#revision-record-t-3)). UUIDv7
is the object's identity; SHA-256 proves content integrity. There is no
mutable revision state.

Every write that creates a revision takes a `CommitMetadata` (author ID, author
name, message): `Repository::commit`, `commitModel`, `mergeBranches`,
`createRootBranch`, the hydraulic save, edit and merge, and the local Commit of
a draft. The repository adds the commit time when it publishes. Invalid
metadata (empty or overlong name or message, control characters, invalid UTF-8,
non-UUIDv7 author) is `InvalidInput` and writes nothing. History, revision
reads and the revision graph return author, message and commit time with
every revision.

Parent references form a DAG. Commits from multiple branches can share
ancestors without copying immutable revision objects. A two-parent merge
revision preserves both ancestry links; first-parent history is only a
convenient projection, never the entire project history.

## Branches and unborn HEADs

Each named branch has its own UUIDv7, a nullable HEAD revision and
immutable creation provenance (`originBranchId`, `forkRevisionId`). A project
is created with an unborn `main` branch and **zero committed revisions**.
Only the first successful commit supplies its first HEAD UUIDv7.

The branch HEAD is the latest **committed** revision, not the user's
working copy. Several branches can point at the same committed revision.
`head(branch)` is independent of other branch HEADs and of any GUI selection.
No repository-global checkout HEAD exists in the multi-user service.

## Personal repositories and private working copies

Local committed HEADs and shared published HEADs are separate refs in
separate DVMDB repositories. The working copy is durable uncommitted state
inside the editor's personal DVMDB, never an immutable history vertex.
The complete contract is in
[private working repositories](private-working-repositories.md).

Checkout creates an independent, editable working copy with UUIDv7, project
UUID, branch UUID, owner/session, exact committed base UUID (or null), and
private model changes. Multiple people may check out the same branch at the
same base. The committed revision remains immutable. Opening a historical
revision for inspection does not make it writable or move any branch HEAD.

## Commit and conflict behavior

A local Commit pins its local branch and exact expected local HEAD/null,
validates the complete proposed model, durably creates a new immutable
revision, and atomically advances only that local branch ref. Publish/Push
then transfers verified history and conditionally advances the separate
shared branch from an exact expected remote HEAD/null.
Stale HEADs fail without publishing changes. A stale HEAD requires an explicit
reconcile/rebase/merge workflow; a stale base is not automatically a content
conflict. Successful writes remain discoverable after ambiguous timeouts
through idempotency and authoritative reads.

## Graph and read APIs

Native history, immutable revision lookup, diffs and full revision graph
queries must return the same UUIDv7 identities and SHA-256 bindings as REST.
Paginated full-graph queries capture a consistent set of all branch HEADs,
include all reachable committed revisions and both merge parents, and
validate ancestry closure. Unborn branches appear in the branch list but
contribute no revision vertex.

Read-only operations must never change HEADs, working-copy bases or owner
session state. See [merge semantics](native-branch-merging.md) and
[GUI integration](gui-integration-requirements.md).

## API

| Operation | `Repository` | HTTP (`/dvmdb/v1/projects/{id}`) |
|---|---|---|
| Create project with initial unborn `main` | `ProjectService::createUnbornProject` | `POST /projects` with `unborn: true` |
| Create another independent unborn branch (same project, no origin revision) | `createUnbornBranch(project, name, optional UUIDv7)` | `POST …/branches` with `name`, `unborn: true`, optional `id` |
| Create the first branch with an empty first revision | `createRootBranch(…, metadata)` | `POST /projects` without `unborn` (needs `author`; roadmap VC-01) |
| Create a branch from a branch UUID, at HEAD or a reachable revision | `createBranchFromId` (retry-safe with a caller-chosen UUID) | `POST …/branches` (`fromBranchId`, optional `revision`, optional `id`) |
| Rename (keeps UUID, HEAD, provenance) | `renameBranch(…, expectedHead)` | `PATCH …/branches/{branch}` |
| Branch by UUID | `branchById` | `GET …/branch-ids/{branchId}` |
| HEAD (null when unborn) | `optionalHead` | `GET …/branches/{branch}/head` |
| First-parent history, newest first | `history(…, maxCount, after)` | `GET …/branches/{branch}/history?limit&after` |
| One revision record | `readRevision` | `GET …/revisions/{revision}` |
| All branches' DAG, pinned and paged | `revisionGraph(…, maxCount, cursor)` | `GET …/revision-graph?limit&cursor` |
| Ancestry test | `revisionReachableFrom(tip, candidate)` | — |
| Diff of two revisions (≤ 1000 keys over HTTP) | `diffRevisions` | `GET …/diff?from&to` |

Revision-graph cursors pin the branch HEADs of the first page; they are signed
with a per-process key, so a cursor is invalid after a server restart or for
another project. A page holds at most 1,000 revisions and 1,024 branch HEADs.
`revisionReachableFrom` walks the DAG once and answers `false` for an unknown
revision without walking.

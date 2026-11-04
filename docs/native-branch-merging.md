# Branch merging and reconciliation

Merge behavior follows [version-control semantics](version-control-semantics.md).
It operates on immutable **committed** revision snapshots, not on the
uncommitted changes of arbitrary editor sessions.

## Required inputs

A merge specifies project UUIDv7, target branch UUIDv7, source branch UUIDv7,
**expected target HEAD** and **expected source HEAD**, both potentially null
only where the particular operation explicitly permits an unborn branch.
The merge engine validates revision ownership and complete two-parent ancestry.

## Outcomes

| Outcome | Semantics |
|---|---|
| Up to date | Source already incorporated; do not publish or move a ref |
| Fast-forward | Target can advance to an existing committed descendant after pinned-head and repository-integrity checks; current hydraulic-specific implementation also imposes stricter domain validation |
| Merged | Divergent compatible histories yield **one** new UUIDv7 immutable revision with ordered `(target, source)` parents |
| Stale | One or both expected HEADs changed; reject without modifying either ref |
| Conflict | Unresolved record/field edit collision or invalid repository structure; preserve both branches and all private working copies; hydraulic diagnostics alone are not future merge conflicts |

A successful merge updates only the **target** branch's committed HEAD;
the source remains unchanged. The merge base comes from actual revision
ancestry, not branch creation provenance or UUIDv7 time order. Recursive
virtual merge bases may be computed without creating published revisions.

## Hydraulic-domain safety and planned semantics

The current hydraulic-specific merge validator rejects some whole-network
domain failures, and the generic merge engine does not. The agreed target is
to preserve **hydraulically unsolvable but referentially consistent networks**
through merge, while enforcing object kind/identity, physical membership,
record integrity, revision ancestry and pinned-HEAD CAS. Missing supply,
disconnected topology and solver problems are diagnostics, not merge vetoes.
Dangling or wrong-type hydraulic model references remain subject to the
current strict rules pending a separate policy decision. Overlapping *edit
collisions* require explicit user choices; no last-writer-wins fallback or
silent force-overwrite is permissible. Non-blocking hydraulic feasibility
in the merge path remains part of release qualification.

An editor with a private working copy may reconcile it against the newly
committed target HEAD; this does not by itself publish a new revision. The
user must explicitly commit the reconciled model under a fresh expected-HEAD
check. A timeout or ambiguous response requires authoritative verification
before retrying.

Both native and REST frontends must implement equivalent integrity checks,
optional hydraulic diagnostics, concurrency checks, immutable revision
records and durable publication.

## API

`Repository::mergeBranches(project, target, source, metadata, expectedTarget,
expectedSource, options)` is the generic record-level three-way merge: domain
callbacks in `MergeOptions` can choose conflict sides, resolve records and
validate the candidate; both pins are checked under the publication lock, and
a conflicting merge leaves both heads and the pack unchanged. It does **no**
hydraulic validation of its own; the current stricter hydraulic-specific API
is `hydraulic::mergeHydraulicBranches` ([hydraulic merging](native-hydraulic-merging.md)).
Neither path should be described as satisfying the future lossless merge
contract until the remaining merge qualification cases in the
[roadmap](roadmap.md) pass.

HTTP: `POST /dvmdb/v1/projects/{id}/merges` (generic) and
`POST /dvmdb/v1/projects/{id}/hydraulic-merges`, both with `target`, `source`,
`expectedTargetHead` and `expectedSourceHead`. The answer has `status`
(`up_to_date`, `fast_forward`, `merged` or `conflicts`), `base`,
`targetBefore`, `sourceHead`, `head` and `conflicts` (entity UUID, base,
target and source record hashes, reason); `conflicts` is returned with HTTP
409. The merge base is the unique lowest common ancestor. A criss-cross
history with several independent lowest common ancestors is refused with a
conflict ("multiple independent merge bases require recursive merging");
recursive virtual merge bases are not implemented.

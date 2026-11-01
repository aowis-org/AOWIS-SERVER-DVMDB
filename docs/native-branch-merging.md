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
| Fast-forward | Target can advance to an existing committed descendant after pinned-head and domain validation |
| Merged | Divergent compatible histories yield **one** new UUIDv7 immutable revision with ordered `(target, source)` parents |
| Stale | One or both expected HEADs changed; reject without modifying either ref |
| Conflict | Unresolvable record or hydraulic-domain conflict; preserve both branches and all private working copies |

A successful merge updates only the **target** branch's committed HEAD;
the source remains unchanged. The merge base comes from actual revision
ancestry, not branch creation provenance or UUIDv7 time order. Recursive
virtual merge bases may be computed without creating published revisions.

## Hydraulic-domain safety

Distinct record-UUID edits are not automatically a safe hydraulic merge.
Validate entity kinds, reference closure, membership order, connectivity and
all application model invariants on the **whole proposed merged snapshot**.
Overlapping changes require explicit conflict resolution. No last-writer-wins
fallback or silent force-overwrite is permissible.

An editor with a private working copy may reconcile it against the newly
committed target HEAD; this does not by itself publish a new revision. The
user must explicitly commit the reconciled model under a fresh expected-HEAD
check. A timeout or ambiguous response requires authoritative verification
before retrying.

Both native and REST frontends must implement equivalent validation,
concurrency checks, immutable revision records and durable publication.

## API

`Repository::mergeBranches(project, target, source, metadata, expectedTarget,
expectedSource, options)` is the generic record-level three-way merge: domain
callbacks in `MergeOptions` can choose conflict sides, resolve records and
validate the candidate; both pins are checked under the publication lock, and
a conflicting merge leaves both heads and the pack unchanged. It does **no**
hydraulic validation of its own; branches holding hydraulic networks must use
`hydraulic::mergeHydraulicBranches` ([hydraulic merging](native-hydraulic-merging.md)).

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

# Hydraulic network persistence

`aowis::dvmdb::hydraulic` (`include/aowis/dvmdb/hydraulic_network_persistence.h`,
CMake target `AOWIS::DvmdbHydraulic`) stores canonical AOWIS
`NetworkHydraulic` models in the native store, reads them back at an immutable
revision, edits them with indexed updates and merges them with hydraulic
validation. It needs Qt Core and the MODEL submodule. Merging is described in
[hydraulic merging](native-hydraulic-merging.md); drafts in
[working copies](working-copies.md).

## How a network is stored

* One **network root** record: network metadata and the ordered list of
  membership-chunk UUIDs per collection.
* **Membership chunks** of at most 256 ordered member UUIDs.
* One independently addressed **entity record** per top-level entity.
* The reverse-reference index answers "who refers to this entity" for
  ownership and deletion checks.

The 18 top-level collections, in their fixed order (the `type` /
`collectionIndex` 0–17 used by the API): pipe materials; time patterns; tank
volume, pump head, pump efficiency, valve headloss, valve characteristic and
general curves; reservoirs, junctions, tanks; demand points; pipes, pumps,
valves; simple controls, rule controls and map labels. Nested MSX settings and
map backdrop data live inside the appropriate records.

Encoding validates UUID kind and uniqueness, finite values, reference closure,
membership order and the MODEL value rules. An entity UUID never silently
changes its kind.

## API

| Operation | C++ | HTTP (`/dvmdb/v1/projects/{id}/branches/{branch}`) |
|---|---|---|
| Save a whole network | `saveHydraulicNetwork(repository, project, branch, network, expectedHead, metadata)` | `POST …/networks` |
| Edit up to 128 entities | `editHydraulicNetwork(repository, project, branch, networkId, changes, expectedHead, metadata)` | `POST …/networks/{networkId}/edits` |
| Load a network | `loadHydraulicNetworkAtRevision(repository, project, revision, networkId)` | `GET …/networks/{networkId}?revision=` |
| List one collection | `listHydraulicEntityIdsAtRevisionLimited(…, type, limit)` | `GET …/networks/{networkId}/entities?type=&revision=` (≤ 10,000) |
| Load one entity | `loadHydraulicEntityAtRevision(…, networkId, entityId)` | `GET …/networks/{networkId}/entities/{entityId}?revision=` |
| Merge branches | `mergeHydraulicBranches(repository, project, target, source, selections, metadata)` | `POST /dvmdb/v1/projects/{id}/hydraulic-merges` |

Payloads are canonical MODEL bytes (`ModelCodec<T>`), base64 in JSON. Load
functions return `std::nullopt` only for "not found"; every other failure is a
`StorageError`.

## Reads

Every read names one immutable revision UUIDv7. Over HTTP an omitted
`?revision=` means "the branch HEAD, captured once"; a given revision must be
the HEAD or one of its ancestors. A client that reads a network in several
calls passes the same revision to each. Entity reads go through the index and
check that the entity belongs to the network.

## Committed writes

Saves and edits take an exact expected HEAD and publish one new revision with
compare-and-swap; a stale HEAD is a conflict and nothing is written. An
identical edit batch creates no revision and returns the current one.

`editHydraulicNetwork` uses bounded indexed paths for scalar, physical and
operating values, catalogue records, eligible pipe endpoint rewiring,
dependent-reference changes and membership insertions and removals. Each path
pins one revision and checks ownership, MODEL value rules and references.
Mixed or ineligible batches fall back to rebuilding and validating the complete
network. Structural changes rewrite only the affected chunks, and the network
root only when the chunk list changes.

These routes need a committed HEAD (`expectedHead` is a revision UUID), so they
do not create the first revision of an unborn branch. Editors work in a draft
instead and commit it locally; see [working copies](working-copies.md).

## Drafts

For private drafts the service adds three hydraulic operations:

* `POST /working-copies/{id}/initialize-network` writes an empty network root
  into an untouched unborn draft.
* `POST /working-copies/{id}/save-events` with a `membership` array: DVMDB
  checks the member payloads against the MODEL event images and rewrites the
  affected chunks and root in the same Save. The network must already exist in
  the base or the candidate. One logical MODEL transaction can also change
  persistent root metadata **and** typed members atomically; the server
  derives membership/chunks first and then applies root metadata to the
  resulting manifest.
* `GET /working-copies/{id}/networks/{networkId}` rebuilds and validates one
  network from the base plus the candidate.

## Validation boundary

The native record/index layer requires canonical MODEL data, valid values,
existing correct-type UUID references and consistent membership. Whole
network saves and hydraulic-specific merges can run additional domain
checks. These are distinct from the **hydraulic solver feasibility** of the
engineering design: a disconnected or unsupplied but referentially
consistent network is a legitimate snapshot and need not pass an EPANET
simulation before local Commit.

Permitting deliberately dangling model UUID references has **not** been
agreed and is not part of normal Save/Commit work. The current reference
checks remain in force. Remaining HTTP/Standalone verification and the
future separation of optional hydraulic-merge diagnostics are in the
[roadmap](roadmap.md).

## History reconstruction

The hydraulic adapter verifies immutable MODEL action archives against
committed native revision deltas. It can replay full, ordered actions (including
root metadata, insertions, deletions and multi-network transactions), check
canonical equality with immutable parent/child states, and construct a
complete candidate overlay relative to the current committed HEAD. With a
live private draft lease it can atomically select a verified archived action
boundary and persist a V6 cursor. An **active draft MODEL journal is rejected**;
this is not continuous Undo/Redo or an HTTP execution endpoint. See
[History](commit-action-archive.md).

## Tests

`aowis-dvmdb-hydraulic-network-tests` covers all 18 collections, the 256-member
chunk boundaries, indexed and fallback edits, stale pins, physical and
reference validation, merge choices and ancestry, fast-forward validation,
concurrent immutable snapshots, compaction, reopening and archived-action
reconstruction/navigation. `aowis-dvmdb-model-value-rules-tests` covers value
rules; the HTTP suite covers routes, draft hydration and membership
materialization. **The current Qt-enabled History navigation regression still
needs a confirmed passing rerun** following the forward-overlay fix.


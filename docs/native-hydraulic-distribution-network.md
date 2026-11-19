# Hydraulic distribution network persistence

`aowis::dvmdb::hydraulic` (`include/aowis/dvmdb/hydraulic_distribution_network_persistence.h`,
CMake target `AOWIS::DvmdbHydraulic`) stores canonical AOWIS
`NetworkHydraulicDistribution` models in the native store, reads them back at an immutable
revision, edits them with indexed updates and merges them with hydraulic
validation. It needs Qt Core and the MODEL submodule. Merging is described in
[hydraulic merging](native-hydraulic-merging.md); drafts in
[working copies](working-copies.md).

## MODEL dependency and existing data

DVMDB requires a MODEL submodule that defines `NetworkHydraulicDistribution`,
`NetworkHydraulicDistributionRoot`, and the corresponding distribution-root
edit helpers. Update the pinned submodule in this repository to the committed
MODEL rename; old submodule checkouts fail configuration.

The native distribution-root record kind is **100**, the membership chunk
kind is **101**, and the 17 member kinds occupy the reserved **201–217** range.
Each distribution network also requires a separate annotation-layer root (kind
110), deterministic from the network UUID, even when its annotation list is
empty. Old pre-release record-kind mappings are not supported.
Archived MODEL root-edit events
using the old `NetworkHydraulicRoot` entity type are not transparently
converted to `NetworkHydraulicDistributionRoot`. Pre-release repositories
containing these events must be recreated or handled by an explicit conversion
before relying on history replay; no migration is bundled with this patch.

`NetworkHydraulicDrainage` is a name reserved for a future independent model;
this adapter implements distribution only and does not introduce a shared
hydraulic network supertype.

## How a network is stored

* One **network root** record: network metadata (including the optional
  project units, `presentation_units`, a display-only unit set) and the
  ordered list of membership-chunk UUIDs per collection.
* **Membership chunks** of at most 256 ordered member UUIDs.
* One independently addressed **entity record** per top-level entity.
* The reverse-reference index answers "who refers to this entity" for
  ownership and deletion checks.

The 17 top-level collections, in their fixed order (the `type` /
`collectionIndex` 0–16 used by the API): pipe materials; time patterns; tank
volume, pump head, pump efficiency, valve headloss, valve characteristic and
general curves; reservoirs, junctions, tanks; demand points; pipes, pumps,
valves; simple controls and rule controls. Map annotations are stored in
independent revisioned layer/member records, not in the hydraulic manifest.
Nested MSX settings and map backdrop data live inside their respective records.

Encoding validates UUID kind and uniqueness, finite values, reference closure,
membership order and the MODEL value rules. An entity UUID never silently
changes its kind.

## API

| Operation | C++ | HTTP (`/dvmdb/v1/projects/{id}/branches/{branch}`) |
|---|---|---|
| Save a whole network (native snapshot helper) | `saveHydraulicDistributionNetwork(repository, project, branch, network, expectedHead, metadata)` | Legacy `POST …/networks` retired (HTTP 410); use working-copy import or initialize |
| Edit up to 128 entities (native snapshot helper) | `editHydraulicDistributionNetwork(repository, project, branch, networkId, changes, expectedHead, metadata)` | Legacy `POST …/networks/{networkId}/edits` retired (HTTP 410); use `save-events` |
| Load a network | `loadHydraulicDistributionNetworkAtRevision(repository, project, revision, networkId)` | `GET …/networks/{networkId}?revision=` |
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

Native convenience saves and edits take an exact expected HEAD and publish
one new revision with compare-and-swap; a stale HEAD is a conflict and nothing
is written. An identical edit batch creates no revision and returns the current
one. When the first distribution root is published, its empty annotation layer
is published in **the same revision**. Every native engineering Commit and
merge receives the candidate-wide annotation/hydraulic ownership validator;
missing annotation layers, orphan members and dangling anchors fail closed.

These C++ convenience functions are snapshot-oriented native helpers (useful
for imports, qualification and benchmarks). They **do not** create the
reversible MODEL action archive. Interactive editor changes must continue to
use fenced working-copy `save-events` and local Commit. The plain generic HTTP
model writer is not an alternative editing entry point. Candidate-wide
validation currently traverses the complete engineering snapshot even for
indexed member updates; any fast-path performance claims must be remeasured.

`editHydraulicDistributionNetwork` uses bounded indexed paths for scalar, physical and
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
boundary and persist a V8 cursor. A **verified active journal at position zero**
can enter an archived Commit without losing its Redo frames, then return to
the draft. This is exposed through the fenced HTTP History navigation endpoint;
GUI activation and unborn-base navigation remain outstanding. See
[History](commit-action-archive.md).

## Tests

`aowis-dvmdb-hydraulic-distribution-network-tests` covers all 17 collections, the 256-member
chunk boundaries, indexed and fallback edits, stale pins, physical and
reference validation, merge choices and ancestry, fast-forward validation,
concurrent immutable snapshots, compaction, reopening and archived-action
reconstruction/navigation. `aowis-dvmdb-model-value-rules-tests` covers value
rules; the HTTP suite covers routes, draft hydration and membership
materialization. **The current Qt-enabled History navigation regression still
needs a confirmed passing rerun** following the forward-overlay fix.


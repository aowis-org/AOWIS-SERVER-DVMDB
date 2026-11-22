# Versioned map annotation layers — DVMDB storage design (A1)

**Status:** Native annotation kinds 110–112, whole-candidate ownership
validation, fenced draft/event publishing, immutable History and branch merge
integration are implemented. The B5 hardening closes generic HTTP publication
bypasses, reserves hydraulic member kinds 201–217 (old 1–17 retired), and
requires explicit semantic validation for engineering native Commit and merge.
Qt-backed full integration and physical power-cut qualification must be run
on the supported development host. This document describes the native
contract; the HTTP API is documented in `dvmdb-http-api.yaml`.

Not implemented yet: layer metadata edits (`MapAnnotationLayerRoot` edits in
the MODEL contract). A Distribution layer is created only implicitly when its
network is initialized or imported; no route creates the pre-network Drainage
layer, changes a layer's metadata or owner, or deletes a network together
with its layer. Clients cannot write native layer records, so such edits are
refused. The validator already enforces the layer rules for layers that
exist.

## Storage boundary

An annotation is project data with an engineering-domain scope, **not a member
of the hydraulic network's 17-collection manifest**. A project repository's
immutable revision already holds a UUID-keyed model trie and reverse-reference
trie. Add a separate `MapAnnotationLayer` root and independently addressed
annotation members to that *same* revision snapshot. No separate database,
revision graph, layer Commit, or user-side storage files.

Current distribution kind 100 (network root) and kind 101 (membership chunk)
remain assigned to the existing adapter. Old hydraulic `MapLabel` member kind
18 and collection index 17 are **removed** during the clean break; no dummy
collection, hidden compatibility type, or dual writer remains. The old
18-collection manifest version is rejected after the format/schema bump.

## Native record graph

Native kinds **110** (`annotation layer root`), **111** (`annotation
membership chunk`) and **112** (`MapAnnotation` member) are now used by the B1
record adapter. Their payload formats carry explicit magic/version markers and
reject wrong-kind, malformed, noncanonical and mismatched-reference records.
The generic store reserves engineering record kinds and requires a semantic
validator at publication; the complete candidate-wide graph validator proves
layer ownership and the exact kind and reference closure.
These IDs are native storage kinds, distinct from MODEL entity type names.

```text
Project repository / immutable revision R
  model trie
    Distribution network root N              kind 100
      -> ordinary hydraulic chunks           kind 101
      -> hydraulic entity records            kinds 201..217 (reserved engineering kinds)
    Annotation layer L                        kind 110
      metadata: uuid L, domain, owner_network_uuid N
      refs: [N, annotation chunk C1, ...]
        Annotation chunk C1                   kind 111
          metadata: layer L, ordered UUIDs [A1, A2, ...] (max 256)
          refs: [A1, A2, ...]
            Annotation A1                     kind 112
              MODEL canonical MapAnnotation payload
              refs: [] or [typed anchor target T]
    Drainage annotation layer D               kind 110
      domain Drainage; owner_network_uuid null (before drainage network)
      refs: [drainage annotation chunks, ...]
```

References in the actual `ModelRecord` envelope are canonical sorted unique
UUID sets, **not** the textual diagram's ordering. Ordered member UUIDs live
inside chunk payloads. The full root and chunk payloads are server-owned
manifests, not bytes accepted directly from the GUI. A root's owner UUID is an
outgoing reference only when present; reverse references thus provide quick
lookup of network-owned layers by network UUID. For the pre-network Drainage
project-domain layer, discovery uses a revision-pinned scan restricted to kind
110, never a fabricated network UUID. Empty layers remain proper records.

### Chunk layout (root version 2)

Member order is the concatenation of the layer's chunks, in the order the root
lists them. Chunks are edited in place, so a user edit writes a bounded number
of records whatever the layer size:

- a text, position, style or anchor edit writes the member record only;
- an insertion appends to the last chunk, or, when that chunk holds 256
  members, starts a new chunk (member, chunk and root);
- a deletion removes the member from its own chunk (member and chunk). A chunk
  that becomes empty is dropped from the root. A chunk with fewer than 64
  members is merged into its previous neighbour, or takes in its next one,
  when the two fit into 256; otherwise it stays small. A deletion therefore
  writes at most four records (member, two chunks, root). Exception: removing
  the last member of the last chunk never merges chunks (it only drops the
  chunk when it empties).

A chunk's UUID is derived from the layer UUID and a chunk serial
(`chunkUuid(layer, serial)`, UUID version 8); the root lists the chunk UUIDs
in order and never lists one twice. A new chunk takes the serial after the
highest one still listed (0 for an empty layer).

Both rules make an insertion exactly reversible: deleting the member that was
just appended restores the previous member, chunk and root records byte for
byte. Unborn-draft History depends on this, because it reconstructs the
draft's starting state by undoing the journal and then proves the saved draft
by replaying it forward. A full rebuild (import, initial draft, membership
reconciliation at merge) writes the canonical layout: chunks of 256 with
serials 0, 1, 2, ...; a merge deletes the tips' actual old chunk records that
the rebuild does not reuse.

Root version 1 (chunk serial = chunk position, every deletion rewrote all
later chunks) is no longer read. Local test repositories that contain
annotation layers must be recreated (also those written by the first chunk-local
build of 2026-11-21, whose root carried a serial counter); there is no deployed
data in either format.

## Invariants and discovery

1. **Every** distribution root in a new-format project snapshot has exactly
   one `Distribution` annotation layer. It is created in the same atomic draft
   initialization/import as the network. Multiple distribution networks have
   different layer UUIDs. A duplicate or missing layer is invalid.
2. Before a Drainage network exists, a project snapshot may have zero or one
   `Drainage` layer with no owning network (created on first annotation use).
   Never treat a null owner as a Distribution layer or as ownership of *all*
   projects. A future drainage network-specific owner replaces the temporary
   domain-wide scope via explicit versioned edits.
3. All annotation members are referenced by exactly one layer chunk; each
   chunk belongs to one layer, and each layer has a unique UUID in the project.
   Reject unowned, duplicate, cross-layer and wrong-kind members.
4. The owner must be a **same-project, same-revision network root** of the
   declared domain. Anchors require correct typed target records that belong
   to the owning network and are supported point-position entity types.
   Drainage with null network owner cannot contain attached annotations.
5. Recompute and verify the exact reverse-reference index, member counts,
   ordered membership, UUID identity/kind, payload digest, chunk ownership,
   range/value validity and canonical decode on every relevant loading,
   mutation, import, merge and repository verification boundary.
6. Network removal includes its annotation layer, chunks and members
   atomically. Anchor target removal requires detachment of dependent
   annotations first **in the same operation**; the index must refuse
   deletion with any remaining dependent records. No dangling references.
7. Root ownership, layer identity and domain are immutable except through
   an explicit validated re-parent operation. An ordinary member update may
   not assign the same UUID a different record kind.

A DVMDB project UUID is the outer repository scope. Neither branch nor
revision UUID is embedded in a `MapAnnotation` record. Snapshot reachability,
Merkle sharing and normal branch ancestry provide version identity.

## Draft transaction and revision rules

- The GUI sends canonical MODEL before/after images and **logical** membership
  intents, never native manifest or membership-chunk bytes. The Save
  validates the resulting candidate (incrementally, see "Validation cost")
  and `ProjectService` publishes it under the same writer lease, base HEAD,
  generation and idempotency fence already used for hydraulic MODEL edits.
- A transaction can combine `MapAnnotation`, a layer-root edit, hydraulic
  entity changes and annotation membership changes. Either the whole event,
  its canonical journal entry and the resulting candidate record overlay
  persist together or none do. Do not sequence dependent calls that can
  partially publish (e.g. detach first, then delete target separately).
- Saves change a *private working copy* only; a local Commit publishes the
  entire model trie, including hydraulic and annotation records, under the
  existing CAS/revision UUIDv7 semantics. Reading a revision or branch HEAD
  pins one snapshot for network **and** annotation layer reads.
- Annotations must be included in accepted archive coverage, inverse/redo,
  sparse delta reconciliation, verified historical boundary navigation,
  merge conflict detection, rebase and bundle verification. A history
  verifier that only reconstructs hydraulic networks must reject, not
  silently skip, annotation edits until generalized.
- For merge: independent member additions may combine; same-field conflicts
  use canonical MODEL field merge and caller choices; deletion versus edit or
  conflicting layer re-parenting fails pending explicit resolution. All
  merged candidates rerun the full ownership/anchor closure rules.
- If a target is moved, the attached annotation's world position is resolved
  against that revision's target geometry. Ordinary moves do not rewrite
  annotation bytes. On target deletion the same transaction updates the
  annotation to an unanchored last resolved WGS84 position, then removes the
  target, with no inconsistent intermediate persisted state.

## API surface

Every read names a project and a pinned revision or a working copy; nothing
mixes in an implicit latest HEAD.

- Library: `loadAnnotationLayer` (any candidate reader),
  `loadAnnotationLayerAtRevision`, `loadAnnotationLayerSummary` (root and
  chunks only), `loadAnnotationMember`, `loadAnnotationLayerMetadata`;
  writes go through `materializeDraftAnnotationEdits` inside the
  `save-events` route and `saveWorkingCopyTransactions`.
- HTTP (see `dvmdb-http-api.yaml`):
  - `GET /projects/{projectId}/branches/{branch}/annotation-layers`: layer
    descriptors of a pinned revision;
  - `GET /projects/{projectId}/branches/{branch}/annotation-layers/{layer}`:
    one layer with members;
  - `GET /working-copies/{draftId}/annotation-layers/{layer}`: one layer of a
    draft;
  - edits: `map.annotation` events in
    `POST /working-copies/{draftId}/save-events`, with the existing lease,
    generation and retry semantics.

The GUI must not inspect DVMDB's private on-disk structures or create native
kind-110/111 records. Standard REST/library reads are authoritative.

## Format break and B-phase acceptance

The implementation reduces the hydraulic membership count 18 -> 17,
removes kind 18, and places the remaining 17 member kinds in 201..217. The
native manifest/schema markers change with this pre-release format break.
Earlier incompatible development repositories must be recreated; they are
not migrated, and their payload kinds must not be reinterpreted. No legacy decoder or implicit
migration is needed; **never** convert unknown bytes to empty data.

B-phase tests must cover: multiple distribution networks and distinct layers;
Drainage's project-domain scope; empty layers; exact snapshot reload;
chunk boundaries 255/256/257; edit/undo/redo/restart; mixed atomic target
move/delete+annotation detach; invalid cross-network anchor; duplicate layer;
unowned chunk/member; wrong record kind; field-merge conflict; independent
branch additions; archive/revision coverage; replication and crash recovery.

## Implemented native boundaries and remaining qualification

The standalone native annotation builder and loader are not write APIs. The
HTTP `save-events` route materializes and validates the annotation and
hydraulic record delta, then persists it atomically with its MODEL journal
inside a fenced private draft. `local-commits` publishes the draft to an
immutable revision. Revision-pinned reads, branch merges, immutable history,
and verification operate over this combined engineering candidate. Legacy
raw direct-to-branch hydraulic POST routes remain HTTP 410.

Generic raw Save and direct MODEL Commit are for non-engineering projects only;
engineering records already present in the pinned candidate or proposed by a
request are rejected before publication. Native Commit and merge require an
explicit full candidate validator for engineering kinds, including orphan
members and fast-forward merges. Generic records in the old 1–17 space remain
ordinary opaque records, never inferred to be hydraulic.

## Reserved UUID version

Network membership chunks, Distribution layer roots and annotation membership
chunks have SHA-256-derived UUIDs with version 8. Version 8 is therefore
reserved for server-derived records: a network root, hydraulic member or
annotation with a version-8 UUID is rejected by the Save delta check and by
the complete validator (Commit, merge, bundle import, verify). Otherwise a
client could create an entity with the UUID of a future chunk and stop that
network or layer from ever growing.

## Validation cost

The complete validator (`validateAnnotationSnapshot`) decodes every network
and every layer of a project. It runs at the trust boundaries: local Commit,
merge (including fast-forward), bundle import and repository verification.

An event Save uses `validateAnnotationSnapshotDelta` instead. The saved draft
candidate already satisfied the contract, so only what the delta can break is
checked:

- graph invariants of the changed records (`engineering_delta_scope.h`,
  Qt-free): record kinds and in-place kind changes, references that must
  resolve, allowed child kinds of containers, exactly one owner for every
  added, moved or changed owned record, no surviving reference to a deleted
  record (chunk entries, pipe end nodes, annotation anchors), and one layer
  per network for every network whose ownership is touched;
- semantics of the touched scope only: for each network whose root, chunk or
  member changed, `checkDraftHydraulicNetworkDelta` checks the root manifest,
  every new or rewritten membership chunk, and every changed or newly entered
  member (canonical payload, value rules, every reference resolved to a
  member of the right type in the same network, stored reference index equal
  to the canonical dependencies, using the canonical network validator on the
  member and its reference targets); demand points attached to a changed pipe
  are re-checked, and members that left the network must not be used by the
  network settings. The complete network loader runs only when the network
  settings change or a member moves to another network. For each layer whose
  root or chunk changed, the root is decoded, the changed and newly listed
  chunks are decoded and checked, and only
  members that entered the layer through them have their anchors resolved
  (unchanged chunks and their members are not read); every other changed annotation, and
  every annotation anchored to a changed or moved hydraulic member, is
  decoded on its own and its anchor is resolved through the reverse-reference
  index (target kind, owning network);
- creating or deleting a network or layer root, a layer ownership or domain
  change, and generic (non-engineering) records fall back to the complete
  validator.

Reads go through a pinned base revision (`ProjectService::pinModelRevision`:
resolved once, no project operation lock). Saves read only the records they
need; a delta that changes network settings loads the base once in bulk,
because that network is then decoded completely.
A candidate that was already invalid before the Save (for example written by
a raw native caller) is still rejected at local Commit.

Archived and live History replay works on changes, not on copies of
revisions:

- The replayed state is a native overlay on the pinned starting revision
  (point reads, reverse-reference queries). Actions are applied with the Save
  materializers.
- The proven position (archive tip, or an active draft's saved cursor) must
  reproduce the exact native difference: the local Commit witness (the
  store's revision difference) or the saved draft overlay.
- Stored revisions and saved drafts already passed the validators when they
  were written and are not re-checked. The selected boundary, when it is
  neither position 0 nor the proven position, is checked with the Save delta
  validator against the starting revision. An archive whose intermediate
  state is invalid can therefore be navigated to its valid boundaries, never
  to the invalid one.
- Annotation and hydraulic ownership of edited entities is looked up in the
  two endpoints through the reverse index (member, chunk, layer or network).
- Network roots of a revision come from `hydraulicDistributionRootKeysAtRevision`:
  the nearest cached ancestor plus the store's revision differences, cached
  per immutable revision. A revision without a usable ancestor is streamed
  once.
- Unborn drafts have no stored revision; their whole model is the draft
  overlay (bounded by the draft change limit), and the reconstructed seed and
  the saved draft are validated completely.

There is no project-size limit. Remaining bounds: 4096 actions and 8 MiB per
archive, 20,000 native changes per draft and per History overlay (also for
the revision difference between an archived fork's parent and HEAD).

## Failure reporting and reads

Validators and History adapters return "invalid" or "not navigable" only for
verification failures: conflicts, invalid input, corrupt or noncanonical
data, limits. Failures of the environment or of the request target are
rethrown (`rethrowIfNotVerificationFailure`) and keep their HTTP status:
missing draft or revision 404, busy project 503 with `Retry-After`, full disk
507, I/O and internal errors 500. A client therefore retries a 503/500 History
request with the same operation ID instead of treating it as stale.

Annotation read routes check the 10,000-member limit from the root and the
membership chunks (`loadAnnotationLayerSummary`) before any member record is
read. The layer listing decodes roots and chunks only. Single-layer reads go
through a pinned revision and hold the project operation lock only while the
branch and revision are resolved; the listing still needs one scan under that
lock, because layer roots have no kind index.

The older native hydraulic convenience writers are not the canonical editing
entry point and require migration to the event-backed path; use the working
copy MODEL journal for normal editing. Before treating Phase B as fully
qualified, execute the Qt-backed integration and existing hydraulic workload
suite, plus a power-cut/restart annotation edit test on a supported host.

# Versioned map annotation layers — DVMDB storage design (A1)

**Status:** B1, increment 2/2 has completed the native annotation
record adapter (`map_annotation_persistence.h/.cpp`) with kinds 110/111/112,
bounded 256-member chunks, canonical MODEL payloads, deterministic chunk IDs,
exact reference-index checks, pinned-revision reads and native-record tests.
The 18-to-17 hydraulic collection break is complete in B1.2. Project-wide
format/owner closure, layer discovery, logical membership edits, HTTP/draft
publication and history integration remain **B2–B4**. These standalone format builders
are not HTTP/GUI write APIs. The canonical MODEL contract is in the MODEL
repository at `docs/map-annotations-contract.md`.

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

## Proposed native record graph

Native kinds **110** (`annotation layer root`), **111** (`annotation
membership chunk`) and **112** (`MapAnnotation` member) are now used by the B1
record adapter. Their payload formats carry explicit magic/version markers and
reject wrong-kind, malformed, noncanonical and mismatched-reference records.
The generic native store is still model-agnostic; candidate-wide kind/owner
closure is the next gate, not a guarantee of this isolated record codec.
These IDs are native storage kinds, distinct from MODEL entity type names.

```text
Project repository / immutable revision R
  model trie
    Distribution network root N              kind 100
      -> ordinary hydraulic chunks           kind 101
      -> hydraulic entity records            kinds 1..17 (after label removal)
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
  intents, never native manifest or membership-chunk bytes. `ProjectService`
  validates a complete candidate under the same writer lease, base HEAD,
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

## Planned API surface (names indicative, not implemented)

Layer enumeration/loading requires an explicit project + pinned revision or
working-copy identifier; no implicit latest-HEAD mixing. Native operations
should include `listAnnotationLayersAtRevision`, `loadAnnotationLayerAtRevision`,
`loadAnnotationAtRevision`, and a draft layer/events writer integrated with
`saveWorkingCopyTransactions`. The HTTP analogue should live under the
project/revision and working-copy API hierarchy, with the same canonical MODEL
payloads and existing lease/generation/retry semantics. The final endpoint
names are decided in B3 alongside the actual handlers and OpenAPI spec.

The GUI must not inspect DVMDB's private on-disk structures or create native
kind-110/111 records. Standard REST/library reads are authoritative.

## Format break and B-phase acceptance

The first implementation must update the hydraulic membership count 18 -> 17,
remove kind 18 and its codec/type support, change the native manifest/schema
markers where required, reject earlier incompatible development repositories,
and update repository fixtures/tests together. No legacy decoder or implicit
migration is needed; **never** convert unknown bytes to empty data.

B-phase tests must cover: multiple distribution networks and distinct layers;
Drainage's project-domain scope; empty layers; exact snapshot reload;
chunk boundaries 255/256/257; edit/undo/redo/restart; mixed atomic target
move/delete+annotation detach; invalid cross-network anchor; duplicate layer;
unowned chunk/member; wrong record kind; field-merge conflict; independent
branch additions; archive/revision coverage; replication and crash recovery.

**A1 is documentation only**: no native kinds, route handlers, or new MODEL
code have been installed by this step.

## B1 implementation gate

B1.1 is a **record-format increment**, not a complete editable persistence
feature. `fullDraftAnnotationLayerChanges()` builds root, chunks and individual
records for one layer and returns an ordinary `ModelChange` set. The caller must
provide the owning distribution network record in the same candidate. The
`loadAnnotationLayerAtRevision()` wrapper pins the entire read to one immutable
revision. An empty layer is represented by a root without chunks. Chunk UUIDs
are SHA-256-derived from a namespaced layer UUID and serial, with a maximum of
4,096 chunks and 256 members per chunk. Independently addressed annotation
records hold canonical MODEL bytes and optional anchor references.

The builder and loader deliberately do **not** infer that an annotation belongs
to a hydraulic network simply because it references some UUID. The missing
candidate-wide ownership/anchor-closure validation is a hard B2 prerequisite
before any untrusted draft publication or general import. The reader verifies
owner record kind 100 when present, but full network-manifest verification and
exactly-one-layer-per-distribution-network constraints remain to be implemented.

DVMDB's archived submodule snapshot in the B1.1 input predates the A2/A3 MODEL
annotation types. Update the actual MODEL Git submodule to the committed new
MODEL contract as part of the **coordinated B1.2 update**. That step must also
remove the old 18th hydraulic collection from DVMDB's adapter and fixtures;
otherwise a full build with A3 MODEL will intentionally fail. No legacy shim
will be added.

## B1.2 — clean hydraulic contract transition

The native hydraulic collection manifest is now **17** collections (member
kinds `1..17`; kind `18` is retired), with manifest version **3**. This is an
intentional pre-release format break; old 18-collection manifolds are rejected,
not migrated. The canonical MODEL dependency must provide the **A3** types,
with the independent annotation codecs and without `HydraulicMapLabel`.
CMake rejects a stale MODEL submodule at configure time. Tests previously
using labels to exercise general membership/merge/history behavior now use
ordinary hydraulic generic curves; annotation-specific storage is tested in
its independent record suite.

The B1 record tests also commit a real hydraulic distribution root and its
annotation layer in **one immutable revision**, then reload both at that
pinned revision. This proves the record formats coexist, not that the
exactly-one-layer ownership rule is enforced on all ingress paths. Mandatory
candidate-wide ownership closure, referential integrity, and trusted layer
creation for every new network remain **B2/B3**, respectively. In particular,
`fullDraftHydraulicDistributionNetworkChanges` is not yet an all-domain
project initializer and must not be mistaken for one.

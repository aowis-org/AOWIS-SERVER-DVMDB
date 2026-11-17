# Versioned map annotation layers — DVMDB storage design (A1)

**Status:** agreed design, **not implemented**. Implementation is planned for
B1–B4. The canonical MODEL contract is in the MODEL repository at
`docs/map-annotations-contract.md`. These proposals do not describe APIs or
record types already present in the current binary.

## Storage boundary

An annotation is project data with an engineering-domain scope, **not a member
of the hydraulic network's 18-collection manifest**. A project repository's
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

Reserve the currently unused native kinds **110** (`annotation layer root`),
**111** (`annotation membership chunk`) and **112** (`MapAnnotation` member)
for implementation. Confirm that the kind registry still has no collision
when B1 starts; once used, kinds must be validated, not blindly inferred.
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

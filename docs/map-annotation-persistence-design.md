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

## B2 — Candidate-wide semantic integrity validator

`AOWIS::DvmdbAnnotationIntegrity` now provides
`annotations::validateAnnotationSnapshot(const MergeCandidate&)`. It accepts
**the complete pinned candidate**, not a delta, and returns a diagnostic on
failure. It checks:

- Every format-3 Distribution root has **exactly one** layer; two networks may
  not claim the same layer. The pre-network Drainage scope allows zero or one
  separate, ownerless layer.
- Canonical, typed hydraulic aggregates and native annotation layers are
  reconstructed through their existing adapters before trusting references.
  Their exact native record closure is checked against the enumerated keys:
  no unowned member, dangling chunk, reused member, or unknown record kind.
- Attached annotations can refer only to junctions, reservoirs, tanks or
  demand points **in the layer's own Distribution network**, with an exact
  canonical MODEL `entity_type`, not merely a matching UUID or record kind.
- The independent layer adapter continues to check exact canonical root,
  membership and member bytes, serialized references, and anchor existence.
  Native reverse-index consistency remains the responsibility of the native
  store's existing physical/reference verifier.

This validation is an explicit **pre-publication prerequisite**, not a
retroactive guarantee that all current `commitModel`, working-copy save,
import, merge or history paths invoke it. The old hydraulic-only merge
validator will still fail closed on annotation records until B3/B4 integrate
annotation-aware reconciliation. **Never expose an untrusted annotation writer
before wiring this validator to the actual fenced publication boundary**;
validating one layer alone does not establish project-wide consistency.

The B2 tests construct full in-memory candidates including real canonical
hydraulic roots and typed entities; they include adversarial duplicate layers,
missing owners, orphans, wrong kinds, cross-network anchors and mixed scopes.

## B3.1: server-owned draft edit materialization

`materializeDraftAnnotationEdits` is a **pure planner**, not a writer. It
accepts canonical `map.annotation` MODEL entity edits in original action order
against one pinned draft view, verifies every before-image, and derives only
the changed native annotation member/chunk/layer records (including tombstones).
It supports sequential edits to the same annotation and preserves membership
ordering. It refuses missing layers, foreign MODEL entity types, malformed
payloads and foreign UUID collisions. It uses only the existing MODEL codec.

This planner must not be called with unrelated/native member kinds, nor may its
result be saved with `saveWorkingCopyChanges` independently of the originating
MODEL events. **B3.2 still must** route it through the fenced,
generation-checked `saveWorkingCopyTransactions`, validate the complete
candidate against B2 *before* publication, and use the identical action bytes
for the durable journal and for replay. Also ensure network bootstrap/import
create their required empty annotation layer atomically, because B2 requires
exactly one Distribution layer per network. Until then, B3.1 is not exposed
as an HTTP write API and does not claim complete atomic publication.

## B3.2 — Fenced draft HTTP publication (current increment)

The HTTP `POST /dvmdb/v1/working-copies/{draft}/save-events` endpoint accepts an
optional `annotationLayerId` (canonical UUID string) alongside existing
`sessionId`, `fencingToken`, `expectedGeneration`, `changes`, and `events`.
The event frames must contain canonical `map.annotation` MODEL before/after
edits addressed to that one layer. `changes` is **not** the way to send
annotation member/chunk/root bytes: native kinds 110/111/112 are server-owned.

The handler pins the draft's generation, checks the non-annotation before-images,
materializes the annotation edit batch against the pinned pre-edit candidate,
then validates the **complete resulting project candidate** using the B2
validator. It passes the derived record delta and **original event frames** to
`ProjectService::saveWorkingCopyTransactions`; that service rechecks the lease,
fencing token and generation and atomically fsyncs the candidate and journal.
The logical request fingerprint (not derived chunk bytes) allows exact
replays after lost HTTP acknowledgements without reapplying membership edits.

Distribution network bootstrap and full-network import create their empty
annotation layers in the **same** working-copy save. The layer UUID is a
namespaced deterministic UUIDv8 derived from the network's UUID, so a
retransmitted initialization request identifies the same native layer.

Read API:

- `GET /dvmdb/v1/working-copies/{draft}/annotation-layers/{layer}` returns
  one pinned draft generation and ordered canonical annotation payloads.
- `GET /dvmdb/v1/projects/{project}/branches/{branch}/annotation-layers`
  lists layer IDs/owners/counts at a single selected immutable revision.
- `GET /dvmdb/v1/projects/{project}/branches/{branch}/annotation-layers/{layer}`
  returns canonical member payloads at that immutable revision. The existing
  `?revision=` selector follows normal branch/revision reachability rules.

The whole-candidate HTTP preflight is performed before acquiring the draft
writer lock to avoid reversing the project's lock order. The generation CAS
makes a successful preflight immutable relative to the subsequent Save: a
concurrent edit changes the generation and the Save is rejected. This is an
initial correctness-first implementation: enumerating the complete base-model
record set is O(project size) on each validated HTTP Save and needs future
incremental indexing/performance work.

**Scope limitations:** This increment integrates the **working-copy HTTP**
writer and its reader/bootstrap routes. It does **not** yet enforce B2 in
native `Repository::commitModel`, the direct branch hydraulic commit route,
merges, bundle import, archive verification or historical action replay.
Those paths remain on the B4/final integration checklist; do not claim that
all possible repository ingress paths enforce annotation integrity yet.

## B4.1 — annotation-aware revision merge preflight

The domain merge route now uses `annotations::mergeDistributionAnnotationBranches`,
which retains the existing hydraulic object-choice and membership resolver,
but adds field-aware MODEL merging of **existing annotation members** with
unchanged anchor references. It verifies the **entire** combined hydraulic and
annotation snapshot through B2 before any diverged merge or fast-forward
publication. The generic HTTP merge route detects hydraulic roots and native
annotation layer roots in the pinned input revisions, forwarding those merges
to the domain validator; raw/unrelated generic merges keep their previous path.

Concurrent modifications to annotation membership chunks or layer roots are
**not automatically reconciled in B4.1**: they result in a merge conflict,
including independent additions that modify the same membership chunk.
Explicit annotation field choices are not yet supported by the domain wrapper;
unused selections are rejected. The planned B4.2 work adds coordinated
membership reconciliation, and B4.3 covers archived action replay and imported
revision candidates. A native caller that directly invokes unvalidated generic
`Repository::mergeBranches` is not automatically domain-validated: callers must
use the domain adapter or supply equivalent `MergeOptions` before publication.

The merge regression test exercises nonconflicting member-field reconciliation,
same-field conflict (HEAD unchanged), source fast-forward with an invalid
missing layer (rejected), and revision immutability after merging.

## B4.2 — ordered three-way annotation membership merging

The annotation-aware merge reconstructs every affected layer against the
actual common-ancestor, target and source revision snapshots, *before* the
native store writes any pack objects or publishes HEAD. The native merge
provides a read-only ancestor reader to a second reconciliation callback; the
existing hydraulic reconciliation still executes first. This extension is
optional and has no effect on other repository merge clients.

Native annotation-layer roots and membership chunks (kinds 110/111) may
temporarily resolve to one branch's structural record, but that record is
**never published as-is**. The three-way reconciler reconstructs canonical
chunk boundaries and root references from the merged ordered annotation
sequence. This is essential for two branches independently allocating the
same deterministic chunk UUID, especially at the 256-member limit.

- Unchanged baseline members retain baseline order; one-sided deletions are
  retained as deletions unless the user explicitly selects a surviving
  delete-vs-modify object. Both branches' additions are appended in target
  order first, then source order, deduplicated by UUID.
- A one-sided reorder is preserved when the other branch's membership is
  unchanged. Concurrent ordering changes are **not** silently linearized;
  those require an explicit resolution workflow not yet exposed through HTTP.
- Native whole-object choices for genuine annotation member conflicts
  (including delete-versus-modify) are honored, with historical membership
  restored when the chosen object survives. Explicit per-field annotation
  conflict choices remain a separate follow-up.
- Absent/different network ownership and concurrent changes to layer
  metadata with no unambiguous three-way answer are conflicts. Unknown
  record kinds, missing anchors, orphaned members and invalid references
  still fail B2 candidate verification.
- Every superseded native chunk or member record is tombstoned in the
  ephemeral candidate, and the complete candidate passes normal reference
  preflight and B2 validation before publication. Old revisions are untouched.

Covered by the expanded `aowis-dvmdb-annotation-merge-tests` target: parallel
first-chunk additions, 255-to-257 boundary splitting, exact membership order,
object-level delete/modify choices, concurrent reorder conflicts and immutable
historical snapshots. Imported history/action replay is still **B4.3**.

## B4.3.1 — Authenticated chronological action order

For independent edits, replaying a reordered sequence can still produce the
identical immutable endpoint. Comparing native deltas is insufficient to prove
which historical state came after each action. The annotation replay verifier
therefore requires the **authenticated action-chain SHA-256 digest** from the
verified local Commit, separate from the untrusted event sequence being checked.
DVMDB already persists `action-chain` and verifies it when reading the immutable
Commit sidecar. `RevisionNativeActionWitness::authenticatedActionChain` exposes
the corresponding digest to the MODEL-aware replay planner. The digest algorithm
is identical to the existing DVMDB journal chain: 32 zero bytes initially,
then SHA-256(previous digest concatenated with each raw MODEL frame).

Replayers must pass the chain from a trusted revision witness; hashing the
caller-supplied action vector on the fly and passing that as the expected chain
would destroy this chronological proof. Transaction UUIDv7 ordering is not a
substitute for an authenticated archive sequence. The original Commit archive
remains immutable and no stored format migration is required.

## B4.3.1 — Immutable annotation-only MODEL archive replay proof

`map_annotation_action_replay.h/.cpp` supplies a **read-only proof primitive**
for archived annotation-only MODEL transactions. It accepts complete parent
and committed native snapshots, exact archived action bytes (oldest first),
and the actual native revision delta from a verified local Commit witness.

It reconstructs intermediate states through the server-owned annotation draft
materializer and validates the *entire* network-and-annotation candidate after
each action. It must reproduce both the byte-exact committed snapshot and the
actual native change set. `actionPosition` selects a boundary, returning a
minimal overlay relative to the immutable parent. Multi-layer edits are
attributed by immutable membership, without accepting an untrusted HTTP layer
selector. It rejects stale images, duplicate transaction identities, unknown
entity types, unjournaled native modifications, and unprovable ownership.

**Not yet a general History implementation:** The existing hydraulic History
planner cannot consume this proof, nor safely publish annotation positions.
Mixed hydraulic/annotation actions, a network/layer creation checkpoint,
archived History forks, and create-then-delete annotations absent from both
immutable snapshots currently fail closed. The last case requires durable
owner-layer provenance in the canonical MODEL action envelope: inventing an
owner from the number of layers in a project would be unsafe. These gaps need
a coordinated MODEL/DVMDB revision before enabling GUI annotation Undo/Redo.

**Imported histories:** The native revision bundle verifies hashes, Merkle
structure, and reference indices, but it does not carry a trusted local
MODEL action archive. Imported revisions are therefore **checkpoints**, not
action-navigable histories. Generalizing candidate-wide B2 semantic validation
to untrusted bundle pre-publication (including every imported revision) and
to generic direct native commits remains an explicit B4.3 follow-up. Do not
claim this read-only helper enforces those ingestion boundaries.


## B4.3 — durable MODEL action owner provenance

The canonical `ModelEntityEdit` for `map.annotation` now includes a required
`ownerLayerId`, authenticated by the transaction frame (version 2) and the
immutable ordered SHA-256 Commit action chain. Ordinary hydraulic edits have
no owner field and no extra encoded bytes. The HTTP `annotationLayerId` request
parameter must match every included annotation edit's authenticated owner; it
is not authoritative archival provenance. The native annotation materializer
verifies the same owner before making any changes.

The archive replay verifier uses `ownerLayerId` to attribute every action, even
when an annotation is created and deleted between immutable endpoints. It also
checks the parent/child membership owners for consistency and rejects any
owner switch of the same annotation UUID during one archive. This change does
not yet connect mixed hydraulic/annotation History navigation or generic
direct Commit validation; those are separate B4.3 requirements. Old MODEL
transaction frames without ownership are intentionally rejected.

### HTTP local Commit candidate preflight (B4.3 incremental)

The HTTP `POST /dvmdb/v1/local-commits` handler independently checks the
complete saved candidate against `validateAnnotationSnapshot` immediately
before invoking the existing fenced `ProjectService::commitWorkingCopy`. This
closes the HTTP pathway where a library-side Save could have supplied native
records without the HTTP `save-events` validator. Replayed Commit operation
UUIDs are reconciled by ProjectService rather than validated against a
newly rebased draft. Generation, base revision, lease and fencing checks
prevent publication after a concurrent change between preflight and Commit.

**Remaining boundary:** direct library callers of `ProjectService::commitWorkingCopy`
still need a native pre-publication domain validation contract. This HTTP
preflight is not a substitute for that or for mixed-history replay.

## Native local Commit semantic gate (B4.3 increment)

`ProjectService::commitWorkingCopy()` cannot rely on a prior HTTP Save: native
callers can place raw `ModelChange` records into the draft. For candidates with
annotation records (110–112), or a newly saved Distribution root (100), the
Qt-free service requires a semantic candidate-validation callback. It resolves
the complete pinned immutable base plus the saved draft overlay under the
project operation and draft locks, calls the supplied validator, and rejects
unvalidated or invalid candidates **before** creating a local Commit intent,
revision index, or advancing HEAD. The callback must be pure; it must not
call `Repository`/`ProjectService` again or retain candidate readers.

The HTTP Commit route supplies the B2 `validateAnnotationSnapshot()` callback;
this is not an HTTP-only integrity rule. Ordinary generic record repositories
still Commit without a domain validator. Rejected new operations leave the
same operation UUID available for a corrected retry. Already-succeeded and
pending operations retain the existing durable idempotency semantics.

**Remaining boundary:** direct legacy-style revisions which already contain a
Distribution root *without any annotation layer*, and which only modify an
ordinary hydraulic member, are not forced through this callback. That format
is unsupported for new project initialization, but older native test fixtures
still exercise it. Full fail-closed treatment of direct `commitModel()` and
all historic root-only native fixtures remains a separate consolidation step.

## B4.3 direct MODEL Commit preflight (native service)

`ProjectService::commitModel()` and `commitModelAtExpectedHead()` now provide a
pure `MergeCandidate` semantic-validation hook. The service builds the complete
candidate from the immutable observed HEAD and submitted changes. If the base
or changes contain native annotation record kinds 110–112, the hook is mandatory,
even for edits that only touch unrelated generic records. Both methods reject
invalid candidates before revision publication and preserve HEAD on rejection.
The generic HTTP `model-commits` route supplies B2's
`validateAnnotationSnapshot()`, exactly as the local-Commit route does. When
validation is needed, even an unpinned direct writer pins the observed HEAD
for the final native storage CAS, avoiding a stale preflight publication.

This validates native MODEL **structure and ownership**, not hydraulic solver
readiness. It deliberately does not require a connected or hydraulically
solvable network. This increment preserves older root-only hydraulic test
fixtures: direct root-only submissions without any annotation record are not
yet forced to supply the semantic validator. Similarly, callers using the
low-level `Repository::commitModel()` directly bypass ProjectService. Both
remaining boundaries must be closed before claiming universal native
engineering-publication integrity. The complete-candidate scan is currently
O(number of records) and should be optimized before large-network benchmarks.

## B4.3 — Low-level Repository Commit preflight

`Repository::commitModel()` is a native API callable without HTTP or
`ProjectService`. Under the repository's file/operation lock, it now builds
an immutable MODEL candidate from the committed HEAD and the proposed changes.
If **any** original or proposed record has annotation kind 110–112, the caller
must supply a `MergeCandidate` semantic validator. A supplied validator runs
before reference preparation, object writes, Commit revision creation, or
branch HEAD update; failure leaves pack and HEAD untouched. The high-level
Commit entry points forward their existing validator to this boundary.

This native store stays free of MODEL/Qt dependencies: trusted engineering
adapters must supply the actual B2 validator, not a permissive placeholder.
Ordinary generic-model repositories still work without such a callback.
When retrying an already durable pending local Commit after a crash, a missing
annotation validator returns a caller error **without** permanently rejecting
the intent; retry with the real domain adapter to complete it.

**Performance and scope:** This correctness-first guard currently scans the
entire pinned base snapshot for annotation kinds on each native MODEL Commit.
Large-project benchmarks and an indexed kind-presence optimization remain
necessary. Root-only legacy hydraulic fixtures (kind 100 without annotation
records) and direct low-level `writeRevision()` publication still need explicit
engineering-format policy. Neither network connectivity nor hydraulic solver
feasibility is a persistence condition.

## B4.3 indexed Commit witness for annotation-only History (read-only)

`verifyIndexedAnnotationActionBoundary()` connects the annotation MODEL replay
primitive to **actual**, indexed, first-parent local Commits. It requests a
`RevisionNativeActionWitness` from `ProjectService`, including the verified
ordered SHA-256 action chain and complete native delta, then loads both full
MODEL snapshots by pinned immutable revision UUID. A malformed or missing
archive, initial Commit without a parent, forked archive, mixed MODEL event,
wrong action coordinate, abandoned Redo suffix, ownership violation or unjournaled native delta
fails closed. Intermediate action boundaries are returned as native deltas
relative to the original immutable parent, **not** a draft-relative overlay.

This increment is intentionally **read-only**: it does not change HTTP
navigation, write any History cursor or claim that mixed hydraulic/annotation
Undo/Redo works. Next: combine the hydraulic and annotation replay planners
into one exact, all-domain candidate under the existing fenced/idempotent
History publication operation; keep per-domain action order and ownership.

### B4.3 — Fenced archived annotation navigation

Annotation-only local Commit archives with an immutable first parent can now
use the same `/working-copies/{draft}/history/navigate` endpoint and native
atomic History publisher as hydraulic archives. The server classifies the
original authenticated MODEL action frames; the client does not select the
replay engine or provide candidate records. Archive SHA-256 ordering, exact
revision delta, ownerLayerId and intermediate snapshots are proven before
computing a complete HEAD-relative replacement overlay. The native publisher
rechecks the lease, generation, HEAD, revision and Commit identity. Retries
reuse the same operation UUID and never advance generation twice. Mixed
hydraulic/annotation actions, forked archives and initial checkpoints are
still refused for annotation action navigation until joint replay is proven.
This is structural integrity validation, not a hydraulic simulation gate.

# Durable action history across Commit boundaries — DVMDB roadmap

**Current foundation:** DVMDB stores verified MODEL events in the active
private draft and retains those events after local Commit in a durable action
archive. The read-only `branchActionHistory` API and its HTTP route return
first-parent Commit markers, real archived action UUIDs and optionally paged
active-draft action UUIDs. A durable revision-keyed index resolves those
archives, and `revisionActionPage` / the frame endpoint expose paged original
MODEL bytes for UI decoding. HIST-03b and HIST-03c4b provide **native C++
checkpoint/action-boundary selection with a durable cursor** for verified,
non-journalled private drafts. There is still **no general Undo/Redo command,
redo eligibility, continuous cursor or public HTTP navigation endpoint**.
See [committed action archives](commit-action-archive.md) for current product
behavior and [history-contract.md](history-contract.md) for the agreed future
navigation semantics. The left History dock belongs to the GUI repository.

## Desired externally visible semantics

One selected branch's timeline contains ordered meaningful editor actions
and clear immutable **Commit markers**. The user can Undo/Redo or jump to
historical states across those markers. No operation rewrites an immutable
revision, changes its SHA-256 ancestry, or silently moves committed local or
shared branch HEADs. Navigation materializes the selected model content in
the user's **private working copy** through new fenced, idempotent events.
This means the candidate can differ from the current committed branch HEAD
while the branch still points to its existing latest revision.

When a new user edit occurs after Undo, ordinary redo eligibility beyond the
cursor is invalidated; the immutable revision DAG and audit record are not
lost. A future Commit of that model is a new successor of the branch's
**current committed** HEAD. Creating a branch with an older revision as its
**actual ancestor** remains a separate operation.

## Current backend foundation

- MODEL transaction frames encode reversible before/after entity/root
  images, logical operation/transaction IDs and object identities.
- `ProjectService::saveWorkingCopyTransactions` persists events atomically
  with candidate changes; HTTP `save-events` also checks membership and
  materializes server-owned hydraulic root/chunks.
- The draft file includes lease/fencing, generation, base revision,
  candidate overlay and **bounded** journal. `GET /working-copies/{id}`
  exposes it to authorized-by-deployment clients (no authentication yet).
- `commitWorkingCopy` uses a durable UUIDv7 operation ledger, reserves an
  immutable revision and rebases the draft on success, **clearing its journal**.
- `revision-graph` and the read-only `action-history` endpoint expose immutable
  checkpoints and retained action UUIDs from succeeded local Commits. Older,
  imported and merge revisions without action archives remain checkpoint-only.

## Required implementation

### HDB-01 — Remaining read-only history acceptance

The immutable archive, revision index, timeline, paginated full-frame
retrieval, display-ready MODEL action metadata, and generation-pinned draft
pagination are implemented. Remaining work is acceptance of import, merge,
cross-repository, backup,
compaction and recovery checkpoint handling. Checkpoint-only revisions must
never acquire fabricated intermediate steps. Earlier unindexed local Commit
archives are not migrated: those revisions remain checkpoint-only.

### HDB-02 — Reconstruction and navigation API

Build on the existing ordered/paged checkpoint, action-UUID listing and
exact original frame retrieval to add a durable state cursor, display-ready
summaries and an exact "materialize state" plan. Reconstruct from the nearest
immutable model checkpoint plus forward/inverse MODEL transactions, verify
canonical before/after images and membership, and never silently repair
missing objects. Do not allow jumps to invent actions between two old
revisions for which only final snapshots remain. For merge commits, expose
the first-parent narrative by default and metadata for the other parent(s).

**HIST-03a checkpoint reconstruction (implemented native foundation):**
`ProjectService::planHistoryCheckpoint(draftId, targetRevision,
expectedGeneration)` now computes the **complete replacement candidate
overlay** against the selected branch's current immutable HEAD. It verifies
first-parent ancestry, project/branch identity, the draft's HEAD pin and
generation, and applies the native overlay-size bound. An older snapshot can
require replacements, resurrected records and tombstones. Existing unrelated
draft edits are deliberately absent from the plan, because navigation must
**replace**, not merge into, the candidate overlay. This method is read-only:
it neither executes a transition nor persists a cursor/event, and is not yet
exposed over HTTP. Native tests cover all three change types, stale
generation/HEAD, foreign revisions, restart and non-mutation. The future
atomic navigation executor must regenerate/revalidate the plan under the
same locks and publish the replacement overlay together with history cursor
and MODEL event journal data; an ordinary incremental Save is **not** a
substitute for that atomic operation.

**HIST-03b native checkpoint selection (implemented, acceptance limited):**
`ProjectService::navigateHistoryCheckpoint(operationId, draftId, sessionId,
fencingToken, expectedGeneration, expectedHead, target)` locks the private
draft and project, validates the lease and immutable first-parent HEAD, then
recomputes the HIST-03a plan. It publishes the replacement candidate and
three cursor/operation fields in **one** atomic `AOWIS-WORKING-COPY-5` meta
update. It is idempotent by UUIDv7 operation ID (including after restart),
rejects stale lease, generation and HEAD, and leaves immutable revisions
untouched. Normal Save or Commit clears the exact-checkpoint cursor.

This is deliberately **not general Undo/Redo**. Active MODEL journals are
rejected instead of silently dropping/rewriting event provenance, and no
synthetic navigation MODEL frames are inserted. Individual action positions,
long-lived redo eligibility, checkpoint navigation audit archival, HTTP
execution, merge-origin navigation and GUI controls still require HDB-03.
The operation must not be wired to Undo/Redo until those contracts are
implemented and tested. The working-copy draft format changed from v4 to v5
without migration because AOWIS is pre-release.

**HIST-03c1 immutable action-boundary planning (implemented native read):**
`ProjectService::readHistoryReverseActionWindow(draftId, revision,
fromPosition, expectedGeneration, limit)` returns the original archived
MODEL frames in **newest-to-oldest** order for a requested position within
one first-parent Commit. Position *N* means immediately after the first *N*
actions; a 100-frame window can advance through a large archive without
skipping an action. The read checks the draft's pinned generation and current
branch HEAD before and after retrieving the index-verified immutable frames.
It rejects foreign/non-ancestor revisions and out-of-range action positions;
checkpoints with no archive remain position-zero markers, without invented
intermediate steps. It never changes the draft or an immutable revision.

**Important limitation:** those frames are the *original forward events* in
reverse retrieval order, **not executable inverses**. An archive can omit
individual edits from older changes-only Save paths, and the native-only
service does not have the MODEL-aware entity/membership codec required to
prove that the archived images reconstruct every intermediate state.
HIST-03c2 must qualify action coverage and image continuity against native
snapshots, generate validated inverse/replay changes, and persist their effect
and a durable cursor atomically. Until then, action-level navigation is
read-only and the GUI buttons remain disabled.

**HIST-03c2a MODEL-aware update replay (implemented, read-only):**
`hydraulic::previewReverseHydraulicActions()` verifies and reverses original
MODEL action images over one already-hydrated terminal network, covering
existing members from all 18 collections and mixed root-metadata updates.
It rejects stale images, unsupported types, corruption, and ordered membership
creation/deletion without a position record. No repository or draft mutation
occurs. The integration regression must pass in a Qt-enabled build before
this step is accepted. Outstanding HIST-03c2 work is proving archives fully
cover the native revision delta, recording membership order for structural
Undo/Redo, and publishing the planned result atomically with its cursor.

**HIST-03c2b ordered membership + full-network archive verification
(implemented read-only, Qt acceptance pending):**
`hydraulic::previewVerifiedHydraulicActionBoundary(parent, committed, frames,
boundary)` now replays the *entire* oldest-first archive from the immutable
parent, using the native append-on-insert / order-preserving-delete rules for
all 18 collections. Mixed root/typed events are verified as one atomic
candidate step. After verifying every canonical MODEL before/after image and
model dependency, it requires the reconstructed **whole network** to equal
the committed snapshot byte for byte before returning any intermediate state.
It rejects incomplete archives, unjournaled root changes, invalid UUID/type,
corrupt frames, unexpected ordering and truncated pages. Native persisted
revision snapshots are exercised as comparison witnesses. This is still
network-scoped, not a proof that every model record in a multi-network
revision is accounted for. The next backend step must bind the verifier to
the **actual pinned archived Commit and revision model records**, qualify
all changed networks and non-network records, and atomically persist the
result/cursor under lease, generation and HEAD fences. The history dock
remains read-only.

**HIST-03c3a immutable native-delta witness (implemented, native tests pass):**
`ProjectService::verifiedRevisionNativeActionWitness(project, branch, revision)`
now links an indexed local Commit's exact original MODEL frames with an
independently verified **complete native first-parent revision delta**. It
compares every changed model key and complete encoded `ModelRecord` against
both the immutable revision and the persisted Commit candidate. It also
checks the original first parent, supports unborn initial commits, detects
omitted/deleted and unexpected native records, rejects foreign checkpoints,
and survives repository restart. This is read-only and introduces no disk or
REST format change. The native test intentionally archives one action alongside
three changed records to demonstrate why **archive authenticity does not prove
that each changed record has a matching MODEL action**. The next step must
perform that MODEL-aware attribution for every affected network, including
server-generated root and chunk records, and explicitly reject unrelated
native changes. Do not publish an intermediate snapshot or enable Undo/Redo
based on the native witness alone.

**HIST-03c3b complete hydraulic native-delta attribution (implemented as a
read-only, fail-closed verifier; Qt acceptance pending):**
`hydraulic::verifyHydraulicRevisionActionCoverage(service, project, branch,
revision)` obtains the authenticated HIST-03c3a witness and an authoritative
bounded scan of the first-parent and committed native root manifests. It
loads both snapshots for *every* hydraulic network, assigns each MODEL edit
by its root/member UUID to exactly one network, splits cross-network actions
only for in-memory replay, and verifies every network's full canonical state
against the immutable child. Every changed native key must belong to a
verified root, chunk, or member and be explained by a root, membership, or
typed MODEL edit. Unrelated native records, changed networks without frames,
or missing/reordered members fail closed. The original archived frames are
never rewritten.

**Scope:** immutable first-parent local Commits containing existing hydraulic
networks. Network creation/removal, non-hydraulic records, merge commits and
unindexed/checkpoint-only revisions are deliberately ineligible. A complete
attribution proof establishes eligibility for a future atomic navigation
executor; it does **not** persist a history cursor or expose Undo/Redo.
Reading large snapshots is bounded to 65,536 native records per revision;
larger collections require a separate paginated verification design.

**HIST-03c4a draft-relative action-boundary planning (implemented, Qt test
acceptance pending):** `hydraulic::planVerifiedHydraulicActionNavigation(
service, draftId, revision, actionPosition, expectedGeneration)` performs the
complete HIST-03c3b archive/native-delta verification, retains the original
global action indices even when one transaction spans several networks,
replays the requested boundary for each affected network, and computes a
**complete candidate replacement overlay relative to the active HEAD**.
It starts with the immutable first-parent checkpoint plan, replaces affected
network records and server-owned membership chunks, and inserts tombstones
for obsolete records. It reopens the computed candidate in memory and demands
exact canonical equality for every affected network. It refuses a live draft
MODEL journal, unproven changes-only draft data, stale generation/HEAD,
incomplete MODEL archives and excessive record counts. This is **read-only**:
no draft, cursor, event journal or immutable revision is written. The next
step must atomically publish the validated overlay and logical cursor with a
fenced idempotent operation; a separate ordinary Save is not sufficient.
The snapshot format and REST contract are unchanged by HIST-03c4a.

**HIST-03c4b atomic verified action navigation (native implementation;
Qt acceptance pending):** `hydraulic::navigateVerifiedHydraulicAction` validates
and materializes the chosen action boundary using HIST-03c4a, then calls the
internal native publication API with the complete overlay and provenance.
DVMDB rechecks the active lease, draft generation, branch HEAD, immutable
indexed archive operation ID and action position under the existing draft
and project locks. Candidate and cursor metadata publish atomically; same-ID
retries (including after restart) return the already published result.
Active MODEL journals and unrelated unproven draft candidates remain
ineligible. The draft marker advances to **AOWIS-WORKING-COPY-6**, adding
`history-action-position` and `history-archive-operation`. Pre-release V5
working copies require recreation; committed revisions do not.

This completes one verified action-boundary publication operation, **not**
continuous Undo/Redo. Recording navigation itself as a permanent action,
redo-eligibility semantics after new edits, cross-Commit action cursor chains,
HTTP operations and GUI activation remain future HDB-03 work.

### HDB-03 — Durable Undo/Redo operation and concurrency

Generate new canonical inverse or forward events with fresh operation IDs,
link them to original actions, and persist all changes through the same
lease/fencing/expected-generation rules as normal editing. Retry an uncertain
Undo/Redo only under the **same operation identity**, preserving exact once
behavior. Handle second writers, stale branch HEAD, new edits after Undo,
Commit during history navigation, cancellation and restart. A history cursor
must never declare a state transition complete unless DVMDB accepted its
model changes. Model-value and referential constraints remain; hydraulic
solver feasibility is not required.

### HDB-04 — Operational limits and verification

Keep explicit capacity limits for active drafts and independently bounded
history retrieval. Define retention/compaction before unbounded archives
become a storage hazard; user-selected pruning must be explicit and visible
in the GUI. Test single and grouped edits, all 18 hydraulic collections,
root metadata, mixed events, large operations, 1- and 2-parent commits,
Undo/Redo across several checkpoints, dropped responses, crash/ENOSPC,
backup/restore and concurrent sessions. Canonical network and immutable
revision hashes/parents must match expected results after every restart.

## Delivery and acceptance

The GUI already has a read-only left History dock showing real first-parent
revision markers and available draft events. It **must keep** Undo/Redo/jump
disabled until HDB-01..03 and the GUI history controller exist. Complete
user-facing acceptance requires real external HTTP and embedded Standalone
sessions. No claims of working cross-Commit Undo should be made on the basis
of the present draft journal or generic reversible MODEL codec alone.

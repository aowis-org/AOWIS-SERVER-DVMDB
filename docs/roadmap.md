# DVMDB version-control roadmap

The delivery plan for the [version-control contract](version-control-semantics.md)
and [private working repositories](private-working-repositories.md). It is the
only page that describes planned work; the other pages describe what exists.
The companion GUI plan (`AOWIS-SERVER-GUI/docs/dvmdb-integration-roadmap.md`)
uses the same **VC/INT** milestone IDs, so work can be handed off milestone by
milestone. Security is **not** part of this roadmap
([security backlog](security-permissions-design.md)).

## A. Fixed contract

These points are settled. Changing one needs a coordinated update of both
roadmaps and the contract.

1. Projects, branches, revisions and drafts are distinct UUIDv7 resources.
   SHA-256 is integrity, never identity.
2. A project starts with an unborn `main` (null HEAD) and no revision; the
   first commit has zero parents. Branching *from uncommitted contents*
   requires a commit, but a project may contain multiple independent unborn
   branches.
3. A branch is a movable ref to an immutable revision; its HEAD is never an
   editable model. Shared and non-leaf HEADs are valid.
4. Editors change private drafts, never revisions. Standalone uses its
   embedded DVMDB; other clients need an explicitly configured private HTTP
   DVMDB. A shared source is never a fallback for drafts.
5. Save, local Commit, Fetch, Publish and Merge are separate actions. A local
   Commit moves only a personal branch; Publish moves a shared branch
   conditionally. Nothing stale overwrites commits or drafts.
6. Creating a branch from a revision creates a ref, not a commit, and records
   the origin branch and exact fork revision. Names are UTF-8 labels, not keys.
7. History is one revision DAG with real zero-, one- and two-parent edges.
   The GUI shows a writable HEAD card for each branch; those cards are not
   immutable revision records.
8. Repository roles, UUIDs, leases and fencing tokens are correctness
   mechanisms, not security. Do not expose unauthenticated writable services
   to untrusted clients.
9. Any *representable* hydraulic state, including an incomplete or invalid
   network, can be saved, committed, branched, fetched, published and merged.
   Database/record/ancestry integrity and concurrency remain mandatory;
   hydraulic diagnostics are independent of version-control acceptance.

## B. Decisions DG-01 – DG-07

All seven are decided and binding.

| Gate | For | Rule |
|---|---|---|
| **DG-01** repository-qualified refs | VC-03, VC-06 | Each repository has its own UUIDv7. A logical project or branch keeps its UUIDv7 when cloned or fetched; a new branch gets a new one. Each repository has its own mutable HEAD under `(repositoryId, projectId, branchId)`. Remote correspondence is explicit tracking state (remote repository, project, branch, observed HEAD or null), never inferred from names. Collisions and wrong targets are rejected. |
| **DG-02** drafts and sessions | VC-04 | One private DVMDB per intended user; a separate draft per session by default. Many readers, one writer: an expiring exclusive lease with fencing token **and** expected generation, checked atomically on every write. An expired lease may be taken over, never used to delete or to overwrite with stale data. A Save is durable before it is acknowledged. |
| **DG-03** idempotent operations | VC-05, VC-07 | Every Commit and Publish carries a caller-chosen UUIDv7 operation ID, a request digest and its target scope. `pending`/`succeeded`/`rejected` is durable; an identical retry returns the same outcome, a different request under the same ID is rejected. After a timeout, ask for the outcome under the same ID. Unresolved operations never expire. |
| **DG-04** pre-release formats | all | No backward compatibility or migration before the first release. Replace formats cleanly and reject unsupported data explicitly. Never silently rewrite history or disguise a failing fixture. |
| **DG-05** reconciliation | VC-08 | Explicit Fetch, safe fast-forward and three-way merge from a committed common ancestor with ordered parents, entity-UUID comparison, manual conflict resolution; hydraulic feasibility is diagnostic only and never a merge/Publish gate. No automatic rebase, force push, octopus merge, last-writer-wins or implicit Publish. An unresolved merge stays a draft. |
| **DG-06** first remote publication | VC-06, VC-07 | Clone keeps branch UUIDs. A local branch is provisioned on the remote **with its UUIDv7**, idempotently, with verified project, origin and fork. First publication of an unborn branch expects `null`; an absent branch is not a null-HEAD branch. No remote branch is synthesized after a failed Publish. |
| **DG-07** non-blocking hydraulic diagnostics | all | Commit, Publish, Fetch and Merge must not require a water source, connected topology or successful simulation. Retain canonical storage, revision and existing model-reference invariants. Permitting dangling hydraulic UUID references is a separate unresolved policy decision, not an INT-04 prerequisite. |

Parameters still to fix inside their milestones: draft quotas and retention,
operation-result retention, conflict report format.

## C. Status

Source status reviewed 2026-11-02. "Built" means an implementation path
exists in this repository, **not** that the full GUI scenario has been
accepted on every platform. Verify with the current test suite.

| Milestone | Built | Open |
|---|---|---|
| **VC-01** unborn projects, nullable HEADs | unborn `main` (`unborn: true`), nullable HEAD in storage, native and REST, history and graph for zero revisions | remove the legacy empty first revision of `POST /projects` without `unborn` (needs the GUI to always send `unborn: true`) |
| **VC-02** branch creation and provenance | from committed revision; multiple independent unborn branches; metadata and identity-based rename even when unborn | old name-addressed rename and model-commit endpoints still require non-null HEAD |
| **VC-03** repository identity and tracking | `repository.id`, one-time role, remote registry, CAS tracking records; `GET /repository/tracking`, `POST /repository/role` | HTTP routes to register remotes and record observations; GUI repository configuration |
| **VC-04** drafts | create/read/list/lease, event Save, first-network initialization, saved draft summary, empty-unborn draft import, network load, discard, crash tests | complete mutation-path and standalone end-to-end verification; general replacement/merge import |
| **VC-05** local Commit | native + HTTP Commit/status and GUI HEAD Commit action; strict nullable-HEAD CAS, reserved revision UUID, durable ledger, replay, draft rebase after success | end-to-end failure-mode/platform acceptance (strict model-reference checks are a separate policy decision) |
| **VC-06** Fetch and clone | verified full/incremental bundles (CLI, HTTP), identity-preserving project and unborn-branch provisioning | Fetch into a personal repository that updates tracking records; pinned remote reads |
| **VC-07** Publish | — | object transfer plus conditional shared-HEAD move, remote branch provisioning, Publish ledger and status |
| **VC-08** reconciliation | generic and hydraulic three-way merges with pinned HEADs and explicit choices | compare/reconcile of a draft against a new HEAD; replace blocking hydraulic merge validation with optional diagnostics; recursive merge bases (criss-cross histories are refused) |
| **VC-09** history and graph | pinned paged revision DAG of all branches, revision records with parent bindings, author, message and commit time on every revision | inspector fields for local vs shared HEADs |
| **VC-10** GUI workflows | Project/HEAD/Revision Inspectors, HEAD opening, selected revision read-only viewing/export, local Commit and saved draft import/export paths | full GUI/Standalone parity, general import replacement/merge, Fetch/Publish |
| **VC-11** qualification | crash, ENOSPC, SIGKILL, recovery, stress and backup suites; opt-in QEMU power-cut and HTTP soak drills | format freeze; repeated power-cut and soak runs on the final build; GUI acceptance evidence |

**Next, in order (unclosed acceptance gates):**

1. Apply DG-07 and resolve P0 repository-pinning, Commit-intent recovery and
   complete working-copy discovery (INT-00..03).
2. Keep hydraulic-solvability diagnostics non-blocking, then prove complete
   `NetworkHydraulic` entity/root capture and persistent exact recovery
   across desktop HTTP and embedded Standalone (INT-04..05).
3. Complete import/replacement semantics and a truly usable local
   HEAD/branch/Commit lifecycle without losing queued edits (INT-06..07).
4. Implement full-project Clone, Fetch, Publish and explicit reconciliation;
   then run the end-to-end and durability qualification gates (INT-08..12).

## C1. Coordinated implementation queue (INT-00 through INT-12)

**Order and ownership.** The matching IDs appear in the GUI roadmap. **DVMDB**
means native store, HTTP contracts and tests; **GUI** includes its desktop and
embedded Standalone client; **MODEL** means canonical model/transaction logic.
Each numbered item is a separately mergeable, independently testable handoff,
not an assertion that the implementation is missing in its entirety. **Planned**
means work remains; **built, acceptance open** means code exists but not all
end-to-end scenarios have been proved. A phase can require follow-up changes in
both repositories. P0 = correctness first; P1 = usable local editing; P2 =
remote collaboration; P3 = qualification and scale.

| ID | Priority | Lead / dependency | Deliverable | Completion gate |
|---|---|---|---|---|
| **INT-00** | P0 | DVMDB + MODEL + GUI; first | Non-blocking hydraulic-feasibility policy, current-path audit, consistent contracts | Unsupplied/disconnected network round-trips; reference restrictions stay strict |
| **INT-01** | P0 | GUI + DVMDB; INT-00 | Repository-pinned metadata writes, explicit stale/unknown-target outcomes | Endpoint switch/replacement cannot mutate another repository |
| **INT-02** | P0 | GUI; existing DVMDB Commit ledger | Durable operation IDs and status reconciliation for Commit | Lost POST *and* status reply, crash/restart, and retries produce one Commit |
| **INT-03** | P0 | GUI; existing DVMDB paging | Complete paginated working-copy discovery and scalable catalogues | 101/250+ drafts and broad branch history remain discoverable |
| **INT-04** | P1 | GUI + DVMDB; INT-00 | Protect non-blocking hydraulic/solver diagnostics across capture, Save, Commit and restart | Unsupplied and disconnected but referentially consistent models round-trip; strict reference checks remain unchanged |
| **INT-05** | P1 | GUI + MODEL + DVMDB; INT-04 | Every hydraulic mutation and network-root field captured atomically | 18/18 collections + root survive exact reopen after both process restarts |
| **INT-06** | P1 | GUI + DVMDB; INT-05 | Explicit safe network creation/import/replacement and selected-entity import | No discarded unsaved events, ambiguous replacement, or partial server mutation |
| **INT-07** | P1 | GUI + DVMDB; INT-02,05 | Complete local HEAD/branch/draft editing + Commit UX | Unborn, branch, Commit, invalid model and restart work without shared HEAD moves |
| **INT-08** | P2 | DVMDB + GUI; INT-01,03,07 | Full-project Clone and Fetch with pinned remote tracking | All branches/history/unborn HEADs retain UUIDs; drafts and local HEADs untouched |
| **INT-09** | P2 | DVMDB + GUI; INT-02,08 | Publish with durable idempotent intent, object transfer, conditional remote HEAD CAS | Stale and timed-out Publish never lose private work or duplicate effects |
| **INT-10** | P2 | DVMDB + GUI; INT-04,08,09 | Compare, fast-forward, explicit three-way conflict resolution, two-parent Commit | Compatible and intentionally invalid merges are preservable; unresolved conflicts never overwrite a draft |
| **INT-11** | P3 | GUI + DVMDB; INT-01..10 | Full desktop HTTP / embedded Standalone / browser-where-supported acceptance | Recorded build, tests and real scenario evidence per deployment |
| **INT-12** | P3 | DVMDB + GUI; after correctness | Journal capacity, bounded discovery, soak, crash/power-cut, backups and scale | No surprise edit loss or unbounded requests; measured release thresholds |

### INT-00 — Fix the model-validity contract (decision, audit, regression)

* **Decided:** Commit, Fetch, Publish, branch creation and Merge may preserve an
  disconnected, unsupplied, incomplete or solver-invalid **but referentially
  consistent** hydraulic network. Hydraulic feasibility alone never blocks
  version-control advancement. Save preserves such intermediate states,
  subject to the same storage and model-reference rules.
* **Hard failures remain:** malformed/noncanonical record bytes, invalid or
  colliding persisted IDs, invalid revision parents, corrupt hashes, broken
  physical membership/index invariants, failed atomic transactions, stale
  leases/generations/HEADs and repository mismatch. These are integrity or
  concurrency failures, not hydraulic assessments.
* **Audit first:** `Repository::commitModel`, MODEL codec, record/reference
  indexing, draft event Save, candidate hydration and the hydraulic merge and
  fast-forward callbacks. Missing/incorrect hydraulic model references are
  currently rejected and remain a **separate policy decision**. Do not
  loosen reference checks without first designing consistent indexing, import
  and repository verification semantics.
* **Evidence:** one hydraulically unsolvable but referentially consistent
  network survives Save → Commit → branch → restore byte for byte. Expand
  Fetch/Publish/Merge regression coverage during INT-08..10; malformed
  records and dangling revision parents must still fail.

**INT-00 implementation audit:** [model-validity boundary audit](model-validity-boundary-audit.md)
records the precise existing guards and an incomplete-network round-trip
regression. The source audit is done; GUI durable queue coverage is added in
INT-04. Full HTTP/Standalone and collaboration acceptance is still open in
INT-05 and INT-08..11.

### INT-01 — Repository-pinned mutable metadata (GUI/HTTP)

* The GUI's `ProjectSourceWriter` must verify expected repository UUID and
  MODEL fingerprint/role before **every** create/rename/project/branch metadata
  mutation, including after Standalone endpoint resolution. Carry expected
  repository identity from the selection; do not infer identity by URL/name.
* Prefer a server-checked expected repository UUID in mutations (or an
  equivalent pinned HTTP contract), so a race between preflight and POST/PATCH
  cannot redirect a write. Confirm what happens after an uncertain metadata
  acknowledgement: refresh authoritative identity and state; never invent
  idempotency where the API lacks it.
* **Evidence:** changed endpoint with colliding project/branch UUIDs,
  disappearing source, wrong role/fingerprint, dropped PATCH acknowledgement.

**INT-01 implementation checkpoint:** DVMDB HTTP now honors the
`X-AOWIS-Expected-Repository-Id` guard on six metadata mutation routes;
the GUI writer now preflights repository UUID/role/MODEL fingerprint and
always sends the pinned header. Regression coverage checks wrong targets and
client refusal before mutation. Final compiled and cross-deployment acceptance
remains open.

### INT-02 — Recover Commit intent across client death

* DVMDB's native/HTTP local Commit ledger and status lookup already exist;
  extend only where a fault test identifies a backend defect.
* GUI persists **before POST** the operation UUIDv7, immutable request
  parameters/digest, repository/project/branch/draft IDs, expected generation
  and HEAD, and a local `pending` state. Persist resolution before permitting
  another Commit, including if both POST and status lookup fail.
* After restart/reconnect, look up the same ID and distinguish pending,
  succeeded, rejected and unreachable. Do not create a fresh ID while the
  first result is unresolved; do not assume a timed-out response was failure.
* **Evidence:** lost first/second response, process death at each phase,
  stale expected HEAD, lease takeover, repeated requests and different body
  under the same ID all produce the documented result with one logical
  revision maximum. A proven `not-found` ledger entry may be retried only
  with that **same** operation ID and identical request, never a new one.

### INT-03 — Correct, complete discovery

* DVMDB already pages `GET /working-copies` with `nextCursor` and optional
  `projectId`; GUI must follow every page, enforce cursor progress and reject
  loops/inconsistent results, ideally using project filtering where relevant.
* Treat absence after a single page as **unknown**, never as proof that a
  draft does not exist. Validate repository binding between pages. Bound
  requests but return explicit incomplete/status rather than silent truncation.
* Expand catalogue/graph handling without hardcoded first-page assumptions;
  preserve pinned history closure and avoid UI hangs on large projects.
* **Evidence:** at least 101 and 250 drafts, filters, concurrent additions,
  100+ projects, broad branch sets and incomplete/changed cursors.

### INT-04 — Non-blocking hydraulic feasibility (without weakening references)

* Preserve hydraulically unsolvable but referentially consistent network
  states: absent reservoir/tank/supply, disconnected components and solver
  infeasibility are not reasons to reject Save, Commit or Branch.
* Verify GUI capture, canonical event staging, durable queue restart and
  native draft/Commit/branch/revision reconstruction. Keep existing guards
  against malformed MODEL bytes, duplicate identities, stale fences and
  dangling or wrong-type hydraulic UUID references unchanged.
* Record the unresolved *reference-policy* choice separately. If permitting
  dangling references is chosen later, design reverse-index integrity,
  bundle verification, import and merge before changing any guard.
* **Evidence:** the INT-00 DVMDB unsupplied-network regression and an INT-04
  GUI disconnected/unsupplied edit + queue-recovery regression. Full HTTP,
  Standalone, Fetch/Publish/Merge qualification remains in later milestones.

### INT-05 — Full hydraulic editing capture and persistence

* **MODEL:** canonical transaction coverage for all 18 top-level collections
  **and** persistent `NetworkHydraulic` root fields (network identity, options,
  times, reporting, MSX settings, map backdrop). Preserve deterministic
  ordering and meaningful before/after images.
* **DVMDB:** lease/generation-fenced atomic events for entity membership and
  root metadata, including mixed edits; no client-authored physical root or
  membership-chunk shortcuts. **GUI:** audit every editing widget, direct
  setter, library, control, label, undo/redo and importer transition.
* **Evidence:** complete canonical equality, not just successful API calls,
  after GUI and server restarts for both ordinary HTTP and embedded
  Standalone, including mixed edits and lost Save acknowledgements.

**INT-05 regression checkpoint (2026-11-03; acceptance pending):** The
native persistence suite compares the **entire canonical** `NetworkHydraulic`
image, rather than checking only selected metadata and entity fields. It
includes a root-metadata materialization followed by immutable Commit,
repository restart, verify and compaction with all 18 collections present.
The GUI test independently captures 18 collection insertions and a rich root
metadata edit in one durable queue, and checks exact equality after queue
restarts and acknowledgements. The subsequent **mixed-save implementation**
allows one MODEL transaction to combine root metadata, typed records and
membership intents. DVMDB derives chunks first, then overwrites the final
server-owned root manifest with the metadata update, so only one native root
change is applied atomically. New HTTP and GUI regressions test the mixed
request, idempotent replay, stale before-image rejection and queue restart.
These tests still require execution in the target Qt environment; complete
external HTTP/Standalone process round-trips, every UI setter and undo/redo
remain outstanding acceptance work.

### INT-06 — Define and deliver explicit import operations

* Preserve the existing **first import into an untouched unborn draft**.
  Separately design (a) import into a new project/draft, (b) atomic replacement
  of an existing *selected* draft, and (c) selected-entity merge/import.
* **Decision gate before code:** replacement's recovery/undo boundary,
  network-root identity, UUID collisions, queued event handling, and whether
  a second draft or branch is created as a safety checkpoint. Default: never
  overwrite an active queue; require explicit target and confirmation.
* Backend preconditions must include exact repository/draft/session/lease/
  generation and durable idempotency or authoritative outcome recovery.
  Aborted/failed operations keep original data and queued edits.
* **Evidence:** retry after lost acknowledgement, collision and mismatch,
  replacement while dirty, library-only/empty-root model, import parity.

### INT-07 — Make local history genuinely usable

* Wire personal-vs-shared state to Project/Branch/HEAD/Revision Inspectors;
  show unborn HEAD, dirty/saved draft, base revision and pending Commit status.
  No disabled/absent personal repository may silently redirect an edit.
* Test branch-from-exact-revision, independent unborn branches, save without
  Commit, local Commit with nonblank message and pinned HEAD, read-only
  immutable revision, reopened post-Commit draft and intentionally invalid
  committed snapshots. One branch update never changes another or the remote.
* **Evidence:** complete scenario in GUI and embedded Standalone including
  process restart, stale writer, duplicate acknowledgements, and a passing
  Projects Inspector test suite (fix test-vs-UI ownership mismatches).

### INT-08 — Full-project clone and Fetch

* Pin the remote repository identity and HEAD snapshot; transfer/verify all
  immutable objects (including both parents), provision project/branches by
  stable UUIDv7, and preserve unborn branches explicitly (absent != null).
* Update tracking observations atomically in the private repo; handle remote
  branch advance and deletion/absence without resetting local HEADs or drafts.
  GUI must expose clone whole project **as distinct from branch checkout**,
  manual Fetch, source/last-fetched HEAD and progress/retry states.
* **Evidence:** two independent private replicas, changed remote during
  transfer, interrupted/restarted Fetch, repeated identical Fetch, merge DAG,
  differing local/remote HEADs and no altered drafts.

### INT-09 — Atomic, recoverable Publish

* Add an idempotent Publish ledger and HTTP status contract to match Commit:
  caller operation UUIDv7/digest, exact remote repository/project/branch and
  expected remote HEAD, missing-object transfer, hash/ancestry verification,
  durable CAS and explicit result recovery. First branch provisioning must
  retain its UUIDv7 and be idempotent and separate from Publish.
* **Do not** reject publication just because a hydraulic snapshot is invalid.
  Publish requires storage/revision integrity and ancestry/fast-forward rules.
* **Evidence:** Alice/Bob race R7→R8 vs R7→B8, both possible lost replies,
  private commits untouched on rejection, null vs absent branch, crash in
  object transfer / ref update and exact-once recovered outcomes.

### INT-10 — Explicit reconciliation and merger

* Compare tracked remote HEAD, local committed HEAD and private dirty draft;
  provide non-mutating change/conflict preview using immutable pinned heads.
  A fast-forward needs approval; divergence uses a three-way merge with
  field/object choices, delete/edit diagnostics and ordered two-parent
  revision in the private repository. Never silently consume a dirty draft.
* Manual **merge conflicts** require resolution; **hydraulic feasibility diagnostic
  findings do not**. A user may deliberately choose the broken model in a
  merge. Existing strict hydraulic merge/fast-forward validation needs a
  non-blocking path consistent with INT-00 and INT-04.
* **Evidence:** compatible and contradictory edits, unsupplied/disconnected
  outcome chosen explicitly, cancellation, stale pins, merge-base ambiguity,
  2-parent DAG and explicit subsequent Publish.

### INT-11 — Real end-to-end acceptance

* Maintain a scenario matrix for desktop GUI ↔ HTTP DVMDB, embedded
  Standalone (persistent data root and restart), and browser ↔ HTTP (where
  features are supported). Record actual command, platform, tests passed and
  skipped; **one focused CTest does not certify a whole workflow**.
* Mandatory user stories: new unborn project; create, edit, autosave and
  reopen; 18 collections + root; intentionally invalid intermediate model;
  local Commit; branch and compare; clone/fetch; competing Publish; merge;
  restore after both process restarts. Include wrong-target/fingerprint,
  power loss, stale lease, bad generation and lost acknowledgements.
* **Evidence:** verified canonical bytes and UUID/ancestry/ref state before
  and after operations, not screenshots alone.

### INT-12 — Capacity and release qualification

* Address the existing **4,096 events per working-copy journal** and hard
  candidate-size limits. Define warning thresholds and safe explicit
  Commit/rotate/archive workflows; never pretend a blocked Save succeeded.
  A rejected Save keeps the durable queue. Evaluate compaction only if it
  preserves recovery, replay and existing draft/history semantics.
* Improve paginated discovery limits and graph responsiveness. Re-run
  native integrity, crash/ENOSPC, power-cut, backup/restore, bundle, HTTP
  soak and realistic GUI workload tests on the final implementation.
* **Evidence:** measured resource targets and repeatable test commands;
  no claim of zero loss without crash/power-cut evidence. Security and
  obsolete-format migration are separate/out of scope.

### Milestone delivery rule

For **each** INT handoff: use fresh current source ZIPs; identify changed
native/HTTP/GUI contracts; supply a smallest meaningful vertical patch and
regression tests; report commands/results versus unexecuted tests honestly;
update both roadmaps' status; only mark accepted after the scenario in the
**completion gate** is demonstrated. Use only changed/added paths in each
repository patch ZIP with fresh timestamps. SQL migration is not a part of
DVMDB's native-store integration.

## D. Milestones

Each milestone is a mergeable checkpoint with native and REST behaviour
changed together; never ship a writer whose reader cannot reload its data.

### VC-01 — Unborn projects and nullable HEADs

Create project and `main` with an explicit null HEAD and no revision; represent
null as a typed optional everywhere; history and graph work with zero
revisions. In write preconditions a missing expected HEAD is invalid, not null.
**Done when** a new `main` has a null HEAD across create, restart, REST, CLI,
graph and bundle round trip.

### VC-02 — Branch creation and provenance

Support both independent unborn branches within an existing project and
branches rooted at a selected committed revision. For the latter, verify
source project/reachability and persist immutable origin and fork. Names
are unique and editable without changing UUIDs. **Done when** branching adds no
revision and does not change the source HEAD, and later commits cannot change a
recorded fork point.

### VC-03 — Repository identity and tracking

Durable repository identity independent of projects and GUI profiles;
repository-qualified refs and tracking records; wrong-repository detection.
**Done when** two private repositories track the same shared branch without
sharing a writable ref.

### VC-04 — Durable drafts

Draft records with branch, base or null, owner session, dirty state, candidate
and generation; create, read, list, lease, save, discard and recovery in the
native core and over HTTP; transactional multi-entity saves fenced by lease,
token and generation; checkout of an unborn branch as an empty model. **Done
when** drafts survive restart, a stale writer cannot overwrite a draft, and a
real editor reopens an autosaved model byte for byte after a restart.

### VC-05 — Local Commit

Commit a persisted draft with expected local HEAD or null and an operation ID;
validate record integrity **but not hydraulic constraints**; one revision, published by
CAS; durable outcome with replay and status lookup; on a HEAD race, a conflict
that keeps the draft. After success the draft continues from the new HEAD.
**Done when** of two racing Commits only one moves the HEAD and the other's
draft survives, and a crash or retry yields one logical commit.

### VC-06 — Fetch and clone

Fetch into a personal repository over local and HTTP transports: verified
history including both merge parents, pinned remote HEAD or null, updated
tracking records, unborn remote projects, without touching drafts or local
refs. **Done when** two private repositories fetch one shared project and keep
isolated branches, drafts and reproducible tracking state.

### VC-07 — Publish

Transfer missing objects, then move exactly one shared branch from an exact
expected remote HEAD or null, fast-forward only, after repository/record
integrity and ancestry checks **without hydraulic validity gating**;
first-publish provisioning is a separate idempotent operation that keeps
the branch UUIDv7; a Publish ledger with status lookup; on a stale HEAD a
conflict with the current remote HEAD and no change to local commits or
drafts. Cover death of either side at every step. **Done when** Alice's Publish
advances R7→R8, Bob's competing R7→B8 is rejected, B8 survives, and retries are
safe.

### VC-08 — Reconciliation

Pinned common-ancestor analysis; field and object changes including
delete-versus-edit and reference conflicts; preview of local tip, remote tip
and base; user-approved fast-forward or manual three-way resolution with
non-blocking whole-model diagnostics; a two-parent merge revision in the
private repository, published only through VC-07. Rebase/replay is a later, separate decision.
**Done when** conflicting and compatible concurrent edits behave predictably
and an aborted merge loses no private work.

### VC-09 — History and graph

One pinned, paged DAG for all branch HEADs with nullable HEADs, revision UUIDs,
record hashes, fork metadata and both parents; branch labels are not vertices;
inspector fields for origin, fork, shared and local HEAD and draft status.
**Done when** GUI and REST render the same DAG for an empty project, shared
tips, divergence and a merge, without invented edges.

### VC-10 — GUI workflows

Private repository configuration and status, checkout and recovery, Save,
Commit, Fetch, Publish, conflict inspector, branch from revision; one semantics
for embedded and HTTP clients; progress, cancellation and recovery of
ambiguous results for every write. **Done when** GUI tests drive two private
repositories and one shared one through a failed Publish and a later
successful merge.

### VC-11 — Qualification and release

Freeze the first-release format; test backup, restore, reopen, compaction and
power-loss recovery; full and incremental transfer of all identities and
nullable HEADs; fault injection (kill, ENOSPC, delayed responses, duplicates,
corruption, high concurrency); all suites in the real Linux, Standalone and
browser configurations, with recorded commands and results. **Done when** the
acceptance scenarios below pass and the evidence is recorded.

## E. Acceptance scenarios

1. New project: `main` HEAD is null, no revision.
2. First checkout of unborn `main`; Save survives a crash and moves no HEAD;
   local Commit creates the zero-parent first revision.
3. First Publish expects null and succeeds only while shared `main` is unborn.
4. Two branches share a revision without a new commit; fork provenance is
   immutable across rename, restart and replication.
5. Commits on two branches diverge; a merge produces one verified two-parent
   revision.
6. Alice and Bob both commit from shared R7; only the first compatible Publish
   advances the shared HEAD; the other keeps its history and drafts.
7. A timeout at any Commit or Publish step is resolved by operation ID without
   duplicates.
8. Wrong project, repository or branch, corrupt hashes, invalid UUIDs, missing
   parents and corrupted/ambiguous storage records fail without moving a HEAD.
   Valid-UUID but dangling *hydraulic* references are currently rejected;
   their permissibility is an unresolved design decision, not a prerequisite
   for solver-invalid network commits.
9. Shared and non-leaf HEADs and several branches on one revision render
   correctly; each revision appears once; drafts are separate.
10. Without a reachable personal repository editing is disabled; nothing falls
    back to the shared server or to volatile memory.
11. Backup, recovery, compaction and replication keep all identities, hashes,
    fork metadata, nullable HEADs and tracking state.
12. Cloning `main` into two private repositories keeps one branch UUIDv7 with
    three independent HEADs; a new branch keeps its UUIDv7 when first
    published; absent and null-HEAD remote branches are distinguished.
13. Two sessions reopen one draft: only the lease holder with the current token
    and generation can save; stale saves after a takeover fail without losing
    data.
14. Lost responses to Commit or Publish are answered by status lookup; replays
    create no duplicates; a different request under the same ID is rejected.
15. Data in an unsupported pre-release format is rejected without being
    modified.
16. Two branches change the same model from a common ancestor: manual
    three-way conflict resolution is required; hydraulic diagnostics are
    optional, and an intentionally invalid merged network is permitted.

## F. Handing off a milestone

Give the new thread the latest DVMDB and GUI snapshots, this roadmap and the
matching GUI milestone, and ask for one milestone or one testable step of it.

1. **Baseline:** read the actual code; don't assume an earlier patch was
   applied.
2. **Decisions:** apply the relevant DG rules; settle formats and wire fields
   in the docs before implementing them.
3. **Scope:** list changed APIs and formats, native and REST callers, GUI
   dependencies, crash implications and fixtures.
4. **Implementation:** a complete vertical slice with typed UUIDv7 identities
   and explicit expected-ref semantics.
5. **Evidence:** run the tests, including negative, race and fault cases;
   report exactly what ran and what did not (for example Qt suites).
6. **Delivery:** a ZIP of changed and added files only, rooted at the
   repository directory, with fresh timestamps, plus
   `./tools/apply_patch.sh ~/Downloads/<zip>` and a one-line commit message.
7. **Status:** update section C of this page with what is now built and what
   remains. Packaging or a syntax check is not evidence.

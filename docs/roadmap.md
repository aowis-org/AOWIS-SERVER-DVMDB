# DVMDB version-control roadmap

The delivery plan for the [version-control contract](version-control-semantics.md)
and [private working repositories](private-working-repositories.md). It is the
only page that describes planned work; the other pages describe what exists.
The companion GUI plan (`AOWIS-SERVER-GUI/docs/dvmdb-integration-roadmap.md`)
uses the same milestone IDs, so work can be handed off milestone by
milestone. Security is **not** part of this roadmap
([security backlog](security-permissions-design.md)).

## A. Fixed contract

These points are settled. Changing one needs a coordinated update of both
roadmaps and the contract.

1. Projects, branches, revisions and drafts are distinct UUIDv7 resources.
   SHA-256 is integrity, never identity.
2. A project starts with an unborn `main` (null HEAD) and no revision; the
   first commit has zero parents. No branch can be created from an unborn
   branch.
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
   Branch labels decorate HEADs; drafts are outside the DAG.
8. Repository roles, UUIDs, leases and fencing tokens are correctness
   mechanisms, not security. Do not expose unauthenticated writable services
   to untrusted clients.

## B. Decisions DG-01 – DG-06

All six are decided and binding.

| Gate | For | Rule |
|---|---|---|
| **DG-01** repository-qualified refs | VC-03, VC-06 | Each repository has its own UUIDv7. A logical project or branch keeps its UUIDv7 when cloned or fetched; a new branch gets a new one. Each repository has its own mutable HEAD under `(repositoryId, projectId, branchId)`. Remote correspondence is explicit tracking state (remote repository, project, branch, observed HEAD or null), never inferred from names. Collisions and wrong targets are rejected. |
| **DG-02** drafts and sessions | VC-04 | One private DVMDB per intended user; a separate draft per session by default. Many readers, one writer: an expiring exclusive lease with fencing token **and** expected generation, checked atomically on every write. An expired lease may be taken over, never used to delete or to overwrite with stale data. A Save is durable before it is acknowledged. |
| **DG-03** idempotent operations | VC-05, VC-07 | Every Commit and Publish carries a caller-chosen UUIDv7 operation ID, a request digest and its target scope. `pending`/`succeeded`/`rejected` is durable; an identical retry returns the same outcome, a different request under the same ID is rejected. After a timeout, ask for the outcome under the same ID. Unresolved operations never expire. |
| **DG-04** pre-release formats | all | No backward compatibility or migration before the first release. Replace formats cleanly and reject unsupported data explicitly. Never silently rewrite history or disguise a failing fixture. |
| **DG-05** reconciliation | VC-08 | Explicit Fetch, safe fast-forward and three-way merge from a committed common ancestor with ordered parents, entity-UUID comparison, manual conflict resolution and whole-model hydraulic validation. No automatic rebase, force push, octopus merge, last-writer-wins or implicit Publish. An unresolved merge stays a draft. |
| **DG-06** first remote publication | VC-06, VC-07 | Clone keeps branch UUIDs. A local branch is provisioned on the remote **with its UUIDv7**, idempotently, with verified project, origin and fork. First publication of an unborn branch expects `null`; an absent branch is not a null-HEAD branch. No remote branch is synthesized after a failed Publish. |

Parameters still to fix inside their milestones: draft quotas and retention,
operation-result retention, conflict report format.

## C. Status

As of 2026-11-01. "Built" means implemented with automated tests in this
repository; nothing below is accepted end to end with the GUI yet.

| Milestone | Built | Open |
|---|---|---|
| **VC-01** unborn projects, nullable HEADs | unborn `main` (`unborn: true`), nullable HEAD in storage, native and REST, history and graph for zero revisions | remove the legacy empty first revision of `POST /projects` without `unborn` (needs the GUI to always send `unborn: true`) |
| **VC-02** branch creation and provenance | branch from a committed revision or source branch, immutable `originBranchId`/`forkRevisionId`, kept across rename, restart and bundles | rename and model commits on an unborn branch (both require a non-null expected HEAD today) |
| **VC-03** repository identity and tracking | `repository.id`, one-time role, remote registry, CAS tracking records; `GET /repository/tracking`, `POST /repository/role` | HTTP routes to register remotes and record observations; GUI repository configuration |
| **VC-04** drafts | create, read, paged list, lease, Save, event Save with MODEL-image checks and server-side membership, first-network initialization, network hydration, discard; incremental on-disk format; crash and concurrency tests | GUI: editor session binding, edit capture and queued autosave, reopen after restart with byte-identical model |
| **VC-05** local Commit | native `commitWorkingCopy`/`localCommitStatus`: strict nullable-HEAD CAS, reserved revision UUID, durable ledger, replay, recovery of pending outcomes, rebase of the draft after success | full hydraulic validation of the candidate before publication; HTTP Commit and status routes; GUI Commit action |
| **VC-06** Fetch and clone | verified full/incremental bundles (CLI, HTTP), identity-preserving project and unborn-branch provisioning | Fetch into a personal repository that updates tracking records; pinned remote reads |
| **VC-07** Publish | — | object transfer plus conditional shared-HEAD move, remote branch provisioning, Publish ledger and status |
| **VC-08** reconciliation | generic and hydraulic three-way merges with pinned HEADs and explicit choices | compare/reconcile of a draft against a new HEAD; recursive merge bases (criss-cross histories are refused) |
| **VC-09** history and graph | pinned paged revision DAG of all branches, revision records with parent bindings, author, message and commit time on every revision | inspector fields for local vs shared HEADs |
| **VC-10** GUI workflows | — | GUI and Standalone/HTTP parity for all actions |
| **VC-11** qualification | crash, ENOSPC, SIGKILL, recovery, stress and backup suites; opt-in QEMU power-cut and HTTP soak drills | format freeze; repeated power-cut and soak runs on the final build; GUI acceptance evidence |

**Next, in order:**

1. GUI: bind first-network initialization and draft hydration to a real editor
   session; capture, queue and flush edits (including creation and deletion)
   across reconnects and restarts.
2. DVMDB: run the complete hydraulic-network validator on the candidate inside
   `commitWorkingCopy`.
3. DVMDB: HTTP routes for local Commit and its status, with the same operation
   ID semantics and the author and message in the request.
4. GUI: Commit action; prove the first commit of an unborn branch end to end.
5. Then Fetch (VC-06) and Publish (VC-07).

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

Create branches only from an existing revision (verified same project and, with
a source branch, reachable from it). Persist name, initial HEAD, origin branch
and fork revision; rename keeps all of them. **Done when** branching adds no
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
validate references **and** hydraulic constraints; one revision, published by
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
expected remote HEAD or null, fast-forward only, after integrity and hydraulic
validation; first-publish provisioning as a separate idempotent operation that
keeps the branch UUIDv7; a Publish ledger with status lookup; on a stale HEAD a
conflict with the current remote HEAD and no change to local commits or
drafts. Cover death of either side at every step. **Done when** Alice's Publish
advances R7→R8, Bob's competing R7→B8 is rejected, B8 survives, and retries are
safe.

### VC-08 — Reconciliation

Pinned common-ancestor analysis; field and object changes including
delete-versus-edit and reference conflicts; preview of local tip, remote tip
and base; user-approved fast-forward or manual three-way resolution with
whole-model validation; a two-parent merge revision in the private repository,
published only through VC-07. Rebase/replay is a later, separate decision.
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
   parents and invalid references all fail without moving a HEAD.
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
    three-way resolution and whole-model validation are required.

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

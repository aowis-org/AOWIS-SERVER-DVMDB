# AOWIS / DVMDB version-control contract

**The normative specification.** Native storage, the service, REST,
replication and the GUI must all follow it. It defines behaviour, not what is
built: the [roadmap](roadmap.md#c-status) says which parts exist. The same
model is elaborated in [architecture](architecture.md),
[revision history](native-revision-history.md),
[branch merging](native-branch-merging.md),
[private working repositories](private-working-repositories.md) and
[GUI integration](gui-integration-requirements.md).

## 1. Concepts and terminology

| Concept | Meaning | UUIDv7 | Mutability |
|---|---|---|---|
| Database/source | Server or embedded store hosting independent projects; a GUI connection may represent it | Connection/profile identity where applicable | Connection details, catalogue |
| Project | Container for one model history, branches, snapshots, and permissions | Project ID | Metadata and refs, not committed content |
| Branch | Persistent, named line of development with a movable pointer to the latest committed revision | Branch ID | Name/HEAD can change; creation provenance cannot |
| Revision / commit | One **finalized immutable** whole-model snapshot, with ordered revision parents | Revision ID | **Never** |
| Branch HEAD | The **latest committed revision referenced by that branch**; nullable for an unborn branch | The referenced revision's ID, not another object | Ref moves atomically |
| Working repository | One user's private durable DVMDB storing local committed history, drafts and remote-tracking refs | Repository ID / configured location | Its local refs and drafts can change |
| Working copy | A **private, mutable editor state** in the user's working repository, based on a committed revision (or an initial empty project) | Working-copy ID | Editable by its owner/session |
| Base revision | Exact committed revision on which a working copy's current edits were based; nullable for an unborn branch | Revision ID or null | Pinned until explicit reconcile/rebase |
| Fork point | Exact immutable source revision from which a new branch was created | Revision ID or null | Immutable creation metadata |
| Record hash | SHA-256 commitment to a canonical immutable revision record | Not an identity | Never changes for a given revision |
| Snapshot root | Content-addressed Merkle root recursively committing to all persisted model records, references and index roots | Not an identity | Immutable for committed revisions |

**DVMDB distinguishes the committed branch HEAD reference from the GUI HEAD
card.** In native storage, `headRevisionId` is the branch's latest immutable
revision or `null` when unborn. A private draft is mutable and is not a
revision. In the GUI, the **HEAD card** represents the editable working tip
of a particular branch and can exist before the first commit. The card is a
UI/workflow concept, **not an extra committed revision**. There is no
server-wide checked-out branch or globally shared mutable model.

A branch's history consists of the committed revisions reachable from its
HEAD, including shared ancestors and both parents of merge commits. This
is the meaning of *a branch is made of revisions*: the branch identifies a
line of development, but **does not exclusively own** any immutable ancestor.
Two branches can legitimately reference the same revision as HEAD; their
identities and working copies remain independent. A branch HEAD does not
need to be a leaf in the project-wide ancestry graph.

## 2. Identity and integrity

- Give every first-class project, branch, committed revision and working copy
  its own **UUIDv7**. Model entities likewise use their model-defined UUIDv7
  identities, never a SHA-256 hash as a logical object ID. Names are
  user-visible labels, never storage keys.
- Generate fresh IDs for new objects; reject accidental ID collisions. Keep
  identities stable across rename, export/import, server transport and
  repository reopening. UUIDv7 ordering is **not** commit ancestry.
- Compute SHA-256 over a **canonical, versioned, immutable revision record**
  that binds revision UUID, project UUID, ordered parent revision UUIDs with
  each parent's record hash, the snapshot root, the reference-index root and
  the commit metadata (author UUIDv7 and name, message, commit time), so
  changing an ancestor or a commit's description cannot pass verification
  unnoticed ([record layout](storage-formats.md#revision-record-t-3)).
- Every revision has an **author** (a UUIDv7 and a display name), a non-empty
  **message** and the **time** the repository published it. Every operation
  that creates a revision — commit, merge, hydraulic save or edit, local
  Commit — supplies author and message; merges and the empty first revision of
  a non-unborn project get a default message. The author is what the client
  states: until there is authentication it is not verified.
- The snapshot's Merkle roots recursively bind **all** committed hydraulic
  and model objects; the complete model need not be copied into one flat hash
  input for each commit. Shared immutable subtrees may be deduplicated.
- A later edit to a revision's **display message** is a separate mutable
  annotation; the original hashed commit message remains in the immutable
  revision record. The UI must not claim the historical record was rewritten.
- Working copies may have transient checksums, but do not receive a committed
  `recordHash` and cannot be addressed as finalized revisions.
- Verify canonical hashes, ancestry closure, project ownership, parent
  ordering, UUID uniqueness, and domain references before accepting or
  publishing untrusted history. SHA-256 is integrity, **not authentication**.

## 3. Creating a project: unborn `main`

`CreateProject(name)` must atomically create a new project UUIDv7 and a new
branch UUIDv7 named `main`, with:

- `main.headRevisionId = null` (an **unborn** branch);
- no committed revisions, no empty synthetic revision R0;
- an empty, editable model state that an editor can check out into its **own
  working copy**; no globally shared mutable HEAD;
- no creation-origin branch or fork revision for `main`.

The project and empty branch can be displayed immediately; the history area
contains **zero immutable revision cards** and the GUI shows an unborn
**HEAD card** for `main`. Starting an editor session makes (or opens) a
private working copy, not a commit. The first successful commit creates
an immutable root revision (zero parents) and atomically moves the branch's
committed HEAD from null to that revision.

## 4. Branch lifecycle and provenance

Creating a branch **from an already committed revision** never creates a
commit and never mutates the source branch. Supply its exact `sourceRevisionId`
and, if the user selected an originating branch, `sourceBranchId`.

DVMDB must persist both as immutable creation metadata:

- `originBranchId`: source branch UUIDv7, when known;
- `forkRevisionId`: exact finalized revision UUIDv7 at creation, when one
  exists (not the source branch's current or future HEAD).

For a new branch from committed Rn:

1. Verify that Rn exists in the same project, is finalized, and is reachable
   from the specified source branch when that source is supplied.
2. Create a new branch UUIDv7, with its own editable-name label.
3. Set its committed HEAD to Rn and persist both creation-origin fields.
4. Return the new branch UUID and exact Rn. **No new revision is created.**

Creating a branch from **a user's uncommitted edits** is a different request.
It must explicitly offer to **commit those edits first** (subject to the
ordinary concurrency check) or start from the last committed base without
the edits. Never silently commit someone else's working copy. A different operation, **create independent unborn branch within a project**,
requires only the project identity and a new unique name, not an existing
revision or source branch. Multiple independent unborn branches are valid,
even after another branch has commits. Branching **from uncommitted content**
still requires first committing that content; do not invent a fork revision.

Branch metadata also contains editable tags and description, separate from
its stable UUID and immutable creation provenance. This metadata does not
participate in the canonical revision hash.

Branches support readable UTF-8 names including spaces. Validate UTF-8,
length and prohibited control/path-delimiter characters; names must be unique
under the repository's documented comparison rule. Rename retains UUIDv7,
HEAD and creation provenance. If the source branch is later renamed, its
origin UUID remains stable. If source branches are removed, do not fabricate
new provenance or erase a valid recorded fork point.

## 5. Personal repositories, working copies and concurrency

Every editor works through a **personal durable DVMDB working repository**.
Standalone uses its configured built-in native DVMDB. Basic desktop and
web/browser GUIs use an explicitly configured **private HTTP DVMDB**.
The personal repository stores local branches, immutable local commits,
remote-tracking refs and one or more separate mutable UUIDv7 working copies.
A working copy is not a repository and is not a committed revision.

The **shared project repository** remains authoritative for published
branches and their committed HEADs. A personal local branch HEAD may advance
without advancing the corresponding shared branch HEAD. Remote-tracking
state identifies the shared source/repository, project and branch UUIDs plus
its last fetched HEAD. A logical branch keeps its UUIDv7 when replicated to
another repository, while each repository's HEAD is an independently mutable
ref addressed by `(repositoryId, projectId, branchId)`. A newly created
logical branch gets a fresh UUIDv7, reused on its first explicit remote
provisioning. Distinct local and remote **ref states** must not be confused.
Private working repositories must not be silently substituted with a shared
source, and their absence disables editing rather than risking data loss.
See [private working repositories](private-working-repositories.md) for the
full deployment, tracking and recovery model.

Each working copy has its own UUIDv7, owner/session, project UUID, local
branch UUID, pinned committed base UUID/null, durable model changes and
validation/dirty status. Alice and Bob may edit from the same shared R7
in separate personal repositories; multiple sessions of one user should
also use distinct drafts by default. Reopening the same draft requires an
exclusive expiring writer lease, a fencing token and an optimistic saved-state
generation on every write. Read-only sessions may share the draft; stale
writers cannot overwrite a recovered draft. No working copy can mutate a committed
revision or either local/shared HEAD as a side effect of ordinary Save.

## 6. Local Commit and shared Publish (optimistic concurrency)

**Save / Autosave** durably updates the selected personal working copy;
it does **not** create a revision or advance a branch HEAD.

**Commit** validates the complete candidate hydraulic model, creates one
new immutable UUIDv7 revision and SHA-256 record in the personal DVMDB,
and atomically advances only its **local** target branch when
`expectedLocalHeadRevisionId` exactly matches (explicit null for unborn).
Local commitment and HEAD update are one logical transaction, with
idempotency and authoritative status recovery after ambiguous timeouts.
A concurrent local edit to the same branch or a stale local draft must not
silently overwrite an existing local commit.

**Publish / Push** sends missing verified immutable objects from the personal
DVMDB to the **shared** DVMDB and conditionally advances the shared branch
from an exact `expectedRemoteHeadRevisionId` (including null for the first
publication). The shared server verifies identities, ancestry, hashes,
model semantics and fast-forward/merge rules before durable publication.
The remote update is atomic and idempotent; a rejected or timed-out publish
**never erases the already successful local commit**. Query the shared
operation status before retrying an ambiguous response. Every Commit and
Publish carries a UUIDv7 operation ID and immutable payload digest; a replay
with the same ID and payload yields the recorded result, not another commit
or remote advancement. A changed payload under the same ID is invalid.

**Fetch** only adds/validates immutable history and updates remote-tracking
refs; it never overwrites private working copies or silently resets local
branch HEADs. On shared HEAD advance, use an explicit compare/reconcile or
explicit fast-forward or three-way merge operation. Overlapping entity
edits and non-overlapping but hydraulically incompatible changes both require
resolution/validation. The initial implementation does not automatically
rebase/replay, force-push, perform octopus merges or silently choose a winner.

Example:

```text
Shared main HEAD R7; Alice and Bob each have personal DVMDBs at R7.
Alice saves draft A, commits locally R8 (shared HEAD still R7).
Bob saves draft B, commits locally B8 (shared HEAD still R7).
Alice publishes R8 with expected remote HEAD R7 -> shared HEAD R8.
Bob publishes B8 with expected remote HEAD R7 -> STALE/REJECTED.
Bob still owns B8 in his personal repository. He fetches R8,
reconciles and validates, then publishes a new accepted successor.
```

***Commit and Publish are distinct user actions and API operations.***
Their exact wire schemas, local tracking-reference behavior and any
future optional combined command must preserve these invariants.

## 7. Revision DAG and merging

The **only** immutable ancestry edges connect committed revisions. Each
revision has zero, one or two ordered parents. Parent UUIDs must refer to
committed revisions in the same project; the graph is acyclic and complete.
One-parent commits advance a development line. Two-parent merge commits bind
both histories; target and source parents have an explicit canonical order.

Branch creation is **not** a revision and does not create a revision-DAG
edge. A committed fork becomes visible as a divergence **only when the
branches make separate commits**. Example:

```text
            R0
             |
            R1  (main and feature may both point here)
           /  \
          R2   F2
          |    |
     main HEAD  feature HEAD
```

The graph need not render a separate edge for `originBranchId` or
`forkRevisionId`. These are provenance metadata, shown in the branch
inspector. Use Git-style revision-ancestry graph lanes as a **layout of one commit DAG**.
In the GUI, branch names appear on distinct writable HEAD cards and may
also annotate immutable revision cards. These HEAD cards are **not** extra
immutable ancestry vertices. Where many branches share a committed revision,
keep one revision UUID/card and show the real references, never duplicate it.

Merge must compare both source and target committed HEADs against expected
values before publishing. An up-to-date merge is a no-op; fast-forward may
advance the target ref to an already committed descendant; a divergent merge
creates **one** new immutable two-parent revision only after validation.
Where editors have uncommitted changes, merging must not silently overwrite
or consume their private working copies. Source-branch HEAD remains unchanged
unless an explicit separate operation requests otherwise.

## 8. Reading history and graph presentation

- Native and REST readers expose **the same** project/branch/revision UUIDv7
  contract. No SHA digest is used as an entity ID.
- History traversal includes every branch HEAD and both parents of merge
  revisions; graph pagination must pin a stable branch-HEAD snapshot and
  validate closure before presenting a complete graph.
- The GUI shows server/project organization, **one committed revision DAG**
  and a distinct **writable HEAD card per branch**. A HEAD card identifies
  private working state but is **not** a committed revision or ancestry edge.
- An unborn project displays its initial `main` HEAD card and a clear
  **No commits yet** state, without a synthetic revision. Multiple independent
  unborn branches are valid and each gets its own HEAD card.
- A branch whose HEAD is also another branch's HEAD is legitimate. A HEAD
  may be an ancestor of commits on another branch. Do not assume global DAG
  leaves alone carry HEAD labels.
- No arbitrary dashed branch-origin connection is required in the revision
  graph; provenance belongs in the branch inspector. The graph's history
  edges must mean revision parenthood only. Display links between GUI source,
  project and branch containers are organization, not ancestry.
- UI selection, source filtering, folding, camera movement and inspector tab
  changes are read-only operations; they never change HEAD or working copies.

## 9. API, replication, atomicity and durability requirements

The API contract must make these distinct operations explicit:

- create project -> project UUID, unborn `main` branch UUID, null HEAD;
- list/get branches -> UUID, name, nullable HEAD, `originBranchId`,
  `forkRevisionId`;
- checkout/start working copy -> UUID, ownership, exact base revision/null;
- read immutable revision/snapshot -> UUID, SHA-256 record/snapshot roots,
  ordered parents with their record hashes, author, message and commit time;
- create branch from **committed** revision, with optional source branch UUID;
- save drafts and commit locally in the personal repository with exact
  expected **local** HEAD and idempotent result;
- fetch immutable objects and remote-tracking HEADs;
- publish/push committed history using exact expected **shared** HEAD and
  idempotent result;
- get paginated full revision DAG and branch HEADs under a pinned snapshot;
- compare, reconcile and merge with explicit expected HEADs.

REST errors must distinguish invalid input, missing objects, permission
failure, stale expected HEAD (`409 Conflict`), semantic validation failure,
and transient service failure. An absent expected HEAD is **not** equivalent
to an explicit null expected HEAD on an unborn branch.

Offline/full/incremental replication must preserve project/branch/revision
UUIDs, immutable branch-origin UUIDs and fork UUIDs, all committed parent
IDs, all integrity hashes and exact branch HEAD states (including unborn).
Never silently rewrite UUIDs or fabricate an ancestry edge to resolve a
replication conflict. Imported branch HEAD publication must obey the same
atomic durability and concurrency rules as a conditional Publish operation.
Copies of a private uncommitted working state are **not** committed history
and require
a distinct explicitly authorized backup/synchronization protocol.

## 10. Required acceptance scenarios

1. New project: UUIDv7 project and `main` exist, `main.head = null`, history
   has zero revisions; checkout has private mutable data.
2. First local commit creates immutable UUIDv7 R0 in the personal repository;
   publishing conditionally advances only the selected shared branch.
3. Creating `feature` from committed R0 creates a distinct branch UUIDv7,
   records source branch and exact R0, does not add a revision.
4. A project may create another **independent unborn branch** immediately,
   with no source revision; branching **from the contents of an uncommitted
   HEAD**, by contrast, requires commit-first or exclude-edits choice.
5. Alice and Bob have separate personal repositories, each with a draft and
   a local commit based on shared R7; Alice's conditional Publish succeeds,
   Bob's stale Publish is rejected without deleting B8 or his drafts.
6. Explicit reconciliation incorporates Alice's history, validates the full
   hydraulic model and permits a later conditional Publish.
7. Shared R0 is one UUIDv7 immutable revision card; distinct branch HEAD
   cards connect to it without duplicating R0 or inventing revision ancestry.
8. Rename, service restart and bundle import/export preserve branch UUID,
   origin UUID, fork UUID and complete committed ancestry.
9. Native and REST preserve UUIDv7 identities, SHA hashes, nullable unborn
   HEADs, pagination and local-versus-remote conflict behavior.
10. Failed, cancelled or ambiguous Save, Commit or Publish operations never
    move the wrong branch HEAD or silently discard local committed/draft work.

## 11. Approved delivery defaults and implementation parameters

The architectural and delivery defaults are **approved**, not open questions.
The definitive DG-01–DG-06 rules are in the
[implementation roadmap](roadmap.md), and the persistent-working-copy design
is in [private working repositories](private-working-repositories.md):

- One logical branch UUIDv7 across repositories; separate repository-qualified
  mutable HEAD states and explicit remote-tracking records.
- A private durable working copy per editor session by default; reopening one
  draft uses exclusive writer lease, fencing and version-checked durable Save.
- UUIDv7 idempotency keys, payload digest and recoverable authoritative
  pending/success/rejection outcomes for local Commit and remote Publish.
- The first release uses one coherent format. Obsolete experimental layouts
  are not supported and need no migration. Invalid data is rejected, not
  reinterpreted; immutable commits within a valid repository stay immutable.
- Initially, Fetch, fast-forward and explicit hydraulic-validated three-way
  merge. No automatic rebase/force-push/silent merge.
- New local branches retain their UUIDv7 at first explicit remote publication;
  a missing remote branch and a null-HEAD remote branch are different cases.

Still to define in their milestones are *engineering parameters*: draft
quotas and retention, operation-status retention and the merge-conflict
report. Lease duration (1–120 s), fencing tokens (UUIDv7) and the draft and
ledger formats are already fixed ([working copies](working-copies.md),
[storage formats](storage-formats.md)). Git-style lane placement is GUI layout
work, not a change to ancestry.

Authentication, authorization, audit and permissions are outside the
version-control implementation roadmap. The independent
[deferred security backlog](security-permissions-design.md) records future
ideas but has no VC milestone. The current functional model must not be
mistaken for an authorization system; unauthenticated writable HTTP editing
should not be exposed to untrusted users.

**This contract overrides convenience assumptions in any API or GUI design.**
A feature is complete only when storage, service, replication and GUI
behavior agree with these invariants.

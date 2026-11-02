# Private working repositories

Every editor works through a **personal, durable DVMDB repository** that
holds its local history and its uncommitted drafts. Published history lives in
**shared** repositories. This page specifies how the two relate: deployment,
clone and checkout, local Commit, Fetch and Publish. The underlying rules are
in the [version-control contract](version-control-semantics.md); what is built
is marked below, and the remaining work is in the [roadmap](roadmap.md).

## Two kinds of repository

| | Personal (private) repository | Shared repository |
|---|---|---|
| Belongs to | one intended user, any number of that user's sessions | a team or project |
| Authoritative for | unpublished local commits, drafts | published branch HEADs |
| Holds | local branches, local history, drafts, remote-tracking state | published history |
| Role (`repository.tracking`) | `private` | `shared` |

The role is set once per storage root ([repository tracking](repository-tracking.md)).
Drafts can only be created in a `private` root. A role is deployment intent,
not access control.

| Client | Personal repository | Shared repositories |
|---|---|---|
| Standalone desktop | its embedded DVMDB at a configured durable location | optional, local or HTTP |
| Desktop GUI without an embedded DVMDB | an explicitly configured private DVMDB over HTTP | configured HTTP sources |
| Browser GUI | an explicitly configured private DVMDB over HTTP | configured HTTP sources |

* A client never edits in a shared repository and never falls back to one
  when its personal repository is unreachable. Without a reachable personal
  repository the GUI may show published history read-only, but must not
  offer editing.
* Browser memory or storage is UI state, not a working repository. Offline
  editing on an HTTP-only client is not promised.
* Several sessions of one user may connect to the same personal repository.
  Each works in its own draft by default; reopening a draft gives one writer
  at a time ([leases](working-copies.md#leases-fencing-tokens-and-generations)).

## Identity across repositories

* Each storage root has its own repository UUIDv7.
* A project and its branches keep the **same** UUIDv7 in every repository
  that holds them; revision UUIDs and hashes are unchanged by any transfer.
* Each repository moves **its own** HEAD for every branch. The full key of a
  ref is `(repositoryId, projectId, branchId)`, so Alice's local `main`, Bob's
  local `main` and the shared `main` can all differ.
* A branch created locally gets a new UUIDv7 and keeps it when it is first
  provisioned on a remote.
* A remote branch that is **absent** and one that exists but is **unborn**
  (null HEAD) are different states, everywhere: in tracking records, in API
  answers and in Publish preconditions.
* Correspondence is never inferred from a name. Wrong repository, wrong
  project and UUID collisions are rejected.

## The workflow

1. **Clone / checkout.** Bring the project's committed history into the
   personal repository and track the shared branch. *Built:* full and
   incremental bundle transfer (`GET`/`POST …/bundle`), identity-preserving
   provisioning of the project (`POST /projects/clone-target`) and of an
   unborn root branch (`POST /projects/{id}/unborn-checkout`, below).
2. **Start a draft** at the local branch HEAD (or null for an unborn branch).
   *Built* ([working copies](working-copies.md)).
3. **Save / autosave** into the draft in the personal repository. It never
   creates a revision, moves a HEAD or touches the shared repository. *Built.*
4. **Commit locally.** Verify durable record/revision integrity, but do
   **not** require hydraulic validity; create one immutable revision in the
   personal repository and move only the **local** branch from the exact
   expected local HEAD (null for the first commit). A failure keeps the
   draft. *Built as native and HTTP APIs; invalid-model support and full
   recovery acceptance remain open.*
5. **Fetch.** Add the shared repository's missing immutable history and
   update remote-tracking records. Never touches drafts or local HEADs.
   *Not built* (tracking records exist; recording them is native-only).
6. **Publish.** Send the missing immutable objects, then move the **shared**
   branch from an exact expected remote HEAD (null for its first
   publication), fast-forward only. A stale expectation is a conflict; the
   local commits and drafts stay. A new local branch is first provisioned on
   the remote with its UUIDv7, as a separate idempotent step. *Not built.*
7. **Reconcile.** Incorporate new remote commits by explicit fast-forward or
   an explicitly resolved three-way merge (hydraulic diagnostics are optional).
   No automatic rebase, force push or silent
   winner; an unresolved merge stays in a draft. *Merge primitives built;
   the Fetch-based workflow is not.*

```text
Shared main at R7; Alice and Bob each cloned R7.
Alice commits R8 locally; Bob commits B8 locally.  Shared main is still R7.
Alice publishes R8 expecting R7  -> shared main = R8.
Bob publishes B8 expecting R7    -> conflict. B8 and Bob's drafts stay.
Bob fetches R8, merges with B8 into M9, publishes M9 expecting R8.
```

The first commit of a project needs the local unborn HEAD to be null; its
first publication needs the shared HEAD to be still null. To fork *from uncommitted content* on an unborn branch, make the first
commit first. This does **not** prohibit creating a different independent
unborn branch directly within the project.

## Operation IDs and recovery

Local Commit and Publish are separate transactions, each with a
caller-chosen UUIDv7 operation ID, a request digest and a durable `pending`,
`succeeded` or `rejected` outcome:

* replaying the same ID and request returns the recorded outcome and never
  creates a second revision or remote advance;
* the same ID with a different request is rejected;
* after a timeout the client asks for the outcome under the **same** ID and
  never resends the write under a new one; a timed-out Commit may exist, a
  timed-out Publish may have moved the shared HEAD;
* unresolved operations are never expired.

The local Commit ledger implements this today
([local Commit](working-copies.md#local-commit)); Publish will follow the same
rules.

## Checkout of an unborn branch

An unborn branch has a UUIDv7 but no revision, so it cannot travel in a
bundle. To check one out over HTTP, the client:

1. reads the source repository's ID and the branch by UUIDv7, and requires a
   null HEAD and no fork provenance;
2. provisions the project with its UUIDv7 in the private repository:
   `POST /dvmdb/v1/projects/clone-target` (`projectId`, `name`; idempotent);
3. creates the branch with its UUIDv7:
   `POST /dvmdb/v1/projects/{projectId}/unborn-checkout` with
   `{"branchId": "<uuidv7>", "name": "main"}`. This requires the `private`
   role and a project without conflicting branches; an identical repeat
   returns the same branch. No record or revision is written;
4. creates a draft with `baseRevision: null` (`POST /dvmdb/v1/working-copies`).

Only a root unborn branch is copied this way; a branch with fork provenance is
never flattened into an independent root.

## Acceptance scenarios

1. Two users clone a project at R7 into separate personal repositories and
   save drafts; shared `main` does not change.
2. Alice commits R8 locally; shared `main` stays at R7 until she publishes.
3. Bob's stale Publish from R7 is rejected; B8 and his drafts survive, and
   after Fetch and an explicitly resolved merge his Publish succeeds.
4. A restarted Standalone application or a reconnecting HTTP GUI finds the
   same draft and pinned base in the same personal repository.
5. Without a reachable personal repository editing is disabled; published
   history is still readable if its source is reachable.
6. Project, branch and revision UUIDs and hashes mean the same in every
   repository while the HEADs advance independently.
7. A Commit or Publish whose response was lost is resolved by its operation
   ID: one result, no duplicate.

**Security:** choosing a private HTTP repository in a configuration is not
access control. Until there is a [security design](security-permissions-design.md),
isolate personal repositories by deployment (localhost, private network,
operating-system permissions).

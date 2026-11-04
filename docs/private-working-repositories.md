# Private and shared DVMDB repositories — current behavior

A DVMDB storage root has a stable UUIDv7 and a one-time `private`, `shared`
or as-yet-unconfigured role. A private repository holds editable drafts,
local commits and optional remote tracking. A shared repository holds
published history. **Role is deployment intent, not permission enforcement.**

| Concept | Private/local | Shared/remote |
|---|---|---|
| Committed HEAD | Local branch ref; can advance through local Commit | Independent branch ref; does not move on local Commit |
| Drafts | Allowed in `private` repository | Editor cannot write a shared source as its personal draft |
| IDs | Project/branch/revision UUIDs preserved from verified sources | Same logical IDs, independent ref state |
| Tracking | Repository and observed remote branch data supported in native store | Remote branch HEAD observation, not proof of publication |

Desktop GUI chooses one verified personal HTTP source in the Database
Inspector; the chosen server has a crown and gold graph card. Standalone
embeds native DVMDB at a persistent configured location behind a dynamic
localhost HTTP endpoint. Browser clients require a reachable private HTTP
DVMDB; browser state is not a replacement database. When the personal
source is absent, no unrelated shared source may be used for writable edits.

## What is implemented

- A selected remote branch can be brought into the private repository via
  verified revision bundles, identity-preserving project provisioning and
  native/HTTP unborn-branch provisioning as appropriate.
- Private working copies can be created, discovered in UUID-ordered pages,
  leased, saved using canonical MODEL events, reopened, discarded and locally
  committed. The saved draft can contain an unborn network without an
  immutable revision.
- A Commit creates an immutable local revision, moves **only that private
  branch HEAD**, rebases its draft and can be reconciled by operation UUIDv7
  after uncertain acknowledgement. A changed shared HEAD is not silently
  overwritten by a local Commit.
- The repository tracks configured remotes and can record observations
  using native tracking APIs. Verified full/incremental bundles exist over
  CLI, native and HTTP paths.

## What does not follow from those APIs

A fully integrated **whole-project Clone + Fetch**, automatic tracking
refresh, **Publish**, and GUI reconcile/merge sequence do not yet exist.
Bundle transfer alone does not Publish an updated shared branch HEAD.
First provision of a new branch on another repository must keep its stable
UUID; an **absent** branch is not a **present unborn** branch. The future
Publish workflow must be conditional on exact expected remote HEAD, have
a durable operation ledger/status and preserve local work on conflicts.
These are documented in [remaining work](roadmap.md).

## Draft concurrency and history

One writer session at a time holds a leased/fenced token for a particular
draft. Private draft events are ordered and durable, with generation CAS.
The active event journal is **cleared after local Commit**, but its accepted
MODEL frames are retained in a **permanent, revision-indexed local-Commit
action archive**. Paged read-only history includes real action UUIDs and
checkpoint-only revisions. A native selector can atomically install a
verified archived action boundary in an empty-journal private draft; it is
**not** yet a continuous Undo/Redo timeline for active draft edits. See
[current History](commit-action-archive.md) and
[remaining History work](history-roadmap.md).

No current DVMDB role, lease or repository ID authenticates a caller. Isolate
writable endpoints using deployment boundaries until a separate security
project provides authentication and authorization.

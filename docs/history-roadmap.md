# Durable action history across Commit boundaries — DVMDB roadmap

**Current foundation:** DVMDB stores verified MODEL events in the active
private draft and retains those events after local Commit in a durable action
archive. The read-only `branchActionHistory` API and its HTTP route now return
first-parent Commit markers, real archived action UUIDs and optionally paged
active-draft action UUIDs. All requests are read-only: there is **no durable
logical cursor, inverse application, Undo/Redo command or jump-to-state API**.
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

### HDB-01 — Persistent indexing and full action retrieval (remaining)

The immutable archive and basic read-only timeline exist. Remaining work:
replace the bounded 10,000-record ledger scan with a durable, recoverable
revision-to-archive index; add paginated retrieval of full archived frames
and display-ready MODEL action summaries; provide server-side snapshot
consistency across draft pages and concurrent Commit; and qualify import,
merge and cross-repository checkpoints. Ensure compaction/recovery preserves
exact archive identities and never fabricates intermediate edit steps for
checkpoint-only revisions.

### HDB-02 — Reconstruction and navigation API

Build on the existing ordered/paged read-only checkpoint and action-UUID
listing to add a durable state cursor, display-ready summaries, exact action
frame retrieval and an exact "materialize state" plan. Reconstruct from the nearest
immutable model checkpoint plus forward/inverse MODEL transactions, verify
canonical before/after images and membership, and never silently repair
missing objects. Do not allow jumps to invent actions between two old
revisions for which only final snapshots remain. For merge commits, expose
the first-parent narrative by default and metadata for the other parent(s).

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

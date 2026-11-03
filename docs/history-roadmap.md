# Durable action history across Commit boundaries — DVMDB roadmap

**HDB-00 contract specified; HDB-01 action archive first slice implemented.**
See [history-contract.md](history-contract.md) for authoritative identities,
cross-Commit cursor semantics, durability, future wire endpoints and
acceptance gates. DVMDB currently has immutable revisions with graph
history and a bounded journal of MODEL events per active draft. The HIST-02
local Commit ledger now **archives acknowledged MODEL frames before immutable
Commit publication** and retains them after draft rebase. That is still
**not** a navigable Undo/Redo history: there is no branch-timeline index,
server-owned cursor, navigation command, or paged HTTP history API.
See [commit action archive](commit-action-archive.md) for the implemented
contract and its current limits. For the intended user-facing left dock, see the
[GUI History roadmap](../../AOWIS-SERVER-GUI/docs/history-roadmap.md).

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
- `revision-graph`, revision records and branch histories expose immutable
  checkpoints, but no permanent per-action record for old commits.

## Required implementation

### HDB-00 — Versioned action-history contract (specified)

The [History contract](history-contract.md) defines history scope, stable
UUIDv7 action/group and mutation IDs, retained MODEL v1 frames, audit versus
logical cursor, persisted checkpoint records, first-parent branch narrative,
snapshot-only gaps, cross-Commit safety, and a versioned proposed transport.
This is **not** a published or implemented history API. Subsequent stages
must implement these contracts and verify them with real storage tests.

### HDB-01 — Permanent archive implemented; indexing and pagination open

The **first slice** is complete: `AOWIS-LOCAL-COMMIT-3` freezes accepted MODEL
frames in the fsynced Commit intent before any immutable revision is
published, verifies frame and chained digests, and exposes an operation-ID
read API after success. Native restart, retry, pending-outcome recovery and
corruption tests exercise this archive. The stable revision UUID is stored in
that same record. Branch/revision indexing, paged retrieval, non-local
publication checkpoints and history-specific retention are not yet built.

Persist accepted action frames and related metadata **beyond** draft rebase
without mutating the immutable revision format unnecessarily. Bind Commit
markers to exact revisions, parent edge(s), action range and checkpoint.
Keep the current active-draft journal bounded for Save/recovery; archive
longer history under explicit retention, hash/integrity and pagination rules.
Reopen and compaction must preserve navigable history. Older revisions that
lack archived events remain viewable **checkpoints**, not fake recoverable
individual steps.

### HDB-02 — Reconstruction and navigation API

Support ordered/paged read of action summaries, state cursor, checkpoint
markers and an exact "materialize state" plan. Reconstruct from the nearest
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

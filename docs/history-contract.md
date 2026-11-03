# Persistent cross-Commit History contract (planned behavior)

This document defines **future** Undo/Redo semantics, not functionality
currently exposed by the DVMDB API. The implemented backend currently offers
read-only Commit checkpoints, retained action UUIDs and draft journal pages.

## Identity and scope

A History timeline belongs to a selected private working copy and follows
its selected branch's first-parent revision ancestry. Immutable MODEL action
UUIDv7 identities and their before/after bytes are retained verbatim;
independent Undo/Redo operations have fresh operation UUIDv7 identities and
refer to their original action. New actions after Undo invalidate ordinary
Redo eligibility without deleting the immutable action audit trail.

Merge commits are first-parent checkpoints by default. Other parents remain
visible through the revision graph, not fabricated into a single linear
sequence. Commits imported or created without action frames appear as
checkpoint-only states; the server cannot invent missing intermediate steps.

## Navigation

Undo moves to the previous meaningful model state; Redo moves forward. Commit
markers are visible states, **not Undo barriers or separate Undo steps**.
Clicking a Commit marker requests its exact immutable snapshot as the desired
*private working-copy content*. It never moves an immutable branch HEAD,
changes committed revision ancestry, or rewrites earlier revisions. A new
Commit after moving backward is a successor of the branch's *current* HEAD;
creating a branch whose ancestry originates at an older revision is a separate
operation.

Each navigation must be an atomic, fenced, idempotent MODEL mutation tied to
the expected repository, draft, generation and branch HEAD. The cursor and
corresponding model change must be durably reconciled together. A dropped
response does not permit a second, differently identified Undo/Redo. If
navigation encounters stale images, a conflicting writer or a missing
referenced entity, it fails without partial application. Hydraulic solver
feasibility itself is never an Undo/Redo precondition.

## Checkpoint state reconstruction (native read-only foundation)

`ProjectService::planHistoryCheckpoint(draftId, revision, expectedGeneration)`
returns the exact candidate overlay needed to represent an immutable
first-parent checkpoint *relative to the draft's currently committed HEAD*.
It rejects stale HEADs and draft generations, and cannot be used as an
unfenced edit. Its output is a **full replacement overlay**, not an
incremental patch suitable for `saveWorkingCopyChanges()`. This **planning** API does not
perform Undo/Redo, write cursor state, or synthesize missing intermediate
actions. Pre-first-Commit unborn states and individual action targets are
separate future reconstruction paths. The eventual navigation mutation must
recompute or validate the plan and atomically persist both candidate and
logical cursor under the same writer/HEAD checks.

## Native exact-checkpoint selection (HIST-03b)

`ProjectService::navigateHistoryCheckpoint()` applies the HIST-03a
replacement overlay in one atomic private-draft metadata publication, with
an exact checkpoint marker, operation UUIDv7, and original generation. It
pins the current lease and branch HEAD and is idempotent for identical
retries across a restart. It never edits any immutable revision. This initial
native facility accepts only a draft with no MODEL action journal, because
no permanent record for the navigation event has yet been defined. A later
normal Save or Commit clears the checkpoint marker. This is **not** a full
history cursor with redo eligibility or cross-Commit step navigation; it is
not yet exposed as an HTTP command or connected to the GUI History dock.

## Retention, restart and UI

The durable action archive survives a Commit; the bounded working-copy event
journal may be cleared after an archived Commit without losing original
actions. The logical History cursor must survive GUI and service restarts.
The left GUI History dock shows the selected branch, Commit markers, archived
and pending actions, and the current-state marker. Undo/Redo and jump
controls stay disabled until mutation/cursor/recovery APIs are implemented.

The first navigation implementation must prove canonical equality after
Undo→Redo and multi-step jumps, correct redo invalidation, first-parent
checkpoints, ambiguous response recovery and no mutation of immutable
revisions. Future retention/pruning must be explicit and must not silently
replace missing actions with guessed edits.

See [current archive behavior](commit-action-archive.md) and the
[remaining implementation roadmap](history-roadmap.md).

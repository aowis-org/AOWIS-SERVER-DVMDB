# History contract — current capabilities and intended Undo/Redo behavior

History has two distinct layers. **Revision history** is an immutable DAG
of model snapshots with branch refs. **Edit history** is an ordered record of
MODEL actions performed in a private draft and archived by a successful
local Commit. Commit markers connect the two layers; they are **not Undo
barriers** or extra synthetic edit steps.

For the actual API and supported targets, see
[committed action archives](commit-action-archive.md). The missing features
are listed only in the [History roadmap](history-roadmap.md).

## Identity and timeline

- A selected private working copy has a repository UUIDv7, project UUIDv7,
  branch UUIDv7, draft UUIDv7, pinned committed HEAD/null and monotonically
  increasing draft generation. Each accepted MODEL transaction has its own
  UUIDv7 and original canonical before/after images.
- The branch's timeline uses **first-parent** committed ancestry. An action
  archive is linked to its immutable revision and original local-Commit
  operation UUIDv7. Merge second parents remain part of the revision DAG,
  not invented linear edit steps.
- A revision without retained original actions is a **checkpoint-only** entry.
  Incomplete, mismatched or corrupt archives cannot be treated as verified
  intermediate steps. Imported and earlier revisions are not retroactively
  assigned fictitious transactions.
- Selecting a past action boundary changes only **private draft content**.
  It does not rewrite a revision, its SHA-256, ordered parents or the committed
  branch HEAD. Committing subsequently creates a new revision after the
  branch's **current committed** HEAD. Creating a branch from an older
  revision is a separate ancestry operation.

## What native DVMDB supports now

| Operation | Behavior and boundary |
|---|---|
| Read checkpoints/actions | Native `branchActionHistory`, `revisionActionPage`, HTTP first-parent history and paged original frames |
| Preview exact committed checkpoint | `planHistoryCheckpoint` computes a full overlay against the current committed HEAD; read-only |
| Select committed checkpoint | `navigateHistoryCheckpoint` atomically replaces the private candidate and stores an exact checkpoint marker; refuses an active MODEL journal |
| Preview action frames | `readHistoryReverseActionWindow` returns original frames newest-first, not executable inverse records |
| Verify one hydraulic action sequence | `previewVerifiedHydraulicActionBoundary` replays ordered insert/delete/update/root operations and checks byte-equal committed network |
| Verify an entire revision | `verifiedRevisionNativeActionWitness` plus `verifyHydraulicRevisionActionCoverage` account for every changed native record across affected networks |
| Plan archived action boundary | `planVerifiedHydraulicActionNavigation` builds an exact, **HEAD-relative replacement** candidate without mutation |
| Select archived action boundary | `navigateVerifiedHydraulicAction` atomically publishes verified replacement state and V7 cursor metadata under lease, HEAD and generation fences, idempotent by navigation operation UUID |
| Navigate an existing network's active draft actions | `planVerifiedHydraulicDraftActionNavigation` verifies all retained MODEL frames against the persisted selected network state; `navigateVerifiedHydraulicDraftAction` publishes the rebuilt HEAD-relative candidate and cursor through the fenced/idempotent native writer. **Currently limited to one existing hydraulic network per committed base snapshot** |
| HTTP action navigation | `GET /working-copies/{draftId}/history/cursor` distinguishes an active journal cursor from an archived Commit coordinate; fenced `POST /working-copies/{draftId}/history/navigate` selects a verified active boundary, or an indexed archived boundary when `revisionId` is supplied. Both use a UUIDv7 operation identity and server-built overlays |
| Continuous cross-Commit Undo/Redo and GUI navigation | **Not implemented** |

The **archived** HTTP/native action selector is restricted to an indexed, complete,
first-parent local hydraulic Commit and a draft with **no active MODEL journal**.
It permits explicit movement between individually verified archived boundaries,
including returning to a clean committed HEAD, but it does not yet perform
automatic stepwise handoff from active draft actions across a Commit marker. Network
creation/removal, non-hydraulic data, merge Commits and missing archives are
not action-level targets today. Returning to the committed HEAD must produce an empty native candidate
overlay (a clean draft); the Qt hydraulic regression has passed locally,
while HTTP archived selection still requires its dedicated regression run.

## Required future navigation semantics

Undo steps back to the previous meaningful model state and Redo forward to
an eligible next state, even across a visible Commit marker. Jumps select a
verified boundary. A new ordinary edit after Undo makes later Redo
**ineligible**, without deleting prior immutable Commits or their audit
frames. Active draft Save, navigation and local Commit must form one
coherent, durable action timeline.

Navigation is a server-owned transaction, **not** a pair of separate
"write model" and "move cursor" requests. Each mutation requires a live
private-draft lease/session, expected generation, expected committed HEAD,
repository identity and a fresh operation UUIDv7. The model candidate,
logical cursor and provenance must be durably published together. Retry
ambiguous responses using the **same** operation ID and parameters; query
persisted status rather than issuing a different Undo. Lost responses,
conflicting writers and stale images fail closed.

All 18 hydraulic collections, root metadata and native membership order
must round-trip byte-identically on Undo → Redo. Canonical model and UUID
reference integrity remains mandatory. **Hydraulic solver feasibility is
not** an Undo/Redo, Save or Commit gate.

## Retention and user interface

Original accepted MODEL frames are retained in a permanent local-Commit
archive; the bounded active-draft journal is cleared after that Commit.
The native MODEL-aware active-journal adapter validates the whole retained
journal, including ordered membership and root metadata, compares the
selected position with the actual persisted canonical network and refuses
unjournaled/foreign native candidate changes. Navigation retains original
frames and a durable cursor. On a **new atomic MODEL event Save** below the
journal tip, the server preserves only the applied prefix and appends the
new actions, making the former redo tail ineligible without changing any
immutable Commit. This Save/cursor transition is atomic and crash-safe;
Commit still rejects an unapplied redo tail. Multi-network and unborn-base drafts remain outside this **single-network**
active-journal slice. HTTP selection exists for eligible existing-network
drafts, but GUI navigation and crossing Commit markers are not supported yet.

The GUI History dock may show committed action metadata, draft actions and
checkpoints already, but it must not advertise operational Undo/Redo until
the continuous cursor, HTTP commands, redo rules and recovery tests exist.
Imported, old and merge checkpoints must be labelled honestly rather than
shown as sequences of invented edits.

No automatic compatibility migration is provided for pre-release draft
formats. The current working-copy marker is `AOWIS-WORKING-COPY-7`;
recreate older disposable drafts. See [storage formats](storage-formats.md).

# History contract — current capabilities and intended Undo/Redo behavior

History has two distinct layers. **Revision history** is an immutable DAG
of model snapshots with branch refs. **Edit history** is an ordered record of
MODEL actions performed in a private draft and archived by a successful
local Commit. Commit markers connect the two layers; they are **not Undo
barriers** or extra synthetic edit steps.

The native draft marker is V9; V8 development drafts must be recreated.
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
| Select archived action boundary | `navigateVerifiedHydraulicAction` atomically publishes verified replacement state and V9 cursor metadata under lease, HEAD and generation fences, idempotent by navigation operation UUID |
| Navigate an existing network's active draft actions | `planVerifiedHydraulicDraftActionNavigation` verifies all retained MODEL frames against the persisted selected network state; `navigateVerifiedHydraulicDraftAction` publishes the rebuilt HEAD-relative candidate and cursor through the fenced/idempotent native writer. **Limited to one unambiguous hydraulic network**; an unborn network initialized/imported before the first MODEL event can replay journaled creations/updates, but ambiguous unborn deletions are refused |
| HTTP action navigation | `GET /working-copies/{draftId}/history/cursor` distinguishes an active journal cursor from an archived Commit coordinate and supplies bounded, server-resolved `previousAction`/`nextAction` coordinates (including adjacent Commit archives); fenced `POST /working-copies/{draftId}/history/navigate` selects a verified active boundary, or an indexed archived boundary when `revisionId` is supplied. Both use a UUIDv7 operation identity and server-built overlays |
| Suspended draft / archived handoff | At active position zero, `previousAction` can address the last archived Commit action; the published V9 cursor preserves original draft frames for Redo. From the newest Commit's final action, `nextAction` returns to the retained draft at zero, then continues to its first action. A new MODEL Save from an archived selection atomically creates a forked active action path and invalidates its forward Redo suffix; immutable Commits are not removed |
| Unborn first-draft creation/update Undo/Redo | **Implemented for one verified initialized network; Qt/HTTP acceptance pending; ambiguous member deletions fail closed** |
| GUI Undo/Redo and end-to-end recovery | **Implemented GUI integration; full live-session/restart acceptance pending** |

The **archived** HTTP/native action selector requires an indexed, complete,
first-parent local hydraulic Commit. It can navigate from an active journal
only when the latter is already verified and selected at position zero. The
suspended journal and archival coordinate persist atomically, and the HTTP
cursor supplies adjacent targets across that boundary. Network
creation/removal, non-hydraulic data, merge Commits and missing archives are
not action-level targets today. Returning to the committed HEAD must produce an empty native candidate
overlay (a clean draft); the Qt hydraulic regression has passed locally,
while HTTP archived selection still requires its dedicated regression run.

## Navigating and branching the active action path

Undo steps back to the previous meaningful model state and Redo forward to
an eligible next state, even across a visible Commit marker. Jumps select a
verified boundary. A new ordinary edit after Undo makes later Redo
**ineligible**, without deleting prior immutable Commits or their audit
frames. An accepted MODEL Save at an archived coordinate records a durable
fork origin (source revision, archived action position and Commit operation
UUID), starts a replacement active journal and clears the archived cursor.
After a new local Commit, its indexed local action archive retains this
origin, so branch action History follows the new logical path and skips the
abandoned committed-action suffix. Immutable revision ancestry remains
first-parent as before: a logical History fork never rewrites it. Active draft Save, navigation and local Commit must form one
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
Commit still rejects an unapplied redo tail. Navigation is currently
limited to one unambiguous hydraulic network. For an unborn private draft,
its initialized/imported network is the unjournaled seed: journaled creations
and updates may be undone/redone without fabricating a Commit. Deletions whose
original membership order cannot be proved fail closed. Multi-network
navigation remains deferred. HTTP adjacent navigation crosses eligible Commit
markers; end-to-end editor/restart acceptance is still outstanding. A new MODEL Save
from a verified archived action is allowed as a durable logical fork; plain
non-event Save and unverified origins still fail closed.

The GUI History dock is wired to the HTTP cursor and action navigation. It
must only enable actions that DVMDB offers as verified adjacent coordinates,
and must reload the authoritative network after successful navigation.
Imported, old and merge checkpoints must be labelled honestly rather than
shown as sequences of invented edits.

No automatic compatibility migration is provided for pre-release draft
formats. The current working-copy marker is `AOWIS-WORKING-COPY-9`;
recreate older disposable drafts. See [storage formats](storage-formats.md).

## Branch and revision deletion (UUIDv7 branch references)

An explicit branch deletion unlinks its UUID-keyed reference, with an exact
expected HEAD UUID CAS (null for an unborn branch). Truncating a committed
branch rewinds its ref to the first parent of the selected revision; selecting
the root or first branch-local revision deletes the entire branch. This is not
an all-DAG recursive deletion: immutable revisions remain in the pack and can
still be reached through surviving merge parents or other branch references.

Both operations reject stored working copies of the branch rather than silently
orphaning their separately fenced edit journals. Users must close/discard such
copies first. Storage compaction and eventual reclamation are separate matters;
no synchronous pack GC occurs during branch deletion. An uncertain network
reply must be reconciled by re-reading authoritative branch refs; a blind
retry based on an old HEAD is rejected. Cross-resource atomicity for draft
creation versus branch removal is not yet provided and requires a coordinated
lifecycle lock in a later hardening pass.

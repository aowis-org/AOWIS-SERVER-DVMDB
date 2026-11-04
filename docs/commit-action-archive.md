# Action history and archived Commit navigation

DVMDB has **two timelines**. The immutable revision DAG records commits and
branch ancestry. The MODEL action log records original accepted edits in a
private draft and, following a successful **local Commit**, preserves those
same frames in a durable archive. The active draft journal is cleared when
the draft rebases; archived actions are not.

The backend provides read-only HTTP timelines and **restricted native C++
action-boundary navigation** for archived Commits and the retained active
journal of one existing hydraulic network. It does **not yet** support
continuous Undo/Redo across Commit boundaries or an HTTP navigation command. See the
[History contract](history-contract.md) and [remaining History roadmap](history-roadmap.md).

## Permanent local-Commit archive

Every successful event-bearing local Commit retains the exact MODEL frame
bytes, event UUIDs and their original order in
`local-commits/<operation-uuidv7>` (`AOWIS-LOCAL-COMMIT-3`). The ledger also
retains the Commit request/candidate and a SHA-256 chain over archived
actions. The pending intent is durable before immutable revision publication;
recovery reconciles an uncertain outcome with actual committed ancestry.
The immutable, checksum-checked `local-commit-revisions/<revision-uuidv7>`
index connects a published revision to its original local-Commit operation.
It is written before publication and avoids scanning all Commit intents.
An orphan index entry for an unpublished revision is never exposed as history.

A revision without an indexed archive (generic native Commit, merge,
imported revision, older/unindexed Commit) is a **checkpoint only**. An empty
archive is not expanded into speculative individual actions. Corrupt index
records or archived frames are errors, not a reason to silently invent or
hide history. No pre-release archive migration is attempted.

## Reading the first-parent timeline

`ProjectService::branchActionHistory(projectId, branch, limit, after,
optionalDraftId, draftOffset, expectedDraftGeneration)` returns immutable
first-parent Commit checkpoints in **newest-first** order, with each
checkpoint's real archived action UUIDs in original order. The exclusive
`after` cursor continues to the next checkpoint page; `limit` is **1–10**.

The HTTP equivalent is:

```text
GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history
    ?limit=10&after={revisionId}&draftId={draftId}
    &draftOffset=0&expectedDraftGeneration={generation}
```

Parameters after `limit` are optional as appropriate. The JSON response
contains `commits`, `nextCursor` and optional `draft`. A Commit records its
immutable revision, nullable `operationId`, real `actions`, `actionCount`
and `checkpointOnly` flag. The draft reports its generation, total journal
count and up to **100** accepted action UUIDs plus display-ready decoded
metadata. The draft's `nextDraftOffset` pages independently from the
committed checkpoint cursor. For a nonzero draft offset the caller must
supply the **exact** `expectedDraftGeneration` from the first page, or the
read fails rather than splicing two generations. Generation and journal
count use decimal strings in JSON.

To retrieve the **original immutable MODEL frames** for one committed
revision use `ProjectService::revisionActionPage(...)` or:

```text
GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history/{revisionId}/events
    ?offset=0&limit=100
```

The response pages `actions: [{id, frameBase64, operation, description,
label, editCount}]`, `actionCount` and `nextOffset`. The limit is **1–100**
and the selected revision must lie on this branch's first-parent history.
MODEL metadata is decoded from immutable bytes; opaque native test frames
retain their real IDs and null display metadata rather than guessed labels.
Both HTTP routes are **read-only** and do not move a committed HEAD or draft.

## Reconstructing and qualifying an archived action boundary

The following native functions operate on **verified source state**, not
client-supplied arbitrary changes:

| Native API | Guarantee |
|---|---|
| `verifiedRevisionNativeActionWitness` | Links an indexed archive to the complete **actual** immutable first-parent native record delta (including deletion), validating parent and candidate record equality |
| `readHistoryReverseActionWindow` | Paged original frames from a selected first-parent revision, **newest first**; these are not precomputed inverse writes |
| `previewReverseHydraulicActions` | Read-only reverse verification for existing members/root metadata; refuses structural insertion/deletion without parent context |
| `previewVerifiedHydraulicActionBoundary` | Full forward replay from a parent network, append-on-insert and ordered-delete rules, byte-equal final canonical network before returning any intermediate boundary |
| `verifyHydraulicRevisionActionCoverage` | Attributes **every changed native record** to verified MODEL actions and server-generated hydraulic membership/root records, even across several networks |
| `planHistoryCheckpoint` | Full draft-relative replacement overlay for an immutable first-parent checkpoint; read-only |
| `planVerifiedHydraulicActionNavigation` | Full **current-HEAD-relative** candidate overlay for a global archived action position; read-only |
| `navigateHistoryCheckpoint` | Atomic, fenced exact-checkpoint candidate+marker publication in a private draft |
| `navigateVerifiedHydraulicAction` | Atomic, fenced, idempotent candidate+action-coordinate publication; restores the selected archived hydraulic state without moving the committed HEAD |
| `planVerifiedHydraulicDraftActionNavigation` | Verifies the full retained active MODEL journal against the draft's selected canonical network and constructs a complete HEAD-relative candidate, read-only. Currently requires one existing hydraulic root in the committed base |
| `navigateVerifiedHydraulicDraftAction` | Calls the MODEL-aware planner and fenced/idempotent native publisher; retains redo frames and survives service restart. Also exposed as restricted HTTP active-journal selection |
| `publishVerifiedDraftActionNavigation` | Atomic storage boundary for a **preverified** live-draft journal position and complete candidate; never accepts unchecked client native overlays |

MODEL actions in a multi-network Commit retain a **single global action
position** even when verification splits their edits by network. The
coverage verifier requires the complete original archive—not merely the
first page—and rejects unjournaled/foreign records, reordered membership,
missing images and incomplete final snapshots. It is conservative: the
archived-Commit selector does **not** support a live MODEL draft journal,
network creation/removal, non-hydraulic revision changes, merge Commits or
checkpoints without fully retained actions. Live-journal navigation is a
separate, currently single-existing-network-only native operation.

For action-boundary navigation the service verifies the draft's private
role, leased session/fencing token, generation and current committed HEAD;
checks the revision's indexed archive/operation and action coordinate
under locks; then replaces the **whole** candidate overlay and publishes
the V7 cursor in the same durable metadata update. Repeating the identical
operation UUID/parameters, including after restart, does not increment
generation twice. New ordinary Save and local Commit clear the one-shot
selection marker. A selected state equal to committed HEAD must be a clean
overlay; the Qt forward-navigation acceptance test still requires a
confirmed passing run against the current correction.

## HTTP active-journal action selection

For **one existing hydraulic network** in a committed base, the draft-scoped
`GET /working-copies/{draftId}/history/cursor` reports the saved journal tip
and selected action boundary. `POST /working-copies/{draftId}/history/navigate`
accepts only an explicit boundary, UUIDv7 operation ID, session/fencing token,
expected generation and HEAD. The server rebuilds the complete native overlay
from the original journal and atomically publishes it with the cursor. It does
not accept client-generated overlay records. Repeating the exact request is
idempotent even if the response was lost.

This endpoint does **not** navigate archived Commit actions or support unborn
and multi-network drafts. Those are separate eligibility boundaries; DVMDB's
storage capability for multiple networks is unchanged. The GUI History dock
remains read-only until continuous navigation is integrated.

## Limits and interpretation

- Active draft journal: **4,096 events / 8 MiB**. Its budget is distinct
  from the permanent action archive. A full draft journal must be committed
  or otherwise deliberately resolved; it is not an infinite Undo buffer.
- Commit history pages: **1–10** checkpoints; archived frames and draft
  action pages: up to **100** per request, with separate cursors.
- The native revision witness is bounded (65,536 changed record keys), and
  navigation is further limited by private candidate budgets.
- A selected action is a **boundary**, with position 0 before the archive's
  first action and position N after its Nth action. Commit markers are not
  extra MODEL edits. A checkpoint-only revision cannot yield fabricated
  internal action positions.
- The MODEL-aware active-draft adapter validates original frames against the
  present canonical network before publishing any cursor change. The native
  publisher retains the journal and records `draft-action-position` across
  restart. Only **one existing network** in a committed base is currently
  supported; unjournaled native changes and ambiguous ownership fail closed.
  Navigation preserves future Redo frames. A **new** atomic MODEL event
  Save behind the tip branches the journal: retains the selected prefix,
  replaces the redo tail with new actions, and publishes the model candidate
  and journal metadata together. Commit still refuses a non-tip cursor.
- The native adapter is **not** complete user-facing History: multi-network
  or unborn-base draft replay, durable navigation-event provenance,
  continuous cross-Commit movement and GUI wiring remain outstanding. The
  existing HTTP History navigation route now also accepts an explicit archived
  revision ID to select a verified committed action boundary when no active
  MODEL journal is present. This is not yet automatic Undo/Redo across a Commit
  marker. The HTTP Save path needs end-to-end Undo → new Save qualification
  before the feature is considered GUI-ready.

See [working-copy storage](storage-formats.md#private-drafts-aowis-working-copy-7)
for the persisted cursor fields and [working copies](working-copies.md) for
the ordinary Save/Commit contract.

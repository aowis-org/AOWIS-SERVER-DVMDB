# Committed MODEL action archive and read-only history

DVMDB retains the exact accepted MODEL transaction frames that were in a
private working copy when its **local Commit** succeeded. The durable
`local-commits/<operation UUIDv7>` record is written before the immutable
revision can be published. It contains the candidate, original action UUIDs
and frames, action count and SHA-256 chain digest. The format is
`AOWIS-LOCAL-COMMIT-3`. Recovery and replay do not discard these frames,
even though the committed draft's *short-lived* event journal is cleared.

DVMDB maintains `local-commit-revisions/<revision UUIDv7>` as an immutable,
checksum-verified revision-to-Commit-operation index. Its directory entry is
fsynced **before** writing the pending local Commit intent and therefore before
publishing its reserved immutable revision. A crash may leave an orphan index
entry for an unpublished revision; history never traverses it. No scan of
unrelated operation records is necessary. Missing entries denote a genuine
checkpoint without an indexed local Commit archive, including pre-index
revisions; older unindexed archives are not silently reconstructed or migrated.

A successful `ProjectService::localCommitActionHistory(operationId)` returns
the verified archive. A pending operation is reconciled against actual
revision ancestry before exposing its committed actions; rejected operations
have no published archive. The immutable revision itself is never changed.

## Branch-aware history reads

`ProjectService::branchActionHistory(projectId, branch, limit, after,
optionalDraftId, draftOffset, expectedDraftGeneration)` returns a **read-only**
first-parent timeline:

- Commit checkpoints in newest-first order, each with its immutable revision
  record, optional local Commit operation UUID and the **actual archived
  action UUIDs**, in original order.
- An exclusive revision UUID cursor (`next`) for the next checkpoint page.
  `limit` is 1–10 checkpoints; the cursor must belong to that branch's
  first-parent ancestry.
- If a private draft is selected, its generation, journal count and a page
  of at most 100 accepted action UUIDs and original frames for display
  metadata. A separate `nextDraftOffset` pages its journal; the draft must
  belong to the selected project and branch. A nonzero continuation offset
  requires `expectedDraftGeneration` from the first page; if the draft has
  changed, the read fails as a Conflict and the client restarts at offset 0.

The equivalent HTTP route is
`GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history` with
`limit`, `after`, `draftId`, `draftOffset` and `expectedDraftGeneration`
query parameters. The JSON response contains `commits`, `nextCursor`, and
`draft`. Each Commit has a `revision` object, nullable `operationId`, `actions`,
`actionCount` and `checkpointOnly`; draft generation and journal count are
**decimal strings**. The draft's `actionDetails` objects contain the original
IDs and display-ready metadata only for the requested page.
The route never performs Undo/Redo or changes HEAD.

For exact original MODEL frames, use
`ProjectService::revisionActionPage(projectId, branch, revision, offset, limit)`
or `GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history/{revisionId}/events`.
`limit` is 1–100 events; `offset` is zero-based with exclusive `nextOffset`.
Each result reports the immutable revision, nullable local Commit operation,
total archived actions, `actions: [{id, frameBase64, operation, description, label, editCount}]`,
and nullable next offset.
The selected revision must occur on the branch's first-parent ancestry.
Checkpoint-only revisions return zero events, not hypothetical inverse deltas.
Full frames are **original canonical MODEL bytes**; metadata is derived
on demand from these immutable bytes without rewriting the archive. The API
does not execute Undo/Redo. Frames that cannot be decoded retain their IDs
and have null operation, description and edit count instead of invented data.

A revision created by a generic native Commit, a merge, an imported revision,
or an old local Commit without retained actions is represented as a genuine
**checkpoint only**. An empty archive is not expanded into hypothetical
editing actions. Forked branches inherit first-parent Commit checkpoints
without duplicating archived actions.

## History checkpoint reconstruction

The native `planHistoryCheckpoint` method can now preview the exact
replacement overlay for choosing an older immutable first-parent revision
as working-copy content. It checks project/branch ownership, current branch
HEAD, draft generation and native candidate limits. It never changes the
draft, commit journal, cursor or immutable revision. It does **not** yet
support navigating to an individual archived action or the unborn state.
The returned overlay must not be passed to the normal incremental Save method
as though it were a delta: future navigation needs an atomic *replacement*
with a durable cursor and action record.

## Current operational limits

- The active draft event journal remains bounded at **4,096 events / 8 MiB**.
  Each local Commit archives up to this journal budget independently.
- Revision checkpoint pages use the durable revision index for each requested
  revision, regardless of how many unrelated local Commit operations exist.
  Each lookup verifies the index checksum and the complete archived operation;
  corruption is reported as an error, not hidden as an empty action list.
- Full frame reads are limited to 100 archived events per request and remain
  separate from the bounded active-draft event journal. Original event bytes
  can be decoded into display summaries by a MODEL client.
- Draft pages are labelled with a generation; the server **requires** the
  same `expectedDraftGeneration` on every nonzero continuation offset. If a
  second writer Saves or Commits, continuation fails with Conflict rather
  than silently assembling inconsistent history pages.
- Action archives are not a durable Undo/Redo cursor. Navigating across
  Commit markers requires a separate server-owned cursor, canonical inverse
  materialization, idempotent mutation IDs and recovery logic. None of these
  navigation commands is implemented yet.

All malformed archived frames and digest mismatches are storage errors; they
are not silently treated as missing history. No old per-action history can be
reconstructed for historical revisions whose original journals were discarded.

See [History contract](history-contract.md), [History roadmap](history-roadmap.md)
and [HTTP workflows](http-workflows.md).

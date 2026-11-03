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
optionalDraftId, draftOffset)` returns a **read-only** first-parent timeline:

- Commit checkpoints in newest-first order, each with its immutable revision
  record, optional local Commit operation UUID and the **actual archived
  action UUIDs**, in original order.
- An exclusive revision UUID cursor (`next`) for the next checkpoint page.
  `limit` is 1–10 checkpoints; the cursor must belong to that branch's
  first-parent ancestry.
- If a private draft is selected, its generation, journal count and a page
  of at most 100 accepted action UUIDs. A separate `nextDraftOffset` pages
  its journal; the draft must belong to the selected project and branch.

The equivalent HTTP route is
`GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history` with
`limit`, `after`, `draftId` and `draftOffset` query parameters. The JSON
response contains `commits`, `nextCursor`, and `draft`. Each Commit has a
`revision` object, nullable `operationId`, `actions`, `actionCount` and
`checkpointOnly`; draft generation and journal count are **decimal strings**.
The route never performs Undo/Redo or changes HEAD.

For exact original MODEL frames, use
`ProjectService::revisionActionPage(projectId, branch, revision, offset, limit)`
or `GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history/{revisionId}/events`.
`limit` is 1–100 events; `offset` is zero-based with exclusive `nextOffset`.
Each result reports the immutable revision, nullable local Commit operation,
total archived actions, `actions: [{id, frameBase64}]`, and nullable next offset.
The selected revision must occur on the branch's first-parent ancestry.
Checkpoint-only revisions return zero events, not hypothetical inverse deltas.
Full frames are **original canonical MODEL bytes**; the API does not execute
Undo/Redo, and user-facing operation names can be decoded later by MODEL clients.

A revision created by a generic native Commit, a merge, an imported revision,
or an old local Commit without retained actions is represented as a genuine
**checkpoint only**. An empty archive is not expanded into hypothetical
editing actions. Forked branches inherit first-parent Commit checkpoints
without duplicating archived actions.

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
- Draft pages are labelled with a generation; clients must check that value
  when paging through edits while a second writer may save new events.
- Action archives are not a durable Undo/Redo cursor. Navigating across
  Commit markers requires a separate server-owned cursor, canonical inverse
  materialization, idempotent mutation IDs and recovery logic. None of these
  navigation commands is implemented yet.

All malformed archived frames and digest mismatches are storage errors; they
are not silently treated as missing history. No old per-action history can be
reconstructed for historical revisions whose original journals were discarded.

See [History contract](history-contract.md), [History roadmap](history-roadmap.md)
and [HTTP workflows](http-workflows.md).

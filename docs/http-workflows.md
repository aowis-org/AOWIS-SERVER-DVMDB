# DVMDB HTTP workflows (current implementation)

The service's routes are under `/dvmdb/v1`. The **source of truth for JSON
schemas** is [OpenAPI](dvmdb-http-api.yaml); this page explains which
resources to use and why. Use [REST design](rest-api-design.md) for error
semantics and [working copies](working-copies.md) for durability and fencing.
DVMDB is pre-release; these endpoints have **no authentication**.

## Establish the correct repository

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/health
curl -fsS http://127.0.0.1:8755/dvmdb/v1/repository
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects
```

`/health` tests the process; `/ready` tests catalogue readability;
`/repository` returns the repository UUID. `{"projects":[]}` means this
**particular storage root** has no enumerated projects; it does not prove
another storage root is empty. Compare the returned `repositoryId` to the
GUI connection's expected identity before changing any data directory.

The server script `./compile_linux.sh` builds only the server/dependencies
and starts it if successful; `--build-only` disables launch.

## Projects, unborn branches and origin

`POST /projects` with `unborn: true` creates a project and its `main`
branch with no committed revision. `POST /projects/{projectId}/branches`
with `unborn: true`, `name` and optional `description`, `tags`, `id`
creates **another independent unborn branch** in that same project. It
has its own UUIDv7 and null committed HEAD; no parent revision or source
branch is needed. Names must be unique within the project. The API rejects
cross-project identity misuse and duplicate names.

Creating a branch **from an immutable revision** uses the other form of
`POST /projects/{projectId}/branches`: `fromBranchId`, optional `revision`
and a new branch name/identity. It records the origin branch and fork revision.
There is no synthetic revision created by either branch-creation operation.

`PATCH /projects/{projectId}` renames the project using a checked expected
name. `PATCH /projects/{projectId}/branch-ids/{branchId}` updates a branch's
name, tags and description (including unborn branches) using its stable ID
and expected name. Renaming does not change revision records or UUIDs.

## Read committed history versus a private saved draft

| You need | Read |
|---|---|
| Complete ancestry and branch refs | `GET /projects/{projectId}/revision-graph` (paged) |
| A particular immutable revision | `GET /projects/{projectId}/revisions/{revisionId}` |
| Committed network inventory/counts | `GET /projects/{projectId}/branches/{branch}/networks/summary?revision={revisionId}` |
| Saved working-state inventory/counts | `GET /working-copies/{draftId}/networks/summary` |
| Network from a saved private draft | `GET /working-copies/{draftId}/networks/{networkId}` |

A branch's committed HEAD may be null while its **unborn private draft already
contains a network**. Likewise, immediately after Commit the draft overlay
may be empty while its new committed base contains the network. To describe
what an editor will reopen, use the **draft-scoped** summary, which includes
its pinned base *and* saved changes, not only a committed revision summary.
Validate `draftId`, `generation` and network UUID; refresh instead of using
stale summary results to authorize destructive operations. Multiple drafts
must be selected explicitly rather than guessed from their branch.

## First import and ongoing editing

`POST /working-copies/{draftId}/import-network` is a fenced bulk import for
an **untouched, empty, unborn private draft**. It is **not** a general
replacement or merge API: import into a populated or stale draft is rejected
with a conflict. The GUI currently converts supported EPANET inputs and
submits the resulting network through this route.

`POST /working-copies/{draftId}/initialize-network` is a separate operation
for creating a new empty network. Ordinary editor changes are saved through
`/save-events`, with a lease, fencing token, expected generation and
membership intents. One canonical MODEL event may contain both typed entity
changes and network-root metadata; DVMDB derives all root/chunk records and
applies them atomically. Save advances the **draft generation**, never the
branch's committed HEAD. Network summaries are read-only and do not save.

## Revision history versus edit-action history

`GET /projects/{id}/revision-graph` exposes the immutable committed DAG;
`GET /working-copies/{id}` exposes a bounded draft event journal. In addition,
the **permanent local-Commit MODEL action archive** is available through the
read-only `/action-history` and `/action-history/{revisionId}/events` routes
below, including real actions across Commit markers. For an **existing single
hydraulic network in a committed base**, HTTP can select verified active
journal and archived Commit action positions, including crossing from active
position zero to the last archived action without discarding Redo. The GUI
History dock remains read-only pending controller integration. See
[History](commit-action-archive.md) and [remaining work](history-roadmap.md).

## Local Commit and message annotations

`POST /local-commits` finalizes a saved, modified private draft using an
operation UUIDv7, exact expected committed HEAD/null, generation, lease and
commit author/message. The message must contain non-whitespace text. On
success an immutable revision is published and the draft continues with a
new committed base. `GET /local-commits/{operationId}` resolves uncertain
results without issuing an unrelated second Commit. **Publish to a shared
repository is a separate, still unfinished workflow.**

The original commit message is inside the immutable hashed revision record.
`PATCH /projects/{projectId}/revisions/{revisionId}/message` edits the
**display message annotation** only; the original message and record hash
remain unchanged and are still available for verification. Distinguish an
editable displayed annotation from cryptographically committed metadata.

## Failures to expose to the user

A 409 may mean stale HEAD, stale draft generation, stale fencing lease,
identity conflict, or nonempty import target; never turn it into a silent
retry or overwrite. A network-summary failure is not proof the network was
deleted. Do not treat a failed project-catalogue read as a failed health
probe. On a 503, respect backpressure; on an uncertain write, query
persisted operation/draft state before deciding to retry.

See [operations](operations.md) for storage-root selection, backup and
recovery, and [roadmap](roadmap.md) for unfinished work.

### MODEL source compatibility handshake

`GET /dvmdb/v1/repository` and `GET /dvmdb/v1/repository/tracking`
return `modelCodecFingerprint`: a lowercase SHA-256 digest of the checked-out
AOWIS-SERVER-MODEL source/header files and codec schema inputs when the server
was built. It is **not** a Git revision: uncommitted source edits change it.
GUI clients must compare this fingerprint with their own build before loading
MODEL networks or submitting private working-copy changes. Missing or different
fingerprints are a compatibility failure, not a request to migrate or discard
queued events. Rebuild both applications against identical MODEL source contents
and restart both processes. This is a compatibility check, not authentication.

## Read-only branch action history

`GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history` returns
first-parent Commit markers (`commits`) with real archived MODEL action UUIDs.
`limit=1..10` bounds the checkpoint page; `after` is an exclusive revision
UUIDv7 cursor and `nextCursor` is null on the last page. Commits without
retained event frames are explicitly `checkpointOnly`; they remain real
immutable revisions. Optional `draftId` adds a separate, generation-labelled
page of up to 100 saved draft action UUIDs, plus `actionDetails` for those
same events (operation, optional description, display label and edit count).
Use `draftOffset` / `nextOffset` to advance independently of `after`. For
**every continuation** where `draftOffset` is nonzero, include
`expectedDraftGeneration=<generation returned by first page>` as a decimal
string. A concurrent Save or Commit changes the generation and returns HTTP
409 instead of joining two different journal versions; restart at offset 0.
Draft IDs from another project or branch are rejected.

`GET /dvmdb/v1/projects/{projectId}/branches/{branch}/action-history/{revisionId}/events?offset&limit`
returns the exact archived MODEL bytes (Base64), in original event order, at
up to 100 actions per page. Every returned action also contains `operation`,
optional `description`, `label`, and `editCount` decoded from the immutable
MODEL frame when valid. Opaque native-only test frames retain their UUIDs and
have null metadata rather than fabricated operation names. Revisions not on
the selected branch's first-parent ancestry are rejected. Both endpoints are **read-only** and do not move HEAD or
provide Undo/Redo.

The two branch-history responses are read-only. The separate draft-scoped
`POST /working-copies/{draftId}/history/navigate` selects a verified active
draft journal boundary or, with `revisionId`, an archived Commit action
boundary. Both require a fenced and pinned private draft. See
[committed action archive](commit-action-archive.md) for current limits and
the [History roadmap](history-roadmap.md) for future work.

## Single-network draft Undo/Redo over HTTP

Read `GET /working-copies/{draftId}/history/cursor` for `generation`,
`baseRevision`, `journalCount` and `actionPosition` (decimal strings). If the
cursor is greater than zero, the adjacent Undo target is `actionPosition - 1`;
if below `journalCount`, the adjacent Redo target is `actionPosition + 1`.
Prefer the server's `previousAction` and `nextAction` objects: each supplies
`selectionKind`, nullable `revisionId` and decimal `actionPosition`. These
coordinates also navigate between two archived Commit action sequences, and
skip the duplicate end/start snapshot at their shared Commit boundary.
At an active draft journal's verified zero position, `previousAction` can
select the last archived action of its pinned HEAD. The server then keeps the
original MODEL journal as a suspended Redo tail while browsing older Commits.
The newest HEAD archive's last step returns to draft position zero and the
next draft step restores its first event. New Save/Commit while on the
archived side is refused until the journal is resumed.
The booleans report **adjacent draft positions**, not verified eligibility;
the server may reject unsupported/unprovable drafts. A GUI must not advance its
cursor optimistically.

Submit `POST /working-copies/{draftId}/history/navigate` with the same draft's
`sessionId`, current `fencingToken`, `expectedGeneration`, `expectedHead`
(revision UUIDv7 or null), a fresh `operationId` UUIDv7 and the explicit
decimal-string `actionPosition`. Pin the selected repository using
`X-AOWIS-Expected-Repository-Id`. No native model bytes are accepted from the
client. On success, **reload the canonical draft network** and cursor; the
branch HEAD and permanent Commit archive stay unchanged. If the reply is lost,
retry the **identical** operation ID, generation and target rather than
computing another relative Undo. HTTP 409 means no verified publication;
refresh the cursor and resolve any uncertain previous request first.

This first executable HTTP slice supports one existing hydraulic network in a
committed base. It intentionally refuses unborn/multi-network drafts, mixed
native-only edits, and crossing from a retained live journal at position zero
into archived actions. Explicit archived action navigation is supported when
the draft journal is empty. DVMDB retains the ability to store multiple
networks; multi-network History is not a prerequisite for the single-network
GUI workflow.

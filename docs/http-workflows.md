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
membership intents. Save advances the **draft generation**, never the
branch's committed HEAD. Network summaries are read-only and do not save.

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

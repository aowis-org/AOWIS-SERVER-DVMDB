# REST API guide

The HTTP API carries the same project, branch, revision and draft semantics as
the native C++ API ([version-control contract](version-control-semantics.md)).
This page explains the conventions and which resources exist; the exact
request and response schemas are in the [OpenAPI contract](dvmdb-http-api.yaml),
which a CTest (`aowis-dvmdb-http-uuidv7-route-contract-tests`) keeps in step
with the registered routes. Running the server, limits and the error table:
[service guide](native-database-service.md).

All routes are under `/dvmdb/v1`. There is no authentication.

## Conventions

* **Identities are UUIDv7**, in canonical lowercase text: projects, branches,
  revisions, drafts, operations, sessions, fencing tokens. Hashes are 64
  lowercase hex digits and are integrity data, never identifiers.
* **Branches** are addressed by name in most routes (names may contain spaces
  and any valid UTF-8; percent-encode them in paths). Requests that create or
  refer to branches durably use the branch UUIDv7 (`branchId`,
  `fromBranchId`); `GET /projects/{id}/branch-ids/{branchId}` returns a
  branch's current name and HEAD.
* **Nullable HEADs.** An unborn branch has `null` as its HEAD everywhere. In
  write preconditions a missing field is invalid; it is not a synonym for
  `null`.
* **64-bit counters** (draft generations, lease expiry, commit times) are
  decimal strings.
* **Authorship.** Every request that creates a revision carries
  `"author": {"id": "<uuidv7>", "name": "<display name>"}` and a `"message"`:
  model commits, network saves and edits, merges and hydraulic merges, and
  `POST /projects` without `unborn`. Merges may omit the message
  (`"Merge <source> into <target>"`), project creation too
  (`"Create project <name>"`). Names are 1–200 bytes of UTF-8 without control
  characters, messages 1–65,536 bytes of UTF-8 where tab and newline are the
  only control characters; anything else is `400`. Revision reads, history and
  the revision graph return `author`, `message` and `committedAtMs`, the
  server's publication time. The author is self-reported; nothing verifies it.
* **Optimistic concurrency.** Every write that moves a branch names the
  expected HEAD(s); every draft write names the lease session, fencing token
  and expected generation. A mismatch is `409 Conflict` and changes nothing.
* **Retry-safe creation.** Projects (with `unborn: true`), branches and drafts
  accept a caller-chosen UUIDv7 `id`. Repeating the request returns the same
  `201` answer instead of a second object; the same `id` for anything else is
  `409`. This covers lost responses and transports that resend a POST on their
  own (Qt before 6.10 resends a POST whose connection closed before the status
  line, including Qt 6.8 in Debian 13).
* **Strict JSON.** Unknown fields and repeated query parameters are rejected.
  JSON bodies must send `Content-Length`.
* **Pinned reads.** Reads that span several objects use one immutable
  revision. Hydraulic reads accept `?revision=` (which must be the branch HEAD
  or one of its ancestors) so several calls see the same snapshot.

### Repository-pinned metadata writes

The GUI verifies `GET /repository/tracking` (repository UUIDv7, role and MODEL
fingerprint) before project creation, project renaming, branch creation,
identity/name-based branch metadata updates and revision display-message
updates. Its mutation then includes the HTTP header
`X-AOWIS-Expected-Repository-Id: <selected UUIDv7>`.

For those six metadata routes, the server checks the supplied expected UUID
immediately before native mutation; a different repository returns **409
Conflict** (`repository_mismatch`, `repository identity changed`) without writing. A missing header
currently means the client has not requested this repository pin. This is a
**correctness/CAS guard, not authentication**. The preflight catches codec and
role mismatches, while the server-side guard closes the endpoint-swap window.
A lost metadata-write acknowledgement still requires authoritative refresh
before a new write. The separate local Commit operation already has a durable
UUIDv7 intent ledger and status lookup.

## Resources

| Area | Routes | Notes |
|---|---|---|
| Service | `GET /health`, `GET /ready` | liveness, catalogue readiness |
| Repository | `GET /repository`, `GET /repository/tracking`, `POST /repository/role` | identity, role, remote tracking ([details](repository-tracking.md)) |
| Projects | `GET`/`POST /projects`, `GET`/`PATCH /projects/{id}`, `POST /projects/clone-target` | create with `unborn: true` for an unborn `main` (see below) |
| Branches | `GET`/`POST /projects/{id}/branches`, `PATCH /projects/{id}/branches/{branch}`, `PATCH /projects/{id}/branch-ids/{branchId}`, `GET …/branches/{branch}/head`, `GET /projects/{id}/branch-ids/{branchId}`, `POST /projects/{id}/unborn-checkout` | create independent unborn or from committed revision; identity-based branch metadata/name update supports unborn branches |
| Revision history | `GET …/branches/{branch}/history`, `GET /projects/{id}/revisions/{revision}`, `PATCH /projects/{id}/revisions/{revision}/message`, `GET /projects/{id}/revision-graph`, `GET /projects/{id}/diff` | first-parent committed history, pinned revision DAG and record-level diff |
| MODEL action history | `GET …/branches/{branch}/action-history`, `GET …/action-history/{revisionId}/events` | read-only paged first-parent Commit markers, original archived MODEL frames and optional generation-pinned active draft event pages; **not** Undo/Redo |
| Model commits | `POST …/branches/{branch}/model-commits` | generic record batch with reference checks only |
| Merges | `POST /projects/{id}/merges`, `POST /projects/{id}/hydraulic-merges` | both need both expected HEADs; the hydraulic route validates the whole network and accepts field/object choices |
| Hydraulic networks | `POST …/branches/{branch}/networks`, `GET …/branches/{branch}/networks/summary`, `GET …/networks/{networkId}`, `GET …/networks/{networkId}/entities`, `GET …/entities/{entityId}`, `POST …/networks/{networkId}/edits` | whole-network save, pinned reads, indexed edits ([details](native-hydraulic-network.md)) |
| Bundles | `GET`/`POST …/branches/{branch}/bundle` | full or incremental (`?base=`) export, verified import ([details](offline-repository-synchronization.md)) |
| Local Commit | `POST /local-commits`, `GET /local-commits/{operationId}` | fenced, idempotent private Commit and outcome lookup |
| Drafts | `GET`/`POST /working-copies`, `GET /working-copies/{id}`, `…/lease`, `…/save`, `…/save-events`, `…/initialize-network`, `…/import-network`, `…/release`, `…/discard`, `GET …/networks/summary`, `GET …/networks/{networkId}` | private repositories only ([details](working-copies.md)) |

### Known deviations

* `POST /projects` without `unborn: true` still creates `main` with an empty
  first revision (which then needs an `author`), which the contract does not
  want for new projects. Clients
  should always send `unborn: true` (it is required with a caller-chosen `id`).
* The older name-addressed branch rename route requires non-null
  `expectedHead`; use identity-addressed `PATCH /projects/{id}/branch-ids/{branchId}`
  to rename/update metadata on unborn branches. `POST …/model-commits` still
  requires a non-null expected HEAD; the first unborn commit uses a private draft.

## Not built yet

These operations are part of the contract but have no route yet; nothing
should assume they exist:

* Fetch and Publish between a personal and a shared repository, including
  branch provisioning on the remote and Publish status lookup;
* registering remotes and recording tracking observations over HTTP;
* reconcile/compare of a stale draft against a new HEAD;
* History Undo/Redo and jump **mutation endpoints** (native C++ only supports restricted verified archived action/checkpoint selection today).

The original commit message is still hashed into the canonical revision.
`PATCH /projects/{id}/revisions/{revision}/message` updates only a separately
stored displayed annotation, not that canonical message or the record hash.
A private HEAD summary must read the saved draft (including its committed
base); committed-only summaries can be empty even when a draft is populated.

For ready-to-use flows, see [HTTP workflows](http-workflows.md).

## Replication and security notes

Bundles preserve branch and revision UUIDs, nullable HEADs, fork provenance and
all integrity hashes; drafts are never part of a bundle. Bundle hashes prove
integrity, not who produced the bundle or that they may change the project.
Neither UUIDv7 nor SHA-256 is a permission mechanism; access control is a
separate future design ([security backlog](security-permissions-design.md)).

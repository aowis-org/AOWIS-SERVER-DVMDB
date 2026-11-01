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
* **64-bit counters** (draft generations, lease expiry) are decimal strings.
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

## Resources

| Area | Routes | Notes |
|---|---|---|
| Service | `GET /health`, `GET /ready` | liveness, catalogue readiness |
| Repository | `GET /repository`, `GET /repository/tracking`, `POST /repository/role` | identity, role, remote tracking ([details](repository-tracking.md)) |
| Projects | `GET`/`POST /projects`, `GET /projects/{id}`, `POST /projects/clone-target` | create with `unborn: true` for an unborn `main` (see below) |
| Branches | `GET`/`POST /projects/{id}/branches`, `PATCH /projects/{id}/branches/{branch}`, `GET …/branches/{branch}/head`, `GET /projects/{id}/branch-ids/{branchId}`, `POST /projects/{id}/unborn-checkout` | create from a source branch UUID (at HEAD or a reachable revision), rename with expected HEAD |
| History | `GET …/branches/{branch}/history`, `GET /projects/{id}/revisions/{revision}`, `GET /projects/{id}/revision-graph`, `GET /projects/{id}/diff` | first-parent history, one revision, the pinned paged DAG of all branches, record-level diff |
| Model commits | `POST …/branches/{branch}/model-commits` | generic record batch with reference checks only |
| Merges | `POST /projects/{id}/merges`, `POST /projects/{id}/hydraulic-merges` | both need both expected HEADs; the hydraulic route validates the whole network and accepts field/object choices |
| Hydraulic networks | `POST …/branches/{branch}/networks`, `GET …/networks/{networkId}`, `GET …/networks/{networkId}/entities`, `GET …/entities/{entityId}`, `POST …/networks/{networkId}/edits` | whole-network save, pinned reads, indexed edits ([details](native-hydraulic-network.md)) |
| Bundles | `GET`/`POST …/branches/{branch}/bundle` | full or incremental (`?base=`) export, verified import ([details](offline-repository-synchronization.md)) |
| Drafts | `GET`/`POST /working-copies`, `GET /working-copies/{id}`, `…/lease`, `…/save`, `…/save-events`, `…/initialize-network`, `…/release`, `…/discard`, `GET …/networks/{networkId}` | private repositories only ([details](working-copies.md)) |

### Known deviations

* `POST /projects` without `unborn: true` still creates `main` with an empty
  first revision, which the contract does not want for new projects. Clients
  should always send `unborn: true` (it is required with a caller-chosen `id`).
* `PATCH …/branches/{branch}` (rename) and `POST …/model-commits` require a
  non-null `expectedHead`, so they do not work on an unborn branch. The first
  revision of an unborn branch comes from a local Commit of a draft.

## Not built yet

These operations are part of the contract but have no route yet; nothing
should assume they exist:

* local Commit of a draft and its status lookup (native
  `commitWorkingCopy` / `localCommitStatus` exist);
* Fetch and Publish between a personal and a shared repository, including
  branch provisioning on the remote and Publish status lookup;
* registering remotes and recording tracking observations over HTTP;
* reconcile/compare of a stale draft against a new HEAD.

## Replication and security notes

Bundles preserve branch and revision UUIDs, nullable HEADs, fork provenance and
all integrity hashes; drafts are never part of a bundle. Bundle hashes prove
integrity, not who produced the bundle or that they may change the project.
Neither UUIDv7 nor SHA-256 is a permission mechanism; access control is a
separate future design ([security backlog](security-permissions-design.md)).

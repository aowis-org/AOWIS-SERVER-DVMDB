# Repository identity, role and remote tracking

Every DVMDB catalogue root has its own identity and, once configured, a role.
It can also remember which remote repositories it knows and the last HEAD it
observed for each of their branches. This metadata is the groundwork for
clone, fetch and publish between a personal and a shared DVMDB
([private working repositories](private-working-repositories.md)); those
operations themselves are not built yet. The file formats are in
[storage formats](storage-formats.md#catalogue-root).

**None of this is security.** A repository UUID, a role or a configured
endpoint proves nothing about who is on the other end. Matching repository
IDs is a wrong-target check, not authentication. Deploy unauthenticated
writable services only on trusted networks
([security backlog](security-permissions-design.md)).

## Identity

* `repositoryId` — UUIDv7 of the catalogue root, created on the first
  `ProjectService` open and kept across restarts and offline backups.
  `ProjectService::repositoryId()`, `GET /dvmdb/v1/repository`.
* `projectId`, `branchId` — logical UUIDv7s. They stay the same when a project
  or branch is cloned or fetched into another repository.
* Each repository keeps its **own** mutable HEAD for every branch. A HEAD is
  never shared by copying or moving a tracking entry; the full key of a ref is
  `(repositoryId, projectId, branchId)`.

## Role

The role starts as `unconfigured` and can be set **once** to `private`
(personal working repository: drafts and local commits) or `shared`.
Repeating the same assignment is accepted; changing it is a conflict.

`POST /dvmdb/v1/repository/role` with `{"expectedRepositoryId", "role"}`.
The caller must echo the repository UUID it just read, so a client cannot
configure an unrelated database by mistake. Native:
`ProjectService::configureRepositoryRole`.

## Remote registry and tracking refs

* A remote is registered by its repository UUIDv7 with an endpoint (an
  `http(s)://` URL or an absolute `file:///` location; no user info, fragment
  or control characters; no credentials are stored). Registration is
  idempotent; an existing UUID is never silently rebound to another endpoint.
* A tracking entry `(remoteRepositoryId, projectId, branchId)` holds the last
  **observed** remote HEAD, which may be an explicit null (the remote branch
  is unborn). A **missing** entry means "never observed" — not the same as an
  observed unborn branch. Clients must likewise distinguish a remote 404 from
  a successful read with a null HEAD.
* Updates are compare-and-swap on the previous observation (absent, null, or
  a revision); a mismatch is a conflict. Callers must only record what they
  actually read from a verified remote; the service does not verify it.

`GET /dvmdb/v1/repository/tracking` returns `repositoryId`, `role`, `remotes`
and `tracking` (with JSON `null` for observed unborn HEADs). Native:
`registerRemoteRepository`, `recordRemoteTrackingRef`, `remoteRepositories`,
`remoteTrackingRefs`. There is no HTTP route for registering remotes or
recording observations yet.

Writes are serialized by the catalogue lock and published atomically; an
uncertain publication stops further writes until restart. Offline backup and
restore validate and preserve the exact file. Tracking metadata never changes
a branch ref or revision.

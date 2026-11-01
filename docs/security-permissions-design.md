# AOWIS / DVMDB security and permission design backlog

**Unscheduled future reference only; not a milestone in the database/GUI
version-control implementation roadmap.** The [DVMDB roadmap](roadmap.md) and
companion GUI roadmap end at VC-11. This document collects security findings,
questions and possible design requirements for an eventual, **separately
approved AOWIS-wide initiative**. It creates **no current implementation task,
numbered milestone, release gate or dependency** for the graph, private drafts,
local commits, fetch, publish or merge. No security solution is selected yet.
The deployment cautions below describe risks, not acceptance criteria for the
current functional roadmap. See also
[private working repositories](private-working-repositories.md) and
[REST design](rest-api-design.md).

## What exists today

* The HTTP service has **no TLS, authentication or authorization**. Anyone who
  can reach the port can read and write every project. Bind it to localhost or
  a trusted network; `deploy/nginx-aowis-server-dvmdb.conf` shows a TLS
  reverse proxy, which adds encryption but no access control.
* `AccessPolicyStore` (`include/aowis/dvmdb/access_policy.h`) and the local
  `aowis-dvmdb-access` tool keep UUID-identified users, groups, roles and
  branch-scoped read/write grants in `access-policy.bin` (atomic replacement,
  SHA-256 corruption check — not a signature). Policy data is separate from
  versioned model revisions, so reverting a branch cannot revert a grant.
  `permits()` trusts the account string its caller passes.
* **No service uses this library.** Its users, roles and grants are not a
  stable API and may be reused, redesigned or replaced once an AOWIS-wide
  security architecture exists. Do not build GUI authorization on it.

## Immediate facts and deployment cautions

- A configured `workingRepository` or repository UUID is **not proof of user
  ownership**. A URL in a GUI config does not restrict who can connect.
- A private DVMDB over HTTP is only usable for single-user editing today
  if access is isolated by deployment controls (local/private network,
  network policy, operating-system permissions, private endpoint or trusted
  reverse proxy). Do not expose an unauthenticated editing service publicly.
- The existing native `AccessPolicyStore` is not an enforced HTTP
  authentication/authorization layer. Do not represent its policy records
  as effective protection at the REST boundary.
- UUIDv7 and SHA-256 provide identity and integrity, **not** authorization,
  confidentiality, source authentication or non-repudiation.
- Credentials/secrets must not be stored in plaintext shared application
  configuration, debug logs or URLs. Define a secure credential provider.

## Security design questions to resolve in a dedicated phase

1. **Identity:** users, service accounts, devices, sessions and recovery;
   stable account identity versus per-device login; federation if needed.
2. **Authentication:** HTTP and native entry points, token expiry,
   verification, revocation, logout and trusted service-to-service calls;
   TLS and credential rotation.
3. **Authorization:** scope ownership for user working repositories,
   projects, branch refs, unpublished drafts, committed snapshots,
   synchronization, merge, deletion, history export and administration.
4. **Draft isolation:** distinct sessions for one user, private copy sharing
   by explicit grant, background autosave authorization, takeover and
   concurrent-draft locking/version checks.
5. **Remote origin trust:** bind tracking refs to authenticated remote
   identity; prevent wrong-project import, poisoned history, cross-tenant
   repo spoofing, unsafe redirect and credential forwarding.
6. **Audit:** successful and rejected editing, commit, publish, merge,
   branch operations, access policy changes, credential events and recovery;
   tamper-evident append-only audit policies and retention.
7. **Integrity and durability:** transport quotas, replay resistance,
   idempotency key ownership, body limits, path traversal, rate limits,
   backup encryption, revocation and secure deletion/retention of drafts.
8. **Native and REST parity:** one policy decision model, explicit
   service/system identity, correct admin bypass controls, no client-supplied
   `ownerId` taken as proof of identity.
9. **Roles and UX:** project/branch read/write/publish rights, private
   repository registration, sharing and denial messaging without exposing
   another user's draft contents.
10. **Threat modeling:** malicious clients, stolen tokens, compromised
    working repositories, untrusted replication bundles, replayed pushes,
    unsafe log contents and denial-of-service via oversized histories.

## Potential qualification for a future multi-user security initiative

- Authentication on every writable HTTP operation and server-side isolation
  for personal working repositories and working copies.
- Authorization and audit checks for fetch, push, history reads, graph
  traversal, bundle import/export, create/delete, draft access and merges.
- Tested tenant-crossing denial, session revocation, token reuse/replay,
  concurrent writers, wrong-repo UUID targeting and private-data leakage.
- Secret management, TLS expectations, deployment guide, backups and
  a release gate explicitly disallowing unauthenticated multi-user access.

**Timing:** no date, milestone number or development sequence is assigned.
The present integration may proceed without these features; unauthenticated
writable HTTP endpoints should be confined to trusted/isolated deployments.
Any decision to support public or multi-user writable access requires its own
explicit product scope and security review.

## Interim constraints for the approved private-repository design

- One intended user has a separate DVMDB working repository, but multiple
  devices/sessions may reach it. Without authentication, this is a functional
  ownership convention only. Keep access operationally isolated and **do not
  expose it as an unauthenticated multi-user service**.
- Draft writer leases, fencing tokens and version checks prevent accidental
  concurrent overwrites; they **are not authorization**. Future policy must
  bind lease issuance, takeover and recovery to authenticated ownership.
- UUIDv7 operation IDs and idempotency payload digests prevent duplicate
  logical operations; they **do not authenticate** the caller. Status-lookup
  visibility must be access-controlled before multi-user deployment.
- Shared branch UUID preservation does not grant access to its ref: the
  repository-qualified target and authenticated project/branch rights must
  be checked before Fetch, provisioning or Publish.
- Explicit three-way merge needs audit provenance for source parents, human
  conflict resolutions, model validation and publication authorization.

Exact roles, credential storage, authentication, revocation, tenancy, quotas,
and possible security-specific review criteria remain open design questions.
They are not prerequisites for isolated developer/test deployments and are not
assigned to any current VC milestone.

## Why this backlog is separate

- The existing `AccessPolicyStore` and `aowis-dvmdb-access` are provisional
  native policy utilities; they do not enforce authentication at the REST
  boundary. Do not build new GUI authorization features on that assumption.
- The `private`, `shared` and `unconfigured` repository roles from VC-03
  describe intended repository use, not access rights. Repository UUIDv7,
  branch UUIDv7, SHA-256 digests and tracked remote identities cannot stand
  in for credentials or verified service/user identities.
- VC-04 leases and fencing tokens are correctness mechanisms for concurrent
  drafts. VC-05/VC-07 idempotency keys are correctness mechanisms for retries.
  Neither grants a user permission to perform an operation.
- A future design must choose how native library users, local Standalone
  storage, desktop HTTP clients, browser clients and shared services receive
  consistent security policy. No policy, identity provider or secret format
  is committed by the current project.
- Keep future security tests and deployment approval requirements in a
  **separately authorized plan**, not the VC-11 functional qualification
  matrix. Completing the current version-control roadmap neither provides
  nor claims internet-facing/multi-user security.

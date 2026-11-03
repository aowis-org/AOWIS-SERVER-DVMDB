# Private working copies and local Commit

A **working copy** (draft) is one editor's uncommitted model state inside a
private DVMDB: an exact project, branch and base revision (null for an unborn
branch), a candidate overlay of changed model records and the MODEL event
journal that produced it. Saving a draft **never creates a revision or moves a
branch HEAD**; only a local Commit does. The product rules are in the
[version-control contract](version-control-semantics.md) and
[private working repositories](private-working-repositories.md); the bytes are
in [storage formats](storage-formats.md#private-drafts-aowis-working-copy-4).

Drafts exist only in a catalogue whose repository role is explicitly set to
`private` ([repository tracking](repository-tracking.md)). The role is
deployment configuration, not access control.

## Lifecycle at a glance

| Step | Native (`ProjectService`) | HTTP (`/dvmdb/v1`) |
|---|---|---|
| Create at the current HEAD | `createWorkingCopy`, `createWorkingCopyForBranch` | `POST /working-copies` |
| Read | `workingCopy(id, includeJournal = true)` | `GET /working-copies/{id}` |
| List (paged, meta only) | `listWorkingCopies(projectId, limit, after)` | `GET /working-copies?limit&after&projectId` |
| Take or renew the writer lease | `acquireWorkingCopyLease` | `POST /working-copies/{id}/lease` |
| Save model changes | `saveWorkingCopyChanges` | `POST /working-copies/{id}/save` |
| Save MODEL events + changes | `saveWorkingCopyTransactions` | `POST /working-copies/{id}/save-events` |
| Create the first network (unborn draft) | via `saveWorkingCopyChanges` | `POST /working-copies/{id}/initialize-network` |
| Read one hydraulic network | — | `GET /working-copies/{id}/networks/{networkId}` |
| Summarize saved HEAD network(s) | native candidate/materialization | `GET /working-copies/{id}/networks/summary` |
| Import network into untouched unborn draft | private saved draft import | `POST /working-copies/{id}/import-network` |
| Release the lease | `releaseWorkingCopyLease` | `POST /working-copies/{id}/release` |
| Discard | `discardWorkingCopy` | `POST /working-copies/{id}/discard` |
| Commit locally | `commitWorkingCopy`, `localCommitStatus` | `POST /local-commits`, `GET /local-commits/{operationId}` |

Request and response fields are in the [OpenAPI contract](dvmdb-http-api.yaml).
Generations, lease expiry and other 64-bit values are **decimal strings** in
JSON, so they survive clients that parse numbers as doubles.

Import is only for an untouched, empty, unborn private draft; it does not
replace a populated model. A `409` on repeat import protects the existing
network. Use the draft-scoped summary when showing what will reopen after a
Commit, since the base revision may now contain all the network data.

## Creating a draft

A draft starts at the branch's **current** local HEAD: the request names the
branch by UUIDv7 and states the expected base (`baseRevision: null` for an
unborn branch). Any other base is a conflict. A caller-chosen `id` makes
creation retry-safe: repeating the request returns the existing draft in its
current state; the same `id` with another project, branch, base or session is
a conflict. A new draft has generation 0, no lease and an empty candidate.

## Leases, fencing tokens and generations

Only one session writes a draft at a time:

* `lease` takes an exclusive lease for 1–120 seconds and returns a fresh
  fencing token, the expiry (Unix epoch ms) and the current generation. The
  holder may renew; every acquisition rotates the token. Another session gets
  the lease only after expiry or release; nothing in the draft is lost.
* Every write (Save, event Save, discard, Commit) must present the lease
  session, its current token, a live lease and the exact current
  **generation**. Anything stale is `409 Conflict` before the draft changes.
  A session that took over an expired lease is the writer and may also Commit.
* Each successful write increments the generation.
* While the granting process runs, a lease ends on a monotonic deadline, so
  wall-clock steps neither shorten nor extend it. After a restart only the
  persisted wall-clock expiry is known.
* Releasing a lease that is no longer held succeeds (a resent release is
  harmless); a lease held by someone else is never released.

Leases and tokens prevent accidental concurrent overwrites. They are **not**
authentication: anyone who can reach the service can read the session IDs.

## Saving

`save` merges up to 128 typed model changes (`uuid` + `delete: true`, or
`uuid` + `kind` + `references` + `payloadBase64`) into the candidate overlay
and returns the draft. Duplicate keys in one batch are invalid. The candidate
holds at most 20,000 changed entities and 8 MiB of encoded records; beyond
that the Save is `413` and changes nothing. A draft that already has a MODEL
journal accepts only event Saves.

`save-events` is the editor's autosave. It persists canonical MODEL edit
frames (`events`, base64) together with the candidate changes they produce,
atomically:

* Event UUIDs are the MODEL transaction UUIDv7s and must be new to the draft.
* The events must describe the changes exactly. Applied in order, each edit's
  before-image must equal the entity's current state (an earlier edit of the
  batch, else the candidate, else the pinned base), and every edited entity's
  candidate record must be its final after-image (a tombstone for a removal).
  The HTTP route decodes the frames with MODEL's transaction codec; native
  callers of `saveWorkingCopyTransactions` must pass consistent data.
* An event that creates or removes a hydraulic entity requires a `membership`
  item (`networkId`, `entityId`, `kind`, `collectionIndex`, `inserted`). DVMDB,
  not the client, rewrites the affected 256-member chunks and the network
  root in the same Save. The owning network must already exist in the base or
  the candidate. A MODEL root-metadata edit **may accompany** typed entity
  and membership changes in the same logical event: after materializing
  membership, the server applies metadata to the resulting root manifest,
  emitting one final atomic candidate change set. Unjournaled extra typed
  changes, duplicate root edits and stale before-images are rejected.
* A retry of the **immediately preceding** identical request (same
  fingerprint, generation one ahead) returns the earlier result instead of
  saving twice. The response lists only this Save's accepted event UUIDs and
  the new total `eventCount`.
* The journal holds at most 4,096 events and 8 MiB. A full journal is `413`
  until a local Commit empties it. This is an **active draft Save journal**,
  not an indefinitely retained Undo/Redo action-history archive.

A Save appends only its own events and rewrites the candidate file only when
the overlay changes; leases, releases and listings touch only the draft's
small `meta` record. The response is sent after everything is fsynced.

`initialize-network` creates the first, empty hydraulic network in an
untouched unborn draft: it writes a server-built network root through the
fenced Save path and leaves the branch HEAD null. Repeating it at exactly the
next generation with the same root returns the saved draft; anything else is
a conflict. Later `save-events` calls add members to that network.

## Reading

`GET /working-copies/{id}` returns the draft with its candidate overlay and
journal. The listing returns metadata only (no candidate, no journal, never a
fencing token), in draft-UUID order, paged with `limit` (1–1000), `after` and
an optional `projectId`; `nextCursor` is null on the last page.

`GET /working-copies/{id}/networks/{networkId}` rebuilds one complete hydraulic
network from the immutable base plus one consistent candidate generation,
validates membership, payloads and references, and returns the canonical MODEL
bytes with the generation they belong to. It takes no lease and changes
nothing.

Readers take no lock. Data files named by a `meta` record stay in place until a
newer `meta` is durable, so a read always sees one complete Save.

## Discarding

`discard` removes a draft for good, fenced like a Save. Committed revisions and
local Commit records are unaffected. Repeating it after success answers `404`.

## Local Commit

`ProjectService::commitWorkingCopy(operationId, draftId, sessionId,
fencingToken, expectedGeneration, expectedLocalHead, metadata)` turns a saved, dirty
candidate into one new revision on the draft's branch in the same private
repository. The operation is available in native code and at `POST /local-commits`;
`GET /local-commits/{operationId}` reports the recorded outcome. The GUI
HEAD Inspector exposes it when a saved modified private draft is available.

1. The caller chooses the operation UUIDv7. The request (draft, writer,
   generation, expected HEAD — explicit null for the first commit — and the
   author and message) is fingerprinted; reusing the ID with a different
   request is a conflict.
2. The draft must be dirty, its base must equal `expectedLocalHead`, and the
   writer must hold the live lease and token at that generation.
3. A `pending` ledger record with the frozen candidate, a reserved revision
   UUIDv7 and commit time, and the author and message is fsynced **before**
   any revision object is written. A retry therefore publishes a byte-identical
   revision record, whenever it runs.
4. The revision is published with an exact compare-and-swap on the branch
   HEAD (null is a real expectation, not "any"), and the outcome
   (`succeeded`, or `rejected` with `conflict`, `invalid-input` or
   `not-found`) replaces the pending record.
5. On success the draft continues from the new HEAD: base = the committed
   revision, empty candidate and journal, generation + 1, lease kept.

A replay with the same operation ID returns the recorded outcome and never
creates a second revision. `localCommitStatus(operationId)` reads the outcome
and resolves a `pending` record from the branch alone: the reserved revision is
reachable from HEAD → `succeeded`; HEAD moved without it → `rejected`
(`conflict`), since it can no longer apply; the branch is gone → `rejected`
(`not-found`); HEAD still at the expected base → still `pending`, and the next
Commit replay applies it. A crash between outcome and draft update heals on
the next replay or status lookup.

**Commit validity boundary:** `Repository::commitModel` enforces canonical
records, native index and MODEL reference consistency. Missing/wrong-type
hydraulic references currently fail these checks; relaxing that rule is
undecided. A network without a water source, with disconnected components
or an unsuccessful hydraulic simulation is not prohibited **solely** for
hydraulic feasibility. No full EPANET solver-success validation is required
by local Commit. The active draft candidate and event journal are **cleared
on successful Commit**; earlier individual edit steps are not retained as a
permanent History subsystem. See [History roadmap](history-roadmap.md).
Actual cross-process recovery acceptance remains work in the
[remaining integration roadmap](roadmap.md).

## Limits

| Limit | Value | Exceeded |
|---|---|---|
| Lease duration | 1–120 s | 400 |
| Changes per Save request | 128 | 400 |
| Events per Save request | 128 | 400 |
| Candidate entities | 20,000 | 413, draft unchanged |
| Candidate size | 8 MiB encoded | 413, draft unchanged |
| Journal events | 4,096 | 413 until a Commit |
| Journal size | 8 MiB | 413 until a Commit |
| Save request body | 8 MiB | 413 before the body is read |
| Listing page | 1–1000 | 400 |

# Durable private working copies

A working copy is a private, uncommitted candidate tied to an exact logical
project UUIDv7, branch UUIDv7 and base revision UUIDv7 (or null for an unborn
branch). Creating or saving it **never moves committed HEADs or creates a
revision**. Each draft has its own UUIDv7 and originating session UUIDv7.

Only repositories explicitly configured with functional role `private` may
create or save drafts. This role is deployment metadata, not authentication
or permissions.

## Native lifecycle

`ProjectService` provides `createWorkingCopy`, `workingCopy`,
`listWorkingCopies`, `acquireWorkingCopyLease`,
`saveWorkingCopyChanges` and `releaseWorkingCopyLease`.

An editor must explicitly acquire an exclusive writer lease, providing its
session UUIDv7 and a duration of 1–120 seconds. The result contains a fresh
fencing token UUIDv7, expiration as Unix epoch milliseconds, and the current
draft generation. Only the active session, with the exact token and expected
generation, may save a batch. Reacquiring a lease rotates the token. Another
session may take over **only after expiry or explicit release**. Expiration
never deletes an unsaved or saved working copy. A stale token, expired lease
or old generation is rejected with Conflict before any candidate is modified.

`saveWorkingCopyChanges` accepts a batch of typed `ModelChange` objects:
entity UUID key, optional `ModelRecord` for create/update, or null for delete.
The batch must not contain duplicate entity keys. A Save merges the entire
batch into the accumulated candidate overlay, stores the new generation and
dirty flag durably, and acknowledges only after publishing the complete new
record and syncing its directory. Reading the draft yields the full overlay.
An empty batch still increments the generation but leaves the overlay intact.
The validation state remains `unvalidated`: domain validation and loading a
complete model from the pinned base still require integration with the editor.

## On-disk format

Each published draft is a directory `working-copies/<draft-uuidv7>/` holding
at most three files. Earlier pre-release formats (including the single-file
`AOWIS-WORKING-COPY-3` record) are rejected as unsupported, never migrated.

* `meta` — a small text record, the only authoritative file. It is replaced
  atomically on every change: written to `.meta-<uuidv7>`, fsynced, renamed
  over `meta`, then the draft directory is fsynced.
* `candidate-<uuidv7>` — the candidate overlay, an immutable binary file.
  A Save that changes the overlay writes a new file before publishing the
  meta that names it; the superseded file is removed afterwards.
* `journal-<uuidv7>` — the MODEL event journal, append-only. A Save appends
  its own frames (after truncating any unacknowledged tail), fsyncs, then
  publishes the meta with the new length and digest. Earlier frames are
  never rewritten.

A lease, a release, a discard check and a listing touch only `meta`. A Save
writes the candidate only when the overlay changes and appends only its own
events, so the cost of a Save no longer grows with the journal.

`meta` fields, in strict order, tab-separated:

```text
AOWIS-WORKING-COPY-4
project\t<project-uuidv7>
branch\t<branch-uuidv7>
base\t<revision-uuidv7-or-dash>
session\t<originating-session-uuidv7>
generation\t<decimal-uint64>
dirty\t<0-or-1>
validation\tunvalidated
lease-session\t<session-uuidv7-or-dash>
lease-token\t<token-uuidv7-or-dash>
lease-expires\t<decimal-unix-epoch-ms-or-zero>
candidate\t<file-or-dash>\t<changes>\t<bytes>\t<sha256-of-file-or-dash>
journal\t<file-or-dash>\t<events>\t<acknowledged-bytes>\t<chain-digest-or-dash>
last-save\t<last-batch-sha256-or-dash>
```

Candidate file: `AOWC` 0x01, then per change, strictly sorted by entity key:
the 16-byte key, a big-endian u32 length (`0xffffffff` = deletion), and the
native typed-record encoding (`AOMD` version 1). Journal file: per event, the
16-byte event UUIDv7, a big-endian u32 length and the exact MODEL frame. The
journal digest chains SHA-256 over the frames: `d0` is 32 zero bytes and
`d(i) = SHA-256(d(i-1) || frame(i))`. Readers check the candidate digest, and
a full read verifies every frame and the chain. The candidate and the journal
are each bounded to 8 MiB; a draft holds at most 20,000 changed keys and 4,096
events. Exceeding a bound is `LimitExceeded` (HTTP 413) and changes nothing.

Readers take no lock. A data file named by a meta stays in place until a
newer meta is durable, so a reader sees one complete Save; if a file was
replaced in between, the reader rereads `meta`. On crash, the old or the new
meta is authoritative and bytes past the acknowledged journal length are
ignored. Creation stages `.creating-<uuidv7>/` with its meta and renames it
into place; discard renames the draft to `.deleting-<uuidv7>` before removing
it. Opening the catalogue removes those directories, stray `.meta-*` files and
unreferenced data files. If publication is uncertain, the service refuses
further writes until restart and inspection. Offline backup/restore verifies
every meta, candidate and journal and compares their acknowledged content.

## Private HTTP draft operations

The server exposes the native draft lifecycle through these routes (all under
`/dvmdb/v1`): `GET/POST /working-copies`, `GET /working-copies/{draftId}`,
`POST /working-copies/{draftId}/lease`, `POST /working-copies/{draftId}/save`
and `POST /working-copies/{draftId}/release`. Drafts are allowed only when the
DVMDB repository is configured with functional role `private`; this role is
not a user permission or a login.

The create request requires `projectId`, the existing branch's `branchId`
UUIDv7 (never its renamable name), originating `sessionId` UUIDv7 and explicit
`baseRevision` (null for unborn). An optional caller-chosen draft `id` makes
creation idempotent: repeating the request returns the same draft, and the id
naming a draft of another project, branch, base or session is 409. Releasing
a lease that is no longer held succeeds (a resent release is harmless); a
lease held by another session or token is never released. The Save
request requires session UUIDv7, current fencing UUIDv7, decimal-string
`expectedGeneration` and 1..128 changes. Generation and lease-expiration
values are represented as **decimal strings**, preserving the complete uint64
value without JSON floating-point rounding. Model changes use the same native
model-record representation as the existing model-commit endpoint. Read/list
responses include lease-holder status, **not** the active fencing token.
The list response omits candidates; `GET /working-copies/{draftId}` returns
the full saved candidate overlay. Only a successful lease acquisition returns a token.
Reopening after restart uses the same persisted draft UUID; stale generations
and stale tokens are rejected, never silently rebased. No draft operation
changes a committed HEAD or creates a revision.

## Durable local Commit coordinator (VC-05 native foundation)

`ProjectService::commitWorkingCopy(operationId, draftId, sessionId,
fencingToken, expectedGeneration, expectedLocalHead)` snapshots a saved,
nonempty candidate from a private repository only. It requires the exact
fenced writer, generation and pinned draft base (including explicit null for
an unborn branch). The target branch is resolved by its **UUIDv7**, not its
renamable label. `localCommitStatus(operationId)` reads the authoritative
result and reconciles an interrupted publication against actual committed
ancestry. This is a **native C++ API**, not yet an HTTP route or GUI button.

Each caller-supplied operation UUIDv7 names one permanently retained record in
`local-commits/` with format `AOWIS-LOCAL-COMMIT-1`. It contains the repository,
project, branch and draft UUIDs, generation and expected local HEAD/null,
request and payload SHA-256 digests, a frozen sorted snapshot of candidate
records, a reserved **revision UUIDv7** and one of `pending`, `succeeded`, or
`rejected`. The request fingerprint includes the original fencing token, but
the token itself is not stored. A repeated ID with changed request fields is
a conflict, while an identical replay returns the same terminal outcome.
The committed immutable revision and branch HEAD are never rolled back merely
because the status reply or persistence fails.

The full pending intent and reserved revision UUID are fsynced before any
revision publication. Immutable revision publication may be retried with that
**same UUIDv7** and the same frozen candidate, so a crash after writing a
revision binding but before advancing HEAD cannot create another revision.
After a crash following the HEAD update but before the success status update,
status recovery checks that the reserved revision is in the **target branch's
committed ancestry**. An orphan immutable revision is not mistaken for a
successful Commit. If HEAD has advanced in an ambiguous way, the operation
remains pending and requires manual resolution rather than a blind replay.
No pending operation is silently discarded. Success rebases the draft onto the
committed revision (empty candidate and journal, generation + 1, lease kept);
a stale or rejected Commit never changes the candidate.

**Remaining integration gate:** the native coordinator currently checks the
MODEL record/index constraints enforced by `Repository::commitModel`, but
does **not yet run the complete canonical hydraulic-network/domain validator**
on the assembled candidate. It must not be wired directly to GUI Commit until
full hydraulic validation is implemented, the HTTP transport exposes the
status/retry contract, and end-to-end GUI/Standalone acceptance passes.

## Integration boundary

The native and HTTP Save paths are durable and fenced, but do not yet implement
historical checkout, materialization/verification of the entire base model,
semantic validation of the assembled hydraulic candidate, or the GUI editor
workflow. A draft can be discarded (`POST /working-copies/{draftId}/discard`,
fenced like a Save). The native local Commit coordinator and durable
operation recovery exist, but full hydraulic validation, HTTP transport and
GUI acceptance have not yet been completed.

## Provisioning a first-clone project for committed revisions

`POST /dvmdb/v1/projects/clone-target` accepts a source logical project
UUIDv7 (`projectId`) and display `name`. It publishes an empty catalogue
project with **that exact UUIDv7**; it creates no local branch or commit.
Repeated provisioning with the same identity and name is idempotent;
a different name for the same existing UUID is a conflict. The personal
repository's own root UUIDv7 remains independent of this logical project
UUIDv7. Following provisioning, a verified full revision bundle can create
the destination branch with its source branch UUIDv7 and immutable history.
The first-clone process does not alter source HEADs or create a draft.
Unborn branch transfer cannot use a nonempty revision bundle and requires
an explicit UUID-preserving unborn-branch operation.

## Atomic MODEL edit journal

`POST /dvmdb/v1/working-copies/{draftId}/save-events` accepts the active
writer session and fencing token, decimal-string `expectedGeneration`, one or
more base64-encoded canonical MODEL editing transactions in `events`, and the
corresponding typed `changes` array. The existing candidate and new journal
entries are published by **one fsynced meta replacement**. A successful
response acknowledges both; it does not create a revision or move HEAD. The
response lists the accepted event UUIDs of this Save and the new `eventCount`.

Each event preserves its MODEL UUIDv7 and original bytes, in user
operation order. `GET` returns both the candidate overlay and the journal;
`GET` list returns only metadata including `eventCount`. The last event-save
request is fingerprinted for an idempotent retry at its original generation.
Replaying an older batch or changing an existing event UUID is a conflict.
An unjournalled `/save` operation is rejected once a journal exists.

The native service verifies the MODEL frame size, SHA-256 and intrinsic UUIDv7;
the Qt-enabled HTTP route additionally runs MODEL's complete transaction
decoder. The native C++ caller must supply MODEL-validated event bytes and
model-record changes that correspond to those events. The membership-aware HTTP path validates canonical member record payloads
and event before/after bytes for insertion and deletion; this is **not** a
complete field-by-field reconciliation of all ordinary update events. The journal
is bounded to 8 MiB and 4,096 events; a local Commit empties it. This is the
first atomic acknowledgement boundary, not the complete undo/redo subsystem.

## Hydraulic membership in atomic event saves

When a MODEL transaction creates or deletes a hydraulic entity, the GUI adds
`membership` to the same `/save-events` request. Each item contains
`networkId`, `entityId` (canonical UUIDs), `kind` (native ModelRecord kind),
`collectionIndex` (0..17 in the established hydraulic collection order), and
`inserted` (boolean). The request includes the matching ordinary entity
creation/deletion in `changes` and the exact reversible MODEL before/after
image in `events`.

DVMDB checks the event image against the typed model payload or the pinned
candidate's existing entity. It computes the native membership-chunk and root
updates itself. Only affected collections are read, and only modified chunks
and necessary root metadata are placed into the resulting candidate overlay.
All generated records, original transactions and entity changes are published
in a **single generation-checked durable draft update**. A failed
materialization leaves the draft unchanged. The request fingerprint is derived
from canonical JSON of the original logical request, not the derived chunk
records, so the immediately preceding identical request can be acknowledged
after a lost response without reapplying its membership changes.

A request whose events create or remove any entity **without** a
`membership` array is rejected with 400 (`missing membership for created or
removed entity`) before the draft is read; only attribute updates may omit
it. Otherwise the candidate could hold an entity record that no membership
chunk lists, or a chunk that still lists a removed entity. This check needs
the MODEL decoder and therefore exists in the hydraulic-enabled HTTP build;
native C++ callers of `saveWorkingCopyTransactions` must uphold it
themselves until a native membership-aware draft API exists.

This endpoint intentionally requires the owning hydraulic network to exist in
the pinned base or existing candidate. Creating a new network root, editing
network-wide metadata, and committing candidate overlays are separate tasks.
The HTTP request is JSON-framed for the existing API; canonical MODEL entity
and event payloads remain binary, encoded as base64 for transport. No ZIP
containers, compatibility reader, or new persisted format are introduced.

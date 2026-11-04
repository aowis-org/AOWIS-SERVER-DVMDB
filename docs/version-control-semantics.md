# DVMDB version-control semantics — current product contract

This page describes the **native data model and implemented service
behavior**. Operations that still require complete workflows (whole-project
Fetch, recoverable Publish, cross-Commit Undo/Redo) are **not** implied to be
available merely by the existence of revision bundles or edit events; see
the [remaining roadmap](roadmap.md).

## Objects and identities

| Object | Identity | Behavior |
|---|---|---|
| Repository/storage root | Repository UUIDv7 | Stores multiple projects, local/shared role, tracking info and draft catalogue |
| Project | UUIDv7 | Owns named branches and committed revision graph |
| Branch | UUIDv7 and editable name | Nullable committed HEAD, immutable creation provenance; can be unborn |
| Committed revision | UUIDv7, SHA-256 record hash | Immutable snapshot; zero, one or two **ordered** parent revisions; author, commit message/time |
| Branch committed HEAD | Revision UUIDv7 or `null` | Movable ref updated atomically by Commit/merge; `null` means unborn |
| Working copy (draft) | UUIDv7 | Private mutable candidate based on a pinned committed revision/null, with lease, generation and journal |
| MODEL entity | MODEL UUID | Canonical typed record identity; not the revision's SHA-256 |

New projects created with `unborn: true` have a real `main` branch but no
committed revision. Additional independent unborn branches can be created
without inventing a source or synthetic revision. A new branch from an
existing immutable revision records its origin branch (if specified) and
fork revision; creating the branch is **not a Commit**. Revisions can be
shared ancestors of many branches and are stored only once. A revision's
parent edges represent actual immutable ancestry, not branch labels or
branch-creation metadata.

DVMDB's **branch HEAD** means the latest committed revision/null. The GUI
**HEAD card** is an editable working-tip representation and is not a revision
vertex. The branch label may change; its stable UUID never does. UUIDv7
ordering must never be mistaken for ancestry order. The SHA-256 record hash
binds canonical revision identity, ordered parents, snapshot roots,
author/message/time and reference-integrity roots; it is not an object ID
or authentication proof.

Revision **display-message annotations** can be edited separately from the
hashed original commit message; this does not mutate immutable revision
bytes. HEAD/Revision GUI Tags and Description currently share a
branch-metadata persistence path rather than separate committed annotations.

## Draft Save and local Commit

Every editable model is in a **personal/private DVMDB working repository**.
The same project/branch UUID may exist in shared and private repositories,
but each repository has its **own committed branch HEAD ref**. A private
draft has a pinned committed base and a generation-checked, fenced writer
lease; autosaved changes update its candidate overlay and MODEL journal,
**not** the branch HEAD. A saved unborn draft may contain a complete model
before any immutable revision exists.

`commitWorkingCopy()` is a separate, idempotent local operation. It requires
an exact expected local HEAD (including explicit `null`), modified/saved
draft, current generation, live writer lease and a nonblank commit message.
DVMDB records the caller's operation UUIDv7 and frozen request, reserves a
revision UUID, publishes a durable immutable revision with branch-HEAD CAS,
then rebases the draft on that new revision. The draft's previous candidate
and **active event journal are cleared** on successful rebase. Status is
available at `GET /local-commits/{operationId}` for uncertain replies.
A local Commit **does not Publish** the result to a shared repository.

Each successful local Commit retains its accepted MODEL event frames in a
**permanent revision-indexed action archive**, even though the draft's active
journal is cleared. Read-only branch timelines and original action pages are
available over HTTP. Native C++ can verify and atomically select a fully
qualified archived hydraulic action boundary in a private draft, with a V6
cursor; **continuous Undo/Redo for current draft actions and HTTP/GUI execution
are not implemented**. See [current History](commit-action-archive.md) and
[remaining work](history-roadmap.md).

## Replication and branch relationships

The native project API supports verification, content-addressed bundles,
project/branch provisioning (including unborn checkout), revision graph
queries, generic merges and hydraulic-aware merges. Full/incremental
bundles transfer immutable history while retaining revision UUID/hash and
parent ancestry; import rejects incorrect hashes, incomplete ancestry or
identity mismatch. A branch HEAD of another repository is a **different
ref**, even if both branches have the same UUID. An absent branch differs
from a present unborn (null-HEAD) branch.

A complete GUI-driven **Fetch** with persistent remote tracking and
**Publish** with durable remote operation/status and expected-HEAD CAS are
not completed product workflows. Likewise, existing native merge APIs do
not yet mean the complete GUI compare/conflict/reconcile interaction exists.
Never silently force-update a remote HEAD, infer branch identity from names
or discard a dirty local draft.

## Integrity versus engineering validity

Canonical MODEL/record bytes, revision hash and parent closure, unique
object identity, membership/reverse-reference indexes, lease/fencing and
expected generation/HEAD are mandatory. Current MODEL/native rules reject
missing or wrong-type hydraulic UUID references; relaxing that is a
**separate undecided model policy**. A network may nevertheless have no
water source, disconnected components or failed hydraulic convergence:
those solver/feasibility issues do **not** prohibit saving or committing an
otherwise structurally consistent model. More details:
[model integrity](model-validity-boundary-audit.md).

## Service boundaries

Native APIs expose the same persisted repository state as the DVMDB HTTP
service (`/dvmdb/v1`). Browser and desktop clients that need a writable
draft connect to a **private** repository, not to a shared browsing source.
Standalone embeds the native service in the GUI process and uses its
normal localhost HTTP interface. Repository role and UUIDv7 pinning are
correctness/deployment mechanisms, **not authentication**; the current
HTTP service has no native authorization/TLS. See [working copies](working-copies.md),
[HTTP workflows](http-workflows.md), and [security](security-permissions-design.md).

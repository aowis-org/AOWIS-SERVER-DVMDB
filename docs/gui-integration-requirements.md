# DVMDB ↔ AOWIS GUI — implemented integration contract

This page records the **current service/client boundary**, not a deferred
feature checklist. Unfinished end-to-end features belong in the
[DVMDB roadmap](roadmap.md) and [GUI roadmap](../../AOWIS-SERVER-GUI/docs/dvmdb-integration-roadmap.md).

## Reading Projects

The GUI reads repository UUID, role and MODEL source fingerprint, catalogue,
branch/HEAD records, pinned paged revision DAG and immutable revision
metadata. A project may have an **unborn** `main` with `headRevisionId:
null` and zero revisions. A revision can be reachable from several branches
and must appear only once in a graph. The GUI distinguishes the committed
branch HEAD from its editable HEAD card; tree/graph/inspector clicks do not
mutate refs.

The saved private network summary uses the entire draft view (committed
base plus candidate overlay) and must not infer "no network" merely because
there are no pending changed records. Revision summaries are pinned to an
immutable revision ID. An unavailable repository or bad summary is a status
error, **not** evidence that saved content was deleted.

## Writing and recovering

1. A user designates a verified ♛ private source. Embedded Standalone still
   exposes the usual DVMDB HTTP routes via a dynamic localhost endpoint.
2. For ordinary editor work, the client obtains or creates a private draft,
   verifies its project/branch/base/generation, acquires a fenced writer,
   and installs the saved model. It never writes into a shared source as a
   fallback.
3. GUI `HydraulicData` emits canonical MODEL transactions for supported
   actions. Client queues each event durably, sends typed records,
   membership intents and optional root metadata edits via `save-events`,
   and only acknowledges a confirmed accepted event/generation. DVMDB
   owns and materializes physical hydraulic root/chunk records.
4. On ambiguous Save result, reconcile authoritative journal and candidate.
   On unresolved writer/target mismatch, block new writes without erasing
   the queue or displaying a false Autosaved state.
5. On local Commit, supply exact expected local HEAD/null, live fenced
   session/generation, nonblank message and caller operation UUIDv7. The
   GUI persists its Commit intent before POST, uses DVMDB status lookup,
   and does not submit a new operation ID while that intent is unresolved.
6. Metadata writers verify the selected repository UUIDv7/role/MODEL
   fingerprint before calling mutable project/branch endpoints; a pinned
   expected-repository header is checked by DVMDB at mutation time.

## Existing network and history boundaries

The Inspector can open a selected writable HEAD, open an immutable Revision
read-only or by explicitly branching, import a first network into an
untouched empty unborn private draft and export a pinned saved network.
General replacement/selected-entity import is not implemented. Branch
checkout and immutable revision bundles exist; whole-project Fetch and
Publish are not yet operational GUI flows.

`GET /revision-graph` exposes the immutable revision DAG. DVMDB's
`action-history` and revision events routes expose **archived original MODEL
actions across local Commit markers**, alongside the bounded live-draft
journal; readers can page both without changing HEAD or draft state.
Native code has a persisted V7 cursor for **restricted, verified archived
action-boundary selection**. There is **no HTTP action-navigation endpoint**,
continuous active-draft Undo/Redo or completed GUI controller. The existing
GUI left History dock is therefore **read-only**, with Undo/Redo disabled.
See [current History](commit-action-archive.md) and
[remaining History work](history-roadmap.md).

## Validation and tests

The actual codec, writer and revision APIs enforce canonical record values,
object-reference integrity, generation/HEAD CAS and UUID identity, not
hydraulic solver convergence. A disconnected or unsupplied network can be
committed if referentially consistent. Missing/wrong-type hydraulic UUID
references remain rejected unless a separate policy decision changes it.

Run GUI and DVMDB source/unit regressions, then actual external HTTP and
embedded Standalone process-restart and lost-acknowledgement scenarios.
A simulated GET/POST fixture or an API method existing in C++ is not
proof of full deployment acceptance. See [remaining work](roadmap.md).

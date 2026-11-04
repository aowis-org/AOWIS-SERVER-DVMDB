# DVMDB — remaining integration and release roadmap

This document lists **unfinished work only**. It is not a record of old
patches, implementation stages or obsolete storage variants. For current
behavior start with the [documentation index](README.md),
[working copies](working-copies.md), [hydraulic persistence](native-hydraulic-network.md)
and [HTTP workflows](http-workflows.md).

DVMDB is pre-release. A native C++ implementation, a passing unit fixture and
an accepted end-to-end GUI/Standalone workflow are **three different things**.
Do not mark a delivery gate complete without recording its test evidence.

## Delivery priorities

| Order | Outstanding deliverable | Definition of done |
|---|---|---|
| 1 | **Qualify hydraulic Save and local Commit** | All 18 typed collections and root options round-trip exactly through real HTTP + GUI + Standalone, across restart and lost responses |
| 2 | **Finish continuous History/Undo/Redo** | A user can Undo, Redo and jump through accepted draft actions and archived Commit markers with durable cursor, correct redo invalidation and restart recovery |
| 3 | **Atomic large edits and import/replacement** | Network creation/replacement and larger logical actions are fully fenced, idempotent, recoverable and never partially visible |
| 4 | **Qualify the private project lifecycle** | Project/unborn branch → saved draft → local Commit → rebase/reopen/branch, including multiple sessions and ambiguous-result recovery |
| 5 | **Whole-project Clone/Fetch** | Verified complete or incremental project history transfer and durable remote observations without moving local branch HEADs or discarding drafts |
| 6 | **Recoverable Publish** | Verified transfer plus expected-remote-HEAD CAS with durable operation status; safe first remote branch provisioning |
| 7 | **GUI reconciliation/Merge** | Pinned base/local/remote refs, explicit conflict choices and correct two-parent result without draft loss |
| 8 | **Release qualification and scale** | Disk-full/power-cut/soak/backup evidence, large histories and graphs, platform-specific acceptance and clear operational recovery |

## 1. Hydraulic persistence and local acceptance

**Already implemented:** canonical MODEL serialization, indexed physical
records, server-owned hydraulic root and membership chunks, 18 collections,
root metadata edits, mixed MODEL events, fenced `save-events`, saved-state
reads and local Commit/reopen. Solver convergence is deliberately **not** a
Commit precondition.

**Remaining:** compare complete `ModelCodec<NetworkHydraulic>` bytes—not just
counts—after external HTTP Save, Commit, GUI reopen and persistent embedded
Standalone restart. Exercise multi-network mixed edits, membership chunk
boundaries, stale before-images, lease/generation errors, network import,
corrupted frames, queue overflow, interrupted requests and recovery.
A test passing against an in-process fake is not sufficient.

## 2. History across Commit boundaries

**Already implemented:** durable indexed archives of original MODEL events,
paged native/HTTP read-only action history, native full-revision coverage
verification, checkpoint planning/selection and native atomic selection of a
verified archived hydraulic action boundary with a V7 draft cursor,
plus a MODEL-verified active-journal navigator for one existing network.
An atomic event Save after Undo invalidates the redo tail safely. The HTTP
server now exposes fenced selection of a verified **active single-network**
draft action boundary, plus the authoritative cursor read. This is not yet
continuous cross-Commit Undo/Redo and the GUI dock remains read-only.

**Remaining:** confirm the corrected Qt hydraulic navigation regression;
support navigation of the normal unborn-base draft, connect continuous
cross-Commit cursor and navigation-event provenance, qualify redo invalidation
through HTTP and wire the GUI History dock. Multi-network navigation can be
deferred without restricting what DVMDB stores. Imported/merge/checkpoint-only revisions
must stay honest; do not synthesize edits from snapshot differences. See the
[dedicated History roadmap](history-roadmap.md) for exact acceptance gates.

## 3. Safe bulk operations

The current journal and per-request candidate budgets are intentional
resource limits, not a general bulk replacement interface. First import
into an untouched empty unborn draft is supported; replacing a populated
network or importing selected libraries/entities remains separate work.

Design coherent logical operations that may be large but are **atomic** at
publication, lease/HEAD/generation checked, UUID/reference validated and
recoverable by operation ID. Avoid exposing intermediate candidate states.
Record each logical bulk edit as one meaningful History action when its
MODEL representation is complete; never claim Undo support for an
unjournaled import.

## 4. Private project and Commit lifecycle

Qualify designation of a personal repository, identity-preserving source
checkout, independent unborn branches, draft creation/discovery and leases,
Save → local Commit → draft rebase → next Save, and branch-from-committed
revision. Verify UUID stability, exact nullable-HEAD semantics, restart and
status-lookup recovery, multi-session fencing and operation UUID reuse.
Saved network content must survive both an external server restart and
embedded Standalone process restart.

## 5. Clone/Fetch

Verified full/incremental revision bundles, remote-tracking storage and
project/branch provisioning primitives exist. What is missing is an
end-to-end **whole-project** workflow that pins the source repository and
MODEL fingerprint, transfers full immutable ancestry (including two-parent
revisions), preserves project/branch UUIDs, updates remote observations and
recovers interrupted transfers. An absent remote branch is not an unborn
branch. Fetch must not move the private local committed HEAD or overwrite a
dirty draft.

## 6. Publish

Transfer immutable objects first, verify full closure, then conditionally
advance the shared branch HEAD against the **exact expected remote HEAD**
(null where appropriate). Preserve the stable branch UUID on first provision;
never match by name alone. Use a durable operation identity/request digest
and status lookup for lost acknowledgements. A stale remote ref is a
conflict, not permission to force-push, rebase or erase local work.

## 7. Merge/reconciliation

The native generic and hydraulic three-way merge primitives exist; the
full collaboration workflow does not. Fetch and pin base/local/remote
revisions; expose actual object/field/delete conflicts; apply deliberate
choices; verify an ordered two-parent immutable result while preserving
any unrelated private draft. Handle concurrent remote updates, ambiguous
merge bases, criss-cross histories and unsupported non-hydraulic changes
explicitly. Solver feasibility remains a non-blocking diagnostic; canonical
model references and storage integrity remain mandatory.

## 8. Durability, capacity and release acceptance

Re-run pack/integrity, power-cut, ENOSPC, crash during Commit/publish,
compaction, backup/restore, transfer corruption and resource/HTTP soak on
the final implementation. Exercise the 4,096-event / 8-MiB active journal,
large project graphs, repository tracking growth and multi-network edits.
History needs a deliberate archive retention/compaction policy, **not** an
unbounded journal or silent action pruning. Record exact build and runtime,
platform, duration, failures, environment and supported limits for Linux
native, external HTTP, Standalone and browser/WASM client paths separately.

## Explicit exclusions

- **No pre-first-release migration or legacy compatibility support** for
  abandoned development storage layouts. Unsupported formats fail closed.
- **No mandatory full hydraulic simulation** as a Save/Commit/History gate.
  Unique IDs, canonical values and existing model/reference integrity checks
  continue to apply.
- **No security deliverable inside this roadmap.** Authentication, TLS and
  authorization are a separate [security initiative](security-permissions-design.md).
  Until then, writable services belong on localhost or trusted isolated
  networks; repository role/UUID and fencing do not authenticate a user.

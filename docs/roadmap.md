# DVMDB — remaining implementation and acceptance roadmap

This is **only a forward-looking roadmap**. Current functionality is described
in [product documentation](README.md), particularly
[working copies](working-copies.md), [revision history](native-revision-history.md),
[hydraulic storage](native-hydraulic-network.md), and
[HTTP workflows](http-workflows.md). GUI-specific remaining work is in
[the GUI roadmap](../../AOWIS-SERVER-GUI/docs/dvmdb-integration-roadmap.md).
The cross-Commit Undo/Redo feature is **not implemented**; its data model and
rollout are in [DVMDB History](history-roadmap.md).

Latest sources reviewed: 2026-11-03. A patch in source is not a passing
regression, and a passing HTTP fixture is not a real external-server or
embedded-Standalone restart acceptance. Record evidence before marking a
future gate complete.

## Proposed sequence

| Priority | Remaining backend/integration work | Acceptance condition |
|---|---|---|
| **1** | Complete mixed hydraulic Save qualification | GUI+HTTP+Standalone full-network equality, 18 typed collections + root; crash/restart and strict mismatch rejection |
| **2** | Design and implement permanent action history | Durable paged edit history across Commit, reconstruction and history cursor without rewriting immutable revisions |
| **3** | Support safe large logical edits and import/replacement | One atomic saved action, robust queue and generation/lease checks, no partial replacement |
| **4** | Complete local project/branch/Commit acceptance | New unborn branch through draft/save/local Commit/branch/restart, durable lost-response recovery and multi-session safety |
| **5** | Implement full-project Clone and Fetch | Verified full/incremental transfer and pinned remote tracking; local branches and drafts untouched |
| **6** | Implement recoverable Publish | Missing objects first, expected remote HEAD CAS, durable operation ledger/status and safe first branch provisioning |
| **7** | Complete reconciliation/Merge workflow | Exact base/local/remote refs, explicit conflict selection, two-parent immutable result and no draft loss |
| **8** | Release durability and scale | ENOSPC/power-cut/crash/soak, large graph/list/journal, supported platform matrix and clear recovery errors |

### 1 — Hydraulic persistence qualification

**Present in source:** server-owned hydraulic root/membership chunks, indexed
entity edits, 18 typed collections, canonical MODEL event transport, root
metadata event materialization and mixed root/entity Save requests. Local
Commit creates immutable revisions and can reopen their saved networks.

**Outstanding evidence/work:** run the latest DVMDB HTTP mixed-save tests
(including new chunk, existing chunk, stale images and exact replay), then
exercise a real external service and persistent Standalone across process
restarts. Compare complete `ModelCodec<NetworkHydraulic>` images, including
root options, timing, report, energy, MSX and backdrop, not sample counts.
Test dropped responses, stale lease/generation, wrong repository/model
fingerprint and power loss. Keep the current canonical storage and
model-reference checks; **hydraulic solvability is not a Commit gate**.

### 2 — Persistent action history and cross-Commit navigation

The current draft journal is bounded and **cleared when a successful local
Commit rebases the working copy**. Immutable revisions retain model snapshots,
not necessarily individual editor actions. This is insufficient for
persistent Undo/Redo across Commit markers.

Design a separately retained action-history store linked to working copy,
branch, committed revisions and source events; support paged reads, selective
checkpoint reconstruction, durable cursor/redo eligibility, crash-safe
inverse/reapply events and explicit merge histories. A history navigation
must never edit old revision records or move a published branch HEAD.
See [history-roadmap.md](history-roadmap.md) for the entire backend contract
and [the GUI History roadmap](../../AOWIS-SERVER-GUI/docs/history-roadmap.md)
for the left dock and interaction semantics.

### 3 — Big edits, network import and replacement

Current per-Save event/changed-record limits protect the service, but can
prevent large user actions. Design a bounded **atomic** large-action mechanism
without exposing partially applied user operations. Preserve the implemented
first import into an untouched empty unborn private draft. Add explicit,
idempotent new-network, replace-existing-draft and selected-entity/library
import operations, with exact repository/draft/network identity, generation,
lease and queue preconditions, collision/reference handling and recovery.
History must treat each bulk operation as a coherent logical action.

### 4 — Complete local lifecycle and recovery evidence

Exercise private repository creation/designation, unborn HEAD, multiple
branches, checkout, network initialization, event Save, immutable local
Commit, draft rebase, read-only historical revisions and new writable HEAD.
Verify caller-chosen UUIDv7 Commit-intent replay and status after dropped
POST+status replies, crash between ledger and ref publication and a GUI
restart. Add wrong-target/idempotent metadata failure cases and stale writer
collisions. The GUI and native DVMDB must agree on UUIDs, nullable HEAD and
error categories. A test being present is not proof of end-to-end acceptance.

### 5 — Whole-project Clone/Fetch and tracking

Verified bundles and identity-preserving project/unborn-branch provisioning
exist; a complete Fetch workflow does not. Pin the source repository ID,
MODEL compatibility and branch HEAD snapshot; import all immutable objects,
including two-parent ancestry, then persist accurate remote-tracking
observations. Preserve logical project/branch UUIDs and allow absent vs
unborn remote branches to remain distinct. Fetch must not move local HEADs
or discard dirty working copies; interrupted transfer must be recoverable.
Provide the GUI with progress and authoritative result/status information.

### 6 — Publish

Use separate server-verified immutable object transfer and conditional
remote HEAD movement; first remote provision of a branch must preserve its
UUID. Every publication carries operation UUID/digest, expected remote
HEAD/null, durable status and safe exact replay after a timeout. On stale
remote HEAD, retain the entire private branch and draft and return a
conflict. No force push, silent rebase or guessed branch-name matching.
Hydraulic simulation failure must not reject an otherwise well-formed
immutable revision; storage and ancestry remain strict.

### 7 — Merge and reconciliation

Connect the existing native generic/hydraulic merge primitives to pinned
Fetch/tracking and a deliberate GUI resolution flow. Compare pinned base,
local and remote commits, present actual field/object/edit collisions,
respect delete/edit cases, preserve the dirty draft, and produce the
correct ordered two-parent revision. Hydraulic solver feasibility is a
non-blocking diagnostic; absent/wrong-type object references still follow
current model-integrity policy unless separately redesigned. Include
criss-cross/ambiguous bases and concurrent remote updates as explicit
error states, not invented automatic winners.

### 8 — Scale and release qualification

Evaluate 4,096-event/8-MiB draft journal ceiling, 128 changed-record/event
Save limits, candidate size and large project/branch graphs. Permanent
History cannot merely raise these quotas; it needs its own retention and
compaction design. Re-run native integrity verification, bundle corruption
checks, disk-full/ENOSPC, crash/power-cut tests and HTTP soak on the final
implementation. Record exact build, platform, results and skips. Linux
native and external HTTP, embedded Standalone and supported browser/WASM
paths require separate signoff.

## Deferred / excluded work

Authentication/TLS/authorization are a **separate security project**, not
an implicit prerelease pass/fail item in this integration roadmap. Keep
unauthenticated writable services on localhost or trusted isolated networks.
No pre-first-release legacy migration is required. Permitting dangling or
wrong-type hydraulic UUID references is **undecided** and must not be
silently implemented as part of non-blocking solver validation.

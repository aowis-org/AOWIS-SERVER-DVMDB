# DVMDB documentation

DVMDB is AOWIS's **native multi-project repository and HTTP service**:
immutable versioned model snapshots, UUIDv7 identity, branches, fenced
personal working copies, canonical hydraulic persistence, original MODEL
action history and verified transfer primitives. Documentation reflects the
**5 November 2026** source. Implementation and deployment acceptance are
different claims.

## Current product contracts

| Area | Documentation |
|---|---|
| Architecture, storage, concurrency | [Architecture](architecture.md), [storage format](storage-formats.md) |
| Projects, branches, revisions and unborn HEAD | [Version control](version-control-semantics.md), [revision DAG](native-revision-history.md) |
| Personal draft, Save and local Commit | [Working copies](working-copies.md), [private repositories](private-working-repositories.md) |
| Continuous Undo/Redo and action archives | [History contract](history-contract.md), [original MODEL action archive](commit-action-archive.md) |
| Sparse MODEL updates and SHA-256 recovery | [History transport/reconciliation](history-transport.md) |
| Hydraulic root, 18 collections and references | [Hydraulic persistence](native-hydraulic-network.md), [MODEL records](model-records.md) |
| Commit/working-copy integrity versus solver feasibility | [Validation boundary](model-validity-boundary-audit.md) |
| Clone/Fetch primitives, tracking and merge | [Offline synchronization](offline-repository-synchronization.md), [tracking](repository-tracking.md), [merge](native-hydraulic-merging.md) |

## APIs, deployment and validation

| Area | Documentation |
|---|---|
| HTTP operations and errors | [HTTP workflows](http-workflows.md), [REST guide](rest-api-design.md), [OpenAPI](dvmdb-http-api.yaml) |
| Server startup, backup and verify | [Service](native-database-service.md), [operations](operations.md), [repository verification](repository-verification.md) |
| GUI contract | [GUI integration](gui-integration-requirements.md) |
| Durability and power-loss | [Qualification](durability-qualification.md), [deletion power-cut](vm-deletion-powercut.md) |
| Benchmarks | [Benchmark procedures](benchmark-plan.md), [observed engineering findings](engineering-findings.md) |
| Format policy | [Storage formats](storage-formats.md), [format policy](format-compatibility.md) |

## Current History behavior

DVMDB keeps original MODEL frames in the active journal and indexes them
permanently after local Commit. Its durable cursor navigates eligible
actions across Commit boundaries without changing immutable revisions.
Navigation returns a verified **sparse change set** for the one selected
hydraulic network, with canonical source/destination SHA-256 values. The
server also provides a lightweight operation/generation/cursor/hash
reconciliation receipt and pinned previous candidate for same-UUID delta
replay after lost acknowledgements or restart. The GUI has a separate
client-side **durable pending navigation intent** for GUI process restart;
it does not require a new DVMDB storage format. Normal opening can still
transfer the complete model once.

The **personal** role is an editing designation: personal DVMDB can be local,
embedded or reachable via REST. A remote personal server does not promise
editing during a total network outage. DVMDB's working-copy limits and
single-network History-navigation scope remain explicit.

## Remaining work

- [Integration and release roadmap](roadmap.md): project checkout, Fetch,
  Publish, reconciliation/merge, large imports and platform qualification.
- [History acceptance](history-roadmap.md): live process-kill, power-loss,
  GUI/editor, Standalone and browser storage qualification; no new Undo
  engine is scheduled.
- [Security design](security-permissions-design.md): separate workstream;
  UUIDs, repository roles and writer leases are **not** authentication.

Unsupported pre-release storage formats are rejected rather than silently
migrated. Historic measurements and audits are evidence, not current feature
roadmaps.

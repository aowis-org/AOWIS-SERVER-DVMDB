# DVMDB documentation

DVMDB is AOWIS's **native multi-project, versioned model store** with a C++
service API and HTTP endpoints. The pages linked here describe behavior
present in the source snapshot of 2026-11-03. Future development is in the
[remaining roadmap](roadmap.md); no old implementation-stage chronicle is
necessary to understand the product.

## Current product contracts

| Guide | What it documents |
|---|---|
| [Architecture](architecture.md) | Store, project service, repository identity and durability |
| [Version-control semantics](version-control-semantics.md) | UUIDv7 projects, branches, unborn HEADs, immutable revisions and drafts |
| [Working copies and local Commit](working-copies.md) | Leased/fenced events, draft journal, generation, idempotent local Commit |
| [Private and shared repositories](private-working-repositories.md) | Roles, local vs shared refs, checkout building blocks, current workflow boundary |
| [Committed revision graph](native-revision-history.md) | Revision records, branch ancestry, graph reads and immutable metadata |
| [Committed actions and branch timeline](commit-action-archive.md) | Verified MODEL action archives, paged read-only Commit checkpoints and draft event UUIDs |
| [Hydraulic persistence](native-hydraulic-network.md) | 18 collections, root metadata, chunks, full/read/edit paths |
| [Model record integrity](model-records.md) | Canonical records, identities, reverse references |
| [Hydraulic feasibility and storage validity](model-validity-boundary-audit.md) | What is allowed despite solver failure versus what is structurally invalid |
| [Generic merge](native-branch-merging.md) / [hydraulic merge](native-hydraulic-merging.md) | Native APIs and current conflict/validation rules |
| [Repository tracking](repository-tracking.md) | Repository role, UUID, remote registry and tracking observations |
| [Revision bundles](offline-repository-synchronization.md) | Actual native/HTTP/CLI replication primitives |

## HTTP service and operations

- [Current HTTP workflows](http-workflows.md) and [REST conventions](rest-api-design.md)
  summarize the implemented endpoints. [OpenAPI](dvmdb-http-api.yaml) documents
  concrete fields and response shapes; verify its source-generated contract
  in tests when making protocol changes.
- [Database service](native-database-service.md) — configuration, limits,
  startup and health.
- [Storage formats](storage-formats.md), [operations](operations.md),
  [verification](repository-verification.md),
  [durability](durability-qualification.md) and
  [engineering measurements](engineering-findings.md) document the native
  persistence implementation and how to evaluate it. Measurements are dated
  evidence, not claims about every later change.
- [GUI integration contract](gui-integration-requirements.md) describes the
  currently exercised API boundary with AOWIS-SERVER-GUI.

## Still future work

- [Remaining database and GUI integration roadmap](roadmap.md): exact
  external/embedded persistence acceptance, import/replacement, whole-project
  Fetch, recoverable Publish, UI Merge, scaling and qualification.
- [Persistent History and cross-Commit Undo/Redo roadmap](history-roadmap.md):
  persistent action indexing, a durable navigation cursor and cross-Commit
  Undo/Redo on top of the already implemented read-only action timeline.
  **Undo/Redo is not implemented.**
- [Security design](security-permissions-design.md) is explicitly deferred.
  Repository role, UUIDs and fencing are not authentication or authorization.

The canonical MODEL codec, edit transactions and value rules are part of
AOWIS-SERVER-MODEL, not a second DVMDB-specific serializer. Before first
release, do not add migration/backward-compatibility machinery solely for
obsolete development storage formats.

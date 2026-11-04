# DVMDB documentation

DVMDB is AOWIS's **native, multi-project model repository**, with immutable
revisions, branches, private working copies, canonical hydraulic persistence,
replication primitives and a C++/HTTP service. This index reflects the
**2026-11-04 source snapshot**. A capability described as *implemented* is not
necessarily qualified for every GUI, browser or Standalone deployment.

Start with the [repository README](../README.md) to build and run the server.

## Product and data contracts

| Topic | Reference |
|---|---|
| System components, storage and concurrency | [Architecture](architecture.md) |
| UUIDs, branches, unborn HEAD, immutable Commit and drafts | [Version-control semantics](version-control-semantics.md) |
| Draft lifecycle, autosave, leases, local Commit and recovery | [Working copies](working-copies.md) |
| Repository roles, private checkout and shared refs | [Private and shared repositories](private-working-repositories.md) |
| Revision DAG, metadata and parent order | [Revision history](native-revision-history.md) |
| **Current History features:** durable Commit action archive, checkpoint/action-boundary selection and exact limitations | [History and action archives](commit-action-archive.md) |
| **Target History behavior:** cross-Commit Undo/Redo and cursor rules | [History contract](history-contract.md) |
| Network roots, 18 collections, membership, edit and read paths | [Hydraulic persistence](native-hydraulic-network.md) |
| MODEL codec, native record identity and references | [Model records](model-records.md) |
| Structural integrity versus hydraulic solvability | [Validation boundary](model-validity-boundary-audit.md) |
| Existing generic and hydraulic merge primitives | [Branch merge](native-branch-merging.md), [hydraulic merge](native-hydraulic-merging.md) |
| Revision bundles and transport primitives | [Offline synchronization](offline-repository-synchronization.md) |
| Repository UUID/role and tracking refs | [Repository tracking](repository-tracking.md) |

## APIs and operation

| Topic | Reference |
|---|---|
| Common HTTP workflows, failure/retry expectations | [HTTP workflows](http-workflows.md) |
| Implemented routes and error conventions | [REST guide](rest-api-design.md), [OpenAPI](dvmdb-http-api.yaml) |
| Native service configuration and startup | [Database service](native-database-service.md) |
| GUI integration boundaries | [GUI integration](gui-integration-requirements.md) |
| On-disk and wire markers (including working-copy V8) | [Storage formats](storage-formats.md), [format policy](format-compatibility.md) |
| Backup, restore, data-root protection | [Operations](operations.md) |
| Full integrity verification and compaction | [Repository verification](repository-verification.md) |
| Test and benchmark procedures | [Durability qualification](durability-qualification.md), [benchmark procedures](benchmark-plan.md) |
| Dated measurements (evidence, not release guarantees) | [Engineering findings](engineering-findings.md) |

## Remaining work — only future milestones

- [**Integration and release roadmap**](roadmap.md) is the primary queue for
  hydraulic acceptance, History, import, Clone/Fetch, Publish, Merge and
  durability/scale qualification.
- [**History implementation roadmap**](history-roadmap.md) details only the
  work still required for reliable Undo/Redo across Commit markers.
- [**Security backlog**](security-permissions-design.md) is intentionally
  outside the current pre-release delivery scope. Repository IDs and leases
  are **not** authentication or authorization.

## Status in one paragraph

The source retains the original MODEL events after local Commit and serves
paged read-only first-parent action timelines over HTTP. Native code can
verify a hydraulic revision's complete action coverage and atomically select
an archived action boundary in a leased private draft, with a durable cursor
and idempotent retry. A native MODEL-verified adapter also navigates active
actions for one existing network, and atomic event Save after Undo invalidates
the redo tail without touching committed revisions. **This is not yet
user-facing Undo/Redo**: multi-network/unborn drafts, the continuous cross-Commit
cursor, HTTP navigation and GUI wiring remain unfinished. Historic revisions
without action archives remain honest
checkpoint-only entries. See [History](commit-action-archive.md).

Before the first release, unsupported development storage formats are rejected
rather than migrated. Always distinguish source functionality, test results,
and end-to-end deployment acceptance.

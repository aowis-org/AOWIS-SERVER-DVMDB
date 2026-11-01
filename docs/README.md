# DVMDB documentation

DVMDB is AOWIS's native, versioned model database. These pages describe what
the code does today; planned work is only in the [roadmap](roadmap.md). Start
with the contract, then the page for the layer you work on.

## Contract

| Page | What it settles |
|---|---|
| [Version-control contract](version-control-semantics.md) | Projects, branches, revisions, HEADs, drafts, Commit vs Publish — the rules every layer must follow |
| [Private working repositories](private-working-repositories.md) | Personal vs shared repositories, clone/checkout, local Commit, Fetch and Publish |
| [GUI integration requirements](gui-integration-requirements.md) | What the GUI may rely on and how it must present history and drafts |
| [Format policy](format-compatibility.md) | One current format, explicit rejection of anything else |

## Design and formats

| Page | Subject |
|---|---|
| [Architecture](architecture.md) | Layers, identity, durability, locking, replication, errors |
| [Storage formats](storage-formats.md) | Byte-level layout of the storage root, repositories, bundles, drafts and the Commit ledger |
| [Model records](model-records.md) | The `AOMD` record envelope, reference index, who owns the MODEL codec |
| [Revision history](native-revision-history.md) | Revisions, branches, unborn HEADs, history and graph reads |
| [Branch merging](native-branch-merging.md) | Generic three-way merge |
| [Hydraulic persistence](native-hydraulic-network.md) | How networks are stored, read, edited and validated |
| [Hydraulic merging](native-hydraulic-merging.md) | Field and structural conflicts, explicit choices |
| [Working copies and local Commit](working-copies.md) | Drafts, leases, autosave of MODEL events, local Commit |
| [Repository tracking](repository-tracking.md) | Repository identity, role, remote registry |
| [Synchronization](offline-repository-synchronization.md) | Revision bundles: CLI, C++ and HTTP |

## HTTP service

| Page | Subject |
|---|---|
| [Running the service](native-database-service.md) | Storage root, configuration, limits, deployment, health, error codes |
| [REST guide](rest-api-design.md) | Conventions and the resource map |
| [OpenAPI contract](dvmdb-http-api.yaml) | Exact routes and schemas (kept in step with the code by a CTest) |

## Operations and evidence

| Page | Subject |
|---|---|
| [Operations](operations.md) | Finding the root, backup and restore, upgrades, disk full, recovery |
| [Repository verification](repository-verification.md) | `aowis-dvmdb-verify` and compaction |
| [Durability qualification](durability-qualification.md) | Crash, ENOSPC and HTTP stress tests; opt-in QEMU power-cut and soak drills |
| [Benchmarks](benchmark-plan.md) | How to run the benchmarks |
| [Engineering findings](engineering-findings.md) | Measured results and the decisions they support |
| [Security backlog](security-permissions-design.md) | What is not protected today, and open questions for a later security design |

The MODEL codec, edit transactions and value rules are documented in the
submodule: [model serialization](../external/AOWIS-SERVER-MODEL/docs/model-serialization.md),
[edit transactions](../external/AOWIS-SERVER-MODEL/docs/edit-transactions.md),
[value rules](../external/AOWIS-SERVER-MODEL/docs/model-value-rules.md).

## Writing these pages

* Describe current behaviour in the topic page and planned work only in the
  [roadmap](roadmap.md). Say plainly when a layer exists but the end-to-end
  workflow does not.
* Byte layouts go in [storage formats](storage-formats.md); topic pages link to
  them instead of repeating them.
* No patch chronology, no "V1/V2" naming, no claims of test results that were
  not recorded. Measured numbers belong in
  [engineering findings](engineering-findings.md) with their date and setup.

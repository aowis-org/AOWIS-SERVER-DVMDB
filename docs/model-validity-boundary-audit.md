# Model integrity and hydraulic feasibility — current behavior

DVMDB distinguishes a network that is **hydraulically unsolvable** from a
network whose **canonical records and references are inconsistent**.
A network with no water source, disconnected components or solver
convergence errors may be saved and locally committed if its MODEL records
are otherwise well formed. An EPANET success result is **not** a Commit
precondition.

## Mandatory structure and concurrency

- Valid canonical MODEL encoding, physical-value rules and unambiguous
  object identities; no duplicate model keys or malformed record envelopes.
- Accurate network membership and reverse-reference indexes; existing
  referenced model targets must exist and have valid types. Removing an
  object with surviving incoming references is rejected by current native
  storage rules.
- Valid repository identity, project/branch ownership, immutable revision
  parent closure, record hashes, Merkle roots and branch-HEAD CAS.
- Live leases, fencing tokens and expected draft generations for mutable
  private working-copy operations, with atomic durable Save/Commit.

An absent immutable **revision parent** is repository corruption, never a
hydraulic diagnostic. A missing or wrong-type **hydraulic model reference**
is currently an integrity-rule failure. Relaxing that rule has **not** been
agreed and is not required to Commit a network that lacks hydraulic supply.

## Current source boundaries

| Implementation | Behavior |
|---|---|
| MODEL `validateHydraulicNetworkWithDependencies` | Validates entity values, types and existing references; this is not an EPANET solver invocation |
| GUI `HydraulicDraftEventAdapter` | Stages canonical events only if before/after and dependencies remain structurally consistent |
| DVMDB `native_store.cpp` | Validates reference targets and rejects deletion of referenced objects |
| DVMDB `hydraulic_network_persistence.cpp` | Serializes/loads root, membership chunks and indexed records with MODEL validation |
| DVMDB `ProjectService::commitWorkingCopy` | Commits a saved, dirty candidate with lease/generation/local HEAD checks and a durable ledger; no solver-success gate |
| Hydraulic-specific merge path | Has additional domain checks; separate hydraulic feasibility from structural/reference failures when completing Merge qualification |

The native `aowis-dvmdb-hydraulic-network-tests` includes an unsupplied
junction-only regression. The GUI transaction test includes an unsupplied
network with an isolated junction, edited and replayed through local queue
restart. Source coverage does not automatically prove external HTTP or
Standalone cross-process acceptance; see the [roadmap](roadmap.md).

**Unresolved, separate design choice:** whether AOWIS should ever allow
intentionally dangling hydraulic UUID references in a historical snapshot.
That decision would require changes to reverse-reference indexing, import,
verification, merge and GUI editing policy. Do not silently relax current
reference guards as part of undo, redo or general Save work.

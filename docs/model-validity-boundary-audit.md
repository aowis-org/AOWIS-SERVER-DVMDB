# INT-00 / INT-04 — Storage integrity versus hydraulic feasibility

Status (2026-11-03): **hydraulic-feasibility policy settled; model-reference
policy undecided**. This audit describes the native and GUI sources examined
for INT-00 and the narrowed INT-04. It does not assert that all incomplete
networks or all transport combinations have passed runtime acceptance.

## Agreed boundary

A hydraulic network may be committed even when it has no water source, has
disconnected components, or cannot be successfully simulated. These are
engineering states that version control must preserve. No EPANET convergence
or hydraulic-feasibility result is a Save, Commit, Branch, Fetch, Publish, or
Merge precondition.

**Storage integrity and concurrency remain mandatory.** The canonical MODEL
codec, non-null/collision-free object identities, exact before/after images,
model membership, native index consistency, revision DAG ancestry and hashes,
atomic writes, repository identity, lease/fencing, generation and HEAD CAS are
not optional. Invalid or noncanonical bytes are never treated as an
"intentionally invalid hydraulic model".

**Hydraulic model-reference consistency is a different policy decision.**
Current GUI, MODEL and DVMDB paths reject nonexistent or wrong-type UUID
references, and deletions that leave surviving incoming references. The user
has *not* decided to remove these guards. INT-04 therefore **must not** weaken
them or bypass the native reverse-reference index. Allowing dangling model
UUID references would require a separate design covering index canonicality,
repository verification, bundle import, draft loads and merge behavior.
Missing immutable revision parents always represent repository corruption.

## Actual source boundaries

| Path | Observed behavior | INT-04 disposition |
|---|---|---|
| `src/project_service.cpp` (`localCommit`) | Durable candidate and revision publication; no EPANET solver-success gate | Retain operation ID, candidate and HEAD/generation checks |
| `src/native_store.cpp` (`prepareModelCommitLocked`) | Rejects unresolved model targets and deleting referenced objects | Keep until the distinct reference-policy decision |
| `src/native_store.cpp` (verification and bundles) | Recomputes and checks reverse references plus target existence | Preserve physical integrity and existing reference semantics |
| `src/hydraulic_network_persistence.cpp` | Canonical membership and dependency validation on strict reads/saves | Do not remove record/ownership checks; verify no solver-feasibility gate |
| `hydraulic::mergeHydraulicBranches` | Enforces hydraulic domain validation through its merge/fast-forward path | In INT-10 separate feasibility diagnostics from structural/ref consistency |
| GUI `src/projects/hydraulic_draft_event_adapter.cpp` | Validates canonical event/after images, membership, and dependencies | Keep referential validation; verify unsupplied/disconnected staging succeeds |
| GUI `src/network/hydraulic_data.cpp` | Validates root metadata and captures canonical edits | Verify a network without a supply can still edit root metadata |

MODEL's dependency validation combines several checks. Rejecting an
**unresolved reference** is not proof that hydraulic solvability is being
checked. Conversely, accepting an unsupplied network does not demonstrate
that dangling references are accepted.

## Regression evidence and limits

* **INT-00 / DVMDB:** `tests/hydraulic_network_persistence_tests.cpp`
  includes a junction-only network without a source. The test covers draft
  records, reconstruction, immutable Commit, branch, revision load, compaction,
  repository reopen and verification.
* **INT-04 / GUI:** `tests/hydraulic_edit_transaction_test.cpp` exercises an
  unsupplied network containing an isolated junction and a pipe. It verifies
  captured entity and root metadata edits, local durable queue restart,
  canonical event acknowledgement, and a second restart without auto-repair.
* These source tests **do not** certify a real network transport, Standalone
  reopen, or Fetch/Publish/Merge. Those remain acceptance work for INT-05,
  INT-08..11. A successful build and CTest execution must be recorded by the
  developer before marking them passed.

## Separate future decision: dangling hydraulic model references

If requested later, the required design must define the behavior of incoming
edges whose target UUID is not present, existing wrong-type references,
removal of referenced entities, verification after import/compaction, and
merge conflict resolution. Until that decision, no permissive storage patch
is authorized and no roadmap milestone should imply it is a prerequisite.

# INT-00 — Model-validity boundary: source audit and acceptance baseline

Status: **policy decided; implementation gate open**. This is an audit of the
November 2, 2026 DVMDB/GUI snapshots, not a claim that all invalid hydraulic
models can already round-trip. See [Coordinated roadmap](roadmap.md) INT-00 and
INT-04 for the required subsequent changes.

## Contract

The database must preserve an engineering state that can be faithfully encoded
and reconstructed, even if the network is incomplete, has unresolved hydraulic
references, is disconnected, or fails simulation. Saving, committing, branching,
fetching, publishing, and explicitly resolved merging must not turn engineering
diagnostics into hard version-control restrictions.

**Hard failures** still include malformed canonical envelopes, null/invalid
persisted object IDs, duplicate keys within a transaction, damaged content
hashes/Merkle objects, wrong revision parentage, a noncanonical or inconsistent
reverse-reference index, manifest/chunk **physical** corruption, stale revision
HEAD/generation/lease fences, and repository identity mismatch. An outgoing
model edge to a valid but absent model UUID is *not* the same as a missing
revision parent or missing content-addressed pack object.

Some self-references, zero-valued optional UUIDs, and nonfinite values require
field-specific policy. Do not loosen those by blindly bypassing all validation:
first separate record/codec representability from engineering diagnostics and
prove the required encoding is lossless.

## Confirmed paths and blockers

| Component | Current behavior (snapshot) | INT-04 / later resolution |
|---|---|---|
| `src/project_service.cpp`, `ProjectService::localCommit` | Durable candidate and revision are committed through `Repository::commitModel`; no independent full-network solver pass. | Keep the durable ledger/expected HEAD and generation checks. Preserve exact candidate payload. |
| `src/native_store.cpp`, `Repository::prepareModelCommitLocked` | Rejects missing model-reference targets, in-batch deleted targets, and removal of any model with surviving incoming edges. | Allow legitimate unresolved model UUID references and deletions, while retaining canonical edges and a complete reverse-reference index, even for absent targets. |
| `src/native_store.cpp`, bundle verification | Confirms incoming index equals outgoing edges **and** target model existence. | Keep exact edge/index equality, canonical envelope checks, and revision ancestry; do not require every target UUID to be present. Test full and incremental bundle imports. |
| `src/native_store.cpp`, merge/fast-forward | Model merge rejects surviving dangling references and deletions; optional `validate` callbacks can block fast-forward. | Separate physical integrity checks from optional diagnostics; preserve conflict choices and immutable history. |
| `src/hydraulic_network_persistence.cpp`, `fullDraftHydraulicNetworkChanges` | Full-network dependency report must be valid before draft import. | Validate encoding and chunk ownership; report unresolved engineering dependencies separately. |
| Same, `loadDraftHydraulicNetwork` / `loadHydraulicNetworkAtRevision` | Reports invalid hydraulic dependencies as storage **corruption**, although canonical payload and membership may still be intact. | Allow lossless read/reconstruction of representable invalid networks; keep canonical payload, chunk, kind, and UUID checks. |
| Same, `materializeDraftRootEdit` / `saveHydraulicNetwork` | Dependency validation blocks writes of invalid intermediate networks. | Maintain exact before-image and transaction fences, but stop treating engineering diagnostics as persistence failures. |
| Same, `validateMergedHydraulicSnapshot` and hydraulic fast-forward callback | Whole-network diagnostic validation is a hard merge gate. | Separate structural ownership/index checks from optional engineering diagnostics. Explicitly resolved invalid networks may merge. |
| GUI `src/projects/hydraulic_draft_event_adapter.cpp` | Dependency validation rejects an edit transaction before durable Save. | Keep transaction/before-image checks; do not reject a representable edit solely because it breaks a network reference. |
| GUI read/preview paths | Depend on DVMDB hydraulic load and therefore inherit its restrictions. | Prove invalid model can reopen in both Monitor and Editor without auto-repair or silently dropping members. |

MODEL `src/model_validation.cpp` computes reference/dependency diagnostics and
value constraints in a combined report. The dependency collector and canonical
codec must be audited together: simply ignoring a failing report can leave the
saved reverse-reference index incomplete if the collector omits unresolved
edges. That is why the audit does **not** remove individual guards in isolation.

## Regression introduced in INT-00

`tests/hydraulic_network_persistence_tests.cpp` now exercises a deliberately
unfinished junction-only network **without a reservoir, tank or supply**. It
passes through canonical draft records, draft hydration, immutable native
Commit, branch creation, revision hydration, compaction, process restart and
verification. The fixture tests a *non-simulatable but representable* state;
it is intentionally not presented as proof that dangling references work.

## Unclosed INT-00 acceptance / implementation handoff

1. Preserve a valid but unresolved junction/pipe UUID reference across draft
   Save, local Commit, branch, full/incremental bundle, merge and reopen.
2. Preserve a deliberate deletion that leaves another object's UUID reference
   unresolved. Confirm the incoming index still describes the actual model
   edges and that fresh and imported repositories verify identically.
3. Retain rejection of forged/corrupt inverse indexes, malformed record bytes,
   invalid revision parents, and broken physical network membership.
4. Prove an invalid-but-representable network can load into the GUI editor,
   save another edit, and be committed without implicit repair.
5. Keep engineering validation available independently with a diagnostic report
   keyed by draft generation or immutable revision, rather than a Save or
   Commit precondition.

**Gate:** INT-00's audit and the incomplete-network regression are delivered;
its full acceptance criterion deliberately remains **open** until INT-04 and
the collaboration phases can run the complete cross-transport scenario.

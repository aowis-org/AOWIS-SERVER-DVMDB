# History — remaining implementation roadmap

This is a **future-work queue**, not a patch diary. The implemented History
API and its limitations belong in [the product description](commit-action-archive.md);
[the History contract](history-contract.md) specifies the agreed user-visible
behavior. This work is part of the [overall DVMDB roadmap](roadmap.md).

## Baseline already available

- Immutable first-parent Commit checkpoints and a revision-indexed,
  checksum-verified archive of original MODEL transaction frames from
  successful local Commits.
- Generation-pinned, paged HTTP history reads for Commit markers, archived
  actions and current draft events. Missing archives produce checkpoint-only
  entries; corrupt archives fail rather than silently disappearing.
- Native reconstruction of committed checkpoints and ordered action boundaries
  across all 18 hydraulic collections, including root metadata, insertions,
  deletions and transactions spanning multiple networks.
- Native proof that a revision's **complete** changed-record set is explained
  by its archived MODEL actions; unjournaled records, unsupported changes and
  incomplete archives are refused.
- Native fenced publication of a verified action boundary into a private draft
  with an atomically persisted coordinate, generation CAS, branch-HEAD pin and
  same-operation retry after restart. Draft metadata uses
  `AOWIS-WORKING-COPY-7`.

An internal native active-draft publication primitive also persists a
selected journal position and full replacement candidate in one durable
operation, retaining all original journal frames, with lease/generation/HEAD
fences, exact-operation retry and restart recovery. Ordinary Save and local
Commit currently **reject** positions below the journal tip; no redo frames
are silently archived or overwritten. No MODEL-aware live-journal planner or
HTTP command invokes this primitive yet.

The archived action operation is **not** a general Undo or Redo command. It accepts only
fully verified archived hydraulic local Commits and a draft with **no active
MODEL journal**. It does not preserve redo eligibility across arbitrary user
edits, generate a continuous action log, or expose an HTTP mutation endpoint.

**Acceptance evidence:** Native working-copy, local-Commit, project-service and
offline-backup suites passed in the most recent reported run. The Qt hydraulic
regression failed at forward navigation in that run; a subsequent overlay
normalization correction is present in the supplied 2026-11-04 source snapshot,
but no passing rerun has been reported. Do not mark this gate complete until
it passes.

## 1. Qualify the native action-boundary selector

- Run the Qt-enabled hydraulic tests with the current source; confirm both
  backward and forward selection, including a **clean** draft when returning
  to the committed HEAD, as well as exact model equality.
- Verify same-operation retries, conflicting ID reuse, stale generation/lease,
  changed branch HEAD, interrupted response, reopen/restart and recovery.
- Test mixed network/root edits, structural membership order, incomplete
  archives and unjournaled native deltas. A failed proof must never publish
  a partial candidate or cursor.

**Gate:** repeatable passing Qt/native tests and at least one process-restart
navigation test, with immutable revision and branch-ref checks.

## 2. Make active draft actions navigable

- Define a single server-owned cursor that can move from the selected
  committed action boundaries into the **current draft event journal**,
  including the unborn first draft.
- Build the MODEL-aware adapter to materialize a complete, verified candidate
  for every active-journal position (including newborn/unborn drafts), and
  invoke the existing native atomic publication primitive only after full
  revision/draft native-record coverage checks.
- Materialize Undo/Redo of accepted draft actions with canonical MODEL
  before/after verification, physical membership/root consistency and an
  atomic candidate+cursor update. Preserve unrelated project and branch refs.
- Represent navigation itself durably as an operation with fresh UUIDv7 and
  provenance to the original action. Preserve an audit trail without
  pretending old immutable revisions were changed.
- Replace the present fail-closed Save/Commit restriction below the journal
  tip with an atomic operation that invalidates forward **eligibility** upon
  a new ordinary edit;
  neither delete the immutable Commit DAG nor fabricate a recovered redo path.
- Specify what Save, Commit, import, branch change, checkout and two sessions
  do when a cursor is away from the tip; reject uncertain situations rather
  than silently resetting a user's selected state.

**Gate:** one continuous model-equivalent sequence of draft edits, Commit
markers, Undo, Redo, new edit and local Commit; survives GUI/service restart.

## 3. Expose safe navigation over HTTP

- Add read methods for the selected cursor, available Undo/Redo boundaries,
  and enough display metadata for Commit markers and draft actions.
- Add fenced navigation commands with repository/draft identity, session
  lease, expected generation, expected branch HEAD and an idempotent
  operation UUID. Never accept caller-supplied unchecked native overlays.
- Ensure identical retry after a lost reply returns the persisted outcome;
  a different request reusing the operation UUID is a conflict. Make stale
  results, absent archives and unsupported merges explicit errors.
- Update `dvmdb-http-api.yaml`, REST examples and HTTP contract tests together.

**Gate:** actual HTTP request/retry/restart tests, not just direct C++ calls.

## 4. Activate the GUI History dock

- Use the existing left History dock and history toolbar action. Show a
  selected branch, ordered actions, Commit markers and an unmistakable
  current-position marker, including checkpoint-only entries.
- Wire **Undo**, **Redo** and selectable jumps only when the backend says the
  target is eligible. Never let a visually disabled action mutate anything.
- After navigation, reload the full canonical hydraulic network, editor
  selection, inspector and pending autosave state from the accepted draft.
- Resolve uncertain operations by querying authoritative server state;
  do not optimistically move the marker on request submission.

**Gate:** real external-server GUI and embedded Standalone sessions, with
working network persistence and restart/recovery, not a mocked source.

## 5. Recovery, volume and edge cases

- Bounded history pages across long branches, 4,096-event / 8 MiB active
  draft budgets, large model replacements and multi-network actions.
- Imported, pre-archive, unindexed and merge revisions remain checkpoint-only
  unless a separate **verified** replay mechanism becomes available. Non-
  hydraulic changes and network creation/removal likewise require explicit
  additional implementation before action-level navigation.
- Prove no state loss under concurrent leases, changed refs, disk-full,
  failed fsync, crash/power cut, backup/restore and uncertain acknowledgements.
- Establish a deliberate retention/compaction policy for permanent archives:
  no silent pruning, guessed MODEL edits or broken cursor ancestry.

**Gate:** a documented test matrix with exact builds, platforms, pass/fail
results and any unsupported boundaries.

## Explicit non-goals

- Moving or rewriting committed local/remote branch HEADs as part of Undo.
- Mandatory hydraulic solver convergence, water supply or connectivity
  before Save, Commit or History navigation. Canonical records and model
  reference/value integrity **remain mandatory**.
- Legacy draft migrations and security/TLS/authorization in this roadmap.

The observable target is a **continuous, recoverable Undo/Redo timeline across
Commit markers in a private working copy**. We should prioritize its working
HTTP and GUI path over additional stand-alone read-only helpers.

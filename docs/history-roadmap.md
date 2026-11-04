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
  `AOWIS-WORKING-COPY-8`.

An internal MODEL-aware active-draft adapter now replays and validates all
retained journal frames for **one existing hydraulic network** from immutable
HEAD, checks that its current selected boundary is byte-equal to the actual
working-copy network, rejects unrelated/unjournaled native edits and
constructs a complete HEAD-relative candidate. Its native operation publishes
that candidate and the cursor atomically, retaining all original journal frames
and the existing lease/generation/HEAD/idempotency protections. It currently
refuses multi-root and unborn-base drafts rather than guessing ownership.
Navigation preserves the original journal and future Redo actions. The
server now exposes a draft-scoped HTTP cursor read and fenced, idempotent
HTTP selection of an explicit verified active-journal position for this
single-network case. Providing an archived `revisionId` instead selects an
explicit verified action boundary within an indexed local Commit. From a
verified active journal selected at position zero, the V8 publisher suspends
the retained journal and allows navigation into that archive. The HTTP cursor now also supplies server-resolved `previousAction` and
`nextAction` targets for active journal steps and eligible archived Commit
boundaries, skipping duplicate states at Commit markers. A client submits
those coordinates to the existing fenced POST selector. The server now resolves adjacent targets across the retained-journal/Commit
boundary, but the Qt HTTP handoff regression and GUI wiring are still pending. A new
atomic MODEL event Save from a non-tip position retains the selected prefix
and replaces the redo tail with the new actions in a crash-safe metadata
publication. Local Commit still **rejects** positions below the journal tip;
no unapplied redo frames are silently archived. A limited HTTP command now invokes this adapter; cross-Commit handoff has native regression coverage but Qt HTTP and GUI
acceptance remain outstanding.

The archived selector accepts only fully verified archived hydraulic local
Commits. At an eligible active draft position zero, it suspends the journal,
retaining Redo frames, and resumes active edits from the newest HEAD archive.
This is still not GUI Undo/Redo, and unverified/checkpoint-only, unborn-base
and multi-network active drafts remain outside this implementation.

**Acceptance evidence:** The latest user-reported Qt hydraulic network suite
passed (2026-11-04), and adjacent native suites previously passed. The HTTP
suite's HDB-03d durability assertion was corrected in the uploaded source,
but the corrected full HTTP suite and the new archived-HTTP regression still
require a passing run. Do not declare continuous cross-Commit Undo/Redo ready.

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

- Qualify the implemented V8 handoff through Qt HTTP tests, simulated lost
  acknowledgements and process restart, and verify Save/Commit remain fenced
  while the original journal is suspended in archived history.
- Support the normal unborn first draft without fabricating any base snapshot.
- Support the normal **unborn first draft** and network initialization/import
  in active history without weakening MODEL/native coverage verification.
  Multiple hydraulic networks in one revision remain legal in storage, but
  multi-network *navigation* is explicitly deferred and is not a gate for
  a usable single-network History dock.
- Qualify the existing action-boundary selection and publication with the Qt
  hydraulic regression, including all 18 collections, physical membership
  order, root metadata, restart, generation/lease conflicts and canonical
  forward/backward equality. Preserve unrelated project and branch refs.
- Represent navigation itself durably as an operation with fresh UUIDv7 and
  provenance to the original action. Preserve an audit trail without
  pretending old immutable revisions were changed.
- Qualify the implemented atomic redo invalidation through the real HTTP
  MODEL Save route, including a stale before-image, wrong generation/lease,
  failed publication and retry after restart. Only accepted atomic event
  Saves branch the journal; Commit must refuse an unresolved non-tip cursor.
  Never alter the immutable Commit DAG or fabricate a recovered redo path.
- Specify what Save, Commit, import, branch change, checkout and two sessions
  do when a cursor is away from the tip; reject uncertain situations rather
  than silently resetting a user's selected state.

**Gate:** one continuous model-equivalent sequence of draft edits, Commit
markers, Undo, Redo, new edit and local Commit; survives GUI/service restart.

## 3. Expose safe navigation over HTTP

- **Available for the active single-network journal and eligible archived
  Commits:** read the persisted cursor and select an explicit verified boundary
  over HTTP using draft/session/lease, expected generation and branch HEAD,
  repository pin and UUIDv7 operation ID. An archived selection uses a
  `revisionId` and stores its action position separately from the draft journal.
- Qualify the implemented single-network HTTP cross-Commit handoff and extend
  navigation to the normal unborn first draft; never accept unchecked overlays.
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

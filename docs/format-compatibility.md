# Native format and integrity policy

The first AOWIS release has **one canonical native repository format**, not an
accumulation of old experimental layouts. AOWIS is pre-release; existing test
and development data do not create a backward-compatibility obligation.
An unknown/obsolete format is rejected explicitly, never silently guessed,
relabelled, or treated as an empty repository. There is no legacy decoder,
implicit migration or format-conversion acceptance gate.

Every persisted record and bundle starts with a marker or magic; the byte
layouts are in [storage formats](storage-formats.md). Names such as
`revision-v2/`, `AOWREV21`–`24` and the `T`/`RID`/`PRJ` version byte `2` are
the **current** format, not a compatibility layer. When a format changes, the
marker changes and the old one is rejected (for example, single-file
`AOWIS-WORKING-COPY-3` drafts are refused since drafts became directories).
`aowis-dvmdb-format-compatibility-tests` keeps a frozen pre-release pack and
its bundles (`tests/fixtures/pack-20261024`) to prove they are refused without
modifying anything.

## Invariants

- Projects, branches, revisions and draft working copies have UUIDv7 identities;
  names never serve as storage keys. Repository identity is distinct from
  logical project identity.
- A newly created project has an unborn `main` (null committed HEAD), no
  synthetic initial commit and no fabricated ancestry.
- Immutable commits contain ordered parent IDs and integrity hashes; canonical
  model records/Merkle roots bind their complete state. A branch ref can move
  only under its specified concurrency rules.
- Branch fork provenance (source branch UUID and exact committed fork UUID)
  is metadata, **not** an additional graph ancestry edge.
- A private draft carries an exact base, candidate overlay and MODEL edit
  journal; saving it never creates a commit or changes a branch HEAD.
- Import/export preserves identities, parent order, hashes and validated
  closure. Wrong-repository, incomplete and corrupt transfers fail closed.

## Release qualification

Verify fresh repository creation, unborn-first-commit, fork/rename, shared and
non-leaf HEADs, merge ancestry, draft save/reopen, lost-response recovery,
complete model restore, backup/restore, compaction, and crash/ENOSPC recovery.
Compare native and REST behavior on the *same* canonical schema. See
[version-control semantics](version-control-semantics.md),
[repository verification](repository-verification.md), and
[roadmap](roadmap.md).

Internal format identifiers can still advance before first release. Each
change should update code, tests and this contract together—**not** add
compatibility shims for unreleased variants.

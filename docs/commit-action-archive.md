# Permanent local Commit action archive

**Implemented in HIST-02 (first slice):** the DVMDB local Commit ledger stores
all acknowledged MODEL transaction frames with the frozen candidate before a
revision can be published. Their original UUIDv7s, order and complete bytes
survive successful Commit, draft rebase, retry, process restart and compaction
of the underlying native object store. The accepted-action chain is verified
whenever a Commit record is decoded. Integrity errors are not masked as
missing history.

The authoritative record is `local-commits/<operation UUIDv7>` with
`AOWIS-LOCAL-COMMIT-3` format, a count of actions and a chained SHA-256 digest.
The local Commit operation UUID is its current lookup key. A successful
`ProjectService::localCommitActionHistory(operationId)` returns the commit
identity/revision, timestamp and original accepted frames. An unaccepted
pending/rejected operation does not expose committed history.

**Not implemented:** persistent branch-timeline indexing, revision-to-action
pagination, durable user-visible history cursor, inverse/redo operation IDs,
Undo/Redo materialization, or GUI navigation. `working-copies/<id>` retains
a bounded active journal and still clears it after Commit; this separate
committed archive is not subject to that draft journal lifetime. Its size is
bounded by the existing 4096-event/8-MiB-per-draft journal limit **per
Commit**, not by a lifetime total. The ledger itself remains retained;
long-term retention and search optimizations are future work.

A Commit made using `saveWorkingCopyChanges` (rather than acknowledged MODEL
transactions) has an empty event archive, which means a **checkpoint-only**
history segment. Do not fabricate edit steps or claim exact action coverage
for such a segment. Likewise, older revisions without a retained action
archive are checkpoint-only. Immutable revisions and their hashes remain
unchanged by this archive. No external HTTP endpoint is introduced by this
first slice; future history API pagination will be added separately.

See [storage formats](storage-formats.md) and
[History roadmap](history-roadmap.md).

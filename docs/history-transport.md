# Sparse hydraulic History transport and reconciliation

DVMDB owns the authoritative working-copy content, saved generation,
History cursor, immutable revisions and original MODEL action archives.
The GUI owns a cached displayed `NetworkHydraulic` and a durable **client-side
navigation intent** for uncertain requests. These are complementary, not
two competing Undo histories.

## Navigating a historical action

`POST /dvmdb/v1/working-copies/{draftId}/history/navigate` accepts the
network UUID, the exact active/archived target coordinate, operation UUIDv7,
expected source generation/base and fenced writer identity. DVMDB verifies
the original MODEL frames and destination, then atomically publishes the
HEAD-relative private candidate and the cursor. Its HTTP `historyDelta`
contains the source and destination **canonical SHA-256** digests, changed
typed entity before/after images (including network-root metadata) and
ordered membership moves across the 18 collections. It does **not** return
every unchanged entity or a full `NetworkHydraulic` just to Undo one action.

The server can reconstruct full snapshots internally; this is not a full
network **transfer**. The GUI checks its source digest, validates every
before-image, applies the delta and verifies the destination digest before
publishing that candidate locally. A distant jump may have a large delta
when many entities actually differ.

## Durable same-operation replay

The working-copy metadata (`AOWIS-WORKING-COPY-10`) references the
`history-source-candidate` from before the latest navigation. The previous
candidate is an immutable **file reference**, not an additional duplicate
file copied on every Undo. Candidate, cursor, generation and source-candidate
reference are published together. An exact retry of the last operation UUID
and source generation can reconstruct the original source and return the
same verified sparse delta, including after a DVMDB restart or writer lease
rotation. A subsequent conflicting write/navigation expires that pinned
source. Unsupported/stale replay **fails closed**.

The on-disk source reference is included in offline backup/verification
checks while it is live. This is a bounded one-navigation recovery mechanism,
not a second permanent Undo journal.

## Small hash/cursor reconciliation

`GET /dvmdb/v1/working-copies/{draftId}/history/reconcile?networkId={uuid}`
returns read-only state: `sessionId`, `baseRevision`, `generation`,
`journalCount`, current selection kind/revision/action boundary, canonical
`networkSha256`, `lastNavigationOperationId` and
`lastNavigationSourceGeneration`.

The client distinguishes:

- **Published and fully matching:** exact operation UUID, expected generation
  and requested coordinate agree, and its canonical network digest matches.
  No model transfer is necessary.
- **Published but model differs:** replay the *same* operation UUID to receive
  its pinned sparse change set.
- **Not published:** original source generation, cursor and digest agree;
  retry the original operation UUID after reacquiring the same session lease.
- **Conflicting identity, cursor, journal, HEAD or digest:** block editing.

A matching SHA-256 **alone** does not imply a matching History coordinate:
MODEL operations can change a value and later change it back. Cursor,
operation UUID and generation are part of the reconciliation protocol.

## Crash/restart boundaries and scope

DVMDB retains the candidate, cursor, last navigation identity and pinned
source durably. The GUI implements `PersonalHistoryNavigationIntentStore`
in its own queue directory, recording the exact source MODEL snapshot,
journal, session, operation UUID, generation and source/target coordinates
**before** issuing an uncertain POST. A restarted GUI checks that intent
before ordinary full-network hydration, then uses this DVMDB reconciliation
endpoint to finish without a full network GET when the state can be proven.
The GUI intent is **not stored inside DVMDB** and does not require any
additional database storage-format change.

Without a pending intent, a normal fresh GUI open may still transfer the
whole authoritative network once. A browser client needs persistent
browser-backed storage for its navigation intent; in-memory WASM state alone
cannot guarantee recovery from page close/restart.

History navigation currently supports **one unambiguous hydraulic network**,
including eligible initialized unborn draft actions. Unprovable original
membership deletion order, checkpoint-only revisions, multi-network History
and some incomplete archived actions are not given synthetic action
positions. DVMDB can still store multiple networks per revision.

See the [History contract](history-contract.md),
[working-copy storage](storage-formats.md), and the GUI's
[recovery details](../../AOWIS-SERVER-GUI/docs/history-recovery.md).

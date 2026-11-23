# Sparse hydraulic and annotation History transport and reconciliation

DVMDB owns the authoritative working-copy content, saved generation,
History cursor, immutable revisions and original MODEL action archives.
The GUI owns a cached displayed `NetworkHydraulicDistribution` and a durable **client-side
navigation intent** for uncertain requests. These are complementary, not
two competing Undo histories.

## Navigating a historical action

`POST /dvmdb/v1/working-copies/{draftId}/history/navigate` accepts the
network UUID, the exact active/archived target coordinate, operation UUIDv7,
expected source generation/base and fenced writer identity. DVMDB verifies
the original MODEL frames and destination, then atomically publishes the
HEAD-relative private candidate and the cursor. Its HTTP `historyDelta`
(version 2) contains changed typed entity before/after images (including
network-root metadata), ordered membership moves across the 17 collections,
the list `orderedTypes` of collections whose membership changed, and source
and destination digests `fromSha256` / `toSha256` (below). It does **not**
return every unchanged entity or a full `NetworkHydraulicDistribution` just
to Undo one action.

The server builds the delta from the native records that differ between the
source and destination drafts; it never loads or encodes the whole network.
The GUI checks the source digest, validates every before-image, applies the
delta and verifies the destination digest before publishing that candidate
locally. A distant jump may have a large delta when many entities actually
differ.

### historyDelta version 2

```json
{"version": 2, "networkId": "<uuid>",
 "fromSha256": "<hex>", "toSha256": "<hex>",
 "orderedTypes": ["HydraulicNodeJunction", ...],
 "edits": [{"type": "...", "id": "<uuid>", "beforeBase64": ..., "afterBase64": ...}],
 "moves": [{"type": "...", "id": "<uuid>", "position": 3}]}
```

- `edits`: the network-root settings edit first (type
  `hydraulicDistributionRootEditType`, images
  `aowis::model::hydraulicDistributionRootImage`), then per collection in
  canonical order. In a collection listed in `orderedTypes`: deletions and
  updates in source order, then insertions in destination order. In any
  other collection: updates only, in UUID byte order.
- `moves`: unchanged from version 1. Apply deletions, append insertions in
  edit order, then each move in order (`position` = final index).
- `orderedTypes`: the collections whose membership changed (insertion,
  deletion or reorder), in canonical collection order.
- Digest: SHA-256, lowercase hex, of the byte string
  1. ASCII `AOWIS-HISTORY-DELTA-2` (no terminator);
  2. `u32be(n)` followed by the `n` bytes of
     `aowis::model::hydraulicDistributionRootImage(network)`;
  3. for each type in `orderedTypes`, in the listed order: `u32be(n)` + the
     UTF-8 type name, `u32be(count)`, then the 16-byte RFC 4122 UUID
     (`QUuid::toRfc4122`) of every member of that collection, in collection
     order.

  `fromSha256` uses the source state and `toSha256` the destination state,
  with the same `orderedTypes`. The GUI computes both from its own network:
  the settings image plus the member order of the listed collections. All
  other content is checked through the before-images of `edits`.

### annotationDelta version 1

The same reply also carries `annotationDelta` for the network's
Distribution annotation layer, so the GUI does not reload the layer after
Undo/Redo. It is absent when the layer is missing at either draft (the
client then reloads the layer) and when `networkId` was not sent.

```json
{"version": 1, "layerId": "<uuid>", "networkId": "<uuid>",
 "orderChanged": true, "fromSha256": "<hex>", "toSha256": "<hex>",
 "edits": [{"id": "<uuid>", "beforeBase64": ..., "afterBase64": ...}],
 "insertions": [{"id": "<uuid>", "position": 5}],
 "orderBase64": null}
```

- `edits`: canonical MODEL `MapAnnotation` images; `beforeBase64` null is a
  creation, `afterBase64` null a deletion. With `orderChanged` false: updates
  only, in UUID byte order. With `orderChanged` true: deletions and updates
  in source layer order, then creations in destination order.
- `orderChanged`: the layer's membership or member order changed (a
  membership chunk or the root's chunk list differs).
- `insertions` (only when `orderBase64` is null): the final index of every
  created member, ascending. Members that stay keep their relative order, so
  the client removes the deletions, then inserts the creations at their
  positions in ascending order.
- `orderBase64`: null, or, when members that stay changed their relative
  order (an action deleted and re-created a member), the complete destination
  order as base64 of the concatenated 16-byte RFC 4122 UUIDs. `insertions`
  is then empty and the client arranges all members in this order.
- Digest: SHA-256, lowercase hex, of ASCII `AOWIS-ANNOTATION-DELTA-1`, the
  16-byte layer UUID, then only when `orderChanged` is true `u32be(count)`
  and the 16-byte UUID of every member in layer order. `fromSha256` uses the
  source order and `toSha256` the destination order. Content is checked
  through the before-images.

The server reads the member order from the membership chunks only when
`orderChanged` is true; member records are never decoded. The layer's
metadata never changes through History. A distant jump may carry many edits;
a reorder of a very large layer sends its complete order (16 bytes per
member).

Version 1 hashed the complete canonical network, which forced the server to
load and encode the whole network on every Undo/Redo. The reconciliation
receipt below still reports the complete-network `networkSha256` (a rare
recovery path).

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

History navigation currently supports **one unambiguous hydraulic distribution network**,
including eligible initialized unborn draft actions. Unprovable original
membership deletion order, checkpoint-only revisions, multi-network History
and some incomplete archived actions are not given synthetic action
positions. DVMDB can still store multiple networks per revision.

See the [History contract](history-contract.md),
[working-copy storage](storage-formats.md), and the GUI's
[recovery details](../../AOWIS-SERVER-GUI/docs/history-recovery.md).

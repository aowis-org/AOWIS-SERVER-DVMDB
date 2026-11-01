# Model records, serialization and reference indexes

DVMDB stores models as independently addressed **model records** in a
UUID-keyed Merkle trie, with a second trie indexing who references whom. This
page covers the generic record layer; the hydraulic adapter built on top of
it is in [hydraulic persistence](native-hydraulic-network.md).

## Who owns what

| Concern | Owner |
|---|---|
| Model types, canonical codec `aowis::model::ModelCodec<T>`, validation, field merge, value rules, MODEL edit transactions | [AOWIS-SERVER-MODEL](../external/AOWIS-SERVER-MODEL/docs/model-serialization.md) (submodule) |
| `ModelRecord` envelope, reverse-reference index, immutable storage, revisions, drafts, HTTP transport | DVMDB |
| Bridge from typed hydraulic networks to records (`aowis/dvmdb/hydraulic_network_persistence.h`) | DVMDB, using MODEL's codec |

DVMDB never builds a second serializer: its adapter aliases point at the MODEL
implementation. The MODEL target checks its 68-structure field manifest and
its enum and value rules at configure time. The encoding is deterministic
binary with strict enumerator and value checks; the REST API carries the same
bytes as base64 inside JSON. Qt Core is needed only for this typed layer; with
`-DAOWIS_DVMDB_WITH_MODEL_CODEC=OFF` the store and project service build
without it.

## Records and changes

A `ModelRecord` is a non-zero `kind`, the canonical payload and the sorted,
de-duplicated UUIDs of the entities it requires. `encodeModelRecord()` writes
the `AOMD` envelope ([format](storage-formats.md#model-record-aomd)). A
`ModelChange` is an entity UUID plus a record, or no record for a deletion.

`Repository::commitModel(project, branch, changes, expectedHead, …)` applies a
batch and publishes one revision. Before anything is written it checks that
every required reference exists in the resulting snapshot (including entities
created in the same batch) and that no deleted entity is still referenced, and
it updates the primary trie and the reverse-reference trie together. A failed
check publishes nothing.

## Reads

Every read names an immutable revision: `getModelAtRevision`,
`referencingModelAtRevision` (sorted UUIDs of the entities that reference a
key, O(log N + references)), `scanAtRevision`, `diffRevisions`. Reads never
follow a moving branch HEAD; a client that makes several calls passes the same
revision UUID to each.

## Imports and verification

Bundle import recomputes the incoming references of every model-indexed
revision from its records and compares them exactly with the stored reverse
index before anything is published; missing, extra, dangling or non-canonical
references are rejected. `aowis-dvmdb-verify` applies the same check to every
historical revision ([verification](repository-verification.md)).

## What references do not prove

A complete reference graph is not a valid hydraulic model: topology, endpoint
types, membership and physical rules are checked by the hydraulic layer before
a commit or merge is published. Indexing and subtree reuse may speed this up
but never replace it.

## Format changes

Before the first release obsolete encodings need not stay readable. Test a new
encoding with strict current-format round-trip, corruption, persistence and
restart tests, not with legacy fixtures ([format policy](format-compatibility.md)).

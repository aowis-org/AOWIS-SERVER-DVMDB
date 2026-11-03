# Storage and wire formats

This is the byte-level reference for everything DVMDB writes to disk or sends
as a bundle. The behaviour these formats implement is described in the topic
documents linked from each section; this page only says what the bytes are.

DVMDB is pre-release and has **one current format**. Every record below carries
a marker or magic; an unknown or older marker is rejected as unsupported, never
guessed or migrated (see the [format policy](format-compatibility.md)). All
integers are big-endian. UUIDs are 16 raw bytes in binary records and canonical
lowercase text (`xxxxxxxx-xxxx-7xxx-[89ab]xxx-xxxxxxxxxxxx`) in text records.
Text records are ASCII, one field per line, `\t`-separated, `\n`-terminated,
fields in the order shown. `-` stands for an absent value.

## Catalogue root

One storage root holds every project of one DVMDB service (or embedded
library instance):

```text
<root>/
├── catalogue.lock          # flock(LOCK_EX) held by the owning ProjectService for its lifetime
├── repository.id           # catalogue (repository) UUIDv7
├── repository.tracking     # optional: role, remote registry, remote-tracking refs
├── projects/
│   └── <project-uuidv7>/
│       ├── project.meta    # project manifest
│       └── repository/     # one native repository (layout below)
├── working-copies/
│   └── <draft-uuidv7>/     # one private draft (layout below)
└── local-commits/
    └── <operation-uuidv7>  # one local Commit ledger record
```

Unpublished staging entries (`.repository-id-creating-*`, `projects/.creating-*`,
`working-copies/.creating-*` and `.deleting-*`, `local-commits/.creating-*` and
`.updating-*`, `repository.tracking.tmp`) are never visible as data and are
removed when the service opens the root.

### `repository.id`

```text
AOWIS-REPOSITORY-1
<repository-uuidv7>
```

Created once, on the first `ProjectService` open, by staging and linking with
no replace. A malformed file fails closed; it is never regenerated.

### `projects/<id>/project.meta`

```text
AOWIS-PROJECT-1
<project-uuidv7>
pack
<display name as lowercase hex of its UTF-8 bytes>
```

The directory name and the second line must agree. `pack` is the only storage
engine. Project creation stages `projects/.creating-<id>/` completely and
renames it into place.

### `repository.tracking`

```text
AOWIS-TRACKING-1
role\t<unconfigured|private|shared>
remote\t<remote-repository-uuidv7>\t<endpoint as lowercase hex of its ASCII bytes>
ref\t<remote-repository-uuidv7>\t<project-uuidv7>\t<branch-uuidv7>\t<revision-uuidv7|->
```

`remote` and `ref` lines are optional and sorted by their UUID keys. A `ref`
with `-` records an observed unborn branch; a missing `ref` means "not
observed". Limits: 64 remotes, 10,000 refs, 8 MiB. The whole file is replaced
atomically (stage, fsync, rename, fsync root). Semantics:
[repository tracking](repository-tracking.md).

## Native repository

```text
repository/
├── objects.pack            # append-only object pack (authoritative)
├── objects.idx             # derived object index, rebuilt from the pack when damaged
├── objects.current         # active pack generation after a compaction (optional)
├── objects.<gen>.pack      # replacement pack written by compaction
├── objects.<gen>.idx
├── refs/heads/<branch-uuidv7>   # one branch record per branch
├── revision-v2/
│   ├── lock
│   ├── project             # owning project binding
│   └── ids/<revision-uuidv7>    # revision UUID → record SHA-256 binding
├── maintenance.lock        # shared for every open handle, exclusive for compaction
├── repo.lock               # held for every repository operation
├── recovered-pack-tails/   # copies of discarded torn pack tails (only if one occurred)
└── tmp/                    # scratch files (mode 0700, O_TMPFILE where available)
```

The directory name `revision-v2` and the `T`/`RID`/`PRJ` version bytes `2` are
part of the current format, not a compatibility layer.

### Pack records

Each object is one record: magic `AOBJ` `0x01` (5 bytes), SHA-256 of the
payload (32), payload length (u64), payload. The payload's SHA-256 is the
object's address. A reader verifies the digest of every payload it reads.

`objects.idx` starts with `AOIDX01\0` and holds 52-byte entries: object hash
(32), pack offset (u64), length (u64), CRC32 (u32). The CRC detects accidental
damage only; the index is derived and is rebuilt from the pack when it is
missing, damaged or behind. Object payloads are typed canonical encodings:
Merkle trie nodes, raw values, model records (below), reverse-reference nodes
and revision records.

### Model record (`AOMD`)

| Offset | Size | Field |
|---|---|---|
| 0 | 5 | `AOMD` `0x01` |
| 5 | 2 | kind (non-zero) |
| 7 | 4 | reference count *n* |
| 11 | 16·*n* | referenced entity UUIDs, sorted, no duplicates, no self-reference |
| 11+16·*n* | 4 | payload length |
| … | … | payload: canonical MODEL bytes (`ModelCodec<T>`) |

See [model records](model-records.md).

### Revision record (`T` 3)

A committed revision is one immutable pack object: 100 + 48·*p* bytes of
identity, roots and parents (*p* = number of parents: 0, 1 or 2), followed by
the commit time, the author and the message.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `T` `0x03` |
| 2 | 16 | revision UUIDv7 |
| 18 | 16 | project UUIDv7 |
| 34 | 32 | model Merkle root (snapshot root) |
| 66 | 32 | reverse-reference Merkle root |
| 98 | 1 | 1 = model-indexed revision, 0 = raw-value revision |
| 99 | 1 | parent count *p* |
| 100 + 48·*i* | 16 | parent *i* revision UUIDv7 (target first for a merge) |
| 116 + 48·*i* | 32 | parent *i* record SHA-256 |
| *m* = 100 + 48·*p* | 8 | commit time: Unix epoch milliseconds, UTC, not 0 |
| *m* + 8 | 16 | author UUIDv7 |
| *m* + 24 | 2 | author name length *a* (1–200) |
| *m* + 26 | *a* | author name, UTF-8 without control characters |
| *m* + 26 + *a* | 4 | message length *n* (1–65,536) |
| *m* + 30 + *a* | *n* | message, UTF-8; tab and newline are the only control characters |

The record ends exactly after the message. The **record hash** is the SHA-256
of these bytes. Binding every parent's record hash makes the record hash commit
to the whole ancestry, and the metadata is covered too: author, message and time
cannot change without changing the revision's identity binding. The commit
time is the moment the repository published the revision (a local Commit
reserves it with the operation, so a retry writes the identical record). The
author is self-reported by the client; nothing authenticates it. Branch names
and fork provenance are deliberately outside the record, so renaming a branch
never changes a revision.

### Revision bindings (`revision-v2/`)

`revision-v2/project` is 20 bytes: `PRJ` `0x02`, project UUID. It binds the
whole repository to one project; a mismatch is a conflict, a missing binding
next to existing `ids/` is corruption.

`revision-v2/ids/<revision-uuidv7>` is 68 bytes: `RID` `0x02`, revision UUID
(16), project UUID (16), record SHA-256 (32). A binding is written once, with
no replace, after its revision record and every parent binding exist. Lookup
by UUID is one binding read plus one record read; the record's SHA-256 and its
parent bindings are checked on every lookup. Full ancestry is checked by
`aowis-dvmdb-verify` and by bundle import. Compaction keeps every bound
revision, referenced by a branch or not.

### Branch records (`refs/heads/`)

One file per branch, named by the branch UUIDv7. The name is display data.

| Marker | Lines after the marker |
|---|---|
| `AOWIS-BRANCH-4` | name, `-`, `-` (unborn: no HEAD, no provenance) |
| `AOWIS-BRANCH-2` | name, HEAD revision UUIDv7, HEAD record SHA-256 |
| `AOWIS-BRANCH-3` | name, HEAD revision UUIDv7, origin branch UUIDv7, HEAD record SHA-256 |
| `AOWIS-BRANCH-5` | name, HEAD revision UUIDv7, origin branch UUIDv7, fork revision UUIDv7, HEAD record SHA-256 |
| `AOWIS-BRANCH-6` | name, HEAD revision UUID/`-`, origin branch UUID/`-`, fork revision UUID/`-`, record SHA-256/`-`, UTF-8 description encoded as hex, comma-separated hex-encoded UTF-8 tags |

Names are 1–96 bytes of valid UTF-8, unique within the repository, not `.` or
`..`, without control characters or `/ \ % ? #`, and without leading or
trailing spaces. A
record is replaced by writing `.stage-<uuidv7>`, fsync, rename, directory
fsync; stage files left by a crash are removed at open. The origin branch and
fork revision never change after creation. Branch name, tags and description
are mutable metadata; a name change never changes the UUID. The branch-6
record supports metadata both for unborn and committed branches and keeps
nullable HEAD/origin/fork explicit. The empty-description representation is
an empty hex field; metadata is validated on read and write.

A separately stored revision **display-message annotation** may change the
message returned to GUI clients, while the original hashed message remains
in the immutable revision record. This is not a rewrite of `AOWREV2x` bytes.

## Revision bundles (`AOWREV21`–`AOWREV24`)

A bundle carries one branch's committed history: a header, then every object
sorted by hash, each as hash (32), length (u64), payload. HTTP uses the MIME
type `application/vnd.aowis.revision-bundle-v2`.

| Magic | Header | Content |
|---|---|---|
| `AOWREV21` | 96 bytes | full history, branch without provenance |
| `AOWREV22` | 144 bytes | delta from a recipient base, branch without provenance |
| `AOWREV23` | 128 bytes | full history, with provenance |
| `AOWREV24` | 176 bytes | delta, with provenance |

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic |
| 8 | 16 | project UUIDv7 |
| 24 | 16 | branch UUIDv7 |
| 40 | 16 | HEAD revision UUIDv7 |
| 56 | 32 | HEAD record SHA-256 |
| 88 | 8 | object count |
| 96 | 16 | *delta only:* base revision UUIDv7 |
| 112 | 32 | *delta only:* base record SHA-256 |
| 96 or 144 | 16 | *provenance only:* origin branch UUIDv7 (zero if absent) |
| 112 or 160 | 16 | *provenance only:* fork revision UUIDv7 (zero if absent) |

A delta carries the revisions not reachable from the base and, per revision,
only the trie objects that differ from a parent; the rest must already exist in
the recipient. An unborn branch has no revision and cannot be bundled. A fork
revision requires an origin branch and must be an ancestor of the HEAD.
Hard ceilings: 64 GiB and 1,000,000 objects per bundle, 64 MiB per object;
callers set lower `BundleLimits`. Import rules:
[synchronization](offline-repository-synchronization.md).

## Private drafts (`AOWIS-WORKING-COPY-4`)

Each draft is a directory `working-copies/<draft-uuidv7>/` (mode 0700):

* `meta` — the only authoritative file; small text, replaced atomically
  (`.meta-<uuidv7>`, fsync, rename, fsync the draft directory).
* `candidate-<uuidv7>` — the candidate overlay, an immutable binary file.
* `journal-<uuidv7>` — the MODEL event journal, append-only binary.

```text
AOWIS-WORKING-COPY-4
project\t<project-uuidv7>
branch\t<branch-uuidv7>
base\t<revision-uuidv7|->
session\t<creating-session-uuidv7>
generation\t<decimal u64>
dirty\t<0|1>
validation\tunvalidated
lease-session\t<session-uuidv7|->
lease-token\t<token-uuidv7|->
lease-expires\t<unix epoch ms, 0 without lease>
candidate\t<file|->\t<changes>\t<bytes>\t<SHA-256 of the file|->
journal\t<file|->\t<events>\t<acknowledged bytes>\t<chain digest|->
last-save\t<SHA-256 fingerprint of the last event Save|->
```

Candidate file: `AOWC` `0x01`, then per change, sorted by entity key: key (16),
length (u32, `0xffffffff` = deletion), `AOMD` model record. Journal file: per
event, event UUIDv7 (16), length (u32), exact MODEL edit frame. The journal
digest chains the frames: d₀ = 32 zero bytes, dᵢ = SHA-256(dᵢ₋₁ ‖ frameᵢ).
Bytes after the acknowledged length are an interrupted append and are ignored
and later overwritten. Limits: 8 MiB candidate, 8 MiB journal, 20,000 changes,
4,096 events. Creation stages `.creating-<id>/` and renames it; discard
renames to `.deleting-<uuidv7>` before deleting. Behaviour:
[working copies](working-copies.md).

### MODEL edit frame (`AOWE`)

Owned by AOWIS-SERVER-MODEL (`model_edit_transaction.cpp`); DVMDB checks the
framing: `AOWE` `0x01` (5), payload length (u32), payload, SHA-256 of the
payload (32). The payload starts with the transaction UUIDv7, which is the
event's identity.

## Local Commit ledger (`AOWIS-LOCAL-COMMIT-3`)

`local-commits/<operation-uuidv7>`, one file per Commit operation, kept
permanently:

```text
AOWIS-LOCAL-COMMIT-3
repository\t<repository-uuidv7>
project\t<project-uuidv7>
branch\t<branch-uuidv7>
draft\t<draft-uuidv7>
generation\t<expected draft generation>
expected\t<expected local HEAD revision-uuidv7|->
reserved\t<reserved revision-uuidv7>
committed-at\t<reserved commit time, Unix epoch ms>
author-id\t<author-uuidv7>
author-name\t<author name as lowercase hex of its UTF-8 bytes>
message\t<message as lowercase hex of its UTF-8 bytes>
request-digest\t<SHA-256 of the request, author and message included>
payload-digest\t<SHA-256 of request and frozen candidate>
state\t<pending|succeeded|rejected>
rejection\t<conflict|invalid-input|not-found|->
changes\t<count>
change\t<entity key, 32 hex>\t<AOMD record hex|->
actions\t<count of archived MODEL event frames>
action-chain\t<SHA-256 chain digest over the archived frames>
action\t<event-uuidv7>\t<full AO WE MODEL frame as lowercase hex>
```

The `pending` record (with the frozen candidate, the reserved revision UUID,
commit time, author, message **and all acknowledged MODEL event frames**) is
fsynced before any revision object is written; the outcome replaces it
atomically. Each frame has intrinsic SHA-256 integrity, the chain authenticates
frame order, and Commit retry/recovery retains the same exact bytes.
A changes-only draft may produce an empty archive; that means *no individual
actions recorded*, not permission to fabricate Undo steps. The draft's bounded
journal is cleared on successful rebase, whereas the Commit ledger remains.
Read currently via `ProjectService::localCommitActionHistory(operationId)`;
paginated branch timelines and GUI Undo/Redo are future work.
Behaviour: [working copies](working-copies.md#local-commit).

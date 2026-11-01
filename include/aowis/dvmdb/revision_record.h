#pragma once

#include "aowis/dvmdb/native_store.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace aowis::dvmdb {

// Canonical immutable revision record, *not* a branch or the branch HEAD.
// UUIDs identify logical revisions; record and snapshot SHA-256 hashes verify
// immutable content. This codec does not change the active repository format.
struct RevisionParent {
    RevisionId id;
    Hash expectedRecordHash{};
};

struct RevisionRecord {
    RevisionId id;
    ProjectId project;
    Hash root{};
    Hash referencesRoot{};
    bool modelIndexed = false;
    std::vector<RevisionParent> parents;
    CommitMetadata metadata;
    std::uint64_t committedAtMs = 0;  // Unix epoch ms (UTC) of publication; never 0
};

// Canonical `T` 0x03 encoding (docs/storage-formats.md): 100 + 48 * parents
// bytes of identity, roots and parents, then the commit time, author and
// message. Input validation failures are StorageError(InvalidInput);
// malformed records obtained from storage are StorageError(Corruption).
Bytes encodeRevisionRecord(const RevisionRecord& record);
RevisionRecord decodeRevisionRecord(const Bytes& bytes);
Hash hashRevisionRecord(const RevisionRecord& record);

// Verify an entire reachable DAG from its immutable record bytes. `getRecord`
// must fetch by UUID (not by a user-controlled filename or display name).
// Recomputes the SHA of each record and verifies every ordered parent UUID/SHA
// binding, common project provenance, and acyclicity. Bounds traversal to
// maxRevisions; no record or branch is published by this operation.
// `trusted` (optional) marks a parent binding that is already published and
// verified locally: it is accepted as a boundary and its ancestry is not
// walked, so an incremental import costs the delta, not the history.
using RevisionRecordLookup =
    std::function<std::optional<Bytes>(const RevisionId&)>;
using TrustedRevision = std::function<bool(const RevisionParent&)>;
Hash verifyRevisionAncestry(const Bytes& tipRecord,
                              const RevisionRecordLookup& getRecord,
                              std::size_t maxRevisions = 250000,
                              const TrustedRevision& trusted = {});

} // namespace aowis::dvmdb

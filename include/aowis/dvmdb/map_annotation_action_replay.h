#pragma once

// B4.3: authenticated, read-only replay of annotation-only MODEL archives.
//
// This deliberately accepts COMPLETE immutable parent/child model candidates
// plus the exact native delta proved by ProjectService's local Commit witness.
// Neither user-selected layer IDs nor native manifest edits are trusted. The
// annotation's layer is established from membership in the two snapshots.
// An event with no provable layer (e.g. create-then-delete within the archive)
// fails closed until the MODEL history envelope carries explicit ownership.
//
// This is a proof/preview primitive, NOT a publisher or an HTTP History cursor.
// Mixed hydraulic/annotation events, layer creation/deletion, and archived
// History forks still require integration in the general revision planner.

#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/project_service.h"

#include <QString>
#include <QUuid>

#include <cstddef>
#include <optional>
#include <vector>

namespace aowis::dvmdb::annotations {

struct VerifiedAnnotationActionBoundary final {
    // Minimal native delta from the immutable parent to the requested action
    // boundary. Can be inspected but MUST NOT be published without the full
    // multi-domain History fence, lease, generation and ancestry checks.
    std::vector<ModelChange> changesFromParent;
    std::vector<QUuid> changedLayerIds;
    std::size_t actionCount = 0;
};

// Every action must be a canonical annotation-only MODEL transaction with a
// unique archive identity. `authenticatedActionChain` MUST come from the
// verified immutable local Commit (RevisionNativeActionWitness); never compute
// it from caller-supplied actions being checked. This binds the original
// chronological order even when independent actions commute and produce the
// same endpoint snapshot. DVMDB's chain uses SHA-256 with a zero initial hash
// and SHA-256(previous || raw MODEL frame) for each archived action.
// The replay derives server-owned member/chunk/root changes, revalidates each
// full candidate, and proves exact equality with the immutable child AND the
// authenticated native revision delta. actionPosition is a boundary
// 0..actions.size(), not an array index.
std::optional<VerifiedAnnotationActionBoundary> replayVerifiedAnnotationActionBoundary(
    const MergeCandidate& parent,
    const MergeCandidate& committed,
    const std::vector<WorkingCopyEvent>& actions,
    const std::vector<ModelChange>& actualNativeChanges,
    const Hash& authenticatedActionChain,
    std::size_t actionPosition,
    QString* error = nullptr);

} // namespace aowis::dvmdb::annotations

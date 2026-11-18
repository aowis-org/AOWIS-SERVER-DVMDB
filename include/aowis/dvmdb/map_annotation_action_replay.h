#pragma once

// B4.3: authenticated replay of annotation-only and bounded combined MODEL archives.
//
// This deliberately accepts COMPLETE immutable parent/child model candidates
// plus the exact native delta proved by ProjectService's local Commit witness.
// Neither user-selected layer IDs nor native manifest edits are trusted.
// Each annotation edit includes a canonical ownerLayerId authenticated by the
// immutable Commit action chain. Endpoint membership additionally proves that
// surviving members have not silently changed ownership. Transient
// create-then-delete actions can now be reconstructed safely.
//
// The replay primitive is read-only. The separate verified annotation-only
// navigation adapter uses the existing fenced native History publisher.
// Combined existing-hydraulic-entity updates/root metadata and annotations are
// supported by a separate verified path. Hydraulic membership changes, layer
// creation/deletion, and archived History forks are not supported yet.

#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/project_service.h"

#include <QString>
#include <QUuid>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
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

// Combined read-only replay for canonical action streams containing both
// annotations and existing hydraulic entity updates/root metadata. It verifies
// every intermediate candidate and the full native revision delta in original
// authenticated action order. Hydraulic member creation/deletion, membership
// reparenting and archived forks fail closed until separately implemented.
std::optional<VerifiedAnnotationActionBoundary> replayVerifiedCombinedActionBoundary(
    const MergeCandidate& parent,
    const MergeCandidate& committed,
    const std::vector<WorkingCopyEvent>& actions,
    const std::vector<ModelChange>& actualNativeChanges,
    const Hash& authenticatedActionChain,
    std::size_t actionPosition,
    QString* error = nullptr);

// B4.3 read-only bridge from a REAL indexed local Commit to the annotation
// replay verifier. The project/branch, immutable first parent, exact native
// delta, and SHA-256 action order are obtained from ProjectService, not from
// caller-supplied snapshots or action arrays. Initial, imported, forked, and
// nonlocal archives remain checkpoint-only in this adapter.
struct VerifiedIndexedAnnotationActionBoundary final {
    std::string revisionId;
    std::string commitOperationId;
    VerifiedAnnotationActionBoundary boundary;
};
std::optional<VerifiedIndexedAnnotationActionBoundary> verifyIndexedAnnotationActionBoundary(
    ProjectService& service, std::string_view projectId, std::string_view branch,
    const RevisionId& revision, std::size_t actionPosition,
    QString* error = nullptr);

std::optional<VerifiedIndexedAnnotationActionBoundary> verifyIndexedCombinedActionBoundary(
    ProjectService& service, std::string_view projectId, std::string_view branch,
    const RevisionId& revision, std::size_t actionPosition,
    QString* error = nullptr);

// Publish an annotation-only archived action boundary through the same atomic,
// lease/generation/HEAD-fenced History writer used by hydraulic navigation.
// Only the server can construct candidateChanges; callers supply coordinates,
// never record deltas. Forks and hydraulic membership changes remain unsupported.
std::optional<WorkingCopyInfo> navigateVerifiedAnnotationAction(
    ProjectService& service, std::string_view operationId,
    std::string_view draftId, std::string_view sessionId,
    std::string_view fencingToken, const RevisionId& expectedHead,
    const RevisionId& revision, std::size_t actionPosition,
    std::uint64_t expectedGeneration, QString* error = nullptr);

// Mixed hydraulic scalar/root-metadata + annotation archived navigation.
// Publication uses the same generation/HEAD/lease fenced writer and is
// allowed only after complete MODEL image/native delta proof.
std::optional<WorkingCopyInfo> navigateVerifiedCombinedAction(
    ProjectService& service, std::string_view operationId,
    std::string_view draftId, std::string_view sessionId,
    std::string_view fencingToken, const RevisionId& expectedHead,
    const RevisionId& revision, std::size_t actionPosition,
    std::uint64_t expectedGeneration, QString* error = nullptr);

} // namespace aowis::dvmdb::annotations

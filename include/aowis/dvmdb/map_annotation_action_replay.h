#pragma once

// B4.3: authenticated replay of annotation-only and bounded combined MODEL archives.
//
// Replay works on changes: the replayed state is a native overlay on a pinned,
// already validated revision (point reads and reverse-reference queries
// only), compared with the exact native delta of ProjectService's local
// Commit witness. Nothing is copied whole, so History has no project-size
// limit. The two replayVerified*ActionBoundary functions below instead take
// COMPLETE caller-supplied candidates and check them completely.
// Neither user-selected layer IDs nor native manifest edits are trusted.
// Each annotation edit includes a canonical ownerLayerId authenticated by the
// immutable Commit action chain. Endpoint membership additionally proves that
// surviving members have not silently changed ownership. Transient
// create-then-delete actions can now be reconstructed safely.
//
// The replay primitive is read-only. The separate verified annotation-only
// navigation adapter uses the existing fenced native History publisher.
// Combined hydraulic root/member changes and annotations are verified by the
// shared native membership materializer. Layer creation/deletion, ambiguous
// multi-network transient hydraulic ownership and archived History forks are
// not supported yet.

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
// The replay derives server-owned member/chunk/root changes with the Save
// materializers, validates the complete parent and child candidates, checks
// the selected boundary with the Save delta validator (not every
// intermediate action: A2), and proves
// exact equality with the immutable child AND the authenticated native
// revision delta. actionPosition is a boundary
// 0..actions.size(), not an array index.
std::optional<VerifiedAnnotationActionBoundary> replayVerifiedAnnotationActionBoundary(
    const MergeCandidate& parent,
    const MergeCandidate& committed,
    const std::vector<WorkingCopyEvent>& actions,
    const std::vector<ModelChange>& actualNativeChanges,
    const Hash& authenticatedActionChain,
    std::size_t actionPosition,
    QString* error = nullptr);

// Combined read-only replay for canonical action streams containing hydraulic
// and annotation edits, including membership creation/deletion with provable
// network ownership. Verifies each intermediate candidate and exact immutable
// native delta in authenticated action order. Ambiguous multi-network transient
// ownership and archived forks fail closed.
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
// caller-supplied snapshots or action arrays. A first Commit (no parent)
// replays from its archived journal seed; one made before seeds were archived,
// imported and nonlocal archives remain checkpoint-only in this adapter.
struct VerifiedIndexedAnnotationActionBoundary final {
    std::string revisionId;
    std::string commitOperationId;
    // `boundary.changesFromParent` is relative to the archive's starting
    // state: `originRevision` + `originChanges`. Without a History fork that
    // is the immutable first parent and no changes; after a fork it is the
    // verified source boundary the archive started from; for a first Commit
    // it is the Commit's own revision with every record removed, plus the
    // archived journal seed.
    VerifiedAnnotationActionBoundary boundary;
    std::optional<RevisionId> originRevision; // always set by the verifier
    std::vector<ModelChange> originChanges;
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

// Live, already-saved MODEL journal navigation. Annotations (and mixed
// hydraulic/annotation actions) replay against their complete persisted
// cursor, then publish through the existing fenced draft History writer.
// Hydraulic-only journals retain their established navigator. Archived forks
// from verified local Commits and unborn drafts with reversible journal
// provenance are supported.

// Read-only eligibility proof for the exact saved action boundary. Used by
// the History cursor on unborn branches so it never advertises an Undo that
// cannot reconstruct its nonjournaled initialization seed.
// Read-only planners: the HEAD-relative native overlay that the matching
// navigate function would publish (multi-domain mode: annotation, hydraulic
// or mixed journals). Nothing is changed.
std::optional<std::vector<ModelChange>> planVerifiedActiveDraftAction(
    ProjectService& service, std::string_view draftId,
    std::optional<RevisionId> expectedHead, std::size_t actionPosition,
    std::uint64_t expectedGeneration, QString* error = nullptr);
std::optional<std::vector<ModelChange>> planVerifiedArchivedAction(
    ProjectService& service, std::string_view draftId, const RevisionId& expectedHead,
    const RevisionId& revision, std::size_t actionPosition,
    std::uint64_t expectedGeneration, QString* error = nullptr);
// The same eligibility for several positions with ONE replay of the journal
// (e.g. both neighbours of the History cursor). One flag per position; all
// false when the journal itself is not proven (`error` says why).
std::vector<bool> canNavigateVerifiedActiveDraftActions(
    ProjectService& service, std::string_view draftId,
    std::optional<RevisionId> expectedHead, const std::vector<std::size_t>& actionPositions,
    std::uint64_t expectedGeneration, QString* error = nullptr);
bool canNavigateVerifiedActiveAnnotationDraftAction(
    ProjectService& service, std::string_view draftId,
    std::optional<RevisionId> expectedHead, std::size_t actionPosition,
    std::uint64_t expectedGeneration, QString* error = nullptr);

std::optional<WorkingCopyInfo> navigateVerifiedActiveDraftAction(
    ProjectService& service, std::string_view operationId,
    std::string_view draftId, std::string_view sessionId,
    std::string_view fencingToken, std::optional<RevisionId> expectedHead,
    std::size_t actionPosition, std::uint64_t expectedGeneration,
    QString* error = nullptr);

} // namespace aowis::dvmdb::annotations

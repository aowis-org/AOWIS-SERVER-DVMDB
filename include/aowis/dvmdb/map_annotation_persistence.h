#pragma once

// Native, revision-scoped map annotation record format. MODEL owns the typed
// values; DVMDB owns record kinds, exact references and membership chunks.
// No GUI or renderer dependency. B3.1 exposes draft planning only; only the
// fenced ProjectService may publish records and corresponding MODEL events.
#include "aowis/dvmdb/engineering_delta_scope.h"
#include "aowis/dvmdb/native_store.h"

#include <aowis/model/map/map_annotation.h>
#include <aowis/model/edit/model_edit_transaction.h>
#include <QList>
#include <QUuid>

#include <cstddef>
#include <functional>
#include <set>
#include <optional>
#include <vector>

namespace aowis::dvmdb::annotations {

inline constexpr std::uint16_t annotationLayerKind = 110;
inline constexpr std::uint16_t annotationChunkKind = 111;
inline constexpr std::uint16_t annotationMemberKind = 112;
inline constexpr std::size_t annotationChunkCapacity = 256;

struct AnnotationLayerSnapshot final {
    MapAnnotationLayer layer;
    QList<MapAnnotation> annotations;
    // Native membership chunk UUIDs as stored (A5: chunk-local layout, so
    // they are not derivable from the member count).
    QList<QUuid> chunkIds;
};

// Stable Distribution layer UUID derived from its network UUID. This is a
// namespace-scoped UUIDv8, not another allocated project/branch/revision ID.
// Deterministic identity makes initialize/import safe to retry after a lost ACK.
QUuid distributionLayerUuid(const QUuid& networkId);

// Pure, server-owned record materialization for an entire new annotation layer.
// Membership order is stable; IDs are deterministic per layer and chunk serial.
// For a Distribution layer its owning network root must be present in the
// SAME candidate; for an unowned Drainage layer no owner reference is written.
// Cross-layer uniqueness and anchor-target ownership are B2 candidate-wide
// checks, not satisfied by calling this builder alone.
std::vector<ModelChange> fullDraftAnnotationLayerChanges(
    const MapAnnotationLayer& layer, const QList<MapAnnotation>& annotations);

// B3.1: plan a reversible annotation-edit batch against ONE pinned draft view.
// Every edit must have entityType "map.annotation", matching UUID, canonical
// MODEL before/after images, and an existing owner layer. Insertions append to
// the layer; deletions retain the order of survivors. Multiple consecutive edits
// to the same annotation are permitted and checked against their intermediate
// before-images. The server derives ALL native membership/root/chunk changes;
// callers must never send those kinds directly.
//
// The return value is a minimal native delta (including deleted-chunk/member
// tombstones), not a complete replacement snapshot. This function performs no
// I/O or publication. B3.2 must atomically persist these changes together
// with the ORIGINAL canonical transactions using ProjectService's fenced
// saveWorkingCopyTransactions, after final candidate-wide B2 validation.
std::vector<ModelChange> materializeDraftAnnotationEdits(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId,
    const std::vector<aowis::model::ModelEntityEdit>& edits);

// Decode and check a complete layer through one pinned snapshot or draft view.
// Invalid records throw StorageError(Corruption); absent layer returns nullopt.
// This validates structure, canonical payloads and direct reference closure.
// Candidate-wide cardinality and anchor scope require B2's graph verifier.
std::optional<AnnotationLayerSnapshot> loadAnnotationLayer(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId);

// Point read of ONE annotation member: record kind, canonical MODEL payload,
// UUID and exact anchor reference are checked. Layer ownership is NOT proven
// here (see validateAnnotationSnapshotDelta). Absent member returns nullopt.
std::optional<MapAnnotation> loadAnnotationMember(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& annotationId);

// Layer root and membership chunks only, both canonically decoded; member
// records are NOT read. For listings and for HTTP limits that must be checked
// before any member is loaded (A6). Absent layer returns nullopt.
struct AnnotationLayerSummary final {
    MapAnnotationLayer layer;
    std::size_t memberCount = 0;
};
std::optional<AnnotationLayerSummary> loadAnnotationLayerSummary(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId);

// D1: one page of a layer, for HTTP reads of large layers. The members of
// the chunks [fromChunk, fromChunk + chunkLimit) in layer order (no limit:
// to the end). The root and the page's chunks are decoded first and
// `memberLimit` is checked before any member record is read (A6), then every
// member is checked as in loadAnnotationLayer. A page that starts at the end
// of the chunk list is empty. Absent layer returns nullopt; a start after the
// end, a zero chunk limit or too many members throw InvalidInput. Members are
// unique within a page; across pages that is guaranteed by the candidate
// validator, so a reader holding several pages must use one draft generation
// or one revision for all of them.
struct AnnotationLayerPage final {
    MapAnnotationLayer layer;
    QList<MapAnnotation> annotations;
    std::size_t chunkCount = 0;  // chunks listed by the root
    std::size_t fromChunk = 0;
    std::size_t nextChunk = 0;   // first chunk of the next page; == chunkCount on the last
};
std::optional<AnnotationLayerPage> loadAnnotationLayerPage(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId, std::size_t fromChunk, std::optional<std::size_t> chunkLimit,
    std::size_t memberLimit);

// D2: the layer root (metadata, chunk list) and, with includeMembers, the
// member UUIDs in layer order read from the membership chunks. Member records
// are never read. Absent layer returns nullopt; damage throws Corruption.
struct AnnotationLayerOrder final {
    MapAnnotationLayer layer;
    QList<QUuid> chunkIds;
    QList<QUuid> members; // empty unless includeMembers
};
std::optional<AnnotationLayerOrder> loadAnnotationLayerOrder(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId, bool includeMembers);

// A5: Save-time check of a layer whose root or chunks changed, WITHOUT
// decoding its unchanged members. `before` must be a candidate that satisfied
// the complete validator and `changedKeys` the keys the delta changed.
// Checks the new root (canonical, owner exists, no chunk listed twice) and
// every chunk that changed or is newly listed (canonical, UUID derived from
// its layer and serial). Returns nullopt for an absent
// layer. `metadataChanged` (also for a newly created layer) means the caller
// must run the complete validator. `enteredMembers` are members listed by a
// checked chunk that were not in this layer before; the caller decodes them
// and checks their anchors individually.
struct AnnotationLayerDelta final {
    MapAnnotationLayer layer;
    bool metadataChanged = false;
    std::vector<QUuid> enteredMembers;
};
std::optional<AnnotationLayerDelta> checkAnnotationLayerDelta(
    const EngineeringCandidateView& before, const EngineeringCandidateView& after,
    const QUuid& layerId, const std::set<Key>& changedKeys);

// Root-only read of a layer's canonical MODEL metadata; no chunk or member is
// read. Absent layer returns nullopt; an invalid root throws Corruption.
std::optional<MapAnnotationLayer> loadAnnotationLayerMetadata(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId);

// Convenience wrapper that pins every lookup to the SAME immutable revision.
std::optional<AnnotationLayerSnapshot> loadAnnotationLayerAtRevision(
    Repository& store, const ProjectId& project, const RevisionId& revision,
    const QUuid& layerId);

} // namespace aowis::dvmdb::annotations

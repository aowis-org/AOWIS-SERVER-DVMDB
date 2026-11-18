#pragma once

// Native, revision-scoped map annotation record format. MODEL owns the typed
// values; DVMDB owns record kinds, exact references and membership chunks.
// No GUI or renderer dependency. B3.1 exposes draft planning only; only the
// fenced ProjectService may publish records and corresponding MODEL events.
#include "aowis/dvmdb/native_store.h"

#include <aowis/model/map/map_annotation.h>
#include <aowis/model/edit/model_edit_transaction.h>
#include <QList>
#include <QUuid>

#include <functional>
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
};

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

// Convenience wrapper that pins every lookup to the SAME immutable revision.
std::optional<AnnotationLayerSnapshot> loadAnnotationLayerAtRevision(
    Repository& store, const ProjectId& project, const RevisionId& revision,
    const QUuid& layerId);

} // namespace aowis::dvmdb::annotations

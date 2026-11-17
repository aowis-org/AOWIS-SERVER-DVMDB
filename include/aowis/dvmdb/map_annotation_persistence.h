#pragma once

// Native, revision-scoped map annotation record format. MODEL owns the typed
// values; DVMDB owns record kinds, exact references and membership chunks.
// No GUI or renderer dependency. Draft publication is deliberately not exposed
// here: B3 will publish these changes under the existing fenced writer.
#include "aowis/dvmdb/native_store.h"

#include <aowis/model/map/map_annotation.h>
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

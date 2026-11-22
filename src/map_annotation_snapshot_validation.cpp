#include "aowis/dvmdb/map_annotation_snapshot_validation.h"

#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/model_field_merge.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/storage_error.h"

#include <aowis/model/map/map_annotation.h>

#include <QMap>
#include <QHash>
#include <QSet>
#include <aowis/model/serialization/model_codec.h>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <stdexcept>
#include <utility>
#include <string>

namespace aowis::dvmdb::annotations {
namespace {

using AnchorTypes = std::map<Key, QString>;

void ensure(bool condition, const char* description) {
    if (!condition)
        throw StorageError(StorageErrorKind::Corruption, description);
}

bool isKnownKind(std::uint16_t kind) {
    return isHydraulicMemberKind(kind) || kind == 100 || kind == 101 ||
           kind == annotationLayerKind || kind == annotationChunkKind ||
           kind == annotationMemberKind;
}

void own(std::set<Key>& owned, const Key& id, const char* description) {
    ensure(owned.insert(id).second, description);
}

void insertAnchor(AnchorTypes& anchors, const QUuid& uuid, const QString& type) {
    ensure(anchors.emplace(record::keyFromUuid(uuid), type).second,
           "duplicate anchor-capable hydraulic entity UUID");
}

AnchorTypes allowedAnchors(const NetworkHydraulicDistribution& network) {
    AnchorTypes anchors;
    for (const HydraulicNodeJunction& item : network.nodes_junctions)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicNodeJunction"));
    for (const HydraulicNodeReservoir& item : network.nodes_reservoirs)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicNodeReservoir"));
    for (const HydraulicNodeTank& item : network.nodes_tanks)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicNodeTank"));
    for (const HydraulicDemandPoint& item : network.demand_points)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicDemandPoint"));
    return anchors;
}

void collectHydraulicOwnership(const MergeCandidate& candidate, const Key& rootId,
                               std::set<Key>& owned) {
    const std::optional<ModelRecord> root = candidate.get(rootId);
    ensure(root && root->kind == hydraulic::hydraulicDistributionNetworkKind,
           "missing or invalid hydraulic root in candidate");
    own(owned, rootId, "duplicate hydraulic network root ownership");
    // The hydraulic loader verifies these native manifest/chunk references
    // against canonical MODEL payloads before we use the indexed closure.
    for (const Key& chunkId : root->references) {
        const std::optional<ModelRecord> chunk = candidate.get(chunkId);
        ensure(chunk && chunk->kind == 101,
               "missing or invalid hydraulic membership chunk");
        own(owned, chunkId, "hydraulic membership chunk belongs to multiple networks");
        for (const Key& memberId : chunk->references) {
            const std::optional<ModelRecord> member = candidate.get(memberId);
            ensure(member && isHydraulicMemberKind(member->kind),
                   "missing or invalid hydraulic collection member");
            own(owned, memberId, "hydraulic entity belongs to multiple networks");
        }
    }
}

void collectAnnotationOwnership(const MergeCandidate& candidate, const Key& layerId,
                                std::set<Key>& owned) {
    const std::optional<ModelRecord> root = candidate.get(layerId);
    ensure(root && root->kind == annotationLayerKind,
           "missing or invalid annotation layer root");
    own(owned, layerId, "duplicate annotation layer ownership");
    // Root references also contain its network owner. It is NOT part of the
    // annotation-owned record set; only membership chunks are owned here.
    for (const Key& reference : root->references) {
        const std::optional<ModelRecord> child = candidate.get(reference);
        ensure(child.has_value(), "missing annotation layer dependency");
        if (child->kind == 100) continue;
        ensure(child->kind == annotationChunkKind,
               "annotation layer references a wrong-kind chunk");
        own(owned, reference, "annotation membership chunk belongs to multiple layers");
        for (const Key& memberId : child->references) {
            const std::optional<ModelRecord> member = candidate.get(memberId);
            ensure(member && member->kind == annotationMemberKind,
                   "annotation chunk contains missing or wrong-kind member");
            own(owned, memberId, "annotation belongs to multiple layers or chunks");
        }
    }
}

} // namespace

std::optional<std::string> validateAnnotationSnapshot(const MergeCandidate& candidate) {
    try {
        std::map<Key, std::uint16_t> existing;
        std::set<Key> networkRoots;
        std::set<Key> layerRoots;
        candidate.forEach([&](const Key& id, const ModelRecord& entry) {
            ensure(isKnownKind(entry.kind), "unsupported native model record kind in annotation candidate");
            ensure(!(isClientIdentifiedEngineeringKind(entry.kind) && isServerDerivedUuidKey(id)),
                   "UUID version 8 is reserved for server-derived records");
            ensure(existing.emplace(id, entry.kind).second,
                   "duplicate UUID in annotation candidate enumeration");
            if (entry.kind == hydraulic::hydraulicDistributionNetworkKind)
                networkRoots.insert(id);
            if (entry.kind == annotationLayerKind)
                layerRoots.insert(id);
        });

        std::map<Key, AnchorTypes> networkAnchors;
        std::set<Key> owned;
        for (const Key& id : networkRoots) {
            const std::optional<NetworkHydraulicDistribution> network =
                hydraulic::loadDraftHydraulicDistributionNetwork(
                    candidate.get, record::uuidFromKey(id));
            ensure(network.has_value(), "invalid candidate distribution network");
            networkAnchors.emplace(id, allowedAnchors(*network));
            collectHydraulicOwnership(candidate, id, owned);
        }

        std::set<Key> assignedNetworks;
        bool drainageLayerSeen = false;
        for (const Key& id : layerRoots) {
            const std::optional<AnnotationLayerSnapshot> layer =
                loadAnnotationLayer(candidate.get, record::uuidFromKey(id));
            ensure(layer.has_value(), "invalid candidate annotation layer");
            if (layer->layer.domain == MapAnnotationDomain::Distribution) {
                ensure(layer->layer.owner_network_uuid.has_value(),
                       "distribution annotation layer lacks network owner");
                const Key ownerId = record::keyFromUuid(*layer->layer.owner_network_uuid);
                const std::map<Key, AnchorTypes>::const_iterator network = networkAnchors.find(ownerId);
                ensure(network != networkAnchors.end(),
                       "annotation layer belongs to an absent or foreign distribution network");
                ensure(assignedNetworks.insert(ownerId).second,
                       "distribution network has multiple annotation layers");
                for (const MapAnnotation& annotation : layer->annotations) {
                    if (!annotation.anchor) continue;
                    const Key anchorId = record::keyFromUuid(annotation.anchor->entity_uuid);
                    const AnchorTypes::const_iterator found = network->second.find(anchorId);
                    ensure(found != network->second.end(),
                           "annotation anchor is not owned by this distribution network");
                    ensure(found->second == annotation.anchor->entity_type,
                           "annotation anchor has the wrong canonical entity type");
                }
            } else if (layer->layer.domain == MapAnnotationDomain::Drainage) {
                ensure(!drainageLayerSeen, "project has multiple pre-network drainage annotation layers");
                drainageLayerSeen = true;
                for (const MapAnnotation& annotation : layer->annotations)
                    ensure(!annotation.anchor, "pre-network drainage annotation must not have an anchor");
            } else {
                ensure(false, "unsupported annotation layer domain");
            }
            collectAnnotationOwnership(candidate, id, owned);
        }
        ensure(assignedNetworks.size() == networkRoots.size(),
               "distribution network is missing its required annotation layer");
        ensure(owned.size() == existing.size(),
               "candidate contains unowned or orphaned native model records");
        for (const std::pair<const Key, std::uint16_t>& entry : existing)
            ensure(owned.contains(entry.first),
                   "candidate contains unowned or orphaned native model record");
        return std::nullopt;
    } catch (const std::exception& error) {
        // A3: an unreadable candidate (I/O, ENOSPC, busy) is not an invalid one.
        rethrowIfNotVerificationFailure();
        return std::string(error.what());
    }
}

namespace {

// The MODEL anchor registry names the entity type; the native hydraulic
// adapter owns the record kind of each collection. Same set as
// allowedAnchors() above.
std::uint16_t anchorRecordKind(const QString& entityType) {
    struct AnchorKind final {
        const char* entityType;
        hydraulic::HydraulicEntityType collection;
    };
    static const AnchorKind anchorKinds[] = {
        {"HydraulicNodeJunction", hydraulic::HydraulicEntityType::Junction},
        {"HydraulicNodeReservoir", hydraulic::HydraulicEntityType::Reservoir},
        {"HydraulicNodeTank", hydraulic::HydraulicEntityType::Tank},
        {"HydraulicDemandPoint", hydraulic::HydraulicEntityType::DemandPoint},
    };
    for (const AnchorKind& entry : anchorKinds) {
        if (entityType == QString::fromLatin1(entry.entityType))
            return hydraulic::hydraulicMemberKindForCollection(
                static_cast<std::size_t>(entry.collection));
    }
    ensure(false, "annotation anchor has an unsupported canonical entity type");
    return 0;
}

Key singleReferrer(const EngineeringCandidateView& view, const Key& id,
                   std::uint16_t kind, const char* description) {
    const std::vector<Key> owners = engineeringReferrersOfKind(view, id, kind);
    ensure(owners.size() == 1, description);
    return owners.front();
}

// Point-read equivalent of the anchor rules in validateAnnotationSnapshot:
// the target exists, has the record kind of its declared MODEL type and is a
// member of the layer's owner network.
void checkAnchorScope(const EngineeringCandidateView& after, const MapAnnotationLayer& layer,
                      const MapAnnotation& annotation) {
    if (!annotation.anchor) return;
    ensure(layer.domain == MapAnnotationDomain::Distribution &&
           layer.owner_network_uuid.has_value(),
           "pre-network drainage annotation must not have an anchor");
    const Key target = record::keyFromUuid(annotation.anchor->entity_uuid);
    const std::optional<ModelRecord> targetRecord = after.get(target);
    ensure(targetRecord.has_value(), "annotation anchor target is missing");
    ensure(targetRecord->kind == anchorRecordKind(annotation.anchor->entity_type),
           "annotation anchor has the wrong canonical entity type");
    const Key chunk = singleReferrer(after, target, engineeringNetworkChunkKind,
                                     "annotation anchor is not owned by this distribution network");
    const Key network = singleReferrer(after, chunk, engineeringNetworkKind,
                                       "annotation anchor is not owned by this distribution network");
    ensure(network == record::keyFromUuid(*layer.owner_network_uuid),
           "annotation anchor is not owned by this distribution network");
}

} // namespace

std::optional<std::string> validateAnnotationSnapshotDelta(
    const EngineeringCandidateView& before, const EngineeringCandidateView& after,
    const std::vector<ModelChange>& delta,
    const std::function<std::optional<std::string>()>& completeValidation) {
    try {
        const EngineeringDeltaScope scope = engineeringDeltaScope(before, after, delta);
        if (scope.fullValidationRequired) return completeValidation();
        // Hydraulic semantics (values, topology, catalogue references) are
        // proven by the canonical network loader, one touched network at a time.
        for (const Key& network : scope.networks) {
            const std::optional<NetworkHydraulicDistribution> loaded =
                hydraulic::loadDraftHydraulicDistributionNetwork(
                    after.get, record::uuidFromKey(network));
            ensure(loaded.has_value(), "invalid candidate distribution network");
        }
        // A5: a layer whose root or chunks changed is checked through its
        // root and its changed chunks only; members are decoded only when they
        // changed (scope.annotations) or newly entered the layer.
        std::set<Key> changed;
        for (const ModelChange& change : delta) changed.insert(change.key);
        std::set<Key> checked;
        for (const Key& layerKey : scope.layers) {
            const std::optional<AnnotationLayerDelta> current =
                checkAnnotationLayerDelta(before, after, record::uuidFromKey(layerKey), changed);
            ensure(current.has_value(), "invalid candidate annotation layer");
            // Domain or owner changes alter project-wide layer cardinality.
            if (current->metadataChanged) return completeValidation();
            for (const QUuid& member : current->enteredMembers) {
                const std::optional<MapAnnotation> annotation =
                    loadAnnotationMember(after.get, member);
                ensure(annotation.has_value(), "missing annotation member in candidate");
                checkAnchorScope(after, current->layer, *annotation);
                checked.insert(record::keyFromUuid(member));
            }
        }
        for (const Key& member : scope.annotations) {
            if (checked.contains(member)) continue;
            const std::optional<MapAnnotation> annotation =
                loadAnnotationMember(after.get, record::uuidFromKey(member));
            ensure(annotation.has_value(), "missing annotation member in candidate");
            const Key chunk = singleReferrer(after, member, engineeringLayerChunkKind,
                                             "annotation belongs to multiple layers or chunks");
            const Key layerKey = singleReferrer(after, chunk, engineeringLayerKind,
                                                "annotation membership chunk belongs to multiple layers");
            const std::optional<MapAnnotationLayer> layer =
                loadAnnotationLayerMetadata(after.get, record::uuidFromKey(layerKey));
            ensure(layer.has_value(), "missing annotation layer in candidate");
            checkAnchorScope(after, *layer, *annotation);
        }
        return std::nullopt;
    } catch (const std::exception& error) {
        rethrowIfNotVerificationFailure();
        return std::string(error.what());
    }
}

} // namespace aowis::dvmdb::annotations

namespace aowis::dvmdb::annotations {
namespace {

// Synthetic merge field for an annotation's placement: `anchor` together
// with `position_wgs84`. Attaching keeps the last explicit position and
// derives offsets from it, so a concurrent free drag must never be combined
// with an attach, detach or re-target (A10). Clients select it like any other
// field choice: {"field": "placement", "side": "target"|"source"}.
const QString placementField = QStringLiteral("placement");

QByteArray placementBytes(const MapAnnotation& annotation) {
    MapAnnotation placement;
    placement.uuid = annotation.uuid;
    placement.text = QStringLiteral("placement");
    placement.position_wgs84 = annotation.position_wgs84;
    placement.anchor = annotation.anchor;
    return aowis::model::ModelCodec<MapAnnotation>::encode(placement);
}

void applyPlacement(MapAnnotation& annotation, const MapAnnotation& winner) {
    annotation.position_wgs84 = winner.position_wgs84;
    annotation.anchor = winner.anchor;
}

// A4: field-level annotation merge with the caller's field choices. Text and
// style fields merge like hydraulic entity fields; the placement merges as one
// unit; the native anchor reference is derived from the merged image, so an
// attach on one branch combines with a text edit on the other. The complete
// candidate validator re-checks every anchor target after the merge.
// A choice for a field that does not conflict is rejected (InvalidInput).
MergeResolution resolveAnnotationMember(const Key& key, const ModelRecord& base,
                                        const ModelRecord& target, const ModelRecord& source,
                                        const QMap<QString, bool>& choices) {
    MergeResolution conflict;
    if (base.kind != annotationMemberKind || target.kind != annotationMemberKind ||
        source.kind != annotationMemberKind)
        return conflict;
    const QUuid id = record::uuidFromKey(key);
    const QByteArray baseBytes = record::qByteArrayFromBytes(base.payload);
    const std::optional<MapAnnotation> baseValue = aowis::model::ModelCodec<MapAnnotation>::decode(baseBytes);
    const std::optional<MapAnnotation> targetValue = aowis::model::ModelCodec<MapAnnotation>::decode(
        record::qByteArrayFromBytes(target.payload));
    const std::optional<MapAnnotation> sourceValue = aowis::model::ModelCodec<MapAnnotation>::decode(
        record::qByteArrayFromBytes(source.payload));
    if (!baseValue || !targetValue || !sourceValue) return conflict;

    QMap<QString, bool> fieldChoices = choices;
    const bool placementChosen = fieldChoices.contains(placementField);
    const bool placementFromSource = fieldChoices.take(placementField);
    const QByteArray basePlacement = placementBytes(*baseValue);
    const QByteArray targetPlacement = placementBytes(*targetValue);
    const QByteArray sourcePlacement = placementBytes(*sourceValue);
    if (basePlacement.isEmpty() || targetPlacement.isEmpty() || sourcePlacement.isEmpty())
        return conflict;
    const bool targetMoved = targetPlacement != basePlacement;
    const bool sourceMoved = sourcePlacement != basePlacement;
    const bool placementConflict = targetMoved && sourceMoved && targetPlacement != sourcePlacement;
    if (placementChosen && !placementConflict)
        throw StorageError(StorageErrorKind::InvalidInput,
                           "merge selection is not a conflicting field: placement");
    const bool takeSourcePlacement = placementConflict ? placementFromSource
                                                       : (sourceMoved && !targetMoved);
    // Both sides carry the winning placement, so the per-field MODEL merge can
    // never split anchor and position. Without a choice the target placement
    // is used only to list the remaining conflicts.
    MapAnnotation normalizedTarget = *targetValue;
    MapAnnotation normalizedSource = *sourceValue;
    const MapAnnotation& winner = takeSourcePlacement ? *sourceValue : *targetValue;
    applyPlacement(normalizedTarget, winner);
    applyPlacement(normalizedSource, winner);
    const QByteArray targetBytes = aowis::model::ModelCodec<MapAnnotation>::encode(normalizedTarget);
    const QByteArray sourceBytes = aowis::model::ModelCodec<MapAnnotation>::encode(normalizedSource);
    if (targetBytes.isEmpty() || sourceBytes.isEmpty()) return conflict;

    if (!fieldChoices.isEmpty()) {
        const std::optional<ModelFieldMerge> preview = mergeModelFields(
            id, QStringLiteral("map.annotation"), baseBytes, targetBytes, sourceBytes);
        if (!preview)
            throw StorageError(StorageErrorKind::Corruption,
                               "cannot resolve fields of an invalid canonical annotation");
        for (QMap<QString, bool>::const_iterator choice = fieldChoices.cbegin();
             choice != fieldChoices.cend(); ++choice) {
            if (!preview->conflicting_fields.contains(choice.key()))
                throw StorageError(StorageErrorKind::InvalidInput,
                                   "merge selection is not a conflicting field: " +
                                   choice.key().toStdString());
        }
    }
    const std::optional<ModelFieldMerge> merged = mergeModelFields(
        id, QStringLiteral("map.annotation"), baseBytes, targetBytes, sourceBytes, fieldChoices);
    if (!merged) return conflict;
    for (const QString& field : merged->conflicting_fields)
        conflict.conflictingFields.push_back(field.toStdString());
    if (placementConflict && !placementChosen)
        conflict.conflictingFields.push_back(placementField.toStdString());
    if (!conflict.conflictingFields.empty() || merged->content.isEmpty()) return conflict;
    const std::optional<MapAnnotation> result =
        aowis::model::ModelCodec<MapAnnotation>::decode(merged->content);
    if (!result) return conflict;
    ModelRecord resolved = target;
    resolved.payload = record::bytesFromQByteArray(merged->content);
    resolved.references.clear();
    if (result->anchor) resolved.references.push_back(record::keyFromUuid(result->anchor->entity_uuid));
    return MergeResolution{std::move(resolved), {}};
}

// Server-owned layer/chunk records are synthetic representations of one
// ordered logical sequence. A concurrent structural difference can be kept
// temporarily; reconciliation below rebuilds the canonical 256-item chunks
// using all three immutable branch images before the candidate is published.
MergeResolution resolveAnnotationStructure(const Key&,
    const std::optional<ModelRecord>& ancestor,
    const std::optional<ModelRecord>& target,
    const std::optional<ModelRecord>& source) {
    const ModelRecord* example = target ? &*target : (source ? &*source : nullptr);
    if (!example || (example->kind != annotationLayerKind &&
                     example->kind != annotationChunkKind))
        return MergeResolution{};
    for (const std::optional<ModelRecord>& record : {ancestor, target, source})
        if (record && record->kind != example->kind) return MergeResolution{};
    // Layer deletion/modification is NOT a mergeable internal change.
    if (example->kind == annotationLayerKind && (!target || !source))
        return MergeResolution{};
    return MergeResolution{target ? *target : *source, {}};
}

using SnapshotReader = std::function<std::optional<ModelRecord>(const Key&)>;
using SnapshotSetter = std::function<void(const Key&, std::optional<ModelRecord>)>;

QList<QUuid> memberIds(const AnnotationLayerSnapshot& layer) {
    QList<QUuid> result;
    result.reserve(layer.annotations.size());
    for (const MapAnnotation& member : layer.annotations) result.append(member.uuid);
    return result;
}

bool appendedOrRemovedOnly(const QList<QUuid>& baseline, const QList<QUuid>& branch) {
    QHash<QUuid, qsizetype> baselinePositions;
    baselinePositions.reserve(baseline.size());
    for (qsizetype index = 0; index < baseline.size(); ++index)
        baselinePositions.insert(baseline.at(index), index);
    qsizetype previous = -1;
    bool sawAddition = false;
    for (const QUuid& id : branch) {
        const QHash<QUuid, qsizetype>::const_iterator found = baselinePositions.constFind(id);
        if (found == baselinePositions.cend()) { sawAddition = true; continue; }
        if (sawAddition) return false;
        const qsizetype index = found.value();
        if (index <= previous) return false;
        previous = index;
    }
    return true;
}

bool equalLayerMetadata(const MapAnnotationLayer& first, const MapAnnotationLayer& second) {
    return aowis::model::ModelCodec<MapAnnotationLayer>::encode(first) ==
           aowis::model::ModelCodec<MapAnnotationLayer>::encode(second);
}

// The records a tip's layer actually stores (root, chunks, members). With the
// chunk-local layout (A5) a tip's chunk UUIDs are not those of a canonical
// rebuild, so they are taken from the loaded snapshot.
void includeSnapshotRecords(std::set<Key>& ids,
                            const std::optional<AnnotationLayerSnapshot>& snapshot) {
    if (!snapshot) return;
    ids.insert(record::keyFromUuid(snapshot->layer.uuid));
    for (const QUuid& chunk : snapshot->chunkIds) ids.insert(record::keyFromUuid(chunk));
    for (const MapAnnotation& annotation : snapshot->annotations)
        ids.insert(record::keyFromUuid(annotation.uuid));
}

std::optional<std::string> reconcileAnnotationMembership(const MergeCandidate& candidate,
    const SnapshotSetter& set, const SnapshotReader& ancestor,
    const SnapshotReader& target, const SnapshotReader& source) {
    try {
        std::set<Key> layerIds;
        candidate.forEach([&layerIds](const Key& id, const ModelRecord& record) {
            if (record.kind == annotationLayerKind) layerIds.insert(id);
        });
        // A layer may be deleted from the candidate but still exist on a tip.
        // Such deletion needs its own explicit domain operation; the B2 checker
        // must never silently let an owning Distribution layer disappear.
        for (const Key& id : layerIds) {
            const QUuid layerId = record::uuidFromKey(id);
            const std::optional<AnnotationLayerSnapshot> baseline =
                loadAnnotationLayer(ancestor, layerId);
            const std::optional<AnnotationLayerSnapshot> left =
                loadAnnotationLayer(target, layerId);
            const std::optional<AnnotationLayerSnapshot> right =
                loadAnnotationLayer(source, layerId);
            if (baseline && (!left || !right))
                return std::string("annotation layer deletion requires an explicit resolution");
            if (!left && !right) continue;
            const MapAnnotationLayer& leftMetadata = left ? left->layer : right->layer;
            const MapAnnotationLayer& rightMetadata = right ? right->layer : left->layer;
            MapAnnotationLayer metadata = leftMetadata;
            if (!equalLayerMetadata(leftMetadata, rightMetadata)) {
                if (!baseline)
                    return std::string("concurrent annotation layer metadata differs");
                if (equalLayerMetadata(leftMetadata, baseline->layer))
                    metadata = rightMetadata;
                else if (!equalLayerMetadata(rightMetadata, baseline->layer))
                    return std::string("concurrent annotation layer metadata conflict");
            }
            const QList<QUuid> baseIds = baseline ? memberIds(*baseline) : QList<QUuid>{};
            const QList<QUuid> leftIds = left ? memberIds(*left) : QList<QUuid>{};
            const QList<QUuid> rightIds = right ? memberIds(*right) : QList<QUuid>{};
            const bool leftChanged = leftIds != baseIds;
            const bool rightChanged = rightIds != baseIds;
            QSet<QUuid> leftMembers;
            QSet<QUuid> rightMembers;
            QSet<QUuid> originalMembers;
            for (const QUuid& id : leftIds) leftMembers.insert(id);
            for (const QUuid& id : rightIds) rightMembers.insert(id);
            for (const QUuid& id : baseIds) originalMembers.insert(id);
            QList<QUuid> requested;
            if (leftChanged && !rightChanged) requested = leftIds;
            else if (rightChanged && !leftChanged) requested = rightIds;
            else {
                // Saves only append and remove members. A different relative
                // order of baseline members can only come from an earlier
                // merge, and no merge choice can express a member order.
                if (!appendedOrRemovedOnly(baseIds, leftIds) ||
                    !appendedOrRemovedOnly(baseIds, rightIds))
                    return std::string("annotation member order changed on both branches; "
                                       "a member order cannot be chosen in a merge, so merge "
                                       "one of the reorderings first");
                // Baseline members preserve their historical order. Deletions
                // win when the other branch only kept an unchanged member;
                // delete-versus-edit of the member record is a native conflict
                // unless the caller chose the surviving/deleted object.
                for (const QUuid& id : baseIds)
                    if (leftMembers.contains(id) || rightMembers.contains(id))
                        requested.append(id);
                // Concurrent inserts are ordered: target additions first,
                // followed by new source additions, preserving each local order.
                for (const QUuid& id : leftIds)
                    if (!originalMembers.contains(id)) requested.append(id);
                for (const QUuid& id : rightIds)
                    if (!originalMembers.contains(id) && !leftMembers.contains(id))
                        requested.append(id);
            }
            // A whole-object choice may preserve an annotation deleted on
            // the other branch. Reintroduce it in baseline order rather than
            // silently discarding the user's explicit selection: before the
            // next later baseline member still present, otherwise before the
            // first newly added member, otherwise at the end. One pass over
            // the lists with hash lookups (no quadratic search).
            QSet<QUuid> requestedSet;
            for (const QUuid& memberId : requested) requestedSet.insert(memberId);
            QHash<QUuid, QList<QUuid>> restoredBefore;
            QList<QUuid> restoredBeforeAdditions;
            std::optional<QUuid> nextPresent;
            QList<QUuid> restoredReversed;
            QList<std::optional<QUuid>> anchors;
            for (qsizetype index = baseIds.size() - 1; index >= 0; --index) {
                const QUuid memberId = baseIds.at(index);
                if (requestedSet.contains(memberId)) {
                    nextPresent = memberId;
                    continue;
                }
                if (!candidate.get(record::keyFromUuid(memberId))) continue;
                if (!leftMembers.contains(memberId) && !rightMembers.contains(memberId))
                    return std::string("selected annotation has no historical membership");
                restoredReversed.append(memberId);
                anchors.append(nextPresent);
            }
            for (qsizetype index = restoredReversed.size() - 1; index >= 0; --index) {
                if (anchors.at(index)) restoredBefore[*anchors.at(index)].append(restoredReversed.at(index));
                else restoredBeforeAdditions.append(restoredReversed.at(index));
            }
            if (!restoredReversed.isEmpty()) {
                QList<QUuid> ordered;
                ordered.reserve(requested.size() + restoredReversed.size());
                bool additionsPlaced = restoredBeforeAdditions.isEmpty();
                for (const QUuid& memberId : requested) {
                    if (!additionsPlaced && !originalMembers.contains(memberId)) {
                        ordered.append(restoredBeforeAdditions);
                        additionsPlaced = true;
                    }
                    const QHash<QUuid, QList<QUuid>>::const_iterator group =
                        restoredBefore.constFind(memberId);
                    if (group != restoredBefore.cend()) ordered.append(group.value());
                    ordered.append(memberId);
                }
                if (!additionsPlaced) ordered.append(restoredBeforeAdditions);
                requested = ordered;
            }
            QList<MapAnnotation> combined;
            QSet<QUuid> seen;
            for (const QUuid& memberId : requested) {
                if (seen.contains(memberId))
                    return std::string("duplicate member in reconciled annotation order");
                seen.insert(memberId);
                const std::optional<ModelRecord> member =
                    candidate.get(record::keyFromUuid(memberId));
                if (!member) continue; // selected deletion / one-sided removal
                if (member->kind != annotationMemberKind)
                    return std::string("reconciled annotation member has wrong record kind");
                QString error;
                const std::optional<MapAnnotation> decoded =
                    aowis::model::ModelCodec<MapAnnotation>::decode(
                        record::qByteArrayFromBytes(member->payload), &error);
                if (!decoded || decoded->uuid != memberId)
                    return std::string("reconciled annotation member is invalid");
                combined.append(*decoded);
            }
            const std::vector<ModelChange> canonical =
                fullDraftAnnotationLayerChanges(metadata, combined);
            std::map<Key, ModelRecord> expected;
            for (const ModelChange& item : canonical)
                expected.emplace(item.key, *item.record);
            std::set<Key> oldIds;
            includeSnapshotRecords(oldIds, baseline);
            includeSnapshotRecords(oldIds, left);
            includeSnapshotRecords(oldIds, right);
            for (const std::pair<const Key, ModelRecord>& item : expected) {
                const std::optional<ModelRecord> current = candidate.get(item.first);
                if (!current || current->kind != item.second.kind ||
                    current->references != item.second.references ||
                    current->payload != item.second.payload)
                    set(item.first, item.second);
            }
            for (const Key& key : oldIds)
                if (!expected.contains(key) && candidate.get(key)) set(key, std::nullopt);
        }
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

} // namespace

MergeResult mergeDistributionAnnotationBranches(
    Repository& repository, const ProjectId& project,
    std::string_view target, std::string_view source,
    const hydraulic::HydraulicMergeSelections& selections,
    const CommitMetadata& metadata) {
    // Everything below is invoked under the native Repository's merge lock;
    // callbacks must be pure and may not call back into the repository.
    const std::function<void(MergeOptions&)> configure =
        [&selections](MergeOptions& options) {
        // Annotation members are resolved through the hydraulic merge's
        // domain hook (below), which also passes and consumes field choices.
        options.resolveStructural = resolveAnnotationStructure;
        const std::vector<hydraulic::HydraulicObjectChoice> objects = selections.objects;
        options.reconcileWithBase = [objects](const MergeCandidate& candidate,
            const SnapshotSetter& set, const SnapshotReader& ancestor,
            const SnapshotReader& target, const SnapshotReader& source) {
            // Internal layer/chunk records are not valid user selections.
            // Their bytes are rebuilt, never selected as a semantic answer.
            for (const hydraulic::HydraulicObjectChoice& choice : objects) {
                const Key id = record::keyFromUuid(choice.entityUuid);
                for (const SnapshotReader& reader : {ancestor, target, source}) {
                    const std::optional<ModelRecord> record = reader(id);
                    if (record && (record->kind == annotationLayerKind ||
                                   record->kind == annotationChunkKind))
                        return std::optional<std::string>(
                            "cannot explicitly select internal annotation layer/chunk records");
                }
            }
            return reconcileAnnotationMembership(candidate, set, ancestor, target, source);
        };
        // The B2 checker reconstructs complete hydraulic aggregates as well as
        // annotation layers; it subsumes the old hydraulic-only candidate check.
        options.validate = validateAnnotationSnapshot;
    };
    const hydraulic::DomainMergeResolver annotationResolver =
        [](const Key& key, const ModelRecord& base, const ModelRecord& targetRecord,
           const ModelRecord& sourceRecord,
           const QMap<QString, bool>& fieldChoices) -> std::optional<MergeResolution> {
        if (base.kind != annotationMemberKind && targetRecord.kind != annotationMemberKind &&
            sourceRecord.kind != annotationMemberKind)
            return std::nullopt;
        return resolveAnnotationMember(key, base, targetRecord, sourceRecord, fieldChoices);
    };
    return hydraulic::mergeHydraulicBranches(repository, project, target, source,
                                           selections, metadata, configure, annotationResolver);
}

} // namespace aowis::dvmdb::annotations
